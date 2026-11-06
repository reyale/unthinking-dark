#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <string>
#include "config.hpp"
#include "frame.hpp"
#include "match.hpp"
#include "replay_io.hpp"
#include "world.hpp"
#include "wasm_bot.hpp"
#include "../../fixtures/idle_bot.hpp"

// An idle WASM bot: exports the required ABI surface, always returns 0 commands.
//
// Memory layout:
//   pages 0-4 (320 KB): snapshot buffer (SNAPSHOT_ADDR = 0)
//   page 5    ( 64 KB): command buffer  (COMMAND_ADDR  = 327680)
// This covers the worst-case snapshot size for v1 caps.
static constexpr std::string_view IDLE_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func   (export "abi_version") (result i32) i32.const @ABI@)
  (func   (export "init")    (param i32 i64))
  (func   (export "on_tick") (result i32) i32.const 0)
)
)wat";

// Minimal WAT with all required exports but only 1 page (64 KB) — intentionally
// too small for the worst-case snapshot so load validation rejects it.
static constexpr std::string_view TOO_SMALL_WAT = R"wat(
(module
  (memory (export "memory") 1)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 60000))
  (func   (export "abi_version") (result i32) i32.const @ABI@)
  (func   (export "init")    (param i32 i64))
  (func   (export "on_tick") (result i32) i32.const 0)
)
)wat";

// WAT bot that burns all its fuel in a tight loop during init.
static constexpr std::string_view FUEL_HOG_INIT_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64)
    (block $break
      (loop $loop
        br $loop)))
  (func (export "on_tick") (result i32) i32.const 0)
)
)wat";

// Reads SnapshotHeader.tick (offset 0) and writes it into Command[0].ax
// (COMMAND_ADDR + 8), then returns 1 command.  Proves snapshot bytes reach the bot.
static constexpr std::string_view TICK_ECHO_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (i32.store
      (i32.const 327688)
      (i32.load (i32.const 0)))
    (i32.const 1))
)
)wat";

// Writes a known magic command (entity_id=57005, ax=7, ay=9) to COMMAND_ADDR and
// returns 1.  Proves command bytes written by the bot are delivered to the engine.
static constexpr std::string_view COMMAND_WRITER_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (i32.store (i32.const 327680) (i32.const 57005))
    (i32.store (i32.const 327688) (i32.const 7))
    (i32.store (i32.const 327692) (i32.const 9))
    (i32.const 1))
)
)wat";

// Returns SnapshotHeader.my_unit_count (offset 32) as the command count.
// Proves the engine delivers the correct unit count in the header.
static constexpr std::string_view UNIT_COUNT_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (i32.load (i32.const 32)))
)
)wat";

// Reads my_units_off from SnapshotHeader (offset 44), loads UnitView[0].id from
// that offset, writes it as Command[0].entity_id, returns 1.
// Proves the full snapshot layout (header offsets → array data → unit id) is correct.
static constexpr std::string_view UNIT_ID_ROUNDTRIP_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (local $units_off i32)
    (local.set $units_off (i32.load (i32.const 44)))
    (i32.store
      (i32.const 327680)
      (i32.load (local.get $units_off)))
    (i32.const 1))
)
)wat";

// Burns all per-tick fuel in on_tick (not init) — verifies tick forfeit behaviour.
static constexpr std::string_view TICK_FUEL_HOG_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (block $break
      (loop $loop
        br $loop))
    i32.const 0)
)
)wat";

// Executes unreachable in on_tick — verifies trap recovery behaviour.
static constexpr std::string_view TRAP_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    unreachable)
)
)wat";

// Bot reporting the wrong ABI version — must be rejected at load time.
static constexpr std::string_view ABI_MISMATCH_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const 5)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32) i32.const 0)
)
)wat";

// ---- Helpers ----------------------------------------------------------------

static std::string fill_abi(std::string_view wat) {
  std::string s(wat);
  const std::string tok = "@ABI@";
  const std::string ver = std::to_string(game::cfg::ABI_VERSION);
  for (size_t p; (p = s.find(tok)) != std::string::npos; )
    s.replace(p, tok.size(), ver);
  return s;
}

static std::vector<uint8_t> to_wasm(std::string_view wat) {
  return runner::wat_to_wasm(fill_abi(wat));
}

static game::World make_world(uint64_t seed = 42) {
  game::World w;
  w.map = game::Map::make(20, 20);
  w.rng = game::Rng{seed};
  w.rng_seed = seed;
  w.spawn_structure({0}, game::StructureType::CommandCore, {2, 2});
  w.spawn_unit({0}, game::UnitType::Drone, {3, 2});
  w.spawn_structure({1}, game::StructureType::CommandCore, {17, 17});
  w.spawn_unit({1}, game::UnitType::Drone, {16, 17});
  return w;
}

// ---- Tests ------------------------------------------------------------------

TEST(WasmBot, IdleBotLoads) {
  auto wasm = to_wasm(IDLE_WAT);
  runner::WasmBot bot(wasm);
  EXPECT_TRUE(bot.healthy());
  EXPECT_TRUE(bot.last_error().empty());
}

TEST(WasmBot, IdleBotRunsFullMatch) {
  auto wasm = to_wasm(IDLE_WAT);
  runner::WasmBot bot_a(wasm);
  runner::WasmBot bot_b(to_wasm(IDLE_WAT));

  auto w = make_world(7);
  auto rec = game::run_match(w, bot_a, 0, bot_b, 1, 200);

  EXPECT_TRUE(bot_a.healthy());
  EXPECT_TRUE(bot_b.healthy());
  // Both bots idle → tick cap reached; territory 0-0 so result is Draw.
  EXPECT_EQ(rec.outcome.reason, game::WinReason::Draw);
}

