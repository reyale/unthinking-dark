#include <gtest/gtest.h>
#include <optional>
#include "statehash.hpp"
#include "tick.hpp"
#include "command.hpp"
#include "config.hpp"
#include "economy.hpp"
#include "fixed.hpp"
#include "movement.hpp"
#include "world.hpp"

static game::World make_world() {
  game::World w;
  w.map = game::Map::make(10, 10);
  w.rng = game::Rng{0};
  return w;
}

TEST(World, SpawnUnitAssignsAscendingIds) {
  auto w = make_world();
  auto& a = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  auto& b = w.spawn_unit({0}, game::UnitType::Interceptor, {2, 2});
  EXPECT_EQ(a.id.value, 1u);
  EXPECT_EQ(b.id.value, 2u);
  EXPECT_LT(a.id, b.id);
}

TEST(World, SpawnUnitSetsHpFromStats) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Frigate, {0, 0});
  EXPECT_EQ(u.hp, game::cfg::FRIGATE_HP);
}

TEST(World, SpawnStructureSetsHp) {
  auto w = make_world();
  auto& s = w.spawn_structure({0}, game::StructureType::CommandCore, {5, 5});
  EXPECT_EQ(s.hp, game::cfg::CMD_CORE_HP);
}

TEST(World, FindUnitReturnsNullForMissing) {
  auto w = make_world();
  EXPECT_EQ(w.find_unit(game::UnitId{99}), nullptr);
}

TEST(World, PurgeDeadRemovesKilledUnits) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  u.hp = 0;
  w.purge_dead();
  EXPECT_EQ(w.units.size(), 0u);
}

TEST(World, StructureByPosPopulatedOnSpawn) {
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::CommandCore, {3, 3});
  EXPECT_EQ(w.structure_by_pos.count(game::Vec2{3, 3}), 1u);
}

TEST(World, StructureByPosRemovedOnPurge) {
  auto w = make_world();
  auto& s = w.spawn_structure({0}, game::StructureType::CommandCore, {3, 3});
  s.hp = 0;
  w.purge_dead();
  EXPECT_EQ(w.structure_by_pos.count(game::Vec2{3, 3}), 0u);
}

TEST(World, CommandCoreFindsLivingCore) {
  auto w = make_world();
  game::FactionId f0{0};
  w.spawn_structure(f0, game::StructureType::CommandCore, {5, 5});
  EXPECT_NE(w.command_core(f0), nullptr);
}

TEST(World, CommandCoreNullAfterDestroyed) {
  auto w = make_world();
  game::FactionId f0{0};
  auto& s = w.spawn_structure(f0, game::StructureType::CommandCore, {5, 5});
  s.hp = 0;
  w.purge_dead();
  EXPECT_EQ(w.command_core(f0), nullptr);
}

TEST(Grid, PassabilityByTerrain) {
  auto m = game::Map::make(4, 4);
  m.set_terrain({1, 1}, game::Terrain::Asteroid);
  EXPECT_FALSE(m.passable({1, 1}));
  EXPECT_TRUE(m.passable({0, 0}));
}

TEST(Grid, OutOfBoundsNotPassable) {
  auto m = game::Map::make(4, 4);
  EXPECT_FALSE(m.passable({-1, 0}));
  EXPECT_FALSE(m.passable({4, 0}));
}

TEST(Grid, ManhattanDistance) {
  EXPECT_EQ(game::manhattan({0, 0}, {3, 4}), 7);
  EXPECT_EQ(game::manhattan({2, 2}, {2, 2}), 0);
}

// ---- Command validation -------------------------------------------------

static game::Command make_cmd(uint32_t uid, game::CommandKind kind) {
  game::Command c{};
  c.unit_id = uid;
  c.kind    = static_cast<uint16_t>(kind);
  return c;
}

TEST(Command, DuplicateUnitFirstCommandWins) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  game::Command c1 = make_cmd(u.id.value, game::CommandKind::Move);
  c1.ax = 2; c1.ay = 1;
  game::Command c2 = make_cmd(u.id.value, game::CommandKind::Move);
  c2.ax = 3; c2.ay = 1;
  auto vc = game::validate_commands(w, 0, {c1, c2});
  ASSERT_EQ(vc.size(), 1u);
  EXPECT_EQ(vc.begin()->second.ax, 2);
}

