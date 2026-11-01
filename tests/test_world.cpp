#include <gtest/gtest.h>
#include <optional>
#include "statehash.hpp"
#include "tick.hpp"
#include "combat.hpp"
#include "stats.hpp"
#include "grid.hpp"
#include "command.hpp"
#include "config.hpp"
#include "economy.hpp"
#include "fixed.hpp"
#include "movement.hpp"
#include "snapshot.hpp"
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
  c.entity_id = uid;
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

TEST(Command, GatherIsAlwaysRejected) {
  // Reserved: a Drone beside a node harvests without being told to.
  auto w = make_world();
  w.map.set_terrain({2, 1}, game::Terrain::ResourceNode, 100);
  auto& u = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::Command c = make_cmd(u.id.value, game::CommandKind::Gather);
  c.ax = 2; c.ay = 1;
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

TEST(Economy, DroneHarvestsIntoItsCargoNotTheBank) {
  auto w = make_world();
  w.map.set_terrain({2, 1}, game::Terrain::ResourceNode, 100);
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_EQ(d.cargo_energy, game::cfg::HARVEST_ENERGY);
  EXPECT_EQ(d.cargo_alloy, game::cfg::HARVEST_ALLOY);
  EXPECT_EQ(w.res({0}).energy, 0);
  EXPECT_EQ(w.res({0}).alloy, 0);
}

TEST(Economy, DroneAtEdgeCausesOOBNeighborCheck) {
  // Drone at column 0 — left neighbor (-1,y) is out of bounds.
  auto w = make_world();
  w.map.set_terrain({1, 0}, game::Terrain::ResourceNode, 100);
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {0, 0});
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_GT(d.cargo_energy, 0); // valid right neighbor is still harvested
}

TEST(Economy, OneHarvestPerTickHoweverManyNodesTouched) {
  auto w = make_world();
  w.map.set_terrain({2, 1}, game::Terrain::ResourceNode, 100);
  w.map.set_terrain({0, 1}, game::Terrain::ResourceNode, 100);
  w.map.set_terrain({1, 1}, game::Terrain::ResourceNode, 100);
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_EQ(d.cargo_energy, game::cfg::HARVEST_ENERGY);
}

TEST(Economy, CargoStopsAtCapacity) {
  auto w = make_world();
  w.map.set_terrain({2, 1}, game::Terrain::ResourceNode, 100);
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::ValidatedCommands empty;
  for (int t = 0; t < 50; ++t) game::run_economy(w, empty, empty);
  EXPECT_EQ(d.cargo_energy, game::cfg::DRONE_CARGO_ENERGY);
  EXPECT_EQ(d.cargo_alloy, game::cfg::DRONE_CARGO_ALLOY);
}

TEST(Economy, CargoIsBankedAtOwnCoreOrClaimOnly) {
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::CommandCore, {2, 2});
  w.spawn_structure({0}, game::StructureType::ClaimNode, {8, 2});
  w.spawn_structure({1}, game::StructureType::ClaimNode, {2, 8});
  auto& at_core  = w.spawn_unit({0}, game::UnitType::Drone, {3, 2});
  auto& at_claim = w.spawn_unit({0}, game::UnitType::Drone, {8, 3});
  auto& at_enemy = w.spawn_unit({0}, game::UnitType::Drone, {2, 7});
  auto& too_far  = w.spawn_unit({0}, game::UnitType::Drone, {4, 2}); // distance 2 from core
  for (game::Unit* d : {&at_core, &at_claim, &at_enemy, &too_far}) {
    d->cargo_energy = 6;
    d->cargo_alloy = 3;
  }
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_EQ(w.res({0}).energy, 12);
  EXPECT_EQ(w.res({0}).alloy, 6);
  EXPECT_EQ(at_core.cargo_energy, 0);
  EXPECT_EQ(at_claim.cargo_alloy, 0);
  EXPECT_EQ(at_enemy.cargo_energy, 6);
  EXPECT_EQ(too_far.cargo_energy, 6);
  EXPECT_EQ(w.res({1}).energy, 0);
}

TEST(Economy, DroneBetweenNodeAndDropOffBanksTheSameTick) {
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::ClaimNode, {1, 2});
  w.map.set_terrain({2, 1}, game::Terrain::ResourceNode, 100);
  w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  game::ValidatedCommands empty;
  game::run_economy(w, empty, empty);
  EXPECT_EQ(w.res({0}).energy, game::cfg::HARVEST_ENERGY);
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
  c.entity_id = fac.id.value; // the producer issues Produce
  c.kind      = static_cast<uint16_t>(game::CommandKind::Produce);
  c.aux       = static_cast<uint16_t>(game::UnitType::Interceptor);
  game::ValidatedCommands cmds;
  cmds.emplace(fac.id, c);
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
  c.entity_id = fac.id.value;
  c.kind      = static_cast<uint16_t>(game::CommandKind::Produce);
  c.aux       = static_cast<uint16_t>(game::UnitType::Interceptor);
  game::ValidatedCommands cmds;
  cmds.emplace(fac.id, c);
  game::ValidatedCommands empty;
  game::run_economy(w, cmds, empty);
  EXPECT_FALSE(fac.production_queue.has_value());
}

TEST(Economy, BuildFactorySpawnsStructure) {
  auto w = make_world();
  auto& builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  w.res({0}).energy = game::cfg::FACTORY_COST_E;
  w.res({0}).alloy = game::cfg::FACTORY_COST_A;
  game::Command c = make_cmd(builder.id.value, game::CommandKind::Build);
  c.ax  = 4; c.ay = 3;
  c.aux = static_cast<uint16_t>(game::StructureType::Factory);
  game::ValidatedCommands cmds;
  cmds.emplace(builder.id, c);
  game::ValidatedCommands empty;
  game::run_economy(w, cmds, empty);
  EXPECT_EQ(w.res({0}).energy, 0);
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

// Enough for any placement, so only the rule under test can refuse it.
static constexpr int32_t kFunds = 1000;
static void fund(game::World& w, game::FactionId f) {
  w.res(f).energy = kFunds;
  w.res(f).alloy = kFunds;
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
    fund(w, {0});
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 0);
    EXPECT_EQ(w.res({0}).alloy, kFunds) << "a refused placement costs nothing";
    EXPECT_EQ(w.res({0}).energy, kFunds);
  }
}

TEST(Economy, PlacementOnBuilderOwnTileRejected) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    fund(w, {0});
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {3, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 0);
    EXPECT_EQ(w.res({0}).alloy, kFunds);
  }
}