TEST(WasmBot, WasmMatchReproducesHash) {
  auto wasm = to_wasm(IDLE_WAT);
  auto w = make_world(99);
  runner::WasmBot bot_a(wasm);
  runner::WasmBot bot_b(to_wasm(IDLE_WAT));
  auto rec = game::run_match(w, bot_a, 0, bot_b, 1, 200);

  uint64_t replayed = game::replay(rec.replay);
  EXPECT_EQ(replayed, rec.replay.expected_hash);
}

TEST(WasmBot, WasmAndScriptedBotProduceSameHash) {
  // An idle WASM bot and our C++ IdleBot are semantically identical.
  // Running the same seed through both should produce the same final hash.
  auto wasm = to_wasm(IDLE_WAT);

  auto run_wasm = [&](uint64_t seed) {
    auto w = make_world(seed);
    runner::WasmBot a(wasm), b(to_wasm(IDLE_WAT));
    return game::run_match(w, a, 0, b, 1, 200).replay.expected_hash;
  };

  auto run_scripted = [](uint64_t seed) {
    auto w = make_world(seed);
    game::IdleBot a, b;
    return game::run_match(w, a, 0, b, 1, 200).replay.expected_hash;
  };

  for (uint64_t seed : {1ULL, 42ULL, 99ULL}) {
    EXPECT_EQ(run_wasm(seed), run_scripted(seed))
        << "hash mismatch at seed " << seed;
  }
}

TEST(WasmBot, TooSmallMemoryRejectedAtLoad) {
  auto wasm = to_wasm(TOO_SMALL_WAT);
  runner::WasmBot bot(wasm);
  EXPECT_FALSE(bot.healthy());
  EXPECT_FALSE(bot.last_error().empty());
}

TEST(WasmBot, MissingExportRejectedAtLoad) {
  // A module with no exports at all.
  auto wasm = to_wasm("(module (memory 6))");
  runner::WasmBot bot(wasm);
  EXPECT_FALSE(bot.healthy());
}

TEST(WasmBot, AbiMismatchRejectedAtLoad) {
  auto wasm = to_wasm(ABI_MISMATCH_WAT);
  runner::WasmBot bot(wasm);
  EXPECT_FALSE(bot.healthy());
  EXPECT_NE(bot.last_error().find("mismatch"), std::string::npos);
}

TEST(WasmBot, InitFuelExhaustionMarksUnhealthy) {
  auto wasm = to_wasm(FUEL_HOG_INIT_WAT);
  runner::WasmBot bot(wasm);
  ASSERT_TRUE(bot.healthy()); // loads fine

  auto w = make_world();
  bot.on_init(w.map, 0, 0); // burns all init fuel

  EXPECT_FALSE(bot.healthy());
  EXPECT_FALSE(bot.last_error().empty());
}

TEST(WasmBot, DeterministicAcrossRuns) {
  auto wasm = to_wasm(IDLE_WAT);

  auto run = [&](uint64_t seed) {
    auto w = make_world(seed);
    runner::WasmBot a(wasm), b(to_wasm(IDLE_WAT));
    return game::run_match(w, a, 0, b, 1, 100).replay.expected_hash;
  };

  for (uint64_t seed : {5ULL, 17ULL, 31ULL}) {
    EXPECT_EQ(run(seed), run(seed)) << "non-deterministic at seed " << seed;
  }
}

// ---- Snapshot/command round-trip tests --------------------------------------

// Helper: empty snapshot with just the counts set.
static game::Snapshot make_snap(uint32_t tick = 0) {
  game::Snapshot s;
  s.header.tick = tick;
  return s;
}

TEST(WasmBot, SnapshotTickDeliveredToBot) {
  // TICK_ECHO_WAT reads SnapshotHeader.tick and writes it to Command[0].ax.
  runner::WasmBot bot(to_wasm(TICK_ECHO_WAT));
  ASSERT_TRUE(bot.healthy());

  auto snap = make_snap(42);
  auto cmds = bot.on_tick(snap);

  ASSERT_EQ(cmds.size(), 1u);
  EXPECT_EQ(cmds[0].ax, 42);
}

TEST(WasmBot, CommandWrittenByBotIsDelivered) {
  // COMMAND_WRITER_WAT stores known values to COMMAND_ADDR and returns 1.
  runner::WasmBot bot(to_wasm(COMMAND_WRITER_WAT));
  ASSERT_TRUE(bot.healthy());

  auto cmds = bot.on_tick(make_snap());

  ASSERT_EQ(cmds.size(), 1u);
  EXPECT_EQ(cmds[0].entity_id, 57005u); // 0xDEAD
  EXPECT_EQ(cmds[0].ax, 7);
  EXPECT_EQ(cmds[0].ay, 9);
}

TEST(WasmBot, SnapshotUnitCountReadByBot) {
  // UNIT_COUNT_WAT returns my_unit_count from the header as the command count.
  runner::WasmBot bot(to_wasm(UNIT_COUNT_WAT));
  ASSERT_TRUE(bot.healthy());

  game::Snapshot snap;
  snap.header.my_unit_count = 3;
  snap.my_units.resize(3); // matching data so memcpy is valid

  auto cmds = bot.on_tick(snap);
  EXPECT_EQ(cmds.size(), 3u);
}

