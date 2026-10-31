#include "config.hpp"
#include "match.hpp"
#include "profiler.hpp"
#include "snapshot.hpp"
#include "wasm_bot.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef SDK_EXAMPLES_DIR
#define SDK_EXAMPLES_DIR ""
#endif

static std::vector<uint8_t> example(const char* name) {
  const std::string path = std::string(SDK_EXAMPLES_DIR) + "/" + name;
  if (!std::filesystem::exists(path)) return {};
  return runner::load_wasm_file(path);
}

#define REQUIRE_BOT(var, name)                                                  \
  const auto var = example(name);                                               \
  if (var.empty()) GTEST_SKIP() << name << " not built (clang++-18 required)"

static std::vector<uint8_t> wat_bot(std::string_view body) {
  std::string wat = R"wat(
(module
  (memory (export "memory") 6)
  (global (export "SNAPSHOT_ADDR") i32 (i32.const 0))
  (global (export "COMMAND_ADDR")  i32 (i32.const 327680))
  (func (export "abi_version") (result i32) i32.const @ABI@)
  (func (export "init") (param i32))
  (func (export "on_tick") (result i32) @BODY@)
))wat";
  wat.replace(wat.find("@ABI@"), 5, std::to_string(game::cfg::ABI_VERSION));
  wat.replace(wat.find("@BODY@"), 6, body);
  return runner::wat_to_wasm(wat);
}

static const profiler::Scenario& scenario(std::string_view name) {
  const auto* s = profiler::find_scenario(name);
  EXPECT_NE(s, nullptr) << name;
  return *s;
}

// The trust requirement (DESIGN §10): the profiler's number is the match meter's
// number. Replay each scenario as an ordinary match with a plain WasmBot and
// compare every tick.
TEST(Profiler, FuelAndMemoryMatchARealMatchExactly) {
  REQUIRE_BOT(rush, "rush_bot.wasm");
  for (const char* name : {"early_game", "army_scaling/50", "dense_battle", "max_legal"}) {
    const auto& s = scenario(name);
    const auto report = profiler::run_scenario(rush, s);
    ASSERT_TRUE(report.loaded) << report.error;

    game::World world = s.build();
    runner::WasmBot bot(rush);
    profiler::PassiveBot opponent;
    const auto rec = game::run_match(world, bot, 0, opponent, 1, s.ticks);

    EXPECT_EQ(report.init.fuel_consumed, rec.replay.init_fuel_a) << name;
    ASSERT_EQ(report.ticks.size(), rec.replay.ticks.size()) << name;
    for (size_t t = 0; t < report.ticks.size(); ++t) {
      EXPECT_EQ(report.ticks[t].fuel, rec.replay.ticks[t].fuel_a) << name << " tick " << t;
      EXPECT_EQ(report.ticks[t].memory, rec.replay.ticks[t].mem_bytes_a) << name << " tick " << t;
    }
    EXPECT_GT(report.worst_fuel, 0u) << name;
  }
}

TEST(Profiler, RepeatRunsAreIdentical) {
  REQUIRE_BOT(rush, "rush_bot.wasm");
  const auto& s = scenario("dense_battle");
  const auto a = profiler::run_scenario(rush, s);
  const auto b = profiler::run_scenario(rush, s);
  ASSERT_EQ(a.ticks.size(), b.ticks.size());
  for (size_t t = 0; t < a.ticks.size(); ++t) EXPECT_EQ(a.ticks[t].fuel, b.ticks[t].fuel);
}

// Scenarios must not end early (the enemy Core is out of reach) and must present
// the snapshot sizes they claim.
TEST(Profiler, ScenariosRunFullLengthAndShowWhatTheyClaim) {
  REQUIRE_BOT(idle, "idle_bot.wasm");
  for (const auto& s : profiler::scenarios()) {
    const auto r = profiler::run_scenario(idle, s);
    ASSERT_TRUE(r.loaded) << s.name << ": " << r.error;
    EXPECT_EQ(r.ticks.size(), s.ticks) << s.name << " ended early";
    EXPECT_EQ(r.forfeits(), 0u) << s.name;
  }
  for (uint32_t n : {10u, 50u, 200u, static_cast<uint32_t>(game::cfg::UNIT_HARD_CAP)}) {
    const auto r = profiler::run_scenario(idle, scenario("army_scaling/" + std::to_string(n)));
    EXPECT_EQ(r.my_units, n);
    EXPECT_EQ(r.visible_enemies, 20u) << "army_scaling/" << n;
  }
  const auto wide = profiler::run_scenario(idle, scenario("wide_visibility"));
  const uint32_t map_tiles = game::cfg::MAP_MAX_W * game::cfg::MAP_MAX_H;
  EXPECT_GT(wide.visible_tiles, map_tiles * 3 / 4) << "should see most of the largest map";
  const auto max = profiler::run_scenario(idle, scenario("max_legal"));
  EXPECT_GT(max.my_units, static_cast<uint32_t>(game::cfg::UNIT_HARD_CAP) * 9 / 10);
  EXPECT_EQ(max.visible_enemies, max.my_units) << "one enemy beside every friendly unit";
  const auto world = scenario("max_legal").build();
  const auto snap = game::build_snapshot(world, 0, game::cfg::TICK_CAP);
  EXPECT_EQ(snap.my_structures.size(), size_t{game::cfg::STRUCTURE_HARD_CAP});
  EXPECT_EQ(snap.visible_enemy_structures.size(), size_t{game::cfg::STRUCTURE_HARD_CAP} - 1)
      << "every enemy structure but the walled-off Core";
}