TEST(Economy, PlacementOnStructureTileRejectedAndLookupIntact) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    game::StructureId core = w.spawn_structure({1}, game::StructureType::CommandCore, {4, 3}).id;
    game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    fund(w, {0});
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(w.structures.size(), 1u);
    EXPECT_EQ(w.res({0}).alloy, kFunds);
    ASSERT_EQ(w.structure_by_pos.count({4, 3}), 1u);
    EXPECT_EQ(w.structure_by_pos.at({4, 3}), core);
  }
}

TEST(Economy, PlacementOnAsteroidRejected) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    w.map.set_terrain({4, 3}, game::Terrain::Asteroid, 0);
    game::UnitId builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    fund(w, {0});
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder, place_cmd(builder, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 0);
    EXPECT_EQ(w.res({0}).alloy, kFunds);
  }
}

TEST(Economy, TwoPlacementsOnOneTileLowerIdWins) {
  for (const auto& pc : kPlaceCases) {
    auto w = make_world();
    game::UnitId first  = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
    game::UnitId second = w.spawn_unit({0}, game::UnitType::Drone, {5, 3}).id;
    fund(w, {0});
    game::ValidatedCommands cmds, empty;
    cmds.emplace(first,  place_cmd(first,  pc.kind, {4, 3}));
    cmds.emplace(second, place_cmd(second, pc.kind, {4, 3}));
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(count_structures(w, pc.made), 1);
    EXPECT_EQ(w.res({0}).alloy, kFunds - pc.alloy_cost) << "only the winner pays";
  }
}

TEST(Economy, BuildAcrossFactionsSameTileOnlyOneStands) {
  auto w = make_world();
  game::UnitId a = w.spawn_unit({0}, game::UnitType::Drone, {3, 3}).id;
  game::UnitId b = w.spawn_unit({1}, game::UnitType::Drone, {5, 3}).id;
  fund(w, {0});
  fund(w, {1});
  game::ValidatedCommands cmds_a, cmds_b;
  cmds_a.emplace(a, place_cmd(a, game::CommandKind::DeployClaim, {4, 3}));
  cmds_b.emplace(b, place_cmd(b, game::CommandKind::DeployClaim, {4, 3}));
  game::run_economy(w, cmds_a, cmds_b);
  ASSERT_EQ(w.structures.size(), 1u);
  EXPECT_EQ(w.structures.begin()->second.faction.value, 0u) << "lower unit id resolves first";
  EXPECT_EQ(w.res({0}).alloy, kFunds - game::cfg::CLAIM_COST_A);
  EXPECT_EQ(w.res({1}).alloy, kFunds);
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
  fund(w, {0});
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
  fund(w, {0});
  game::Command mv = make_cmd(mover.value, game::CommandKind::Move);
  mv.ax = 4; mv.ay = 3;
  tick_raw(w, {mv, place_cmd(builder, game::CommandKind::Build, {4, 3})});
  EXPECT_EQ(w.find_unit(mover)->pos, (game::Vec2{4, 3}));
  EXPECT_EQ(count_structures(w, game::StructureType::Factory), 0);
  EXPECT_EQ(w.res({0}).alloy, kFunds);
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
  c.entity_id = uid.value;
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

// MoveAttack halts at full range whatever the target is. Marches along a row toward
// an enemy and returns the distance at which the unit stopped.
static int32_t halt_distance(game::UnitType shooter, bool target_is_structure,
                             game::StructureType stype = game::StructureType::CommandCore) {
  game::World w;
  w.map = game::Map::make(20, 5);
  w.rng = game::Rng{0};
  game::Vec2 enemy_at{15, 2};
  if (target_is_structure)
    w.spawn_structure({1}, stype, enemy_at);
  else
    w.spawn_unit({1}, game::UnitType::Drone, enemy_at);
  game::UnitId id = w.spawn_unit({0}, shooter, {0, 2}).id;
  game::ValidatedCommands empty;
  for (int t = 0; t < 30; ++t)
    game::run_movement(w, move_cmd(id, enemy_at.x, enemy_at.y, game::CommandKind::MoveAttack),
                       empty);
  return game::manhattan(w.find_unit(id)->pos, enemy_at);
}

TEST(Movement, MoveAttackHaltsAtFullRangeFromUnits) {
  for (auto t : {game::UnitType::Interceptor, game::UnitType::Frigate,
                 game::UnitType::Artillery, game::UnitType::Battleship})
    EXPECT_EQ(halt_distance(t, false), game::stats_for(t).range) << game::unit_type_name(t);
}

TEST(Movement, MoveAttackHaltsAtFullRangeFromStructures) {
  for (auto st : {game::StructureType::CommandCore, game::StructureType::Factory,
                  game::StructureType::ClaimNode})
    for (auto t : {game::UnitType::Interceptor, game::UnitType::Frigate,
                   game::UnitType::Artillery, game::UnitType::Battleship})
      EXPECT_EQ(halt_distance(t, true, st), game::stats_for(t).range)
          << game::unit_type_name(t) << " vs " << game::structure_type_name(st);
}

TEST(Movement, MoveAttackIgnoresFriendlyStructures) {
  auto w = make_world();
  w.spawn_structure({0}, game::StructureType::Factory, {4, 3}); // beside the path
  auto& unit = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  game::ValidatedCommands empty;
  for (int t = 0; t < 10; ++t)
    game::run_movement(w, move_cmd(unit.id, 8, 1, game::CommandKind::MoveAttack), empty);
  EXPECT_EQ(unit.pos, (game::Vec2{8, 1}));
}

TEST(Movement, DroneMoveAttackNeverHalts) {
  auto w = make_world();
  w.spawn_structure({1}, game::StructureType::CommandCore, {5, 2});
  auto& drone = w.spawn_unit({0}, game::UnitType::Drone, {1, 1}); // range 0
  game::ValidatedCommands empty;
  for (int t = 0; t < 10; ++t)
    game::run_movement(w, move_cmd(drone.id, 8, 1, game::CommandKind::MoveAttack), empty);
  EXPECT_EQ(drone.pos, (game::Vec2{8, 1}));
}

TEST(Combat, TargetInRangeSeesUnitsAndStructuresOfTheEnemyOnly) {
  auto w = make_world();
  auto& inter = w.spawn_unit({0}, game::UnitType::Interceptor, {1, 1});
  EXPECT_FALSE(game::target_in_range(w, inter));
  w.spawn_structure({0}, game::StructureType::Factory, {2, 1}); // friendly
  w.spawn_unit({0}, game::UnitType::Frigate, {1, 2});           // friendly
  EXPECT_FALSE(game::target_in_range(w, inter));
  w.spawn_structure({1}, game::StructureType::ClaimNode, {4, 1}); // distance 3 > range 2
  EXPECT_FALSE(game::target_in_range(w, inter));
  w.spawn_structure({1}, game::StructureType::ClaimNode, {2, 2}); // distance 2
  EXPECT_TRUE(game::target_in_range(w, inter));
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

static void add_move(game::ValidatedCommands& vc, game::UnitId uid, int32_t tx, int32_t ty) {
  vc.merge(move_cmd(uid, tx, ty));
}

TEST(Movement, ColumnAdvancesTogether) {
  auto w = make_world();
  auto& rear = w.spawn_unit({0}, game::UnitType::Interceptor, {3, 5});
  auto& mid  = w.spawn_unit({0}, game::UnitType::Interceptor, {4, 5});
  auto& lead = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5});
  game::ValidatedCommands cmds, empty;
  add_move(cmds, rear.id, 10, 5);
  add_move(cmds, mid.id, 10, 5);
  add_move(cmds, lead.id, 10, 5);
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(rear.pos.x, 5); // each follows into the tile vacated ahead of it, every step
  EXPECT_EQ(mid.pos.x, 6);
  EXPECT_EQ(lead.pos.x, 7);
}