TEST(WasmBot, UnitIdRoundTripThroughSnapshot) {
  // UNIT_ID_ROUNDTRIP_WAT reads my_units_off from the header, loads UnitView[0].id
  // from that offset, and writes it as Command[0].entity_id.
  runner::WasmBot bot(to_wasm(UNIT_ID_ROUNDTRIP_WAT));
  ASSERT_TRUE(bot.healthy());

  game::Snapshot snap;
  snap.header.my_unit_count = 1;
  game::UnitView uv{};
  uv.id = 999;
  snap.my_units.push_back(uv);

  auto cmds = bot.on_tick(snap);

  ASSERT_EQ(cmds.size(), 1u);
  EXPECT_EQ(cmds[0].entity_id, 999u);
}

// A fault mid-tick may leave the bot's memory half-updated, so the bot is done: it loses.
TEST(WasmBot, TickFaultEndsTheBot) {
  for (auto wat : {TICK_FUEL_HOG_WAT, TRAP_WAT}) {
    runner::WasmBot bot(to_wasm(wat));
    ASSERT_TRUE(bot.healthy());
    bot.on_init(make_world().map, 0, 0);
    ASSERT_TRUE(bot.healthy());

    EXPECT_TRUE(bot.on_tick(make_snap()).empty());
    EXPECT_FALSE(bot.healthy());
    EXPECT_FALSE(bot.last_error().empty());
    EXPECT_TRUE(bot.on_tick(make_snap(1)).empty()) << "never called again";
  }
}

// ---- SDK compiled .wasm lifecycle tests -------------------------------------
// These tests load .wasm files built from sdk/cpp/examples/ by clang++-18.
// The CMake custom command ensures they are compiled before runner_tests runs.
// If clang++-18 is unavailable the files won't exist and tests skip with a
// clear message rather than a cryptic failure.

#ifndef SDK_EXAMPLES_DIR
#define SDK_EXAMPLES_DIR ""
#endif

static std::string sdk_path(const char* name) {
  return std::string(SDK_EXAMPLES_DIR) + "/" + name;
}

static bool file_exists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

// World with combat units on both sides — used across SDK lifecycle tests.
static game::World make_combat_world(uint64_t seed) {
  game::World w;
  w.map = game::Map::make(20, 20);
  w.rng = game::Rng{seed};
  w.rng_seed = seed;
  w.spawn_structure({0}, game::StructureType::CommandCore, {2, 2});
  w.spawn_unit({0}, game::UnitType::Frigate,     {3, 2});
  w.spawn_unit({0}, game::UnitType::Interceptor, {4, 2});
  w.spawn_structure({1}, game::StructureType::CommandCore, {17, 17});
  w.spawn_unit({1}, game::UnitType::Drone, {16, 17});
  return w;
}

// ---- Step 1: load -----------------------------------------------------------

TEST(SdkLifecycle, IdleBotLoads) {
  const std::string path = sdk_path("idle_bot.wasm");
  if (!file_exists(path)) GTEST_SKIP() << "idle_bot.wasm not built (clang++-18 required)";

  runner::WasmBot bot(runner::load_wasm_file(path));
  EXPECT_TRUE(bot.healthy()) << bot.last_error();
}

TEST(SdkLifecycle, RushBotLoads) {
  const std::string path = sdk_path("rush_bot.wasm");
  if (!file_exists(path)) GTEST_SKIP() << "rush_bot.wasm not built (clang++-18 required)";

  runner::WasmBot bot(runner::load_wasm_file(path));
  EXPECT_TRUE(bot.healthy()) << bot.last_error();
}

// ---- Step 2: match execution ------------------------------------------------

TEST(SdkLifecycle, IdleBotVsIdleEndsAtTickCap) {
  const std::string path = sdk_path("idle_bot.wasm");
  if (!file_exists(path)) GTEST_SKIP() << "idle_bot.wasm not built (clang++-18 required)";

  auto wasm = runner::load_wasm_file(path);
  runner::WasmBot a(wasm), b(wasm);
  auto w = make_world(55);
  auto rec = game::run_match(w, a, 0, b, 1, 200);

  EXPECT_TRUE(a.healthy());
  EXPECT_TRUE(b.healthy());
  EXPECT_EQ(rec.outcome.reason, game::WinReason::Draw);
}

TEST(SdkLifecycle, RushBotVsIdleEndsInBaseDeath) {
  const std::string rush_path = sdk_path("rush_bot.wasm");
  const std::string idle_path = sdk_path("idle_bot.wasm");
  if (!file_exists(rush_path) || !file_exists(idle_path))
    GTEST_SKIP() << "sdk wasm bots not built (clang++-18 required)";

  runner::WasmBot a(runner::load_wasm_file(rush_path));
  runner::WasmBot b(runner::load_wasm_file(idle_path));
  ASSERT_TRUE(a.healthy()) << a.last_error();
  ASSERT_TRUE(b.healthy()) << b.last_error();

  auto w = make_combat_world(13);
  auto rec = game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);

  EXPECT_TRUE(a.healthy());
  EXPECT_EQ(rec.outcome.winner, game::FactionId{0});
  EXPECT_EQ(rec.outcome.reason, game::WinReason::BaseDestroyed);
}

// ---- Step 3: in-memory replay -----------------------------------------------

