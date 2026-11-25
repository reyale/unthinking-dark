#include "grid.hpp"

namespace game {

// Dimensions from outside (replay and map files) are checked where they are read
// (replay_io.cpp, replay.cpp, map_io.cpp). This file is also built into every bot, which has no
// exceptions, so it can only assert.
Map Map::make(int32_t w, int32_t h, Terrain fill) {
  assert(w > 0 && h > 0);
  Map m;
  m.width = w;
  m.height = h;
  m.tiles_.assign(static_cast<size_t>(w) * static_cast<size_t>(h), Tile{fill, 0});
  m.recount_passable();
  return m;
}

void Map::set_terrain(Vec2 p, Terrain t, int32_t resource_amount) {
  assert(in_bounds(p));
  tile_at(p) = {t, resource_amount};
}

void Map::recount_passable() {
  passable_count_ = 0;
  for (const auto& tile : tiles_)
    if (is_passable(tile.terrain))
      ++passable_count_;
}

} // namespace game