TEST(Movement, FollowsIntoTileAnEnemyVacates) {
  auto w = make_world();
  auto& a = w.spawn_unit({0}, game::UnitType::Frigate, {3, 5});
  auto& b = w.spawn_unit({1}, game::UnitType::Frigate, {4, 5});
  game::run_movement(w, move_cmd(a.id, 9, 5), move_cmd(b.id, 9, 5));
  EXPECT_EQ(a.pos.x, 4);
  EXPECT_EQ(b.pos.x, 5);
}

TEST(Movement, FriendlySwapIsBlocked) {
  auto w = make_world();
  auto& a = w.spawn_unit({0}, game::UnitType::Interceptor, {3, 5});
  auto& b = w.spawn_unit({0}, game::UnitType::Interceptor, {4, 5});
  game::ValidatedCommands cmds, empty;
  add_move(cmds, a.id, 4, 5);
  add_move(cmds, b.id, 3, 5);
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(a.pos.x, 3);
  EXPECT_EQ(b.pos.x, 4);
}

TEST(Movement, RingOfMoversIsBlocked) {
  // Four units rotating round a 2x2 block: a closed loop like a swap, so nobody moves.
  auto w = make_world();
  auto& a = w.spawn_unit({0}, game::UnitType::Frigate, {3, 3});
  auto& b = w.spawn_unit({0}, game::UnitType::Frigate, {4, 3});
  auto& c = w.spawn_unit({0}, game::UnitType::Frigate, {4, 4});
  auto& d = w.spawn_unit({0}, game::UnitType::Frigate, {3, 4});
  game::ValidatedCommands cmds, empty;
  add_move(cmds, a.id, 4, 3);
  add_move(cmds, b.id, 4, 4);
  add_move(cmds, c.id, 3, 4);
  add_move(cmds, d.id, 3, 3);
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(a.pos, (game::Vec2{3, 3}));
  EXPECT_EQ(b.pos, (game::Vec2{4, 3}));
  EXPECT_EQ(c.pos, (game::Vec2{4, 4}));
  EXPECT_EQ(d.pos, (game::Vec2{3, 4}));
}

TEST(Movement, UnitQueuedBehindASwapIsBlocked) {
  auto w = make_world();
  auto& back = w.spawn_unit({0}, game::UnitType::Frigate, {2, 5});
  auto& a    = w.spawn_unit({0}, game::UnitType::Frigate, {3, 5});
  auto& b    = w.spawn_unit({0}, game::UnitType::Frigate, {4, 5});
  game::ValidatedCommands cmds, empty;
  add_move(cmds, back.id, 9, 5);
  add_move(cmds, a.id, 4, 5);
  add_move(cmds, b.id, 3, 5);
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(back.pos.x, 2);
  EXPECT_EQ(a.pos.x, 3);
  EXPECT_EQ(b.pos.x, 4);
}

TEST(Movement, ChainStopsAtAParkedUnit) {
  auto w = make_world();
  auto& rear   = w.spawn_unit({0}, game::UnitType::Frigate, {3, 5});
  auto& front  = w.spawn_unit({0}, game::UnitType::Frigate, {4, 5});
  auto& parked = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5});
  game::ValidatedCommands cmds, empty;
  add_move(cmds, rear.id, 9, 5);
  add_move(cmds, front.id, 9, 5);
  game::run_movement(w, cmds, empty);
  EXPECT_EQ(rear.pos.x, 3);
  EXPECT_EQ(front.pos.x, 4);
  EXPECT_EQ(parked.pos.x, 5);
}