TEST(SdkLifecycle, RushBotMatchReproducesHashInMemory) {
  const std::string rush_path = sdk_path("rush_bot.wasm");
  const std::string idle_path = sdk_path("idle_bot.wasm");
  if (!file_exists(rush_path) || !file_exists(idle_path))
    GTEST_SKIP() << "sdk wasm bots not built (clang++-18 required)";

  runner::WasmBot a(runner::load_wasm_file(rush_path));
  runner::WasmBot b(runner::load_wasm_file(idle_path));
  auto w = make_combat_world(33);
  auto rec = game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);

  EXPECT_EQ(game::replay(rec.replay), rec.replay.expected_hash);
}

// ---- Step 4: file round-trip ------------------------------------------------

TEST(SdkLifecycle, RushBotReplayFileRoundTrip) {
  const std::string rush_path = sdk_path("rush_bot.wasm");
  const std::string idle_path = sdk_path("idle_bot.wasm");
  if (!file_exists(rush_path) || !file_exists(idle_path))
    GTEST_SKIP() << "sdk wasm bots not built (clang++-18 required)";

  runner::WasmBot a(runner::load_wasm_file(rush_path));
  runner::WasmBot b(runner::load_wasm_file(idle_path));
  auto w = make_combat_world(44);
  auto rec = game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);

  const std::string tmp = "/tmp/ud_sdk_roundtrip.ud";
  game::write_replay_file(rec.replay, tmp);
  auto read_log = game::read_replay_file(tmp);
  std::remove(tmp.c_str());

  EXPECT_EQ(game::replay(read_log), rec.replay.expected_hash);
}

// ---- Step 5: frame extraction (viz pipeline) --------------------------------

TEST(SdkLifecycle, RushBotFramesExtractedFromReplay) {
  const std::string rush_path = sdk_path("rush_bot.wasm");
  const std::string idle_path = sdk_path("idle_bot.wasm");
  if (!file_exists(rush_path) || !file_exists(idle_path))
    GTEST_SKIP() << "sdk wasm bots not built (clang++-18 required)";

  runner::WasmBot a(runner::load_wasm_file(rush_path));
  runner::WasmBot b(runner::load_wasm_file(idle_path));
  auto w = make_combat_world(55);
  auto rec = game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);

  auto frames = game::replay_frames(rec.replay);

  // Initial frame + at least one tick frame
  ASSERT_GT(frames.size(), 1u);
  // First frame is pre-tick state; no result yet
  EXPECT_FALSE(frames.front().result.has_value());
  // Last frame carries the match result
  ASSERT_TRUE(frames.back().result.has_value());
  EXPECT_EQ(frames.back().result->reason, game::WinReason::BaseDestroyed);
}

// ---- Step 6: determinism ----------------------------------------------------

TEST(SdkLifecycle, RushBotDeterministicAcrossSeeds) {
  const std::string rush_path = sdk_path("rush_bot.wasm");
  const std::string idle_path = sdk_path("idle_bot.wasm");
  if (!file_exists(rush_path) || !file_exists(idle_path))
    GTEST_SKIP() << "sdk wasm bots not built (clang++-18 required)";

  auto rush_bytes = runner::load_wasm_file(rush_path);
  auto idle_bytes = runner::load_wasm_file(idle_path);

  auto run = [&](uint64_t seed) {
    auto w = make_combat_world(seed);
    runner::WasmBot a(rush_bytes), b(idle_bytes);
    return game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP).replay.expected_hash;
  };

  for (uint64_t seed : {7ULL, 42ULL, 99ULL})
    EXPECT_EQ(run(seed), run(seed)) << "non-deterministic at seed " << seed;
}

// ---- Step 7: metrics captured in replay -------------------------------------

TEST(SdkLifecycle, FuelAndMemoryRecordedInReplay) {
  const std::string rush_path = sdk_path("rush_bot.wasm");
  const std::string idle_path = sdk_path("idle_bot.wasm");
  if (!file_exists(rush_path) || !file_exists(idle_path))
    GTEST_SKIP() << "sdk wasm bots not built (clang++-18 required)";

  runner::WasmBot a(runner::load_wasm_file(rush_path));
  runner::WasmBot b(runner::load_wasm_file(idle_path));
  auto w = make_combat_world(66);
  auto rec = game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);

  // Init fuel: wasm bots consume some fuel even in a trivial init.
  EXPECT_GT(rec.replay.init_fuel_a, 0u);
  EXPECT_GT(rec.replay.init_fuel_b, 0u);
  // Init fuel must be within the startup budget.
  EXPECT_LE(rec.replay.init_fuel_a, game::cfg::FUEL_STARTUP);
  EXPECT_LE(rec.replay.init_fuel_b, game::cfg::FUEL_STARTUP);

  // Memory is non-zero (wasm linear memory is at least 1 page = 64KB).
  EXPECT_GT(rec.replay.init_mem_bytes_a, 0u);
  EXPECT_GT(rec.replay.init_mem_bytes_b, 0u);

  ASSERT_FALSE(rec.replay.ticks.empty());
  for (const auto& entry : rec.replay.ticks) {
    // Rush bot issues commands so it burns fuel each tick.
    EXPECT_GT(entry.fuel_a, 0u);
    EXPECT_LE(entry.fuel_a, game::cfg::FUEL_PER_TICK);
    // Idle bot returns immediately but still burns some fuel.
    EXPECT_GT(entry.fuel_b, 0u);
    EXPECT_LE(entry.fuel_b, game::cfg::FUEL_PER_TICK);
    // Memory is stable (wasm doesn't grow memory in these bots).
    EXPECT_EQ(entry.mem_bytes_a, rec.replay.init_mem_bytes_a);
    EXPECT_EQ(entry.mem_bytes_b, rec.replay.init_mem_bytes_b);
  }
}

