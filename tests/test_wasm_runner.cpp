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
  (func   (export "init")    (param i32))
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
  (func   (export "init")    (param i32))
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
  (func (export "init") (param i32)
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
  (func (export "init") (param i32))
  (func (export "on_tick") (result i32)
    (i32.store
      (i32.const 327688)
      (i32.load (i32.const 0)))
    (i32.const 1))
)
)wat";

// Writes a known magic command (unit_id=57005, ax=7, ay=9) to COMMAND_ADDR and
// returns 1.  Proves command bytes written by the bot are delivered to the engine.
static constexpr std::string_view COMMAND_WRITER_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32))
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
  (func (export "init") (param i32))
  (func (export "on_tick") (result i32)
    (i32.load (i32.const 32)))
)
)wat";

// Reads my_units_off from SnapshotHeader (offset 44), loads UnitView[0].id from
// that offset, writes it as Command[0].unit_id, returns 1.
// Proves the full snapshot layout (header offsets → array data → unit id) is correct.
static constexpr std::string_view UNIT_ID_ROUNDTRIP_WAT = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32))
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
  (func (export "init") (param i32))
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
  (func (export "init") (param i32))
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
  (func (export "init") (param i32))
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
  bot.on_init(w.map, 0); // burns all init fuel

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
  EXPECT_EQ(cmds[0].unit_id, 57005u); // 0xDEAD
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
  // from that offset, and writes it as Command[0].unit_id.
  runner::WasmBot bot(to_wasm(UNIT_ID_ROUNDTRIP_WAT));
  ASSERT_TRUE(bot.healthy());

  game::Snapshot snap;
  snap.header.my_unit_count = 1;
  game::UnitView uv{};
  uv.id = 999;
  snap.my_units.push_back(uv);

  auto cmds = bot.on_tick(snap);

  ASSERT_EQ(cmds.size(), 1u);
  EXPECT_EQ(cmds[0].unit_id, 999u);
}

TEST(WasmBot, TickFuelExhaustionReturnsEmptyAndStaysHealthy) {
  // TICK_FUEL_HOG_WAT infinite-loops in on_tick, burning all per-tick fuel.
  // The engine should treat this as a tick forfeit: empty commands, bot stays healthy.
  runner::WasmBot bot(to_wasm(TICK_FUEL_HOG_WAT));
  ASSERT_TRUE(bot.healthy());
  bot.on_init(make_world().map, 0);
  ASSERT_TRUE(bot.healthy());

  auto cmds = bot.on_tick(make_snap());
  EXPECT_TRUE(cmds.empty());
  EXPECT_TRUE(bot.healthy());
  EXPECT_FALSE(bot.last_error().empty());

  // Must be callable again next tick.
  auto cmds2 = bot.on_tick(make_snap(1));
  EXPECT_TRUE(cmds2.empty());
  EXPECT_TRUE(bot.healthy());
}

TEST(WasmBot, TrapInTickReturnsEmptyAndStaysHealthy) {
  // TRAP_WAT hits unreachable in on_tick.
  // Same semantics as fuel exhaustion: empty commands, bot stays healthy.
  runner::WasmBot bot(to_wasm(TRAP_WAT));
  ASSERT_TRUE(bot.healthy());
  bot.on_init(make_world().map, 0);
  ASSERT_TRUE(bot.healthy());

  auto cmds = bot.on_tick(make_snap());
  EXPECT_TRUE(cmds.empty());
  EXPECT_TRUE(bot.healthy());
  EXPECT_FALSE(bot.last_error().empty());
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
// A grow past the cap is refused (memory.grow returns -1) and that tick is forfeited:
// its commands are discarded even if the call completed. Only that tick: the bot's
// memory never actually exceeds the cap, so the next tick plays normally.

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
  (func (export "init") (param i32))
  (func (export "on_tick") (result i32)
    (i32.store (i32.const 327680) (i32.const 57005))   ;; unit_id
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

TEST(WasmBot, GrowPastMemoryCapForfeitsTheTick) {
  runner::WasmBot bot(to_wasm(grow_wat(kCapPages, false)));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0);
  const uint64_t before = bot.last_metrics().memory_bytes;

  auto cmds = bot.on_tick(make_snap());
  EXPECT_TRUE(cmds.empty()) << "commands discarded even though the call completed";
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::OutOfMemory);
  EXPECT_EQ(bot.last_metrics().memory_bytes, before) << "the grow was refused";
  EXPECT_TRUE(bot.healthy());
}

