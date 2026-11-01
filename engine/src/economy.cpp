#include "economy.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "config.hpp"
#include <algorithm>
#include <set>

namespace game {

static bool touches_resource(const World& world, Vec2 pos) {
  for (const Vec2& p : {pos, pos + DIRS[0], pos + DIRS[1], pos + DIRS[2], pos + DIRS[3]}) {
    if (!world.map.in_bounds(p))
      continue;
    const Tile& tile = world.map.tile_at(p);
    if (tile.terrain == Terrain::ResourceNode && tile.resource_amount > 0)
      return true;
  }
  return false;
}

static bool at_drop_off(const World& world, const Unit& drone) {
  for (const auto& [sid, s] : world.structures)
    if (s.alive() && s.faction == drone.faction &&
        (s.type == StructureType::CommandCore || s.type == StructureType::ClaimNode) &&
        s.complete() &&
        manhattan(s.pos, drone.pos) <= cfg::DEPOSIT_RANGE)
      return true;
  return false;
}

// Harvest before depositing, so a Drone between a node and a drop-off banks every tick.
static void harvest(World& world) {
  for (auto& [uid, unit] : world.units) {
    if (!unit.alive() || unit.type != UnitType::Drone)
      continue;
    if (touches_resource(world, unit.pos)) {
      unit.cargo_energy =
          std::min(cfg::DRONE_CARGO_ENERGY, unit.cargo_energy + cfg::HARVEST_ENERGY);
      unit.cargo_alloy = std::min(cfg::DRONE_CARGO_ALLOY, unit.cargo_alloy + cfg::HARVEST_ALLOY);
    }
    if ((unit.cargo_energy > 0 || unit.cargo_alloy > 0) && at_drop_off(world, unit)) {
      world.res(unit.faction).energy += unit.cargo_energy;
      world.res(unit.faction).alloy += unit.cargo_alloy;
      unit.cargo_energy = 0;
      unit.cargo_alloy = 0;
    }
  }
}

static int32_t drone_cap(const World& world, FactionId f) {
  int32_t cap = 0;
  for (const auto& [sid, s] : world.structures) {
    if (!s.alive() || s.faction != f || !s.complete())
      continue;
    if (s.type == StructureType::ClaimNode) cap += cfg::DRONES_PER_CLAIM;
    if (s.type == StructureType::CommandCore) cap += cfg::DRONES_PER_CORE;
  }
  return cap;
}

static int32_t factory_cap(const World& world, FactionId f) {
  int32_t claims = 0;
  for (const auto& [sid, s] : world.structures)
    if (s.alive() && s.faction == f && s.type == StructureType::ClaimNode && s.complete())
      ++claims;
  return std::max(claims / cfg::CLAIMS_PER_FACTORY, cfg::MIN_FACTORY_CAP);
}

static int32_t factory_count(const World& world, FactionId f) {
  int32_t n = 0;
  for (const auto& [sid, s] : world.structures)
    if (s.alive() && s.faction == f && s.type == StructureType::Factory) ++n;
  return n;
}

static int32_t drone_count(const World& world, FactionId f) {
  int32_t n = 0;
  for (const auto& [uid, u] : world.units)
    if (u.alive() && u.faction == f && u.type == UnitType::Drone) ++n;
  for (const auto& [sid, s] : world.structures)
    if (s.alive() && s.faction == f && s.production_queue == UnitType::Drone) ++n;
  return n;
}

// Live units plus ships already paid for and queued: a queued ship spawns whatever happens next,
// so it counts against UNIT_HARD_CAP from the moment it is bought.
static int32_t unit_count(const World& world, FactionId f) {
  int32_t n = 0;
  for (const auto& [uid, u] : world.units)
    if (u.alive() && u.faction == f) ++n;
  for (const auto& [sid, s] : world.structures)
    if (s.alive() && s.faction == f && s.production_queue) ++n;
  return n;
}

static int32_t structure_count(const World& world, FactionId f) {
  int32_t n = 0;
  for (const auto& [sid, s] : world.structures)
    if (s.alive() && s.faction == f) ++n;
  return n;
}

static void start_construction(Structure& s) {
  s.hp = cfg::BUILD_START_HP;
  s.build_ticks_left = cfg::BUILD_TICKS;
}

static void spend(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b) {
  // Process in deterministic unit-id order across both factions.
  ValidatedCommands all;
  for (const auto& [k, v] : cmds_a)
    all.emplace(k, v);
  for (const auto& [k, v] : cmds_b)
    all.emplace(k, v);

  for (const auto& [uid, cmd] : all) {
    auto kind = static_cast<CommandKind>(cmd.kind);

    if (kind == CommandKind::Produce) {
      Structure* fac = world.find_structure(uid.structure());
      if (!fac || !fac->alive() || !fac->complete() || fac->production_queue.has_value())
        continue;
      if (unit_count(world, fac->faction) >= cfg::UNIT_HARD_CAP)
        continue;

      auto utype = static_cast<UnitType>(cmd.aux);
      if (utype == UnitType::Drone &&
          drone_count(world, fac->faction) >= drone_cap(world, fac->faction))
        continue;
      int32_t cost_e = 0, cost_a = 0;
      switch (utype) {
      case UnitType::Drone:
        cost_e = cfg::DRONE_COST_E;
        cost_a = cfg::DRONE_COST_A;
        break;
      case UnitType::Interceptor:
        cost_e = cfg::INTERCEPTOR_COST_E;
        cost_a = cfg::INTERCEPTOR_COST_A;
        break;
      case UnitType::Frigate:
        cost_e = cfg::FRIGATE_COST_E;
        cost_a = cfg::FRIGATE_COST_A;
        break;
      case UnitType::Artillery:
        cost_e = cfg::ARTILLERY_COST_E;
        cost_a = cfg::ARTILLERY_COST_A;
        break;
      case UnitType::Battleship:
        cost_e = cfg::BATTLESHIP_COST_E;
        cost_a = cfg::BATTLESHIP_COST_A;
        break;
      default:
        continue;
      }
      auto& res = world.res(fac->faction);
      if (res.energy < cost_e || res.alloy < cost_a)
        continue;
      res.energy -= cost_e;
      res.alloy -= cost_a;
      fac->production_queue = utype;
      fac->production_ticks_left = cfg::PRODUCTION_TICKS;

    } else if (kind == CommandKind::Build) {
      const Unit* builder = world.find_unit(uid.unit());
      if (!builder)
        continue;
      if (manhattan(builder->pos, {cmd.ax, cmd.ay}) > cfg::BUILD_RANGE)
        continue;
      auto stype = static_cast<StructureType>(cmd.aux);
      int32_t cost_e = 0, cost_a = 0;
      if (stype == StructureType::Factory) {
        cost_e = cfg::FACTORY_COST_E;
        cost_a = cfg::FACTORY_COST_A;
        if (factory_count(world, builder->faction) >= factory_cap(world, builder->faction))
          continue;
      } else
        continue;
      if (structure_count(world, builder->faction) >= cfg::STRUCTURE_HARD_CAP)
        continue;
      // Checked here, after movement, rather than in validation: a tile vacated this
      // tick is buildable, and one just moved onto is not. Commands run in unit-id
      // order, so of two builds on one tile the lower id wins.
      if (!world.tile_free({cmd.ax, cmd.ay}))
        continue;
      auto& res = world.res(builder->faction);
      if (res.energy < cost_e || res.alloy < cost_a)
        continue;
      res.energy -= cost_e;
      res.alloy -= cost_a;
      start_construction(world.spawn_structure(builder->faction, stype, {cmd.ax, cmd.ay}));

    } else if (kind == CommandKind::DeployClaim) {
      const Unit* builder = world.find_unit(uid.unit());
      if (!builder)
        continue;
      if (structure_count(world, builder->faction) >= cfg::STRUCTURE_HARD_CAP)
        continue;
      // A deploying Drone's one command this tick is the deploy, so it hasn't moved: range is
      // from where it stood when the bot decided.
      if (manhattan(builder->pos, {cmd.ax, cmd.ay}) > cfg::CLAIM_DEPLOY_RANGE)
        continue;
      if (!world.tile_free({cmd.ax, cmd.ay}))
        continue;
      auto& res = world.res(builder->faction);
      if (res.alloy < cfg::CLAIM_COST_A)
        continue;
      res.alloy -= cfg::CLAIM_COST_A;
      start_construction(
          world.spawn_structure(builder->faction, StructureType::ClaimNode, {cmd.ax, cmd.ay}));
    }
  }
}

static void tick_production(World& world) {
  for (auto& [sid, s] : world.structures) {
    if (!s.alive() || !s.production_queue.has_value())
      continue;
    if (s.production_ticks_left > 0 && --s.production_ticks_left > 0)
      continue;

    // Spawn on the first free neighbour in DIRS order. If every neighbour is taken
    // the finished unit stays in the producer (blocking the queue) and is retried
    // each tick; if nothing ever moves out of the way it is never produced.
    // Producers (Command Cores and Factories) are iterated in id order, so two sharing
    // a free tile resolve deterministically: the lower id spawns first and the tile is
    // then taken.
    for (const Vec2& d : DIRS) {
      const Vec2 tile = s.pos + d;
      if (world.tile_free(tile)) {
        world.spawn_unit(s.faction, *s.production_queue, tile);
        s.production_queue.reset();
        break;
      }
    }
  }
}

// A structure under construction gets one tick of work when an own Drone is within its
// placement range, and waits otherwise. A Drone works on one structure at a time: they take
// the nearby Drones in id order, lowest structure id first. The placing tick counts as work.
static void tick_construction(World& world) {
  std::set<UnitId> busy;
  for (auto& [sid, s] : world.structures) {
    if (!s.alive() || s.complete())
      continue;
    const int32_t span = structure_max_hp(s.type) - cfg::BUILD_START_HP;
    for (const auto& [uid, u] : world.units) {
      if (!u.alive() || u.faction != s.faction || u.type != UnitType::Drone ||
          busy.contains(uid) || manhattan(u.pos, s.pos) > placement_range(s.type))
        continue;
      busy.insert(uid);
      // Gains sum to exactly `span`, so an undamaged structure finishes at full hp (a claim's
      // then meets the fragility cap).
      const int32_t done = cfg::BUILD_TICKS - s.build_ticks_left;
      s.hp += span * (done + 1) / cfg::BUILD_TICKS - span * done / cfg::BUILD_TICKS;
      --s.build_ticks_left;
      break;
    }
  }
}

void run_economy(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b) {
  harvest(world);
  spend(world, cmds_a, cmds_b);
  tick_production(world);
  tick_construction(world);
}

} // namespace game