// ---- Memory cap (cfg::MEMORY_CAP_MB) ----------------------------------------
//
// A grow past the cap is refused (memory.grow returns -1) and the bot faults: its
// commands are discarded even if the call completed, and it loses the match.

static constexpr uint32_t kCapPages = game::cfg::MEMORY_CAP_MB * 16; // 64 KiB pages

// Writes a command, then asks for `@PAGES@` more pages, then returns 1 command.
// Records memory.grow's result in a global and reports it as ax on the next call.
static std::string grow_wat(uint32_t pages, bool only_even_ticks) {
  std::string wat = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (global $last (mut i32) (i32.const 12345))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (i32.store (i32.const 327680) (i32.const 57005))   ;; entity_id
    (i32.store (i32.const 327688) (global.get $last))  ;; ax = previous grow result
    (if (i32.or (i32.const @ALWAYS@) (i32.eqz (i32.rem_u (i32.load (i32.const 0)) (i32.const 2))))
      (then (global.set $last (memory.grow (i32.const @PAGES@)))))
    (i32.const 1))
)
)wat";
  wat.replace(wat.find("@PAGES@"), 7, std::to_string(pages));
  wat.replace(wat.find("@ALWAYS@"), 8, only_even_ticks ? "0" : "1");
  return wat;
}

TEST(WasmBot, GrowPastMemoryCapFaultsTheBot) {
  runner::WasmBot bot(to_wasm(grow_wat(kCapPages, false)));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0, 0);
  const uint64_t before = bot.last_metrics().memory_bytes;

  auto cmds = bot.on_tick(make_snap());
  EXPECT_TRUE(cmds.empty()) << "commands discarded even though the call completed";
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::OutOfMemory);
  EXPECT_EQ(bot.last_metrics().memory_bytes, before) << "the grow was refused";
  EXPECT_FALSE(bot.healthy()) << "a fault even though the bot coped with the refusal";
}

TEST(WasmBot, GrowingExactlyToTheCapIsAllowed) {
  runner::WasmBot bot(to_wasm(grow_wat(kCapPages - 6, false))); // 6 initial pages
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0, 0);
  EXPECT_EQ(bot.on_tick(make_snap()).size(), 1u);
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::None);
  EXPECT_EQ(bot.last_metrics().memory_bytes, uint64_t{game::cfg::MEMORY_CAP_MB} << 20);

  EXPECT_TRUE(bot.on_tick(make_snap()).empty()) << "now at the cap, any further grow is over";
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::OutOfMemory);
}

TEST(WasmBot, SmallGrowIsNotAFault) {
  runner::WasmBot bot(to_wasm(grow_wat(10, false)));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0, 0);
  EXPECT_EQ(bot.on_tick(make_snap()).size(), 1u);
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::None);
  EXPECT_EQ(bot.last_metrics().memory_bytes, 16u * 65536);
}

TEST(WasmBot, InitGrowPastMemoryCapFailsLoad) {
  std::string wat = grow_wat(kCapPages, false);
  wat.replace(wat.find("(func (export \"init\") (param i32 i64))"),
              std::string("(func (export \"init\") (param i32 i64))").size(),
              "(func (export \"init\") (param i32 i64) (drop (memory.grow (i32.const " +
                  std::to_string(kCapPages) + "))))");
  runner::WasmBot bot(to_wasm(wat));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0, 0);
  EXPECT_FALSE(bot.healthy()) << "an init overrun is a load failure";
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::OutOfMemory);
}

TEST(WasmBot, InitialMemoryOverCapRejectedAtLoad) {
  std::string wat(IDLE_WAT);
  wat.replace(wat.find("(memory (export \"memory\") 6)"), 28,
              "(memory (export \"memory\") " + std::to_string(kCapPages + 1) + ")");
  runner::WasmBot bot(to_wasm(wat));
  EXPECT_FALSE(bot.healthy());
  EXPECT_FALSE(bot.last_error().empty());
}

TEST(WasmBot, FuelAndTrapForfeitsCarryTheirCause) {
  runner::WasmBot hog(to_wasm(TICK_FUEL_HOG_WAT));
  hog.on_init(make_world().map, 0, 0);
  hog.on_tick(make_snap());
  EXPECT_EQ(hog.last_metrics().fault, game::TickFault::OutOfFuel);

  runner::WasmBot crash(to_wasm(TRAP_WAT));
  crash.on_init(make_world().map, 0, 0);
  crash.on_tick(make_snap());
  EXPECT_EQ(crash.last_metrics().fault, game::TickFault::Trap);

  runner::WasmBot fine(to_wasm(IDLE_WAT));
  fine.on_init(make_world().map, 0, 0);
  fine.on_tick(make_snap());
  EXPECT_EQ(fine.last_metrics().fault, game::TickFault::None);
}

// ---- A fault loses the match ----------------------------------------------

// Traps on snapshot tick @TICK@, plays (no commands) before it.
static std::string trap_on_tick_wat(uint32_t tick) {
  std::string wat = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (if (i32.eq (i32.load (i32.const 0)) (i32.const @TICK@)) (then unreachable))
    (i32.const 0))
)
)wat";
  wat.replace(wat.find("@TICK@"), 6, std::to_string(tick));
  return wat;
}