TEST(Movement, FollowerStaysWhenTheUnitAheadLosesItsTile) {
  // front (Drone, priority 1) loses (5,5) to the Battleship; its tile never empties.
  auto w = make_world();
  auto& rear  = w.spawn_unit({0}, game::UnitType::Frigate, {3, 5});
  auto& front = w.spawn_unit({0}, game::UnitType::Drone, {4, 5});
  auto& bs    = w.spawn_unit({1}, game::UnitType::Battleship, {5, 6});
  game::ValidatedCommands cmds;
  add_move(cmds, rear.id, 9, 5);
  add_move(cmds, front.id, 9, 5);
  game::run_movement(w, cmds, move_cmd(bs.id, 5, 5));
  EXPECT_EQ(bs.pos, (game::Vec2{5, 5}));
  EXPECT_EQ(front.pos.x, 4);
  EXPECT_EQ(rear.pos.x, 3);
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

// ---- Hard caps (bound the snapshot buffer) ----------------------------------

static game::World make_big_world() {
  game::World w;
  w.map = game::Map::make(40, 40);
  w.rng = game::Rng{0};
  return w;
}

// Faction 0's claims fill columns 0-19 of the bottom half, faction 1's columns 20-39.
static void fill_structures(game::World& w, game::FactionId f, int32_t n) {
  const int32_t x0 = f.value == 0 ? 0 : 20;
  for (int32_t i = 0; i < n; ++i)
    w.spawn_structure(f, game::StructureType::ClaimNode, {x0 + i % 20, 20 + i / 20});
}

static size_t structures_of(const game::World& w, game::FactionId f) {
  size_t n = 0;
  for (const auto& [sid, s] : w.structures)
    if (s.alive() && s.faction == f) ++n;
  return n;
}

TEST(Economy, StructureCapRefusesBuildAndClaim) {
  for (auto kind : {game::CommandKind::Build, game::CommandKind::DeployClaim}) {
    auto w = make_big_world();
    fill_structures(w, {0}, game::cfg::STRUCTURE_HARD_CAP);
    auto& builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
    fund(w, {0}); // enough for either, so only the cap refuses
    game::Command c = make_cmd(builder.id.value, kind);
    c.ax = 4; c.ay = 3;
    c.aux = static_cast<uint16_t>(game::StructureType::Factory);
    game::ValidatedCommands cmds, empty;
    cmds.emplace(builder.id, c);
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(structures_of(w, {0}), size_t{game::cfg::STRUCTURE_HARD_CAP});
    EXPECT_EQ(w.res({0}).alloy, kFunds) << "a refused placement costs nothing";
    EXPECT_EQ(w.res({0}).energy, kFunds);
  }
}

TEST(Economy, StructureCapIsPerFactionAndFreedByDeath) {
  auto w = make_big_world();
  fill_structures(w, {1}, game::cfg::STRUCTURE_HARD_CAP);
  fill_structures(w, {0}, game::cfg::STRUCTURE_HARD_CAP - 1);
  auto& builder = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  w.res({0}).alloy = 1000;
  game::ValidatedCommands cmds, empty;
  game::Command c = make_cmd(builder.id.value, game::CommandKind::DeployClaim);
  c.ax = 4; c.ay = 3;
  cmds.emplace(builder.id, c);
  game::run_economy(w, cmds, empty);
  EXPECT_EQ(structures_of(w, {0}), size_t{game::cfg::STRUCTURE_HARD_CAP})
      << "the enemy's count is theirs";

  w.structures.begin()->second.hp = 0; // faction 1's first claim dies
  w.spawn_unit({1}, game::UnitType::Drone, {10, 3});
  game::ValidatedCommands cmds_b;
  game::Command cb = make_cmd(w.units.rbegin()->first.value, game::CommandKind::DeployClaim);
  cb.ax = 11; cb.ay = 3;
  cmds_b.emplace(w.units.rbegin()->first, cb);
  w.res({1}).alloy = 1000;
  game::run_economy(w, empty, cmds_b);
  EXPECT_EQ(structures_of(w, {1}), size_t{game::cfg::STRUCTURE_HARD_CAP})
      << "a dead structure frees its slot";
}

static game::ValidatedCommands produce(game::StructureId fac) {
  game::Command c{};
  c.entity_id = fac.value;
  c.kind      = static_cast<uint16_t>(game::CommandKind::Produce);
  c.aux       = static_cast<uint16_t>(game::UnitType::Interceptor);
  game::ValidatedCommands cmds;
  cmds.emplace(fac, c);
  return cmds;
}

TEST(Economy, UnitCapRefusesProduce) {
  auto w = make_big_world();
  for (int32_t i = 0; i < game::cfg::UNIT_HARD_CAP; ++i)
    w.spawn_unit({0}, game::UnitType::Drone, {i % 40, i / 40});
  auto& fac = w.spawn_structure({0}, game::StructureType::Factory, {20, 30});
  w.res({0}).energy = 1000;
  w.res({0}).alloy  = 1000;
  game::ValidatedCommands empty;
  game::run_economy(w, produce(fac.id), empty);
  EXPECT_FALSE(fac.production_queue.has_value());
  EXPECT_EQ(w.res({0}).energy, 1000);
}

TEST(Economy, QueuedShipsCountTowardTheUnitCap) {
  auto w = make_big_world();
  for (int32_t i = 0; i < game::cfg::UNIT_HARD_CAP - 1; ++i)
    w.spawn_unit({0}, game::UnitType::Drone, {i % 40, i / 40});
  auto& f1 = w.spawn_structure({0}, game::StructureType::Factory, {20, 30});
  auto& f2 = w.spawn_structure({0}, game::StructureType::Factory, {30, 30});
  w.res({0}).energy = 1000;
  w.res({0}).alloy  = 1000;
  game::ValidatedCommands empty;
  game::run_economy(w, produce(f1.id), empty);
  ASSERT_TRUE(f1.production_queue.has_value());
  game::run_economy(w, produce(f2.id), empty);
  EXPECT_FALSE(f2.production_queue.has_value()) << "the first ship is already paid for";
}

// ---- Snapshot: my structures ------------------------------------------------

TEST(Snapshot, ListsAllOwnLiveStructuresRegardlessOfFog) {
  auto w = make_big_world();
  w.spawn_unit({0}, game::UnitType::Drone, {0, 0});
  auto& core = w.spawn_structure({0}, game::StructureType::CommandCore, {1, 1});
  auto& fac  = w.spawn_structure({0}, game::StructureType::Factory, {39, 39}); // far outside sight
  auto& dead = w.spawn_structure({0}, game::StructureType::ClaimNode, {20, 20});
  dead.hp = 0;
  w.spawn_structure({1}, game::StructureType::CommandCore, {2, 1}); // enemy, in sight
  fac.production_queue = game::UnitType::Frigate;
  fac.production_ticks_left = 2;

  auto snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  ASSERT_EQ(snap.my_structures.size(), 2u) << "own and alive only; enemy structures are not listed";
  EXPECT_EQ(snap.header.my_structure_count, 2u);

  const auto& c = snap.my_structures[0];
  EXPECT_EQ(c.id, core.id.value);
  EXPECT_EQ(c.type, static_cast<uint16_t>(game::StructureType::CommandCore));
  EXPECT_EQ(c.x, 1);
  EXPECT_EQ(c.y, 1);
  EXPECT_EQ(c.hp, core.hp);
  EXPECT_EQ(c.producing, game::PRODUCING_NONE);
  EXPECT_EQ(c.production_ticks_left, 0u);

  const auto& f = snap.my_structures[1];
  EXPECT_EQ(f.id, fac.id.value);
  EXPECT_EQ(f.producing, static_cast<uint16_t>(game::UnitType::Frigate));
  EXPECT_EQ(f.production_ticks_left, 2u);
}

TEST(Snapshot, OffsetsLayOutArraysBackToBack) {
  auto w = make_big_world();
  w.spawn_unit({0}, game::UnitType::Drone, {0, 0});
  w.spawn_unit({0}, game::UnitType::Drone, {1, 0});
  w.spawn_unit({1}, game::UnitType::Drone, {2, 0});
  w.spawn_structure({0}, game::StructureType::CommandCore, {1, 1});
  w.spawn_structure({0}, game::StructureType::Factory, {3, 3});
  w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 5});
  w.spawn_structure({1}, game::StructureType::Factory, {2, 1});
  w.spawn_structure({1}, game::StructureType::ClaimNode, {3, 0});

  const auto h = game::build_snapshot(w, 0, game::cfg::TICK_CAP).header;
  EXPECT_EQ(h.my_units_off, sizeof(game::SnapshotHeader));
  EXPECT_EQ(h.my_structures_off, h.my_units_off + 2 * sizeof(game::UnitView));
  EXPECT_EQ(h.enemies_off, h.my_structures_off + 3 * sizeof(game::StructureView));
  EXPECT_EQ(h.enemy_structures_off, h.enemies_off + 1 * sizeof(game::EnemyView));
  EXPECT_EQ(h.visible_enemy_structure_count, 2u);
  EXPECT_EQ(h.tiles_off, h.enemy_structures_off + 2 * sizeof(game::EnemyStructureView));
}