TEST(Command, DeadUnitRejected) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  u.hp = 0;
  auto vc = game::validate_commands(w, 0, {make_cmd(u.id.value, game::CommandKind::Move)});
  EXPECT_TRUE(vc.empty());
}

TEST(Command, EnemyFactionRejected) {
  auto w = make_world();
  auto& u = w.spawn_unit({1}, game::UnitType::Interceptor, {1, 1});
  auto vc = game::validate_commands(w, 0, {make_cmd(u.id.value, game::CommandKind::Move)});
  EXPECT_TRUE(vc.empty());
}

TEST(Command, MoveOutOfBoundsRejected) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Move);
  c.ax = 99; c.ay = 99;
  EXPECT_TRUE(game::validate_commands(w, 0, {c}).empty());
}

TEST(Command, AttackNonexistentTargetRejected) {
  // target_id refers to neither a unit nor a structure — hits the
  // "explicit target provided but invalid" continue branch.
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Attack);
  c.target_id = 99;
  EXPECT_TRUE(game::validate_commands(w, 0, {c}).empty());
}

TEST(Command, AttackValidEnemyStructureAccepted) {
  // Spawn two structures first so enemy core gets struct_id=2;
  // the only unit has unit_id=1, so find_unit(2) returns null
  // and the code falls through to the structure check.
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::CommandCore, {1, 1}); // struct id=1
  auto& ec = w.spawn_structure({1}, game::StructureType::CommandCore, {8, 8}); // struct id=2
  auto& u  = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5}); // unit id=1
  game::Command c = make_cmd(u.id.value, game::CommandKind::Attack);
  c.target_id = ec.id.value; // struct id=2, no unit with id=2
  EXPECT_EQ(game::validate_commands(w, 0, {c}).size(), 1u);
}

TEST(Command, GatherOutOfBoundsRejected) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Gather);
  c.ax = 99; c.ay = 99;
  EXPECT_TRUE(game::validate_commands(w, 0, {c}).empty());
}

TEST(Command, GatherNonResourceTileRejected) {
  auto w = make_world(); // all Open
  auto& u = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Gather);
  c.ax = 2; c.ay = 2;
  EXPECT_TRUE(game::validate_commands(w, 0, {c}).empty());
}

TEST(Command, BuildImpassableTargetRejected) {
  auto w = make_world();
  w.map.set_terrain({3, 3}, game::Terrain::Asteroid);
  auto& u = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Build);
  c.ax = 3; c.ay = 3;
  EXPECT_TRUE(game::validate_commands(w, 0, {c}).empty());
}

TEST(Command, BuildPassableTargetAccepted) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Build);
  c.ax = 2; c.ay = 2;
  c.aux = static_cast<uint16_t>(game::StructureType::Factory);
  EXPECT_EQ(game::validate_commands(w, 0, {c}).size(), 1u);
}

TEST(Command, ProduceInvalidFactoryRejected) {
  auto w = make_world();
  auto& u = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Produce);
  c.target_id = 99;
  EXPECT_TRUE(game::validate_commands(w, 0, {c}).empty());
}

TEST(Command, ProduceOnNonFactoryRejected) {
  auto w = make_world();
  auto& core = w.spawn_structure({0}, game::StructureType::CommandCore, {2, 2});
  auto& u    = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Produce);
  c.target_id = core.id.value;
  EXPECT_TRUE(game::validate_commands(w, 0, {c}).empty());
}

// ---- Fixed-point math ---------------------------------------------------

TEST(Fixed, IsqrtNonPositiveReturnsZero) {
  EXPECT_EQ(game::isqrt(0).raw, 0);
  EXPECT_EQ(game::isqrt(-1).raw, 0);
}

TEST(Fixed, IsqrtPositiveReturnsNonZero) {
  EXPECT_GT(game::isqrt(4).raw, 0);
  EXPECT_GT(game::isqrt(100).raw, 0);
}

TEST(Fixed, IsqrtMonotonic) {
  // Larger inputs produce larger raw values.
  EXPECT_GT(game::isqrt(100).raw, game::isqrt(4).raw);
}