// The faulting tick is recorded, with its cause, but not played: the match ends as it stood.
TEST(WasmBot, FaultLosesTheMatchThere) {
  constexpr uint32_t kFaultTick = 5;
  auto w = make_world();
  runner::WasmBot crash(to_wasm(trap_on_tick_wat(kFaultTick)));
  game::IdleBot idle;
  ASSERT_TRUE(crash.healthy()) << crash.last_error();
  auto rec = game::run_match(w, idle, 0, crash, 1, 100);

  EXPECT_EQ(rec.outcome.reason, game::WinReason::Forfeit);
  EXPECT_EQ(rec.outcome.winner.value, 0u);
  EXPECT_EQ(rec.ticks_played, kFaultTick) << "the faulting tick isn't played";
  ASSERT_EQ(rec.replay.ticks.size(), kFaultTick + 1);
  EXPECT_EQ(rec.replay.ticks.back().fault_b, game::TickFault::Trap);
  EXPECT_EQ(rec.replay.ticks.back().fault_a, game::TickFault::None);

  game::MemoryWriter mw;
  game::write_replay(rec.replay, mw);
  game::MemoryReader mr(mw.data);
  const auto back = game::read_replay(mr);
  EXPECT_EQ(back.outcome.reason, game::WinReason::Forfeit);
  EXPECT_EQ(back.ticks.back().fault_b, game::TickFault::Trap);
  EXPECT_EQ(game::replay(back), rec.replay.expected_hash);

  uint64_t fingerprint = 0;
  const auto frames = game::replay_frames(back, &fingerprint);
  EXPECT_EQ(fingerprint, rec.replay.expected_hash);
  ASSERT_EQ(frames.size(), kFaultTick + 2) << "initial frame, one per tick, the fault's";
  const auto& last = frames.back();
  ASSERT_TRUE(last.result.has_value());
  EXPECT_EQ(last.result->reason, game::WinReason::Forfeit);
  EXPECT_EQ(last.forfeits[1], game::TickFault::Trap);
  EXPECT_EQ(game::result_reason(*last.result, last.forfeits, {"A", "B"}), "B forfeited: crashed");
  EXPECT_EQ(last.tick, frames[frames.size() - 2].tick) << "the board as it stood";
  for (size_t f = 0; f + 1 < frames.size(); ++f) EXPECT_FALSE(frames[f].result.has_value());
}

TEST(WasmBot, BothFaultingInOneTickIsADraw) {
  auto w = make_world();
  runner::WasmBot a(to_wasm(trap_on_tick_wat(3))), b(to_wasm(trap_on_tick_wat(3)));
  auto rec = game::run_match(w, a, 0, b, 1, 100);
  EXPECT_EQ(rec.outcome.reason, game::WinReason::Draw);
  EXPECT_EQ(rec.ticks_played, 3u);
  EXPECT_EQ(game::result_reason(rec.outcome, {game::TickFault::Trap, game::TickFault::Trap},
                                {"A", "B"}),
            "A forfeited: crashed; B forfeited: crashed");
  EXPECT_EQ(game::replay(rec.replay), rec.replay.expected_hash);
}

TEST(WasmBot, MemoryFaultIsRecordedAsItsCause) {
  auto w = make_world();
  runner::WasmBot hog(to_wasm(grow_wat(kCapPages, false)));
  game::IdleBot idle;
  auto rec = game::run_match(w, hog, 0, idle, 1, 20);
  EXPECT_EQ(rec.outcome.reason, game::WinReason::Forfeit);
  EXPECT_EQ(rec.outcome.winner.value, 1u);
  ASSERT_EQ(rec.replay.ticks.size(), 1u);
  EXPECT_EQ(rec.replay.ticks[0].fault_a, game::TickFault::OutOfMemory);
  EXPECT_TRUE(rec.replay.ticks[0].raw_a.empty()) << "the commands it returned are discarded";
  EXPECT_EQ(game::result_reason(rec.outcome, {rec.replay.ticks[0].fault_a, game::TickFault::None},
                                {"A", "B"}),
            "A forfeited: went over the memory cap");
}

// A bot that can't finish init can't play: it loses at tick 0, with its init fault as cause.
TEST(WasmBot, InitFaultLosesAtTickZero) {
  auto w = make_world();
  runner::WasmBot hog(to_wasm(FUEL_HOG_INIT_WAT));
  game::IdleBot idle;
  auto rec = game::run_match(w, idle, 0, hog, 1, 20);
  EXPECT_EQ(rec.outcome.reason, game::WinReason::Forfeit);
  EXPECT_EQ(rec.outcome.winner.value, 0u);
  EXPECT_EQ(rec.ticks_played, 0u);
  ASSERT_EQ(rec.replay.ticks.size(), 1u);
  EXPECT_EQ(rec.replay.ticks[0].fault_b, game::TickFault::OutOfFuel);
  EXPECT_EQ(game::result_reason(rec.outcome, {game::TickFault::None, rec.replay.ticks[0].fault_b},
                                {"A", "B"}),
            "B forfeited: ran out of fuel");
}

// ---- Bulk memory (disabled) -------------------------------------------------
//
// memory.fill/memory.copy cost one unit of fuel whatever their length, so they would let a bot
// burn unbounded CPU inside its budget. The runner rejects any module that uses them; the SDK
// builds with -mno-bulk-memory and supplies loop-based memcpy/memset/memmove instead.