// Enemy structures follow the enemy-unit rule exactly: listed only while on a tile within sight
// of one of our units. Nothing is remembered once it drops out of view.
TEST(Snapshot, EnemyStructuresOnlyWithinSight) {
  auto w = make_big_world();
  auto& scout = w.spawn_unit({0}, game::UnitType::Drone, {10, 10});
  const int32_t sight = game::stats_for(game::UnitType::Drone).sight;
  auto& near = w.spawn_structure({1}, game::StructureType::Factory, {10 + sight, 10}); // edge
  w.spawn_structure({1}, game::StructureType::CommandCore, {10 + sight + 1, 10});      // just out
  w.spawn_structure({1}, game::StructureType::ClaimNode, {39, 39});                    // far
  auto& dead = w.spawn_structure({1}, game::StructureType::ClaimNode, {10, 11});
  dead.hp = 0;
  w.spawn_structure({0}, game::StructureType::CommandCore, {30, 30}); // ours: not an enemy

  auto snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  ASSERT_EQ(snap.visible_enemy_structures.size(), 1u);
  const auto& es = snap.visible_enemy_structures[0];
  EXPECT_EQ(es.id, near.id.value);
  EXPECT_EQ(es.type, static_cast<uint16_t>(game::StructureType::Factory));
  EXPECT_EQ(es.x, near.pos.x);
  EXPECT_EQ(es.y, near.pos.y);
  EXPECT_EQ(es.hp, near.hp);

  scout.pos = {0, 30};
  EXPECT_TRUE(game::build_snapshot(w, 0, game::cfg::TICK_CAP).visible_enemy_structures.empty())
      << "out of sight, out of the snapshot";
}

// ---- Who can build, and from how far ----------------------------------------

TEST(Command, OnlyDronesBuildOrDeploy) {
  for (auto kind : {game::CommandKind::Build, game::CommandKind::DeployClaim}) {
    auto w = make_world();
    auto& drone = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
    auto& frigate = w.spawn_unit({0}, game::UnitType::Frigate, {6, 6});
    game::Command by_drone = make_cmd(drone.id.value, kind);
    by_drone.ax = 4; by_drone.ay = 3;
    game::Command by_frigate = make_cmd(frigate.id.value, kind);
    by_frigate.ax = 6; by_frigate.ay = 7;
    auto vc = game::validate_commands(w, 0, {by_drone, by_frigate});
    EXPECT_TRUE(vc.contains(drone.id));
    EXPECT_FALSE(vc.contains(frigate.id)) << "kind " << static_cast<int>(kind);
  }
}

static game::ValidatedCommands deploy_at(const game::Unit& d, game::Vec2 at) {
  game::Command c = make_cmd(d.id.value, game::CommandKind::DeployClaim);
  c.ax = at.x;
  c.ay = at.y;
  game::ValidatedCommands cmds;
  cmds.emplace(d.id, c);
  return cmds;
}

static size_t claims(const game::World& w) {
  size_t n = 0;
  for (const auto& [sid, s] : w.structures)
    if (s.type == game::StructureType::ClaimNode) ++n;
  return n;
}

TEST(Economy, ClaimDeployRangeIsManhattan) {
  const int32_t r = game::cfg::CLAIM_DEPLOY_RANGE;
  for (auto [at, ok] : std::vector<std::pair<game::Vec2, bool>>{
           {{3 + r, 3}, true},          // straight out, at range
           {{3 + r - 1, 4}, true},      // diagonal-ish, at range
           {{3 + r + 1, 3}, false},     // one past
           {{3 + r, 4}, false}}) {      // Manhattan, not Chebyshev
    auto w = make_world();
    auto& d = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
    w.res({0}).alloy = game::cfg::CLAIM_COST_A;
    game::ValidatedCommands empty;
    game::run_economy(w, deploy_at(d, at), empty);
    EXPECT_EQ(claims(w), ok ? 1u : 0u) << "(" << at.x << "," << at.y << ")";
    EXPECT_EQ(w.res({0}).alloy, ok ? 0 : game::cfg::CLAIM_COST_A) << "refusal is free";
  }
}

// ---- Production: who makes what, issued by the producer ----------------------

static game::Command produce_cmd(uint32_t issuer, game::UnitType what) {
  game::Command c{};
  c.entity_id = issuer;
  c.kind = static_cast<uint16_t>(game::CommandKind::Produce);
  c.aux = static_cast<uint16_t>(what);
  return c;
}

TEST(Command, CoreProducesDronesFactoryProducesShips) {
  auto w = make_world();
  auto& core = w.spawn_structure({0}, game::StructureType::CommandCore, {1, 1});
  auto& fac  = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  auto& claim = w.spawn_structure({0}, game::StructureType::ClaimNode, {8, 8});
  auto& drone = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  using UT = game::UnitType;
  auto accepted = [&](uint32_t issuer, UT what) {
    return game::validate_commands(w, 0, {produce_cmd(issuer, what)}).size() == 1;
  };
  EXPECT_TRUE(accepted(core.id.value, UT::Drone));
  for (UT ship : {UT::Interceptor, UT::Frigate, UT::Artillery, UT::Battleship}) {
    EXPECT_FALSE(accepted(core.id.value, ship)) << "the Core makes only Drones";
    EXPECT_TRUE(accepted(fac.id.value, ship));
  }
  EXPECT_FALSE(accepted(fac.id.value, UT::Drone)) << "Factories make only ships";
  EXPECT_FALSE(accepted(claim.id.value, UT::Drone)) << "a Claim Node produces nothing";
  EXPECT_FALSE(accepted(drone.id.value, UT::Drone)) << "units don't produce";
}

