#include "map_gen.hpp"
#include "map_validate.hpp"
#include <stdexcept>
#include <algorithm>

namespace maps {

static constexpr int32_t MIN_SPAWN_CLEARANCE   = 4;
static constexpr int32_t MIN_CLUSTER_CLEARANCE = 3;
static constexpr int32_t BLOB_MIN_TILES        = 3;
static constexpr int32_t BLOB_MAX_TILES        = 6;
// Nebula patches: soft cover, one per NEBULA_AREA_PER_PATCH tiles of map, grown over open
// ground only (never over rock, resources or the spawns' surroundings).
static constexpr int32_t NEBULA_AREA_PER_PATCH = 300;
static constexpr int32_t NEBULA_MIN_TILES      = 6;
static constexpr int32_t NEBULA_MAX_TILES      = 14;

static const int32_t DX[4] = {0, 0, 1, -1};
static const int32_t DY[4] = {1, -1, 0, 0};

static MapData generate_once(const GenParams& p, game::Rng& rng) {
  int32_t w = p.width;
  int32_t h = p.height;

  MapData data;
  data.name            = p.name;
  data.resource_amount = p.resource_amount;
  data.map             = game::Map::make(w, h, game::Terrain::Open);
  data.spawn[0]        = {2, 2};
  data.spawn[1]        = {w - 3, h - 3};

  auto set_sym = [&](game::Vec2 pos, game::Terrain t) {
    if (!data.map.in_bounds(pos))
      return;
    game::Vec2 mirror{w - 1 - pos.x, h - 1 - pos.y};
    if (!data.map.in_bounds(mirror))
      return;
    int32_t ra = (t == game::Terrain::ResourceNode) ? p.resource_amount : 0;
    data.map.set_terrain(pos, t, ra);
    data.map.set_terrain(mirror, t, ra);
  };

  for (int32_t x = 0; x < w; ++x) {
    data.map.set_terrain({x, 0}, game::Terrain::Asteroid);
    data.map.set_terrain({x, h - 1}, game::Terrain::Asteroid);
  }
  for (int32_t y = 0; y < h; ++y) {
    data.map.set_terrain({0, y}, game::Terrain::Asteroid);
    data.map.set_terrain({w - 1, y}, game::Terrain::Asteroid);
  }

  constexpr int32_t kOffsets[4][2] = {{3, 0}, {0, 3}, {4, 2}, {2, 4}};
  for (auto& off : kOffsets) {
    game::Vec2 pos{data.spawn[0].x + off[0], data.spawn[0].y + off[1]};
    if (data.map.in_bounds(pos) &&
        data.map.tile_at(pos).terrain != game::Terrain::Asteroid)
      set_sym(pos, game::Terrain::ResourceNode);
  }

  game::Vec2 center_a{w / 2 - 1, h / 2};
  game::Vec2 center_b{w / 2, h / 2 - 1};
  if (!(center_a == center_b)) {
    if (data.map.in_bounds(center_a) &&
        data.map.tile_at(center_a).terrain != game::Terrain::Asteroid)
      set_sym(center_a, game::Terrain::ResourceNode);
  }

  int32_t n_clusters = std::max(2, w * h / 80);
  for (int32_t ci = 0; ci < n_clusters; ++ci) {
    int32_t cx = static_cast<int32_t>(rng.next_u32(static_cast<uint32_t>(w / 2 - 3))) + 2;
    int32_t cy = static_cast<int32_t>(rng.next_u32(static_cast<uint32_t>(h - 4))) + 2;

    game::Vec2 sp0 = data.spawn[0];
    game::Vec2 sp1 = data.spawn[1];
    if (game::manhattan({cx, cy}, sp0) < MIN_SPAWN_CLEARANCE ||
        game::manhattan({cx, cy}, sp1) < MIN_SPAWN_CLEARANCE)
      continue;

    int32_t blob_size =
        BLOB_MIN_TILES +
        static_cast<int32_t>(
            rng.next_u32(static_cast<uint32_t>(BLOB_MAX_TILES - BLOB_MIN_TILES + 1)));

    int32_t bx = cx, by = cy;
    for (int32_t t = 0; t < blob_size; ++t) {
      uint32_t dir = rng.next_u32(4);
      bx += DX[dir];
      by += DY[dir];

      if (bx <= 0 || bx >= w - 1 || by <= 0 || by >= h - 1)
        continue;

      game::Vec2 pos{bx, by};
      game::Vec2 mpos{w - 1 - bx, h - 1 - by};

      if (game::manhattan(pos, sp0) < MIN_CLUSTER_CLEARANCE ||
          game::manhattan(pos, sp1) < MIN_CLUSTER_CLEARANCE ||
          game::manhattan(mpos, sp0) < MIN_CLUSTER_CLEARANCE ||
          game::manhattan(mpos, sp1) < MIN_CLUSTER_CLEARANCE)
        continue;

      data.map.set_terrain(pos, game::Terrain::Asteroid);
      data.map.set_terrain(mpos, game::Terrain::Asteroid);
    }
  }

  // Drawn after the asteroids, so adding nebulae left every seed's rock layout unchanged.
  const int32_t n_nebulae = std::max(1, w * h / NEBULA_AREA_PER_PATCH);
  for (int32_t ni = 0; ni < n_nebulae; ++ni) {
    int32_t bx = static_cast<int32_t>(rng.next_u32(static_cast<uint32_t>(w / 2 - 3))) + 2;
    int32_t by = static_cast<int32_t>(rng.next_u32(static_cast<uint32_t>(h - 4))) + 2;
    const int32_t size =
        NEBULA_MIN_TILES +
        static_cast<int32_t>(
            rng.next_u32(static_cast<uint32_t>(NEBULA_MAX_TILES - NEBULA_MIN_TILES + 1)));
    for (int32_t t = 0; t < size; ++t) {
      const uint32_t dir = rng.next_u32(4);
      bx = std::clamp(bx + DX[dir], 1, w - 2);
      by = std::clamp(by + DY[dir], 1, h - 2);
      const game::Vec2 pos{bx, by};
      if (data.map.tile_at(pos).terrain != game::Terrain::Open) continue;
      if (game::manhattan(pos, data.spawn[0]) < MIN_SPAWN_CLEARANCE ||
          game::manhattan(pos, data.spawn[1]) < MIN_SPAWN_CLEARANCE ||
          game::manhattan({w - 1 - bx, h - 1 - by}, data.spawn[0]) < MIN_SPAWN_CLEARANCE ||
          game::manhattan({w - 1 - bx, h - 1 - by}, data.spawn[1]) < MIN_SPAWN_CLEARANCE)
        continue;
      if (data.map.tile_at({w - 1 - bx, h - 1 - by}).terrain != game::Terrain::Open) continue;
      set_sym(pos, game::Terrain::Nebula);
    }
  }

  data.map.set_terrain(data.spawn[0], game::Terrain::Open);
  data.map.set_terrain(data.spawn[1], game::Terrain::Open);
  data.map.recount_passable();
  return data;
}

MapData generate_map(const GenParams& p, game::Rng& rng, int max_attempts) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    MapData candidate = generate_once(p, rng);
    if (validate_map(candidate).empty())
      return candidate;
  }
  throw std::runtime_error(
      "generate_map: failed to produce a valid map after " +
      std::to_string(max_attempts) + " attempts");
}

} // namespace maps
