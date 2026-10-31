#include <gtest/gtest.h>
#include "config.hpp"
#include "entity.hpp"
#include "../fixtures/idle_bot.hpp"
#include "match.hpp"
#include "snapshot.hpp"
#include "statehash.hpp"
#include "territory.hpp"
#include "tick.hpp"
#include "wincheck.hpp"
#include "world.hpp"
#include <set>

static game::World make_world(int32_t w = 20, int32_t h = 20) {
  game::World world;
  world.map = game::Map::make(w, h);
  world.rng  = game::Rng{0};
  world.tick = 0;
  return world;
}

// ---- Voronoi partition -------------------------------------------------------

TEST(Territory, NoClaimNodesZeroPct) {
  auto w = make_world();
  auto ts = game::run_territory(w, 1000);
  EXPECT_EQ(ts.pct_faction[0], 0u);
  EXPECT_EQ(ts.pct_faction[1], 0u);
}

TEST(Territory, SingleNodeOwnsNearbyTiles) {
  auto w = make_world(20, 20);
  w.spawn_structure({0}, game::StructureType::ClaimNode, {10, 10});
  auto ts = game::run_territory(w, 1000);
  EXPECT_GT(ts.pct_faction[0], 0u);
  EXPECT_EQ(ts.pct_faction[1], 0u);
}

TEST(Territory, TwoOpposingNodesSplitMap) {
  // Symmetric 20×20 map; nodes at opposite corners → roughly equal split.
  auto w = make_world(20, 20);
  w.spawn_structure({0}, game::StructureType::ClaimNode, {0, 10});
  w.spawn_structure({1}, game::StructureType::ClaimNode, {19, 10});
  auto ts = game::run_territory(w, 1000);
  // Each side should hold ~50% (within influence radius).
  EXPECT_GT(ts.pct_faction[0], 0u);
  EXPECT_GT(ts.pct_faction[1], 0u);
  // Neither should be larger than the other by more than a few percent
  // (exact split depends on odd column count and influence radius).
  int32_t diff = static_cast<int32_t>(ts.pct_faction[0]) -
                 static_cast<int32_t>(ts.pct_faction[1]);
  if (diff < 0) diff = -diff;
  EXPECT_LE(diff, 10);
}

TEST(Territory, PctIsShareOfPassableTiles) {
  // Half the map is asteroid; one claim whose radius covers the whole map holds all of the
  // passable half. Counting asteroids would report 50%.
  auto w = make_world(10, 10);
  for (int32_t y = 0; y < 10; ++y)
    for (int32_t x = 5; x < 10; ++x) w.map.set_terrain({x, y}, game::Terrain::Asteroid);
  w.spawn_structure({0}, game::StructureType::ClaimNode, {2, 5});
  auto ts = game::run_territory(w, 1000);
  EXPECT_EQ(ts.pct_faction[0], 100u);
}

