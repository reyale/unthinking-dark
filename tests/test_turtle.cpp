#include "config.hpp"
#include "expander_bot.hpp"
#include "map_gen.hpp"
#include "map_io.hpp"
#include "match.hpp"
#include "match_setup.hpp"
#include "turtle_bot.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// Plan v1 (docs/archive), Phase 3: a turtle reliably loses to the descending territory threshold.
// TurtleBot holds one claim at home and can't be ground down; ExpanderBot never attacks and
// can only win on territory. If the threshold works, the expander wins on it, never by
// reaching the tick cap, from either side of every map.

static const std::string kDataDir = MAPS_DATA_DIR;

struct Played {
  game::MatchRecord rec;
  game::TerritoryState territory;
  bool core_alive[2];
};

template <class A, class B>
static Played play(const maps::MapData& md, uint64_t seed = 7) {
  auto w = maps::world_from_map(md, seed);
  A a;
  B b;
  Played p{game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP), w.last_territory, {}};
  for (uint32_t f = 0; f < 2; ++f) p.core_alive[f] = w.command_core({f}) != nullptr;
  return p;
}

static std::vector<maps::MapData> small_maps() {
  std::vector<std::string> paths;
  for (const auto& e : std::filesystem::directory_iterator(kDataDir))
    if (e.path().extension() == ".json") paths.push_back(e.path().string());
  std::sort(paths.begin(), paths.end());
  std::vector<maps::MapData> out;
  for (const auto& p : paths) out.push_back(maps::load_map(p));
  for (uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    game::Rng rng{seed};
    maps::GenParams gp;
    gp.name = "generated_" + std::to_string(seed);
    out.push_back(maps::generate_map(gp, rng));
  }
  return out;
}

static void expect_expander_wins(const Played& p, uint32_t expander, const std::string& where) {
  EXPECT_EQ(p.rec.outcome.reason, game::WinReason::TerritoryThreshold) << where;
  EXPECT_EQ(p.rec.outcome.winner.value, expander) << where;
  EXPECT_LT(p.rec.ticks_played, game::cfg::TICK_CAP) << where;
  EXPECT_TRUE(p.core_alive[1 - expander]) << where << ": the turtle was out-claimed, not destroyed";
}

TEST(Turtle, LosesToTheThresholdOnEveryMap) {
  for (const auto& md : small_maps()) {
    expect_expander_wins(play<game::TurtleBot, game::ExpanderBot>(md), 1, md.name + " (turtle A)");
    expect_expander_wins(play<game::ExpanderBot, game::TurtleBot>(md), 0, md.name + " (turtle B)");
  }
}

// On a full-size map one side can't cover 78% of it, so the expander can only win once the
// threshold has come down to the share it holds. Without the descent this would go to the cap.
TEST(Turtle, ThresholdHasToDescendToBeatItOnALargeMap) {
  game::Rng rng{9};
  maps::GenParams gp;
  gp.width = game::cfg::MAP_DEFAULT_W;
  gp.height = game::cfg::MAP_DEFAULT_H;
  const auto md = maps::generate_map(gp, rng);

  const auto p = play<game::TurtleBot, game::ExpanderBot>(md);
  expect_expander_wins(p, 1, "80x80");
  const auto held = static_cast<int32_t>(p.territory.pct_faction[1]);
  EXPECT_LT(held, game::cfg::THRESH_A) << "would have won at the opening threshold";
  EXPECT_GE(held, game::win_threshold(p.rec.ticks_played, game::cfg::TICK_CAP));
}

// Draws are allowed: two sides that never contest each other split the map, and at the tick
// cap a territory difference within DRAW_TERRITORY_MARGIN is a draw.
TEST(Turtle, TwoTurtlesDraw) {
  const auto md = maps::load_map(kDataDir + "/twin_peaks.json");
  const auto p = play<game::TurtleBot, game::TurtleBot>(md);
  EXPECT_EQ(p.rec.outcome.reason, game::WinReason::Draw);
  EXPECT_EQ(p.rec.ticks_played, game::cfg::TICK_CAP);
  EXPECT_TRUE(p.rec.outcome.winner.is_null());
}

TEST(Turtle, ReplayReproducesHash) {
  const auto md = maps::load_map(kDataDir + "/twin_peaks.json");
  const auto p = play<game::ExpanderBot, game::TurtleBot>(md);
  EXPECT_EQ(game::replay(p.rec.replay), p.rec.replay.expected_hash);
}