TEST(Command, ProduceFromAnEnemyOrDeadStructureRejected) {
  auto w = make_world();
  auto& enemy_core = w.spawn_structure({1}, game::StructureType::CommandCore, {1, 1});
  auto& dead_core = w.spawn_structure({0}, game::StructureType::CommandCore, {8, 8});
  dead_core.hp = 0;
  EXPECT_TRUE(game::validate_commands(w, 0, {produce_cmd(enemy_core.id.value,
                                                          game::UnitType::Drone)}).empty());
  EXPECT_TRUE(game::validate_commands(w, 0, {produce_cmd(dead_core.id.value,
                                                          game::UnitType::Drone)}).empty());
}

TEST(World, UnitsAndStructuresShareOneIdSequence) {
  auto w = make_world();
  auto& a = w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  auto& b = w.spawn_structure({0}, game::StructureType::Factory, {2, 2});
  auto& c = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  EXPECT_EQ(a.id.value + 1, b.id.value);
  EXPECT_EQ(b.id.value + 1, c.id.value);
  EXPECT_EQ(w.find_structure(game::StructureId{a.id.value}), nullptr);
}

TEST(Economy, CoreProducesADroneBesideIt) {
  auto w = make_world();
  auto& core = w.spawn_structure({0}, game::StructureType::CommandCore, {5, 5});
  w.res({0}).energy = game::cfg::DRONE_COST_E;
  game::ValidatedCommands cmds, empty;
  cmds.emplace(core.id, produce_cmd(core.id.value, game::UnitType::Drone));
  game::run_economy(w, cmds, empty);
  ASSERT_TRUE(core.production_queue.has_value());
  EXPECT_EQ(w.res({0}).energy, 0);
  for (int32_t t = 1; t < game::cfg::PRODUCTION_TICKS; ++t) game::run_economy(w, empty, empty);
  ASSERT_EQ(w.units.size(), 1u);
  const auto& d = w.units.begin()->second;
  EXPECT_EQ(d.type, game::UnitType::Drone);
  EXPECT_EQ(game::manhattan(d.pos, core.pos), 1);
}

TEST(Economy, DroneCapIsOnePerClaimPlusOneForTheCore) {
  auto w = make_world();
  auto& core = w.spawn_structure({0}, game::StructureType::CommandCore, {5, 5});
  w.res({0}).energy = 1000;
  game::ValidatedCommands empty;
  auto produce = [&] {
    game::ValidatedCommands cmds;
    cmds.emplace(core.id, produce_cmd(core.id.value, game::UnitType::Drone));
    game::run_economy(w, cmds, empty);
    for (int32_t t = 1; t < game::cfg::PRODUCTION_TICKS; ++t) game::run_economy(w, empty, empty);
  };
  produce();
  EXPECT_EQ(w.units.size(), 1u);
  produce(); // cap 1: refused at no cost
  EXPECT_EQ(w.units.size(), 1u);
  EXPECT_EQ(w.res({0}).energy, 1000 - game::cfg::DRONE_COST_E);

  auto& claim = w.spawn_structure({0}, game::StructureType::ClaimNode, {9, 9});
  w.spawn_structure({1}, game::StructureType::ClaimNode, {1, 9}); // the enemy's adds nothing
  produce();
  EXPECT_EQ(w.units.size(), 2u);
  produce();
  EXPECT_EQ(w.units.size(), 2u);

  claim.hp = 0; // losing the claim leaves both Drones alive
  produce();
  EXPECT_EQ(w.units.size(), 2u);
}

TEST(Economy, QueuedDroneCountsTowardTheDroneCap) {
  auto w = make_world();
  auto& core = w.spawn_structure({0}, game::StructureType::CommandCore, {5, 5});
  auto& core2 = w.spawn_structure({0}, game::StructureType::CommandCore, {9, 9});
  w.spawn_unit({0}, game::UnitType::Drone, {1, 1});
  w.res({0}).energy = 1000;
  game::ValidatedCommands cmds, empty;
  cmds.emplace(core.id, produce_cmd(core.id.value, game::UnitType::Drone));
  cmds.emplace(core2.id, produce_cmd(core2.id.value, game::UnitType::Drone));
  game::run_economy(w, cmds, empty); // cap 2, one Drone alive: only the first is bought
  EXPECT_TRUE(core.production_queue.has_value());
  EXPECT_FALSE(core2.production_queue.has_value());
}

// Builds a Factory next to a fresh Drone; true if it went up.
static bool build_factory(game::World& w, game::Vec2 drone_at) {
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, drone_at);
  fund(w, {0});
  const size_t before = w.structures.size();
  game::ValidatedCommands cmds, empty;
  cmds.emplace(d.id, place_cmd(d.id, game::CommandKind::Build, drone_at + game::Vec2{1, 0}));
  game::run_economy(w, cmds, empty);
  return w.structures.size() > before;
}

TEST(Economy, FactoryCapIsHalfTheClaimsButAtLeastOne) {
  auto w = make_big_world();
  EXPECT_TRUE(build_factory(w, {1, 1})); // no claims: still one
  EXPECT_FALSE(build_factory(w, {1, 3}));
  for (int32_t i = 0; i < 3; ++i)
    w.spawn_structure({0}, game::StructureType::ClaimNode, {30 + i, 30});
  EXPECT_FALSE(build_factory(w, {1, 5})); // 3 claims / 2 = 1
  w.spawn_structure({0}, game::StructureType::ClaimNode, {33, 30});
  EXPECT_TRUE(build_factory(w, {1, 7})); // 4 claims: 2
  EXPECT_FALSE(build_factory(w, {1, 9}));
}

TEST(Economy, FactoryCapCountsOnlyOwnFinishedClaims) {
  auto w = make_big_world();
  for (int32_t i = 0; i < 4; ++i) {
    w.spawn_structure({1}, game::StructureType::ClaimNode, {30 + i, 30});
    w.spawn_structure({0}, game::StructureType::ClaimNode, {30 + i, 32}).build_ticks_left = 5;
  }
  EXPECT_TRUE(build_factory(w, {1, 1}));
  EXPECT_FALSE(build_factory(w, {1, 3})) << "enemy and unfinished claims add nothing";
}