// The territory pass only visits tiles within each claim's influence radius. Check it against
// the definition (every passable tile against every claim) on random layouts.
TEST(Territory, MatchesBruteForceVoronoi) {
  game::Rng rng{2024};
  auto below = [&](int32_t n) {
    return static_cast<int32_t>(rng.next() % static_cast<uint64_t>(n));
  };
  for (int trial = 0; trial < 200; ++trial) {
    const int32_t W = 8 + below(50), H = 8 + below(50);
    auto w = make_world(W, H);
    for (int32_t i = 0; i < W * H / 5; ++i)
      w.map.set_terrain({below(W), below(H)}, game::Terrain::Asteroid);
    const int32_t claims = below(12);
    for (int32_t i = 0; i < claims; ++i) {
      const game::Vec2 p{below(W), below(H)};
      if (w.map.passable(p) && w.tile_free(p))
        w.spawn_structure({static_cast<uint32_t>(below(2))}, game::StructureType::ClaimNode, p);
    }

    const int64_t r2 = int64_t{game::cfg::CLAIM_INFLUENCE} * game::cfg::CLAIM_INFLUENCE;
    uint32_t count[2]{0, 0}, total = 0;
    for (int32_t y = 0; y < H; ++y)
      for (int32_t x = 0; x < W; ++x) {
        if (!w.map.passable({x, y})) continue;
        ++total;
        int64_t best = INT64_MAX;
        std::set<uint32_t> nearest;
        for (const auto& [sid, s] : w.structures) {
          const int64_t dx = x - s.pos.x, dy = y - s.pos.y, d = dx * dx + dy * dy;
          if (d < best) nearest.clear();
          if (d <= best) {
            best = d;
            nearest.insert(s.faction.value);
          }
        }
        if (best <= r2 && nearest.size() == 1) ++count[*nearest.begin()];
      }

    auto ts = game::run_territory(w, 1000);
    ASSERT_EQ(ts.pct_faction[0], total ? count[0] * 100 / total : 0) << "trial " << trial;
    ASSERT_EQ(ts.pct_faction[1], total ? count[1] * 100 / total : 0) << "trial " << trial;
  }
}

TEST(Territory, EquidistantTileIsNeutral) {
  // Two nodes equidistant from tile (10,10) on a 21×21 map so the midpoint
  // is exactly a tile centre.
  auto w = make_world(21, 21);
  w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 10});
  w.spawn_structure({1}, game::StructureType::ClaimNode, {15, 10});
  // Tile (10,10) is exactly equidistant → neutral; total pct must be < 100.
  auto ts = game::run_territory(w, 1000);
  EXPECT_LT(ts.pct_faction[0] + ts.pct_faction[1], 100u);
}

TEST(Territory, TilesBeyondInfluenceAreNeutral) {
  // Place a single node and verify total claimed tiles is bounded by the
  // influence circle area (π*r² ≤ tiles ≤ (2r+1)²).
  auto w = make_world(128, 128);
  w.spawn_structure({0}, game::StructureType::ClaimNode, {64, 64});
  auto ts = game::run_territory(w, 1000);
  // At most (2*CLAIM_INFLUENCE+1)² tiles can be claimed on a 128×128 map.
  uint32_t box_tiles = static_cast<uint32_t>(
    (2 * game::cfg::CLAIM_INFLUENCE + 1) * (2 * game::cfg::CLAIM_INFLUENCE + 1));
  uint32_t total_tiles = 128u * 128u;
  uint32_t max_pct = (box_tiles * 100) / total_tiles + 1; // +1 for rounding
  EXPECT_LE(ts.pct_faction[0], max_pct);
}

TEST(Territory, DeadNodeNotCounted) {
  auto w = make_world(20, 20);
  auto& s = w.spawn_structure({0}, game::StructureType::ClaimNode, {10, 10});
  s.hp = 0; // kill it
  auto ts = game::run_territory(w, 1000);
  EXPECT_EQ(ts.pct_faction[0], 0u);
}

// ---- Claim Node fragility ---------------------------------------------------

TEST(Territory, FragilityClampsFutureHp) {
  auto w = make_world();
  auto& s = w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 5});
  s.hp = game::cfg::CLAIM_HP_BASE; // full health

  // Run at half-time → eff_hp ≈ BASE/2 + FLOOR; hp should be clamped.
  uint32_t tick_cap = 1000;
  w.tick = tick_cap / 2;
  game::run_territory(w, tick_cap);

  int32_t expected_eff =
    game::cfg::CLAIM_HP_BASE / 2 + game::cfg::CLAIM_HP_FLOOR;
  EXPECT_LE(s.hp, expected_eff);
  EXPECT_GT(s.hp, 0);
}

