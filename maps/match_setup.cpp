#include "match_setup.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include <cstdio>

namespace maps {

static void place_faction(game::World& w, game::FactionId faction, game::Vec2 spawn) {
  w.spawn_structure(faction, game::StructureType::CommandCore, spawn);

  // Bias unit placement inward toward the map centre.
  int32_t dx = (spawn.x * 2 < w.map.width)  ? +1 : -1;
  int32_t dy = (spawn.y * 2 < w.map.height) ? +1 : -1;

  const game::Vec2 offsets[] = {
    {dx, 0}, {dx*2, 0}, {0, dy}, {dx, dy}, {dx*3, 0}, {0, dy*2},
  };

  const game::UnitType roster[] = {
    game::UnitType::Drone,
    game::UnitType::Interceptor,
    game::UnitType::Frigate,
  };

  for (auto type : roster) {
    bool placed = false;
    for (auto [ox, oy] : offsets) {
      game::Vec2 pos = {spawn.x + ox, spawn.y + oy};
      if (w.tile_free(pos)) {
        w.spawn_unit(faction, type, pos);
        placed = true;
        break;
      }
    }
    if (!placed)
      fprintf(stderr, "warning: no free tile near spawn %d,%d for faction %u's %s\n", spawn.x,
              spawn.y, faction.value, game::unit_type_name(type));
  }
}

game::World world_from_map(const MapData& md, uint64_t seed) {
  game::World w;
  w.map      = md.map;
  w.rng      = game::Rng{seed};
  w.rng_seed = seed;
  place_faction(w, {0}, md.spawn[0]);
  place_faction(w, {1}, md.spawn[1]);
  return w;
}

} // namespace maps