// ---- Economy ------------------------------------------------------------

TEST(Economy, DroneHarvestsAdjacentResource) {
  auto w = make_world();
  w.map.set_terrain({2, 1}, game::Terrain::ResourceNode, 100);
  w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_GT(w.res({0}).energy, 0);
  EXPECT_GT(w.res({0}).alloy, 0);
}

TEST(Economy, DroneAtEdgeCausesOOBNeighborCheck) {
  // Drone at column 0 — left neighbor (-1,y) is out of bounds.
  auto w = make_world();
  w.map.set_terrain({1, 0}, game::Terrain::ResourceNode, 100);
  w.spawn_unit({0}, game::UnitType::Drone, {0, 0});
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_GT(w.res({0}).energy, 0); // valid right neighbor is still harvested
}

TEST(Economy, DepletedResourceNodeYieldsNothing) {
  auto w = make_world();
  w.map.set_terrain({2, 1}, game::Terrain::ResourceNode, 0);
  w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_EQ(w.res({0}).energy, 0);
}

TEST(Economy, ProduceInterceptorDeductsEnergy) {
  auto w = make_world();
  auto& fac = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  w.res({0}).energy = 100;
  w.res({0}).alloy  = 100;
  game::Command c{};
  c.unit_id   = 1; // issuing unit id (not checked for Produce)
  c.kind      = static_cast<uint16_t>(game::CommandKind::Produce);
  c.target_id = fac.id.value;
  c.aux       = static_cast<uint16_t>(game::UnitType::Interceptor);
  game::ValidatedCommands cmds;
  cmds.emplace(game::UnitId{1}, c);
  game::ValidatedCommands empty;
  game::run_economy(w, cmds, empty);
  EXPECT_LT(w.res({0}).energy, 100); // energy was deducted (INTERCEPTOR_COST_E)
  EXPECT_TRUE(fac.production_queue.has_value());
}

TEST(Economy, ProduceInsufficientResourcesIgnored) {
  auto w = make_world();
  auto& fac = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  w.res({0}).energy = 0; // can't afford Interceptor (costs 20)
  game::Command c{};
  c.unit_id   = 1;
  c.kind      = static_cast<uint16_t>(game::CommandKind::Produce);
  c.target_id = fac.id.value;
  c.aux       = static_cast<uint16_t>(game::UnitType::Interceptor);
  game::ValidatedCommands cmds;
  cmds.emplace(game::UnitId{1}, c);
  game::ValidatedCommands empty;
  game::run_economy(w, cmds, empty);
  EXPECT_FALSE(fac.production_queue.has_value());
}

TEST(Economy, BuildFactorySpawnsStructure) {
  auto w = make_world();
  auto& builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  w.res({0}).alloy = 50; // FACTORY_COST_A
  game::Command c = make_cmd(builder.id.value, game::CommandKind::Build);
  c.ax  = 4; c.ay = 3;
  c.aux = static_cast<uint16_t>(game::StructureType::Factory);
  game::ValidatedCommands cmds;
  cmds.emplace(builder.id, c);
  game::ValidatedCommands empty;
  game::run_economy(w, cmds, empty);
  EXPECT_EQ(w.res({0}).alloy, 0);
  bool found = false;
  for (const auto& [sid, s] : w.structures)
    if (s.type == game::StructureType::Factory) found = true;
  EXPECT_TRUE(found);
}

TEST(Economy, DeployClaimNodeSpawnsClaimNode) {
  auto w = make_world();
  auto& builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  w.res({0}).alloy = 20; // CLAIM_COST_A
  game::Command c = make_cmd(builder.id.value, game::CommandKind::DeployClaim);
  c.ax = 4; c.ay = 3;
  game::ValidatedCommands cmds;
  cmds.emplace(builder.id, c);
  game::ValidatedCommands empty;
  game::run_economy(w, cmds, empty);
  EXPECT_EQ(w.res({0}).alloy, 0);
  bool found = false;
  for (const auto& [sid, s] : w.structures)
    if (s.type == game::StructureType::ClaimNode) found = true;
  EXPECT_TRUE(found);
}

// ---- Build / DeployClaim need a free tile ----------------------------------