static std::string bulk_wat(std::string_view instr) {
  std::string wat(IDLE_WAT);
  const std::string tick = "(func   (export \"on_tick\") (result i32) i32.const 0)";
  wat.replace(wat.find(tick), tick.size(),
              "(func (export \"on_tick\") (result i32) (" + std::string(instr) +
                  " (i32.const 0) (i32.const 0) (i32.const 65536)) i32.const 0)");
  return wat;
}

TEST(WasmBot, BulkMemoryInstructionsRejectedAtLoad) {
  for (std::string_view instr : {"memory.fill", "memory.copy"}) {
    runner::WasmBot bot(to_wasm(bulk_wat(instr)));
    EXPECT_FALSE(bot.healthy()) << instr;
    EXPECT_NE(bot.last_error().find("bulk memory"), std::string::npos)
        << instr << ": " << bot.last_error();
  }
}

TEST(SdkLifecycle, LargeCopiesRunAsMeteredLoops) {
  const std::string path = sdk_path("copy_bot.wasm");
  if (!file_exists(path)) GTEST_SKIP() << "copy_bot.wasm not built (clang++-18 required)";

  runner::WasmBot bot(runner::load_wasm_file(path));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0, 0);
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  for (uint32_t t = 0; t < 3; ++t) {
    bot.on_tick(make_snap(t));
    EXPECT_EQ(bot.last_metrics().fault, game::TickFault::None)
        << "tick " << t << ": the bot traps if a copy, clear or move is wrong";
    // A 64 KiB copy plus clear plus move: at one word per iteration, tens of thousands of fuel.
    EXPECT_GT(bot.last_metrics().fuel_consumed, 50'000u) << "copies were not charged per word";
  }
}

// ---- Snapshot: my structures ------------------------------------------------

// Reads my_structures_off (header offset 60) and my_structure_count (56); writes
// StructureView[0].id as Command[0].entity_id and StructureView[0].x as ax; returns the count.
static constexpr std::string_view STRUCTURE_ECHO_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (local $off i32)
    (local.set $off (i32.load (i32.const 60)))
    (i32.store (i32.const 327680) (i32.load (local.get $off)))
    (i32.store (i32.const 327688) (i32.load (i32.add (local.get $off) (i32.const 8))))
    (i32.load (i32.const 56)))
)
)wat";

TEST(WasmBot, StructureViewRoundTripThroughSnapshot) {
  static_assert(offsetof(game::SnapshotHeader, my_structures_off) == 60);
  static_assert(offsetof(game::SnapshotHeader, my_structure_count) == 56);
  runner::WasmBot bot(to_wasm(STRUCTURE_ECHO_WAT));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();

  game::Snapshot snap;
  snap.my_units.resize(2); // structures sit after the units: the offset must account for them
  game::StructureView sv{};
  sv.id = 4242;
  sv.x = 17;
  snap.my_structures.push_back(sv);
  auto cmds = bot.on_tick(snap);
  ASSERT_EQ(cmds.size(), 1u);
  EXPECT_EQ(cmds[0].entity_id, 4242u);
  EXPECT_EQ(cmds[0].ax, 17);
}

// Reads enemy_structures_off (header offset 68) and the count (64); writes
// EnemyStructureView[0].id as Command[0].entity_id and its y as ay; returns the count.
static constexpr std::string_view ENEMY_STRUCTURE_ECHO_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
  (func (export "on_tick") (result i32)
    (local $off i32)
    (local.set $off (i32.load (i32.const 68)))
    (i32.store (i32.const 327680) (i32.load (local.get $off)))
    (i32.store (i32.const 327692) (i32.load (i32.add (local.get $off) (i32.const 12))))
    (i32.load (i32.const 64)))
)
)wat";

TEST(WasmBot, EnemyStructureViewRoundTripThroughSnapshot) {
  static_assert(offsetof(game::SnapshotHeader, enemy_structures_off) == 68);
  static_assert(offsetof(game::SnapshotHeader, visible_enemy_structure_count) == 64);
  runner::WasmBot bot(to_wasm(ENEMY_STRUCTURE_ECHO_WAT));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();

  game::Snapshot snap; // arrays before it, so the offset has to account for each
  snap.my_units.resize(2);
  snap.my_structures.resize(3);
  snap.visible_enemies.resize(4);
  game::EnemyStructureView es{};
  es.id = 777;
  es.y = 23;
  snap.visible_enemy_structures.push_back(es);
  auto cmds = bot.on_tick(snap);
  ASSERT_EQ(cmds.size(), 1u);
  EXPECT_EQ(cmds[0].entity_id, 777u);
  EXPECT_EQ(cmds[0].ay, 23);
}

// The engine never builds an over-cap snapshot; if one reached the runner, the copy must still
// stay inside the buffer the bot was validated to have.
TEST(WasmBot, OverCapSnapshotIsClampedToTheBuffer) {
  runner::WasmBot bot(to_wasm(UNIT_COUNT_WAT));
  ASSERT_TRUE(bot.healthy());
  game::Snapshot snap;
  snap.my_units.resize(game::cfg::UNIT_HARD_CAP + 50);
  snap.my_structures.resize(game::cfg::STRUCTURE_HARD_CAP + 50);
  snap.visible_enemy_structures.resize(game::cfg::STRUCTURE_HARD_CAP + 50);
  auto cmds = bot.on_tick(snap);
  EXPECT_EQ(cmds.size(), size_t{game::cfg::UNIT_HARD_CAP});
}