TEST(Territory, FragilityFloorAtTickCap) {
  auto w = make_world();
  auto& s = w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 5});
  s.hp = game::cfg::CLAIM_HP_BASE;

  uint32_t tick_cap = 100;
  w.tick = tick_cap; // at or past cap
  game::run_territory(w, tick_cap);

  EXPECT_EQ(s.hp, game::cfg::CLAIM_HP_FLOOR);
}

TEST(Territory, FragilityDoesNotIncreaseHp) {
  auto w = make_world();
  auto& s = w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 5});
  s.hp = 1; // already low
  w.tick = 0;
  game::run_territory(w, 1000);
  EXPECT_EQ(s.hp, 1); // should not be raised
}

// ---- Win threshold ----------------------------------------------------------

TEST(Territory, ThresholdStartsHigh) {
  // At tick 0 threshold = THRESH_A.
  EXPECT_EQ(game::win_threshold(0, 1000), game::cfg::THRESH_A);
}

TEST(Territory, ThresholdDropsOverTime) {
  int32_t early = game::win_threshold(100, 1000);
  int32_t late  = game::win_threshold(900, 1000);
  EXPECT_GT(early, late);
}

TEST(Territory, ThresholdFloorsAtMin) {
  // At tick_cap the threshold must equal THRESH_MIN.
  int32_t t = game::win_threshold(1000, 1000);
  EXPECT_EQ(t, game::cfg::THRESH_MIN);
}

// ---- Integration: snapshot reflects wired territory -------------------------

// Build a world where faction 0 has a ClaimNode covering the whole small map.
// After one tick the snapshot should report non-zero territory_pct and the
// correct win_threshold for that tick.
TEST(Territory, SnapshotCarriesTerritoryAfterOneTick) {
  // 10×10 map; influence radius 12 covers every tile.
  game::World world;
  world.map = game::Map::make(10, 10);
  world.rng  = game::Rng{0};
  world.tick = 0;

  world.spawn_structure({0}, game::StructureType::CommandCore, {0, 0});
  world.spawn_structure({1}, game::StructureType::CommandCore, {9, 9});
  world.spawn_structure({0}, game::StructureType::ClaimNode,   {5, 5});

  constexpr uint32_t tick_cap = 1000;
  game::StateHash hash;
  game::IdleBot a, b;
  game::run_tick(world, a, 0, b, 1, tick_cap, hash);

  game::Snapshot snap0 = game::build_snapshot(world, 0, tick_cap);
  game::Snapshot snap1 = game::build_snapshot(world, 1, tick_cap);

  EXPECT_GT(snap0.header.territory_pct, 0u) << "faction 0 should have territory";
  EXPECT_EQ(snap1.header.territory_pct, 0u) << "faction 1 has no ClaimNode";

  // win_threshold at tick 1 should equal THRESH_A (drop is negligible at g≈0).
  EXPECT_EQ(snap0.header.win_threshold, static_cast<uint32_t>(game::cfg::THRESH_A));
}

// ---- Integration: TerritoryThreshold match win ------------------------------

// Faction 0 has a ClaimNode on a tiny map so it controls >78% from tick 1.
// Both bots idle → faction 0 wins by TerritoryThreshold within a few ticks.
TEST(Territory, TerritoryThresholdWinsMatch) {
  game::World world;
  // 6×6 = 36 tiles. ClaimNode at (3,3) with influence 12 covers all 36 tiles.
  world.map = game::Map::make(6, 6);
  world.rng  = game::Rng{7};
  world.rng_seed = 7;
  world.tick = 0;

  world.spawn_structure({0}, game::StructureType::CommandCore, {0, 0});
  world.spawn_structure({1}, game::StructureType::CommandCore, {5, 5});
  world.spawn_structure({0}, game::StructureType::ClaimNode,   {3, 3});

  constexpr uint32_t tick_cap = 500;
  game::IdleBot a, b;
  auto rec = game::run_match(world, a, 0, b, 1, tick_cap);

  EXPECT_EQ(rec.outcome.winner.value, 0u);
  EXPECT_EQ(rec.outcome.reason, game::WinReason::TerritoryThreshold);
  EXPECT_LT(rec.ticks_played, tick_cap);
}