static game::Command place_cmd(game::UnitId builder, game::CommandKind kind, game::Vec2 at) {
  game::Command c = make_cmd(builder.value, kind);
  c.ax = at.x;
  c.ay = at.y;
  if (kind == game::CommandKind::Build)
    c.aux = static_cast<uint16_t>(game::StructureType::Factory);
  return c;
}

static int count_structures(const game::World& w, game::StructureType t) {
  int n = 0;
  for (const auto& [sid, s] : w.structures)
    n += (s.type == t);
  return n;
}

struct PlaceCase {
  game::CommandKind   kind;
  game::StructureType made;
  int32_t             alloy_cost;
};
static const PlaceCase kPlaceCases[] = {
  {game::CommandKind::Build,       game::StructureType::Factory,   game::cfg::FACTORY_COST_A},
  {game::CommandKind::DeployClaim, game::StructureType::ClaimNode, game::cfg::CLAIM_COST_A},
};

TEST(Economy, PlacementOnUnitTileRejectedAndFree) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    w.spawn_unit({1}, game::UnitType::Interceptor, {4, 3}); // squatter, any faction
    w.res({0}).alloy = 100;
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 0);
    EXPECT_EQ(w.res({0}).alloy, 100) << "a refused placement costs nothing";
  }
}

TEST(Economy, PlacementOnBuilderOwnTileRejected) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    w.res({0}).alloy = 100;
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {3, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 0);
    EXPECT_EQ(w.res({0}).alloy, 100);
  }
}

TEST(Economy, PlacementOnStructureTileRejectedAndLookupIntact) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    game::StructureId core = w.spawn_structure({1}, game::StructureType::CommandCore, {4, 3}).id;
    game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    w.res({0}).alloy = 100;
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(w.structures.size(), 1u);
    EXPECT_EQ(w.res({0}).alloy, 100);
    ASSERT_EQ(w.structure_by_pos.count({4, 3}), 1u);
    EXPECT_EQ(w.structure_by_pos.at({4, 3}), core);
  }
}

TEST(Economy, PlacementOnAsteroidRejected) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    w.map.set_terrain({4, 3}, game::Terrain::Asteroid, 0);
    game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    w.res({0}).alloy = 100;
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 0);
    EXPECT_EQ(w.res({0}).alloy, 100);
  }
}

TEST(Economy, TwoPlacementsOnOneTileLowerIdWins) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    game::UnitId first  = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    game::UnitId second = w.spawn_unit({0}, game::UnitType::Drone, {5, 3}).id;
    w.res({0}).alloy = 100;
    game::ValidatedCommands cmds, empty;
    cmds.emplace(first,  place_cmd(first,  pc.kind, {4, 3}));
    cmds.emplace(second, place_cmd(second, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 1);
    EXPECT_EQ(w.res({0}).alloy, 100 - pc.alloy_cost) << "only the winner pays";
  }
}

TEST(Economy, BuildAcrossFactionsSameTileOnlyOneStands) {
  auto w = make_world();
  game::UnitId a = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
  game::UnitId b = w.spawn_unit({1}, game::UnitType::Drone, {5, 3}).id;
  w.res({0}).alloy = 100;
  w.res({1}).alloy = 100;
  game::ValidatedCommands cmds_a, cmds_b;
  cmds_a.emplace(a, place_cmd(a, game::CommandKind::DeployClaim, {4, 3}));
  cmds_b.emplace(b, place_cmd(b, game::CommandKind::DeployClaim, {4, 3}));
  game::run_economy(w, cmds_a, cmds_b);
  ASSERT_EQ(w.structures.size(), 1u);
  EXPECT_EQ(w.structures.begin()->second.faction.value, 0u) << "lower unit id resolves first";
  EXPECT_EQ(w.res({0}).alloy, 100 - game::cfg::CLAIM_COST_A);
  EXPECT_EQ(w.res({1}).alloy, 100);
}

// Free-tile check happens after movement, inside the same tick.
static std::optional<game::MatchResult> tick_raw(game::World& w, std::vector<game::Command> a) {
  game::StateHash h;
  return game::run_tick_phases(w, a, 0, {}, 1, game::cfg::TICK_CAP, h);
}