// ---- Sandbox ----------------------------------------------------------------
// Bots are other people's binaries. The runner links no host functions at all (no WASI), so a
// bot can reach nothing outside its own memory: no files, network, clock, environment or
// processes. These modules try the ways out and the ways to take more of the host than a match
// allows.

static std::string sandbox_wat(std::string_view imports, std::string_view extra,
                               std::string_view tick = "i32.const 0",
                               std::string_view memory = R"((memory (export "memory") 6))") {
  std::string s = "(module\n";
  s += imports;
  s += "\n";
  s += memory;
  s += R"(
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32 i64))
)";
  s += extra;
  s += "\n(func (export \"on_tick\") (result i32)\n";
  s += tick;
  s += "))";
  return s;
}

TEST(Sandbox, EveryHostImportIsRefused) {
  for (std::string_view imp : {
           R"((import "wasi_snapshot_preview1" "fd_write" (func (param i32 i32 i32 i32) (result i32))))",
           R"((import "wasi_snapshot_preview1" "fd_read" (func (param i32 i32 i32 i32) (result i32))))",
           R"((import "wasi_snapshot_preview1" "path_open" (func)))",
           R"((import "wasi_snapshot_preview1" "sock_accept" (func (param i32 i32 i32) (result i32))))",
           R"((import "wasi_snapshot_preview1" "sock_send" (func)))",
           R"((import "wasi_snapshot_preview1" "sock_recv" (func)))",
           R"((import "wasi_snapshot_preview1" "clock_time_get" (func)))",
           R"((import "wasi_snapshot_preview1" "random_get" (func)))",
           R"((import "wasi_snapshot_preview1" "environ_get" (func)))",
           R"((import "wasi_snapshot_preview1" "proc_exit" (func (param i32))))",
           R"((import "wasi_unstable" "fd_write" (func)))",
           R"((import "wasi:sockets/tcp" "connect" (func)))",
           R"((import "env" "system" (func)))",
           R"((import "env" "table" (table 1 funcref)))",
           R"((import "env" "g" (global i32)))",
       }) {
    runner::WasmBot bot(to_wasm(sandbox_wat(imp, "")));
    EXPECT_FALSE(bot.healthy()) << imp;
    EXPECT_NE(bot.last_error().find("import"), std::string::npos) << imp << ": " << bot.last_error();
  }
  // Memory the host would share with the bot.
  runner::WasmBot bot(to_wasm(sandbox_wat("", "", "i32.const 0",
                                          R"((import "env" "memory" (memory 6)) (export "memory" (memory 0)))")));
  EXPECT_FALSE(bot.healthy());
  EXPECT_NE(bot.last_error().find("import"), std::string::npos) << bot.last_error();
}

// A start function runs during instantiation, before any budget is set: it must get none.
TEST(Sandbox, StartFunctionGetsNoFuel) {
  runner::WasmBot bot(to_wasm(sandbox_wat("", "(func $spin (loop br 0)) (start $spin)")));
  EXPECT_FALSE(bot.healthy());
}

// Each of these could take host memory or CPU outside the metered, capped linear memory.
TEST(Sandbox, FeaturesBeyondTheAbiAreRefused) {
  const std::vector<std::pair<std::string_view, std::string>> cases = {
      {"second memory", sandbox_wat("", "(memory 1000)")},
      {"memory64", sandbox_wat("", "", "i32.const 0", R"((memory (export "memory") i64 6))")},
      {"shared memory", sandbox_wat("", "", "i32.const 0", R"((memory (export "memory") 6 6 shared))")},
      {"huge table", sandbox_wat("", "(table 100000000 funcref)")},
      {"gc arrays", sandbox_wat("", "(type $a (array (mut i8)))",
                                "(drop (array.new_default $a (i32.const 1000000000))) i32.const 0")},
  };
  for (const auto& [name, wat] : cases) {
    runner::WasmBot bot(to_wasm(wat));
    EXPECT_FALSE(bot.healthy()) << name;
  }
}

// table.grow is one instruction, so fuel can't bound it; the store's table limit does.
TEST(Sandbox, TableGrowthIsCapped) {
  runner::WasmBot bot(to_wasm(sandbox_wat(
      "", "(table $t 1 funcref)",
      "(if (i32.ne (table.grow $t (ref.null func) (i32.const 100000000)) (i32.const -1))"
      "  (then unreachable)) i32.const 0")));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_tick(game::Snapshot{});
  EXPECT_TRUE(bot.healthy()) << bot.last_error();
}

TEST(Sandbox, RunawayRecursionIsACrashNotTheHosts) {
  runner::WasmBot bot(to_wasm(sandbox_wat("", "(func $r (call $r))", "call $r i32.const 0")));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_tick(game::Snapshot{});
  EXPECT_FALSE(bot.healthy());
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::Trap);
}

// The buffer addresses are the bot's to give: ones that point outside its memory are refused.
TEST(Sandbox, BufferAddressesOutsideTheBotsMemoryAreRefused) {
  for (const std::string_view global : {"(i32.const 0))", "(i32.const 327680))"}) {
    std::string wat = sandbox_wat("", "");
    wat.replace(wat.find(global), global.size(), "(i32.const -16))");
    runner::WasmBot bot(to_wasm(wat));
    EXPECT_FALSE(bot.healthy()) << global;
  }
}
