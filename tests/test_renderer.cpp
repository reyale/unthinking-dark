#include <gtest/gtest.h>
#include "render.hpp"
#include "entity.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "world.hpp"

// ---- Helpers ----------------------------------------------------------------

static game::Map make_map(int w, int h) {
  return game::Map::make(w, h);
}

// Build a FrameState containing a single unit.
static game::FrameState one_unit_frame(game::UnitType type, game::FactionId faction,
                                       game::Vec2 pos) {
  game::FrameState f;
  f.tick = 1;
  game::UnitFrame uf;
  uf.id = game::UnitId{1};
  uf.faction = faction;
  uf.type = type;
  uf.pos = pos;
  uf.hp = game::stats_for(type).hp;
  uf.max_hp = uf.hp;
  f.units.push_back(uf);
  return f;
}

// Build a FrameState containing a single structure.
static game::FrameState one_structure_frame(game::StructureType type, game::FactionId faction,
                                            game::Vec2 pos) {
  game::FrameState f;
  f.tick = 1;
  game::StructureFrame sf;
  sf.id = game::StructureId{1};
  sf.faction = faction;
  sf.type = type;
  sf.pos = pos;
  sf.hp = 100;
  sf.max_hp = 100;
  f.structures.push_back(sf);
  return f;
}

// ---- Glyphs -----------------------------------------------------------------

TEST(Renderer, UnitGlyphs) {
  auto map = make_map(5, 5);
  struct Case { game::UnitType type; char glyph; };
  for (auto [type, expected] : std::initializer_list<Case>{
      {game::UnitType::Drone,       'd'},
      {game::UnitType::Interceptor, 'i'},
      {game::UnitType::Frigate,     'f'},
      {game::UnitType::Artillery,   'a'},
  }) {
    auto f = one_unit_frame(type, {0}, {2, 2});
    auto rf = viz::build_render_frame(f, map, -1, 0, 1);
    EXPECT_EQ(rf.at(2, 2).glyph, expected) << "wrong glyph for unit type " << (int)type;
  }
}

TEST(Renderer, CarryingDroneIsUppercase) {
  auto map = make_map(5, 5);
  auto f = one_unit_frame(game::UnitType::Drone, {0}, {2, 2});
  f.units[0].cargo_alloy = 1;
  EXPECT_EQ(viz::build_render_frame(f, map, -1, 0, 1).at(2, 2).glyph, 'D');
}

TEST(Renderer, StructureGlyphs) {
  auto map = make_map(5, 5);
  struct Case { game::StructureType type; char glyph; };
  for (auto [type, expected] : std::initializer_list<Case>{
      {game::StructureType::CommandCore, 'C'},
      {game::StructureType::Factory,     'T'},
      {game::StructureType::ClaimNode,   'N'},
  }) {
    auto f = one_structure_frame(type, {0}, {1, 1});
    auto rf = viz::build_render_frame(f, map, -1, 0, 1);
    EXPECT_EQ(rf.at(1, 1).glyph, expected) << "wrong glyph for structure type " << (int)type;
  }
}

TEST(Renderer, TerrainGlyphs) {
  auto map = make_map(5, 5);
  map.set_terrain({0, 0}, game::Terrain::Asteroid);
  map.set_terrain({1, 0}, game::Terrain::Nebula);
  map.set_terrain({2, 0}, game::Terrain::ResourceNode, 100);
  map.set_terrain({3, 0}, game::Terrain::Open);
  map.recount_passable();

  game::FrameState empty;
  auto rf = viz::build_render_frame(empty, map, -1, 0, 1);
  EXPECT_EQ(rf.at(0, 0).glyph, '#');
  EXPECT_EQ(rf.at(1, 0).glyph, '~');
  EXPECT_EQ(rf.at(2, 0).glyph, '*');
  EXPECT_EQ(rf.at(3, 0).glyph, '.');
}

// ---- Colors -----------------------------------------------------------------