TEST(Economy, BuildOnTileVacatedThisTickSucceeds) {
  auto w = make_world();
  game::UnitId mover   = w.spawn_unit({0}, game::UnitType::Frigate, {4, 3}).id;
  game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
  w.res({0}).alloy = 100;
  game::Command mv = make_cmd(mover.value, game::CommandKind::Move);
  mv.ax = 4; mv.ay = 6;
  tick_raw(w, {mv, place_cmd(builder, game::CommandKind::Build, {4, 3})});
  EXPECT_NE(w.find_unit(mover)->pos, (game::Vec2{4, 3}));
  EXPECT_EQ(count_structures(w, game::StructureType::Factory), 1);
}

TEST(Economy, BuildOnTileEnteredThisTickFails) {
  auto w = make_world();
  game::UnitId mover   = w.spawn_unit({0}, game::UnitType::Frigate, {4, 4}).id;
  game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
  w.res({0}).alloy = 100;
  game::Command mv = make_cmd(mover.value, game::CommandKind::Move);
  mv.ax = 4; mv.ay = 3;
  tick_raw(w, {mv, place_cmd(builder, game::CommandKind::Build, {4, 3})});
  EXPECT_EQ(w.find_unit(mover)->pos, (game::Vec2{4, 3}));
  EXPECT_EQ(count_structures(w, game::StructureType::Factory), 0);
  EXPECT_EQ(w.res({0}).alloy, 100);
}

TEST(Economy, FactoryWithoutQueueDoesNothing) {
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::Factory, {5, 5}); // no queue
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_TRUE(w.units.empty());
}

TEST(Economy, ProductionInProgressDecrements) {
  auto w = make_world();
  auto& fac = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  fac.production_queue      = game::UnitType::Interceptor;
  fac.production_ticks_left = 3;
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_EQ(fac.production_ticks_left, 2);
  EXPECT_TRUE(fac.production_queue.has_value());
}

TEST(Economy, ProductionCompletionSpawnsUnit) {
  auto w = make_world();
  auto& fac = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  fac.production_queue      = game::UnitType::Drone;
  fac.production_ticks_left = 1; // completes this tick
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_FALSE(fac.production_queue.has_value());
  bool found = false;
  for (const auto& [uid, u] : w.units)
    if (u.type == game::UnitType::Drone) found = true;
  EXPECT_TRUE(found);
}

TEST(Economy, ProducedUnitSpawnsOnFirstFreeNeighbour) {
  auto w = make_world();
  auto& fac = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  game::UnitId blocker = w.spawn_unit({0}, game::UnitType::Drone, {5, 4}).id; // N taken
  fac.production_queue      = game::UnitType::Frigate;
  fac.production_ticks_left = 1;
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_FALSE(fac.production_queue.has_value());
  int spawned = 0;
  for (const auto& [uid, u] : w.units) {
    if (uid == blocker) continue;
    ++spawned;
    EXPECT_EQ(u.pos, (game::Vec2{6, 5})) << "should take E, the next free tile in DIRS order";
  }
  EXPECT_EQ(spawned, 1);
}

TEST(Economy, ProductionWaitsWhileFactoryIsBoxedIn) {
  auto w = make_world();
  auto& fac = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  std::vector<game::UnitId> ring;
  for (const auto& d : game::DIRS)
    ring.push_back(w.spawn_unit({0}, game::UnitType::Drone, fac.pos + d).id);
  fac.production_queue      = game::UnitType::Interceptor;
  fac.production_ticks_left = 1;
  game::ValidatedCommands empty;
  for (int t = 0; t < 5; ++t)
    game::run_economy(w, empty, empty);
  EXPECT_EQ(w.units.size(), 4u) << "nothing spawns while every neighbour is taken";
  EXPECT_TRUE(fac.production_queue.has_value()) << "the finished unit waits in the factory";

  w.find_unit(ring[2])->pos = {5, 8}; // S moves away
  game::run_economy(w, empty, empty);
  EXPECT_FALSE(fac.production_queue.has_value());
  EXPECT_EQ(w.units.size(), 5u);
  bool on_south = false;
  for (const auto& [uid, u] : w.units)
    on_south |= (u.type == game::UnitType::Interceptor && u.pos == game::Vec2{5, 6});
  EXPECT_TRUE(on_south);
}