TEST(Economy, LosingClaimsKeepsFactoriesButBlocksMore) {
  auto w = make_big_world();
  std::vector<game::StructureId> claims;
  for (int32_t i = 0; i < 4; ++i)
    claims.push_back(w.spawn_structure({0}, game::StructureType::ClaimNode, {30 + i, 30}).id);
  ASSERT_TRUE(build_factory(w, {1, 1}));
  ASSERT_TRUE(build_factory(w, {1, 3}));
  for (auto id : claims) w.find_structure(id)->hp = 0;
  w.purge_dead();
  int32_t factories = 0;
  for (const auto& [sid, s] : w.structures) factories += s.type == game::StructureType::Factory;
  EXPECT_EQ(factories, 2);
  EXPECT_FALSE(build_factory(w, {1, 5}));
}

TEST(Economy, BuildRangeIsOne) {
  for (auto [at, ok] : std::vector<std::pair<game::Vec2, bool>>{
           {{4, 3}, true}, {{3, 4}, true}, {{5, 3}, false}, {{4, 4}, false}}) {
    auto w = make_world();
    auto& d = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
    w.res({0}).energy = game::cfg::FACTORY_COST_E;
    w.res({0}).alloy = game::cfg::FACTORY_COST_A;
    game::Command c = make_cmd(d.id.value, game::CommandKind::Build);
    c.ax = at.x;
    c.ay = at.y;
    c.aux = static_cast<uint16_t>(game::StructureType::Factory);
    game::ValidatedCommands cmds, empty;
    cmds.emplace(d.id, c);
    game::run_economy(w, cmds, empty);
    EXPECT_EQ(w.structures.size(), ok ? 1u : 0u) << "(" << at.x << "," << at.y << ")";
  }
}

// Every structure sees STRUCTURE_SIGHT around itself, so a base with no units nearby
// isn't blind.
TEST(Snapshot, StructuresSeeAroundThemselves) {
  auto w = make_big_world();
  w.spawn_structure({0}, game::StructureType::ClaimNode, {20, 20});
  const int32_t r = game::cfg::STRUCTURE_SIGHT;
  auto& in = w.spawn_unit({1}, game::UnitType::Drone, {20 + r, 20});
  w.spawn_unit({1}, game::UnitType::Drone, {20 + r, 21}); // Manhattan r + 1
  auto& enemy_fac = w.spawn_structure({1}, game::StructureType::Factory, {20, 20 - r});
  auto snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  ASSERT_EQ(snap.visible_enemies.size(), 1u);
  EXPECT_EQ(snap.visible_enemies[0].id, in.id.value);
  ASSERT_EQ(snap.visible_enemy_structures.size(), 1u);
  EXPECT_EQ(snap.visible_enemy_structures[0].id, enemy_fac.id.value);
}

// ---- Claim construction -------------------------------------------------------

static game::Structure& deploy_claim(game::World& w, game::Unit& drone, game::Vec2 at) {
  w.res(drone.faction).alloy += game::cfg::CLAIM_COST_A;
  game::ValidatedCommands cmds, empty;
  cmds.emplace(drone.id, place_cmd(drone.id, game::CommandKind::DeployClaim, at));
  game::run_economy(w, cmds, empty);
  return *w.find_structure(w.structure_by_pos.at(at));
}

static void economy_ticks(game::World& w, int32_t n) {
  game::ValidatedCommands empty;
  for (int32_t i = 0; i < n; ++i) game::run_economy(w, empty, empty);
}

TEST(Claim, DeployStartsConstructionAndTheDeployTickCounts) {
  auto w = make_world();
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  auto& c = deploy_claim(w, d, {4, 3});
  EXPECT_FALSE(c.complete());
  EXPECT_EQ(c.build_ticks_left, game::cfg::BUILD_TICKS - 1);
  EXPECT_GT(c.hp, game::cfg::BUILD_START_HP);
  EXPECT_LT(c.hp, game::cfg::CLAIM_HP_BASE);
}

TEST(Claim, FinishesAtFullHpAfterBuildTicks) {
  auto w = make_world();
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  auto& c = deploy_claim(w, d, {5, 3}); // distance 2: the edge of CLAIM_DEPLOY_RANGE
  economy_ticks(w, game::cfg::BUILD_TICKS - 2);
  EXPECT_EQ(c.build_ticks_left, 1);
  economy_ticks(w, 1);
  EXPECT_TRUE(c.complete());
  EXPECT_EQ(c.hp, game::cfg::CLAIM_HP_BASE);
  economy_ticks(w, 5);
  EXPECT_EQ(c.hp, game::cfg::CLAIM_HP_BASE); // no growth after it is done
}

TEST(Claim, ConstructionWaitsWhileNoDroneIsInRange) {
  auto w = make_world();
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  auto& c = deploy_claim(w, d, {4, 3});
  const int32_t left = c.build_ticks_left;
  const int32_t hp = c.hp;
  d.pos = {7, 3}; // distance 3
  w.spawn_unit({1}, game::UnitType::Drone, {4, 4}); // the enemy's Drone doesn't help
  w.spawn_unit({0}, game::UnitType::Frigate, {4, 2}); // nor does a non-Drone
  economy_ticks(w, 5);
  EXPECT_EQ(c.build_ticks_left, left);
  EXPECT_EQ(c.hp, hp);
  d.pos = {6, 3}; // back within range: resumes
  economy_ticks(w, 1);
  EXPECT_EQ(c.build_ticks_left, left - 1);
}

TEST(Claim, ADroneBuildsOneClaimAtATime) {
  auto w = make_world();
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {4, 4});
  auto& a = w.spawn_structure({0}, game::StructureType::ClaimNode, {4, 5});
  auto& b = w.spawn_structure({0}, game::StructureType::ClaimNode, {4, 3});
  a.build_ticks_left = b.build_ticks_left = 10;
  economy_ticks(w, 1);
  EXPECT_EQ(a.build_ticks_left, 9); // lower id first
  EXPECT_EQ(b.build_ticks_left, 10);
  w.spawn_unit({0}, game::UnitType::Drone, {5, 4});
  economy_ticks(w, 1);
  EXPECT_EQ(a.build_ticks_left, 8);
  EXPECT_EQ(b.build_ticks_left, 9);
  (void)d;
}