TEST(Renderer, FactionColors) {
  auto map = make_map(5, 5);
  {
    auto f = one_unit_frame(game::UnitType::Drone, {0}, {0, 0});
    auto rf = viz::build_render_frame(f, map, -1, 0, 1);
    EXPECT_EQ(rf.at(0, 0).color, viz::CellColor::FactionA);
  }
  {
    auto f = one_unit_frame(game::UnitType::Drone, {1}, {0, 0});
    auto rf = viz::build_render_frame(f, map, -1, 0, 1);
    EXPECT_EQ(rf.at(0, 0).color, viz::CellColor::FactionB);
  }
}

// ---- Visibility -------------------------------------------------------------

TEST(Renderer, GodViewShowsAll) {
  auto map = make_map(20, 20);
  // Faction 0 unit near top-left, faction 1 unit near bottom-right
  game::FrameState f;
  game::UnitFrame a, b;
  a.id = {1}; a.faction = {0}; a.type = game::UnitType::Drone; a.pos = {1, 1};
  a.hp = a.max_hp = 20;
  b.id = {2}; b.faction = {1}; b.type = game::UnitType::Drone; b.pos = {18, 18};
  b.hp = b.max_hp = 20;
  f.units = {a, b};

  auto rf = viz::build_render_frame(f, map, -1, 0, 1);
  EXPECT_TRUE(rf.at(1,  1).visible);
  EXPECT_TRUE(rf.at(18, 18).visible);
  EXPECT_EQ(rf.at(1,  1).color, viz::CellColor::FactionA);
  EXPECT_EQ(rf.at(18, 18).color, viz::CellColor::FactionB);
}

TEST(Renderer, FogHidesEnemyOutsideRange) {
  auto map = make_map(40, 40);
  game::FrameState f;
  // Faction 0 drone at (0,0), sight=4. Faction 1 drone at (39,39) — far out of range.
  game::UnitFrame own, enemy;
  own.id = {1}; own.faction = {0}; own.type = game::UnitType::Drone; own.pos = {0, 0};
  own.hp = own.max_hp = 20;
  enemy.id = {2}; enemy.faction = {1}; enemy.type = game::UnitType::Drone; enemy.pos = {39, 39};
  enemy.hp = enemy.max_hp = 20;
  f.units = {own, enemy};

  auto rf = viz::build_render_frame(f, map, 0, 0, 1); // faction 0 view
  EXPECT_TRUE(rf.at(0,  0).visible);  // own unit always visible
  EXPECT_FALSE(rf.at(39, 39).visible); // enemy outside sight radius
}

TEST(Renderer, FogShowsEnemyInsideRange) {
  auto map = make_map(20, 20);
  game::FrameState f;
  // Drone sight = 4. Place enemy 3 tiles away.
  game::UnitFrame own, enemy;
  own.id   = {1}; own.faction   = {0}; own.type   = game::UnitType::Drone; own.pos   = {5, 5};
  own.hp = own.max_hp = 20;
  enemy.id = {2}; enemy.faction = {1}; enemy.type = game::UnitType::Drone; enemy.pos = {8, 5};
  enemy.hp = enemy.max_hp = 20;
  f.units = {own, enemy};

  auto rf = viz::build_render_frame(f, map, 0, 0, 1);
  EXPECT_TRUE(rf.at(8, 5).visible); // manhattan(5,5 → 8,5) = 3 ≤ sight(4)
  EXPECT_EQ(rf.at(8, 5).color, viz::CellColor::FactionB);
}

TEST(Renderer, OwnStructureAlwaysVisible) {
  auto map = make_map(20, 20);
  auto f = one_structure_frame(game::StructureType::CommandCore, {0}, {10, 10});
  // No friendly units — structure should still be visible in faction 0's view.
  auto rf = viz::build_render_frame(f, map, 0, 0, 1);
  EXPECT_TRUE(rf.at(10, 10).visible);
}

// ---- Game over --------------------------------------------------------------

TEST(Renderer, ResultPropagated) {
  auto map = make_map(5, 5);
  game::FrameState f;
  f.result = game::MatchResult{game::FactionId{1}, game::WinReason::BaseDestroyed};

  auto rf = viz::build_render_frame(f, map, -1, 0, 1);
  ASSERT_TRUE(rf.result.has_value());
  EXPECT_EQ(rf.result->winner.value, 1u);
  EXPECT_EQ(rf.result->reason, game::WinReason::BaseDestroyed);
}