TEST(World, TileFreeChecksTerrainStructuresAndUnits) {
  auto w = make_world();
  w.map.set_terrain({1, 1}, game::Terrain::Asteroid, 0);
  w.spawn_structure({0}, game::StructureType::Factory, {2, 2});
  w.spawn_unit({1}, game::UnitType::Drone, {3, 3});
  EXPECT_FALSE(w.tile_free({1, 1}));
  EXPECT_FALSE(w.tile_free({2, 2}));
  EXPECT_FALSE(w.tile_free({3, 3}));
  EXPECT_TRUE(w.tile_free({4, 4}));
}

// ---- Movement -----------------------------------------------------------

static game::ValidatedCommands move_cmd(game::UnitId uid, int32_t tx, int32_t ty,
                                         game::CommandKind kind = game::CommandKind::Move) {
  game::Command c{};
  c.unit_id = uid.value;
  c.kind    = static_cast<uint16_t>(kind);
  c.ax      = static_cast<int16_t>(tx);
  c.ay      = static_cast<int16_t>(ty);
  game::ValidatedCommands vc;
  vc.emplace(uid, c);
  return vc;
}

TEST(Movement, MoveAttackHaltsWhenEnemyInRange) {
  auto w = make_world();
  auto& unit  = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1}); // range=1
  w.spawn_unit({1}, game::UnitType::Drone, {2, 1}); // adjacent enemy
  auto cmds = move_cmd(unit.id, 8, 1, game::CommandKind::MoveAttack);
  game::ValidatedCommands empty;
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(unit.pos.x, 1); // halted — enemy in range
}

TEST(Movement, BlockedByAsteroid) {
  auto w = make_world();
  w.map.set_terrain({2, 1}, game::Terrain::Asteroid);
  auto& unit = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  auto cmds  = move_cmd(unit.id, 5, 1);
  game::ValidatedCommands empty;
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(unit.pos.x, 1); // cannot step through asteroid
}

TEST(Movement, HigherCollisionPriorityWinsContestedTile) {
  // Drone (priority=1, id=1) and Interceptor (priority=3, id=2) both move to (5,5).
  // Drone proposes first (lower id), Interceptor challenges with higher priority → wins.
  auto w     = make_world();
  auto& dr   = w.spawn_unit({0}, game::UnitType::Drone,        {4, 5}); // id=1, speed=1
  auto& inter = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5}); // id=2, speed=2
  auto cmds_a = move_cmd(dr.id,   5, 5);
  auto cmds_b = move_cmd(inter.id, 5, 5);
  game::run_movement(w, cmds_a, cmds_b);
  EXPECT_EQ(inter.pos.x, 5); // interceptor wins the tile
  EXPECT_EQ(dr.pos.x, 4);    // drone blocked
}

TEST(Movement, SwapIsBlocked) {
  auto w = make_world();
  auto& a = w.spawn_unit({0}, game::UnitType::Interceptor, {3, 5}); // id=1
  auto& b = w.spawn_unit({1}, game::UnitType::Interceptor, {4, 5}); // id=2
  auto cmds_a = move_cmd(a.id, 4, 5);
  auto cmds_b = move_cmd(b.id, 3, 5);
  game::run_movement(w, cmds_a, cmds_b);
  EXPECT_EQ(a.pos.x, 3); // both blocked — swap detected
  EXPECT_EQ(b.pos.x, 4);
}

TEST(Movement, BlockedByFriendlyStructure) {
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::CommandCore, {2, 1});
  auto& unit = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  auto cmds = move_cmd(unit.id, 5, 1);
  game::ValidatedCommands empty;
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(unit.pos.x, 1); // cannot enter structure tile
}

TEST(Movement, BlockedByEnemyStructure) {
  auto w = make_world();
  w.spawn_structure({1}, game::StructureType::CommandCore, {2, 1});
  auto& unit = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  auto cmds = move_cmd(unit.id, 5, 1);
  game::ValidatedCommands empty;
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(unit.pos.x, 1); // cannot enter enemy structure tile either
}