// ---- Integration: simultaneous base destruction → Draw ----------------------

TEST(Territory, SimultaneousBaseDeathIsDraw) {
  // Two Interceptors (range 1, dmg 12) each adjacent to the opposing core at 1 hp.
  // Both bots issue explicit Attack commands targeting the opposing core.
  // Simultaneous damage kills both cores on tick 1 → Draw.
  game::World world;
  world.map = game::Map::make(20, 20);
  world.rng  = game::Rng{0};
  world.rng_seed = 0;
  world.tick = 0;

  // Cores far apart so Interceptors never reach each other (range 1).
  auto& core0 = world.spawn_structure({0}, game::StructureType::CommandCore, {0,  0});
  auto& core1 = world.spawn_structure({1}, game::StructureType::CommandCore, {19, 0});
  core0.hp = game::cfg::INTERCEPTOR_DMG;
  core1.hp = game::cfg::INTERCEPTOR_DMG;

  // Each Interceptor sits adjacent to the OPPOSING core.
  auto& int_a = world.spawn_unit({0}, game::UnitType::Interceptor, {18, 0}); // next to core1
  auto& int_b = world.spawn_unit({1}, game::UnitType::Interceptor, {1,  0}); // next to core0

  const uint32_t core0_id = core0.id.value;
  const uint32_t core1_id = core1.id.value;
  const uint32_t id_a = int_a.id.value;
  const uint32_t id_b = int_b.id.value;

  struct AttackCoreBot : game::Bot {
    uint32_t my_unit_id;
    uint32_t target_id;
    AttackCoreBot(uint32_t uid, uint32_t tid) : my_unit_id(uid), target_id(tid) {}
    void on_init(const game::Map&, uint32_t) override {}
    std::vector<game::Command> on_tick(const game::Snapshot&) override {
      game::Command cmd{};
      cmd.entity_id   = my_unit_id;
      cmd.kind      = static_cast<uint16_t>(game::CommandKind::Attack);
      cmd.target_id = target_id;
      return {cmd};
    }
    bool healthy() const override { return true; }
  };

  AttackCoreBot a{id_a, core1_id};
  AttackCoreBot b{id_b, core0_id};
  auto rec = game::run_match(world, a, 0, b, 1, 10u);

  EXPECT_EQ(rec.outcome.reason, game::WinReason::Draw);
  EXPECT_EQ(rec.ticks_played, 1u);
}

// ---- Integration: killing a ClaimNode re-partitions territory ---------------

TEST(Territory, DestroyedClaimNodeLosesTerritory) {
  game::World world;
  world.map = game::Map::make(20, 20);
  world.rng  = game::Rng{0};
  world.tick = 0;

  world.spawn_structure({0}, game::StructureType::ClaimNode, {5, 10});
  world.spawn_structure({1}, game::StructureType::ClaimNode, {14, 10});
  constexpr uint32_t tick_cap = 1000;

  auto ts_before = game::run_territory(world, tick_cap);
  EXPECT_GT(ts_before.pct_faction[0], 0u);
  EXPECT_GT(ts_before.pct_faction[1], 0u);

  // Destroy faction 1's ClaimNode.
  for (auto& [sid, s] : world.structures) {
    if (s.faction.value == 1 && s.type == game::StructureType::ClaimNode)
      s.hp = 0;
  }

  auto ts_after = game::run_territory(world, tick_cap);
  EXPECT_GT(ts_after.pct_faction[0], 0u);
  EXPECT_EQ(ts_after.pct_faction[1], 0u);
  EXPECT_GE(ts_after.pct_faction[0], ts_before.pct_faction[0]);
}

// ---- Integration: determinism hash unchanged by ClaimNodes ------------------

