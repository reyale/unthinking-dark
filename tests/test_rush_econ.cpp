#include "config.hpp"
#include "econ_bot.hpp"
#include "expander_bot.hpp"
#include "map_gen.hpp"
#include "match.hpp"
#include "match_setup.hpp"
#include "rush_bot.hpp"
#include "turtle_bot.hpp"
#include <gtest/gtest.h>
#include <string>

// RushBot and EconBot are the two one-plan fixtures on either side of the turtle and the
// expander: the rush breaks bases that don't defend, and the boom out-expands a side that
// doesn't fight and holds off one that only attacks. These tests pin those roles on a full-size
// map, from both sides.

namespace {

maps::MapData full_size(uint64_t seed) {
  game::Rng rng{seed};
  maps::GenParams gp;
  gp.width = game::cfg::MAP_DEFAULT_W;
  gp.height = game::cfg::MAP_DEFAULT_H;
  return maps::generate_map(gp, rng);
}

template <class A, class B> game::MatchResult play(const maps::MapData& md) {
  auto w = maps::world_from_map(md, 7);
  A a;
  B b;
  return game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP).outcome;
}

void expect_win(const game::MatchResult& r, uint32_t winner, game::WinReason reason,
                const std::string& where) {
  EXPECT_EQ(r.winner, game::FactionId{winner}) << where;
  EXPECT_EQ(r.reason, reason) << where;
}

} // namespace

TEST(Rush, DestroysTheTurtleAndTheExpander) {
  const auto md = full_size(9);
  using enum game::WinReason;
  expect_win(play<game::RushBot, game::TurtleBot>(md), 0, BaseDestroyed, "vs turtle");
  expect_win(play<game::TurtleBot, game::RushBot>(md), 1, BaseDestroyed, "vs turtle, as B");
  expect_win(play<game::RushBot, game::ExpanderBot>(md), 0, BaseDestroyed, "vs expander");
  expect_win(play<game::ExpanderBot, game::RushBot>(md), 1, BaseDestroyed,
             "vs expander, as B");
}

// Each from one side only: these play long matches.
TEST(Econ, OutExpandsTheTurtleAndTheExpander) {
  const auto md = full_size(9);
  using enum game::WinReason;
  expect_win(play<game::EconBot, game::TurtleBot>(md), 0, TerritoryThreshold, "vs turtle");
  expect_win(play<game::ExpanderBot, game::EconBot>(md), 1, TerritoryThreshold,
             "vs expander, as B");
}

// The rush never gets through: the boom keeps its base and ends the match with more territory.
TEST(Econ, HoldsOffTheRush) {
  const auto r = play<game::EconBot, game::RushBot>(full_size(9));
  EXPECT_EQ(r.winner, game::FactionId{0});
  EXPECT_NE(r.reason, game::WinReason::BaseDestroyed);
}
