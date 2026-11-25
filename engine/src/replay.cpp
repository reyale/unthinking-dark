#include "replay.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "statehash.hpp"
#include "tick.hpp"
#include "world.hpp"
#include <stdexcept>

namespace game {

Map replay_map(const ReplayLog& log) {
  if (log.map_w < 1 || log.map_w > cfg::MAP_MAX_W || log.map_h < 1 || log.map_h > cfg::MAP_MAX_H)
    throw std::runtime_error("invalid map dimensions in replay");
  const size_t expected = static_cast<size_t>(log.map_w) * static_cast<size_t>(log.map_h);
  if (log.map_tiles.size() != expected)
    throw std::runtime_error("replay tile count does not match map dimensions");

  Map map = Map::make(log.map_w, log.map_h);
  for (int32_t y = 0; y < log.map_h; ++y) {
    for (int32_t x = 0; x < log.map_w; ++x) {
      const auto& t = log.map_tiles[static_cast<size_t>(y) * static_cast<size_t>(log.map_w) +
                                    static_cast<size_t>(x)];
      if (static_cast<uint8_t>(t.terrain) > static_cast<uint8_t>(Terrain::ResourceNode))
        throw std::runtime_error("invalid terrain in replay");
      if (t.terrain != Terrain::Open || t.resource_amount != 0)
        map.set_terrain({x, y}, t.terrain, t.resource_amount);
    }
  }
  map.recount_passable();
  return map;
}

World replay_world(const ReplayLog& log) {
  if (log.faction_a >= MAX_FACTIONS || log.faction_b >= MAX_FACTIONS ||
      log.faction_a == log.faction_b)
    throw std::runtime_error("invalid faction ids in replay");

  World world;
  world.map      = replay_map(log);
  world.rng      = Rng{log.seed};
  world.rng_seed = log.seed;

  // Restore initial entities in recorded order (preserves id assignment)
  for (const auto& e : log.initial_entities) {
    if (e.faction_id >= MAX_FACTIONS)
      throw std::runtime_error("invalid initial entity faction in replay");
    const Vec2 pos{e.x, e.y};
    if (!world.map.in_bounds(pos))
      throw std::runtime_error("initial entity out of map bounds in replay");
    const FactionId fid{e.faction_id};
    if (e.is_structure) {
      if (e.type > static_cast<uint8_t>(StructureType::ClaimNode))
        throw std::runtime_error("invalid initial structure type in replay");
      world.spawn_structure(fid, static_cast<StructureType>(e.type), pos);
    } else {
      if (e.type > static_cast<uint8_t>(UnitType::Battleship))
        throw std::runtime_error("invalid initial unit type in replay");
      world.spawn_unit(fid, static_cast<UnitType>(e.type), pos);
    }
  }
  return world;
}

uint64_t replay(const ReplayLog& log) {
  World world = replay_world(log);

  StateHash hash;
  for (const auto& entry : log.ticks) {
    if (fault_result(log.faction_a, entry.fault_a, log.faction_b, entry.fault_b))
      break;
    run_tick_phases(world, entry.raw_a, log.faction_a, entry.raw_b, log.faction_b, log.tick_cap,
                    hash);
  }

  return hash.fingerprint();
}

} // namespace game
