#include "adaptive_bot.hpp"
#include "config.hpp"
#include "econ_bot.hpp"
#include "expander_bot.hpp"
#include "idle_bot.hpp"
#include "map_gen.hpp"
#include "match.hpp"
#include "match_setup.hpp"
#include "rush_bot.hpp"
#include "turtle_bot.hpp"
#include "wasm_bot.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>

// Every fixture is also built as a WASM module from the same source (fixtures/CMakeLists.txt,
// sdk/cpp/include/ud/bot_host.hpp). These matches prove each module plays exactly as the native
// build: same commands every tick, so the same final hash, and never a forfeit, so it fits the
// fuel and memory budgets a real bot gets.

namespace {

const std::string kWasmDir = FIXTURE_WASM_DIR;

struct Fixture {
  std::string name;
  std::function<std::unique_ptr<game::Bot>()> make;
};

void PrintTo(const Fixture& f, std::ostream* os) { *os << f.name; }

template <class T> Fixture fixture(const char* name) {
  return {name, [] { return std::make_unique<T>(); }};
}

const std::vector<Fixture>& fixtures() {
  static const std::vector<Fixture> all = {
      fixture<game::IdleBot>("idle"),         fixture<game::RushBot>("rush"),
      fixture<game::EconBot>("econ"),         fixture<game::TurtleBot>("turtle"),
      fixture<game::ExpanderBot>("expander"), fixture<game::AdaptiveBot>("adaptive"),
  };
  return all;
}

std::unique_ptr<runner::WasmBot> load(const std::string& name) {
  auto bot = std::make_unique<runner::WasmBot>(runner::load_wasm_file(kWasmDir + "/" + name +
                                                                      ".wasm"));
  EXPECT_TRUE(bot->healthy()) << name << ": " << bot->last_error();
  return bot;
}

game::MatchRecord play(game::Bot& a, game::Bot& b) {
  game::Rng rng{9};
  maps::GenParams gp;
  gp.width = game::cfg::MAP_DEFAULT_W;
  gp.height = game::cfg::MAP_DEFAULT_H;
  auto w = maps::world_from_map(maps::generate_map(gp, rng), 7);
  return game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);
}

bool same(const std::vector<game::Command>& x, const std::vector<game::Command>& y) {
  return x.size() == y.size() &&
         (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
}

class FixtureWasm : public testing::TestWithParam<Fixture> {};

TEST_P(FixtureWasm, PlaysExactlyAsItsNativeBuild) {
  const Fixture& f = GetParam();
  auto native_a = f.make();
  game::AdaptiveBot native_b;
  const auto native = play(*native_a, native_b);

  auto wasm_a = load(f.name);
  auto wasm_b = load("adaptive");
  ASSERT_TRUE(wasm_a->healthy() && wasm_b->healthy());
  const auto wasm = play(*wasm_a, *wasm_b);

  uint64_t peak_a = 0, peak_b = 0;
  for (const auto& t : wasm.replay.ticks) {
    ASSERT_EQ(t.fault_a, game::TickFault::None)
        << f.name << " forfeited tick " << t.tick << " (" << game::tick_fault_name(t.fault_a)
        << ", " << t.fuel_a << " fuel)";
    ASSERT_EQ(t.fault_b, game::TickFault::None)
        << "adaptive forfeited tick " << t.tick << " (" << game::tick_fault_name(t.fault_b)
        << ", " << t.fuel_b << " fuel)";
    peak_a = std::max(peak_a, t.fuel_a);
    peak_b = std::max(peak_b, t.fuel_b);
  }
  ASSERT_EQ(native.replay.ticks.size(), wasm.replay.ticks.size());
  for (size_t i = 0; i < native.replay.ticks.size(); ++i) {
    const auto& n = native.replay.ticks[i];
    const auto& w = wasm.replay.ticks[i];
    ASSERT_TRUE(same(n.raw_a, w.raw_a)) << f.name << " diverged at tick " << n.tick;
    ASSERT_TRUE(same(n.raw_b, w.raw_b)) << "adaptive diverged at tick " << n.tick;
  }
  EXPECT_EQ(native.replay.expected_hash, wasm.replay.expected_hash);
  std::printf("  %s vs adaptive: %zu ticks, peak fuel per tick %llu / %llu (budget %llu)\n",
              f.name.c_str(), wasm.replay.ticks.size(), static_cast<unsigned long long>(peak_a),
              static_cast<unsigned long long>(peak_b),
              static_cast<unsigned long long>(game::cfg::FUEL_PER_TICK));
}

INSTANTIATE_TEST_SUITE_P(Fixtures, FixtureWasm, testing::ValuesIn(fixtures()),
                         [](const auto& info) { return info.param.name; });

// A module built without a matching entry above would never be checked.
TEST(FixtureWasmBuild, EveryBuiltModuleIsTested) {
  std::set<std::string> tested;
  for (const auto& f : fixtures()) tested.insert(f.name);
  std::set<std::string> built;
  for (const auto& e : std::filesystem::directory_iterator(kWasmDir))
    if (e.path().extension() == ".wasm") built.insert(e.path().stem().string());
  EXPECT_EQ(built, tested);
}

} // namespace
