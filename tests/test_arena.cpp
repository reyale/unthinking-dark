#include "arena.hpp"
#include "frame.hpp"
#include "replay_io.hpp"
#include <gtest/gtest.h>
#include <cstdio>
#include <string>

using game::UnitType;

TEST(Arena, MixesSplitTheBudgetEvenly) {
  constexpr int32_t kBudget = 1200, kParts = 4;
  const auto armies = arena::compositions(kBudget, kParts);
  EXPECT_EQ(armies.size(), 35u) << "C(parts + 3, 3) ways to share 4 parts over 4 ships";
  for (const auto& a : armies) {
    EXPECT_LE(a.value(), kBudget) << a.name();
    EXPECT_GE(a.value(), kBudget * 9 / 10) << a.name() << ": a share left mostly unspent";
  }
}

// The field, the commander and the scoring favour neither side: identical armies come out
// exactly even.
TEST(Arena, MirrorFightsAreEven) {
  for (int32_t spacing : {1, 2}) {
    arena::Setup s;
    s.spacing = spacing;
    for (auto t : arena::kShips) {
      const auto a = arena::pure(t, 1200);
      const auto o = arena::fight(a, a, s);
      EXPECT_EQ(o.left_a, o.left_b) << a.name() << ", spacing " << spacing;
    }
    arena::Army mix;
    mix.count = {15, 6, 10, 2};
    const auto o = arena::fight(mix, mix, s);
    EXPECT_EQ(o.left_a, o.left_b) << mix.name() << ", spacing " << spacing;
  }
}

TEST(Arena, ScoreIsAntisymmetric) {
  const arena::Setup s;
  const auto x = arena::pure(UnitType::Frigate, 600), y = arena::pure(UnitType::Battleship, 600);
  EXPECT_DOUBLE_EQ(arena::score(x, y, s), -arena::score(y, x, s));
}

TEST(Arena, FightEndsWhenOneSideIsGone) {
  const arena::Setup s;
  const auto o = arena::fight(arena::pure(UnitType::Battleship, 1200),
                              arena::pure(UnitType::Interceptor, 1200), s);
  EXPECT_LT(o.ticks, s.tick_limit);
  EXPECT_TRUE(o.left_a == 0 || o.left_b == 0);
  EXPECT_EQ(o.start_a, arena::pure(UnitType::Battleship, 1200).value());
}

// The replay a viewer plays is the fight the arena scored.
TEST(Arena, ReplayIsTheScoredFight) {
  const arena::Setup s;
  arena::Army a, b;
  a.count = {15, 6, 0, 2};
  b.count = {0, 6, 10, 0};
  const auto o = arena::fight(a, b, s);
  const std::string path = ::testing::TempDir() + "arena_replay.ud";
  arena::write_replay(a, b, s, path);
  const auto frames = game::replay_frames(game::read_replay_file(path));
  std::remove(path.c_str());
  ASSERT_EQ(frames.size(), static_cast<size_t>(o.ticks) + 1) << "initial frame plus one per tick";
  int32_t left[2] = {0, 0};
  for (const auto& u : frames.back().units)
    if (u.hp > 0) left[u.faction.value] += arena::value(u.type) * u.hp / u.max_hp;
  EXPECT_EQ(left[0], o.left_a);
  EXPECT_EQ(left[1], o.left_b);
}