TEST(Claim, UnfinishedClaimHoldsNoTerritoryIsNoDropOffAndAddsNoDroneSlot) {
  auto w = make_world();
  auto& core = w.spawn_structure({0}, game::StructureType::CommandCore, {0, 0});
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {5, 4});
  auto& c = deploy_claim(w, d, {5, 5});
  EXPECT_EQ(game::run_territory(w, game::cfg::TICK_CAP).pct_faction[0], 0u);

  d.cargo_energy = 4; // adjacent to the claim, but it's no drop-off yet
  economy_ticks(w, 1);
  EXPECT_EQ(d.cargo_energy, 4);

  w.res({0}).energy = 100; // cap is the Core's slot alone, and the Drone fills it
  game::ValidatedCommands cmds, empty;
  cmds.emplace(core.id, produce_cmd(core.id.value, game::UnitType::Drone));
  game::run_economy(w, cmds, empty);
  EXPECT_FALSE(core.production_queue.has_value());

  economy_ticks(w, c.build_ticks_left);
  ASSERT_TRUE(c.complete());
  EXPECT_GT(game::run_territory(w, game::cfg::TICK_CAP).pct_faction[0], 0u);
  game::run_economy(w, cmds, empty); // deposit runs before construction: banked a tick later
  EXPECT_EQ(d.cargo_energy, 0);
  EXPECT_TRUE(core.production_queue.has_value());
}

TEST(Claim, SnapshotShowsConstructionToBothSides) {
  auto w = make_world();
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  auto& c = deploy_claim(w, d, {4, 3});
  w.spawn_unit({1}, game::UnitType::Drone, {6, 3});
  auto mine = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  ASSERT_EQ(mine.my_structures.size(), 1u);
  EXPECT_EQ(mine.my_structures[0].build_ticks_left, static_cast<uint32_t>(c.build_ticks_left));
  auto theirs = game::build_snapshot(w, 1, game::cfg::TICK_CAP);
  ASSERT_EQ(theirs.visible_enemy_structures.size(), 1u);
  EXPECT_EQ(theirs.visible_enemy_structures[0].building, 1);
  economy_ticks(w, c.build_ticks_left);
  theirs = game::build_snapshot(w, 1, game::cfg::TICK_CAP);
  EXPECT_EQ(theirs.visible_enemy_structures[0].building, 0);
  EXPECT_EQ(game::build_snapshot(w, 0, game::cfg::TICK_CAP).my_structures[0].build_ticks_left, 0u);
}

// ---- Nebulae ------------------------------------------------------------------

TEST(Nebula, HidesWhatIsInsideBeyondOneTile) {
  auto w = make_world();
  for (int32_t x = 4; x <= 6; ++x) w.map.set_terrain({x, 5}, game::Terrain::Nebula);
  w.spawn_unit({0}, game::UnitType::Interceptor, {2, 5}); // sight 3
  auto& hidden = w.spawn_unit({1}, game::UnitType::Drone, {4, 5}); // distance 2, in nebula
  w.spawn_unit({1}, game::UnitType::Drone, {2, 7});                // distance 2, open space
  auto snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  ASSERT_EQ(snap.visible_enemies.size(), 1u);
  EXPECT_NE(snap.visible_enemies[0].id, hidden.id.value);
  for (const auto& t : snap.visible_tiles)
    EXPECT_FALSE(t.x == 4 && t.y == 5) << "nebula tile beyond NEBULA_SIGHT was sent";

  w.map.set_terrain({3, 5}, game::Terrain::Nebula);
  hidden.pos = {3, 5}; // still in a nebula, but within NEBULA_SIGHT
  snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  EXPECT_EQ(snap.visible_enemies.size(), 2u);
}

TEST(Nebula, UnitsInsideSeeOut) {
  auto w = make_world();
  w.map.set_terrain({2, 5}, game::Terrain::Nebula);
  w.spawn_unit({0}, game::UnitType::Interceptor, {2, 5});
  auto& e = w.spawn_unit({1}, game::UnitType::Drone, {5, 5});
  auto snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  ASSERT_EQ(snap.visible_enemies.size(), 1u);
  EXPECT_EQ(snap.visible_enemies[0].id, e.id.value);
}

TEST(Nebula, MoveAttackDoesNotHaltForAHiddenEnemy) {
  auto w = make_world();
  w.map.set_terrain({5, 1}, game::Terrain::Nebula);
  auto& f = w.spawn_unit({0}, game::UnitType::Frigate, {0, 1}); // range 4, speed 1
  w.spawn_unit({1}, game::UnitType::Drone, {5, 1});
  game::ValidatedCommands empty;
  game::run_movement(w, move_cmd(f.id, 3, 1, game::CommandKind::MoveAttack), empty);
  EXPECT_EQ(f.pos.x, 1); // the Drone is at distance 4 from (1,1) but unseen: keeps going
  game::run_movement(w, move_cmd(f.id, 3, 1, game::CommandKind::MoveAttack), empty);
  EXPECT_EQ(f.pos.x, 2);
}

TEST(Construction, FactoriesAreBuiltTheSameWayWithinBuildRange) {
  auto w = make_world();
  auto& d = w.spawn_unit({0}, game::UnitType::Drone, {3, 3});
  fund(w, {0});
  game::ValidatedCommands cmds, empty;
  cmds.emplace(d.id, place_cmd(d.id, game::CommandKind::Build, {4, 3}));
  game::run_economy(w, cmds, empty);
  auto& f = *w.find_structure(w.structure_by_pos.at({4, 3}));
  EXPECT_FALSE(f.complete());
  EXPECT_EQ(f.build_ticks_left, game::cfg::BUILD_TICKS - 1);

  d.pos = {2, 3}; // distance 2: in range for a claim, not for a Factory
  economy_ticks(w, 3);
  EXPECT_EQ(f.build_ticks_left, game::cfg::BUILD_TICKS - 1);

  d.pos = {3, 3};
  economy_ticks(w, game::cfg::BUILD_TICKS - 1);
  EXPECT_TRUE(f.complete());
  EXPECT_EQ(f.hp, game::cfg::FACTORY_HP);
}

TEST(Construction, UnfinishedFactoryCannotProduce) {
  auto w = make_world();
  auto& f = w.spawn_structure({0}, game::StructureType::Factory, {5, 5});
  f.build_ticks_left = 3;
  fund(w, {0});
  game::ValidatedCommands cmds, empty;
  cmds.emplace(f.id, produce_cmd(f.id.value, game::UnitType::Interceptor));
  game::run_economy(w, cmds, empty);
  EXPECT_FALSE(f.production_queue.has_value());
  EXPECT_EQ(w.res({0}).energy, kFunds) << "refused at no cost";
  f.build_ticks_left = 0;
  game::run_economy(w, cmds, empty);
  EXPECT_TRUE(f.production_queue.has_value());
}

TEST(Construction, UnfinishedFactoryCountsTowardTheFactoryCap) {
  auto w = make_big_world();
  ASSERT_TRUE(build_factory(w, {1, 1}));
  EXPECT_FALSE(w.structures.begin()->second.complete());
  EXPECT_FALSE(build_factory(w, {1, 3}));
}
