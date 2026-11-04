#include "replay.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "statehash.hpp"
#include "tick.hpp"
#include "world.hpp"

namespace game {

uint64_t replay(const ReplayLog& log) {
  // Rebuild map
  Map map = Map::make(log.map_w, log.map_h);
  for (int32_t y = 0; y < log.map_h; ++y) {
    for (int32_t x = 0; x < log.map_w; ++x) {
      const auto& t = log.map_tiles[static_cast<size_t>(y * log.map_w + x)];
      if (t.terrain != Terrain::Open || t.resource_amount != 0)
        map.set_terrain({x, y}, t.terrain, t.resource_amount);
    }
  }
  map.recount_passable();

  World world;
  world.map = std::move(map);
  world.rng = Rng{log.seed};

  // Restore initial entities in recorded order (preserves id assignment)
  for (const auto& e : log.initial_entities) {
    FactionId fid{e.faction_id};
    Vec2 pos{e.x, e.y};
    if (e.is_structure)
      world.spawn_structure(fid, static_cast<StructureType>(e.type), pos);
    else
      world.spawn_unit(fid, static_cast<UnitType>(e.type), pos);
  }

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