TEST(WasmBot, MemoryForfeitIsOnlyForThatTick) {
  runner::WasmBot bot(to_wasm(grow_wat(kCapPages, true)));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0);

  EXPECT_TRUE(bot.on_tick(make_snap(0)).empty());
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::OutOfMemory);

  auto cmds = bot.on_tick(make_snap(1)); // odd tick: no grow attempt
  ASSERT_EQ(cmds.size(), 1u);
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::None);
  EXPECT_EQ(cmds[0].unit_id, 57005u);
  EXPECT_EQ(cmds[0].ax, -1) << "the bot saw memory.grow fail with -1 and kept running";
}

TEST(WasmBot, GrowingExactlyToTheCapIsAllowed) {
  runner::WasmBot bot(to_wasm(grow_wat(kCapPages - 6, false))); // 6 initial pages
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0);
  EXPECT_EQ(bot.on_tick(make_snap()).size(), 1u);
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::None);
  EXPECT_EQ(bot.last_metrics().memory_bytes, uint64_t{game::cfg::MEMORY_CAP_MB} << 20);

  EXPECT_TRUE(bot.on_tick(make_snap()).empty()) << "now at the cap, any further grow is over";
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::OutOfMemory);
}

TEST(WasmBot, SmallGrowIsNotAFault) {
  runner::WasmBot bot(to_wasm(grow_wat(10, false)));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0);
  EXPECT_EQ(bot.on_tick(make_snap()).size(), 1u);
  EXPECT_EQ(bot.last_metrics().fault, game::TickFault::None);
  EXPECT_EQ(bot.last_metrics().memory_bytes, 16u * 65536);
}

TEST(WasmBot, InitGrowPastMemoryCapFailsLoad) {
  std::string wat = grow_wat(kCapPages, false);
  wat.replace(wat.find("(func (export \"init\") (param i32))"),
              std::string("(func (export \"init\") (param i32))").size(),
              "(func (export \"init\") (param i32) (drop (memory.grow (i32.const " +
                  std::to_string(kCapPages) + "))))");
  runner::WasmBot bot(to_wasm(wat));
  ASSERT_TRUE(bot.healthy()) << bot.last_error();
  bot.on_init(make_world().map, 0);
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
  hog.on_init(make_world().map, 0);
  hog.on_tick(make_snap());
  EXPECT_EQ(hog.last_metrics().fault, game::TickFault::OutOfFuel);

  runner::WasmBot crash(to_wasm(TRAP_WAT));
  crash.on_init(make_world().map, 0);
  crash.on_tick(make_snap());
  EXPECT_EQ(crash.last_metrics().fault, game::TickFault::Trap);

  runner::WasmBot fine(to_wasm(IDLE_WAT));
  fine.on_init(make_world().map, 0);
  fine.on_tick(make_snap());
  EXPECT_EQ(fine.last_metrics().fault, game::TickFault::None);
}

// End to end: the over-cap side stalls every tick, and the forfeit is recorded in the
// replay log, survives the replay file, and reaches the frames the viewer draws.
TEST(WasmBot, MemoryForfeitsAreRecordedThroughReplayAndFrames) {
  auto w = make_world();
  runner::WasmBot hog(to_wasm(grow_wat(kCapPages, false)));
  game::IdleBot idle;
  ASSERT_TRUE(hog.healthy()) << hog.last_error();
  auto rec = game::run_match(w, hog, 0, idle, 1, 20);

  ASSERT_EQ(rec.replay.ticks.size(), 20u);
  for (const auto& t : rec.replay.ticks) {
    EXPECT_EQ(t.fault_a, game::TickFault::OutOfMemory) << "tick " << t.tick;
    EXPECT_TRUE(t.raw_a.empty()) << "stalled: no commands reach the engine";
    EXPECT_EQ(t.fault_b, game::TickFault::None);
  }

  game::MemoryWriter mw;
  game::write_replay(rec.replay, mw);
  game::MemoryReader mr(mw.data);
  const auto back = game::read_replay(mr);
  ASSERT_EQ(back.ticks.size(), rec.replay.ticks.size());
  for (size_t i = 0; i < back.ticks.size(); ++i) {
    EXPECT_EQ(back.ticks[i].fault_a, game::TickFault::OutOfMemory);
    EXPECT_EQ(back.ticks[i].fault_b, game::TickFault::None);
  }
  EXPECT_EQ(game::replay(back), rec.replay.expected_hash);

  const auto frames = game::replay_frames(back);
  EXPECT_EQ(frames[0].forfeits[0], game::TickFault::None) << "initial frame has no tick";
  for (size_t f = 1; f < frames.size(); ++f) {
    EXPECT_EQ(frames[f].forfeits[0], game::TickFault::OutOfMemory) << "frame " << f;
    EXPECT_EQ(frames[f].forfeits[1], game::TickFault::None);
  }
}