// Acceptance: a deliberately O(n^2) bot shows the army-scaling spike; a bot whose
// cost doesn't depend on army size does not.
TEST(Profiler, QuadraticBotSpikesWithArmySize) {
  REQUIRE_BOT(quad, "quadratic_bot.wasm");
  REQUIRE_BOT(rush, "rush_bot.wasm");
  auto fuel_ratio = [&](const std::vector<uint8_t>& bot) {
    const auto small = profiler::run_scenario(bot, scenario("army_scaling/50"));
    const auto big   = profiler::run_scenario(bot, scenario("army_scaling/256"));
    return static_cast<double>(big.mean_fuel()) / static_cast<double>(small.mean_fuel());
  };
  const double size_ratio = 256.0 / 50.0;
  EXPECT_GT(fuel_ratio(quad), size_ratio * size_ratio * 0.75) << "≈quadratic growth expected";
  EXPECT_LT(fuel_ratio(rush), size_ratio) << "rush bot's cost is map-bound, not army-bound";
}

// Acceptance: a deliberately leaking bot shows rising peak memory in the long run.
TEST(Profiler, LeakyBotMemoryRisesOverTheLongRun) {
  REQUIRE_BOT(leaky, "leaky_bot.wasm");
  REQUIRE_BOT(rush, "rush_bot.wasm");
  const auto& s = scenario("long_running");
  const auto leak = profiler::run_scenario(leaky, s);
  const size_t n = leak.ticks.size();
  ASSERT_EQ(n, s.ticks);
  EXPECT_GT(leak.ticks[n - 1].memory, leak.ticks[n / 10].memory + 1024 * 1024);
  for (size_t t = 1; t < n; ++t)
    ASSERT_GE(leak.ticks[t].memory, leak.ticks[t - 1].memory) << "linear memory never shrinks";

  const auto steady = profiler::run_scenario(rush, s);
  EXPECT_EQ(steady.ticks.back().memory, steady.ticks[s.ticks / 10].memory) << "no leak, flat";
}

TEST(Profiler, ReportsFuelForfeits) {
  const auto hog = wat_bot("(loop $l br $l) i32.const 0");
  const auto r = profiler::run_scenario(hog, scenario("early_game"));
  ASSERT_TRUE(r.loaded) << r.error;
  EXPECT_EQ(r.fuel_forfeits, r.ticks.size());
  EXPECT_EQ(r.trap_forfeits, 0u);
  EXPECT_EQ(r.worst_fuel, game::cfg::FUEL_PER_TICK);
  for (const auto& t : r.ticks) EXPECT_EQ(t.fault, game::TickFault::OutOfFuel);
}

TEST(Profiler, ReportsTrapForfeits) {
  const auto crasher = wat_bot("unreachable");
  const auto r = profiler::run_scenario(crasher, scenario("early_game"));
  ASSERT_TRUE(r.loaded) << r.error;
  EXPECT_EQ(r.trap_forfeits, r.ticks.size());
  EXPECT_EQ(r.fuel_forfeits, 0u);
  for (const auto& t : r.ticks) EXPECT_EQ(t.fault, game::TickFault::Trap);
}

TEST(Profiler, ReportsMemoryForfeits) {
  const auto pages = std::to_string(game::cfg::MEMORY_CAP_MB * 16);
  const auto hog = wat_bot("(drop (memory.grow (i32.const " + pages + "))) i32.const 0");
  const auto r = profiler::run_scenario(hog, scenario("early_game"));
  ASSERT_TRUE(r.loaded) << r.error;
  EXPECT_EQ(r.memory_forfeits, r.ticks.size());
  EXPECT_EQ(r.fuel_forfeits + r.trap_forfeits, 0u);
  EXPECT_LE(r.peak_memory, uint64_t{game::cfg::MEMORY_CAP_MB} << 20);
}

TEST(Profiler, CleanBotHasNoFaults) {
  REQUIRE_BOT(rush, "rush_bot.wasm");
  const auto r = profiler::run_scenario(rush, scenario("early_game"));
  EXPECT_EQ(r.forfeits(), 0u);
  for (const auto& t : r.ticks) EXPECT_EQ(t.fault, game::TickFault::None);
}

TEST(Profiler, UnloadableBotReportsError) {
  const std::vector<uint8_t> junk = {0x00, 0x61, 0x73, 0x6d, 0xde, 0xad};
  const auto r = profiler::run_scenario(junk, scenario("early_game"));
  EXPECT_FALSE(r.loaded);
  EXPECT_FALSE(r.error.empty());
}