TEST(Territory, DeterminismHashStableWithClaimNodes) {
  auto make = []() {
    game::World w;
    w.map = game::Map::make(20, 20);
    w.rng  = game::Rng{42};
    w.rng_seed = 42;
    w.tick = 0;
    w.spawn_structure({0}, game::StructureType::CommandCore, {2,  2});
    w.spawn_structure({1}, game::StructureType::CommandCore, {17, 17});
    w.spawn_structure({0}, game::StructureType::ClaimNode,   {5,  5});
    w.spawn_structure({1}, game::StructureType::ClaimNode,   {14, 14});
    return w;
  };

  constexpr uint32_t tick_cap = 50;
  game::IdleBot a1, b1, a2, b2;
  game::World w1 = make(), w2 = make();
  auto rec1 = game::run_match(w1, a1, 0, b1, 1, tick_cap);
  auto rec2 = game::run_match(w2, a2, 0, b2, 1, tick_cap);

  EXPECT_EQ(rec1.hash.fingerprint(), rec2.hash.fingerprint());
  EXPECT_EQ(rec1.outcome.winner.value, rec2.outcome.winner.value);
  EXPECT_EQ(rec1.outcome.reason,       rec2.outcome.reason);
}

// ---- WinCheck tiebreak paths --------------------------------------------

TEST(WinCheck, BothCoresAbsentIsDraw) {
  // Both cores absent (no structures) → both "dead" simultaneously → Draw.
  auto w = make_world();
  game::TerritoryState ts{};
  ts.pct_faction[0] = 60;
  ts.pct_faction[1] = 40;
  auto result = game::run_wincheck(w, ts, 1000);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->reason, game::WinReason::Draw);
}

TEST(WinCheck, TiebreakByCoreHP) {
  // Both factions simultaneously reach territory threshold → tiebreak.
  // Territory equal → falls through to core-HP comparison.
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::Factory,     {3, 3}); // non-core → skipped
  auto& c0 = w.spawn_structure({0}, game::StructureType::CommandCore, {1,  1});
  auto& c1 = w.spawn_structure({1}, game::StructureType::CommandCore, {18, 18});
  c0.hp = 200;
  c1.hp = 100; // faction 0 has more HP

  // Both above threshold at tick=0 (threshold=78%), equal territory → HP decides.
  game::TerritoryState ts{};
  ts.pct_faction[0] = 80;
  ts.pct_faction[1] = 80;
  auto result = game::run_wincheck(w, ts, 1000);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->reason, game::WinReason::TieBreakLadder);
  EXPECT_EQ(result->winner.value, 0u);
}

TEST(WinCheck, TiebreakCoinFlipFactionZeroWins) {
  // Both above threshold, territory equal, HP equal → deterministic coin-flip → f0.
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::CommandCore, {1,  1});
  w.spawn_structure({1}, game::StructureType::CommandCore, {18, 18});
  game::TerritoryState ts{};
  ts.pct_faction[0] = 80;
  ts.pct_faction[1] = 80;
  auto result = game::run_wincheck(w, ts, 1000);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->reason, game::WinReason::TieBreakLadder);
  EXPECT_EQ(result->winner.value, 0u);
}

TEST(WinCheck, TickCapClearWinner) {
  // Tick cap reached; territory difference > DRAW_TERRITORY_MARGIN → TickCap.
  auto w = make_world();
  w.tick = 1000; // = tick_cap
  w.spawn_structure({0}, game::StructureType::CommandCore, {1,  1});
  w.spawn_structure({1}, game::StructureType::CommandCore, {18, 18});
  // Neither fraction reaches the floor threshold (51% at g=1.0) but diff=20 > 5.
  game::TerritoryState ts{};
  ts.pct_faction[0] = 40;
  ts.pct_faction[1] = 20;
  auto result = game::run_wincheck(w, ts, 1000);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->reason, game::WinReason::TickCap);
  EXPECT_EQ(result->winner.value, 0u);
}
