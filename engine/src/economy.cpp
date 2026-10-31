#include "economy.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "config.hpp"

namespace game {

static void harvest(World& world) {
  for (const auto& [uid, unit] : world.units) {
    if (!unit.alive() || unit.type != UnitType::Drone)
      continue;
    // Check current tile and 4 neighbours for resource nodes
    for (int32_t dy = -1; dy <= 1; ++dy) {
      for (int32_t dx = -1; dx <= 1; ++dx) {
        if (dx != 0 && dy != 0)
          continue; // 4-connected only
        Vec2 p{unit.pos.x + dx, unit.pos.y + dy};
        if (!world.map.in_bounds(p))
          continue;
        const Tile& tile = world.map.tile_at(p);
        if (tile.terrain != Terrain::ResourceNode)
          continue;
        if (tile.resource_amount <= 0)
          continue;
        world.res(unit.faction).energy += cfg::HARVEST_ENERGY;
        world.res(unit.faction).alloy += cfg::HARVEST_ALLOY;
      }
    }
  }
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
      if (!fac || !fac->alive() || fac->production_queue.has_value())
        continue;
      if (unit_count(world, fac->faction) >= cfg::UNIT_HARD_CAP)
        continue;

      auto utype = static_cast<UnitType>(cmd.aux);
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
      world.spawn_structure(builder->faction, stype, {cmd.ax, cmd.ay});

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
      world.spawn_structure(builder->faction, StructureType::ClaimNode, {cmd.ax, cmd.ay});
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

void run_economy(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b) {
  harvest(world);
  spend(world, cmds_a, cmds_b);
  tick_production(world);
}

} // namespace game
