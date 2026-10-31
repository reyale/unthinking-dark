#include "snapshot.hpp"
#include "config.hpp"
#include "territory.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include <vector>

namespace game {

Snapshot build_snapshot(const World& world, uint32_t faction_id, uint32_t tick_cap) {
  FactionId my_faction{faction_id};
  const auto& res = world.res(my_faction);

  // --- Compute visibility mask -----------------------------------------
  // A tile is visible if it is within sight (Manhattan) of any friendly unit or structure:
  // a unit's own sight radius, STRUCTURE_SIGHT for every structure. No terrain blocking.
  const int32_t W = world.map.width;
  const int32_t H = world.map.height;
  std::vector<bool> visible(static_cast<size_t>(W * H), false);
  auto see_around = [&](Vec2 c, int32_t sight) {
    const int32_t x0 = std::max(0, c.x - sight);
    const int32_t x1 = std::min(W - 1, c.x + sight);
    const int32_t y0 = std::max(0, c.y - sight);
    const int32_t y1 = std::min(H - 1, c.y + sight);
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x)
        if (manhattan(c, {x, y}) <= sight)
          visible[static_cast<size_t>(y * W + x)] = true;
  };
  for (const auto& [id, unit] : world.units)
    if (unit.faction == my_faction && unit.alive())
      see_around(unit.pos, unit.stats().sight);
  for (const auto& [id, s] : world.structures)
    if (s.faction == my_faction && s.alive())
      see_around(s.pos, cfg::STRUCTURE_SIGHT);

  // --- Build visible-tile occupancy map --------------------------------
  // Maps tile position → occupant faction (0=none, 1=me, 2=enemy).
  std::vector<uint16_t> occupancy(static_cast<size_t>(W * H), 0);
  for (const auto& [id, unit] : world.units) {
    if (!unit.alive())
      continue;
    auto idx = static_cast<size_t>(unit.pos.y * W + unit.pos.x);
    occupancy[idx] = (unit.faction == my_faction) ? 1 : 2;
  }

  // --- Populate snapshot -----------------------------------------------
  Snapshot snap;
  snap.header.tick = world.tick;
  snap.header.my_faction_id = faction_id;
  snap.header.energy = res.energy;
  snap.header.alloy = res.alloy;
  snap.header.territory_pct = world.last_territory.pct_faction[faction_id];
  snap.header.win_threshold = static_cast<uint32_t>(win_threshold(world.tick, tick_cap));
  snap.header.map_w = static_cast<uint32_t>(W);
  snap.header.map_h = static_cast<uint32_t>(H);

  // My units (all, regardless of fog — bots always know their own fleet)
  for (const auto& [id, unit] : world.units) {
    if (unit.faction != my_faction || !unit.alive())
      continue;
    UnitView uv{};
    uv.id = id.value;
    uv.type = static_cast<uint16_t>(unit.type);
    uv.x = unit.pos.x;
    uv.y = unit.pos.y;
    uv.hp = unit.hp;
    uv.current_order = static_cast<uint16_t>(unit.current_order);
    snap.my_units.push_back(uv);
  }

  for (const auto& [id, s] : world.structures) {
    if (s.faction != my_faction || !s.alive())
      continue;
    StructureView sv{};
    sv.id = id.value;
    sv.type = static_cast<uint16_t>(s.type);
    sv.producing =
      s.production_queue ? static_cast<uint16_t>(*s.production_queue) : PRODUCING_NONE;
    sv.x = s.pos.x;
    sv.y = s.pos.y;
    sv.hp = s.hp;
    sv.production_ticks_left =
      s.production_queue ? static_cast<uint32_t>(s.production_ticks_left) : 0;
    snap.my_structures.push_back(sv);
  }

  // Visible enemies and tiles
  for (int32_t y = 0; y < H; ++y) {
    for (int32_t x = 0; x < W; ++x) {
      if (!visible[static_cast<size_t>(y * W + x)])
        continue;

      const auto& tile = world.map.tile_at({x, y});
      TileView tv{};
      tv.x = x;
      tv.y = y;
      tv.terrain = static_cast<uint16_t>(tile.terrain);
      tv.occupant_faction = occupancy[static_cast<size_t>(y * W + x)];
      tv.resource_amount = tile.resource_amount;
      snap.visible_tiles.push_back(tv);
    }
  }

  // Visible enemies: iterate units, emit if enemy and on a visible tile
  for (const auto& [id, unit] : world.units) {
    if (unit.faction == my_faction || !unit.alive())
      continue;
    if (!visible[static_cast<size_t>(unit.pos.y * W + unit.pos.x)])
      continue;
    EnemyView ev{};
    ev.id = id.value;
    ev.type = static_cast<uint16_t>(unit.type);
    ev.x = unit.pos.x;
    ev.y = unit.pos.y;
    ev.hp = unit.hp;
    snap.visible_enemies.push_back(ev);
  }

  for (const auto& [id, s] : world.structures) {
    if (s.faction == my_faction || !s.alive())
      continue;
    if (!visible[static_cast<size_t>(s.pos.y * W + s.pos.x)])
      continue;
    EnemyStructureView es{};
    es.id = id.value;
    es.type = static_cast<uint16_t>(s.type);
    es.x = s.pos.x;
    es.y = s.pos.y;
    es.hp = s.hp;
    snap.visible_enemy_structures.push_back(es);
  }

  snap.header.my_unit_count = static_cast<uint32_t>(snap.my_units.size());
  snap.header.visible_enemy_count = static_cast<uint32_t>(snap.visible_enemies.size());
  snap.header.visible_tile_count = static_cast<uint32_t>(snap.visible_tiles.size());
  snap.header.my_structure_count = static_cast<uint32_t>(snap.my_structures.size());
  snap.header.visible_enemy_structure_count =
    static_cast<uint32_t>(snap.visible_enemy_structures.size());

  // Byte offsets into the flat WASM buffer: header, my units, my structures, enemy units,
  // enemy structures, tiles.
  snap.header.my_units_off = sizeof(SnapshotHeader);
  snap.header.my_structures_off =
    snap.header.my_units_off + static_cast<uint32_t>(snap.my_units.size() * sizeof(UnitView));
  snap.header.enemies_off =
    snap.header.my_structures_off +
    static_cast<uint32_t>(snap.my_structures.size() * sizeof(StructureView));
  snap.header.enemy_structures_off =
    snap.header.enemies_off +
    static_cast<uint32_t>(snap.visible_enemies.size() * sizeof(EnemyView));
  snap.header.tiles_off =
    snap.header.enemy_structures_off +
    static_cast<uint32_t>(snap.visible_enemy_structures.size() * sizeof(EnemyStructureView));

  return snap;
}

} // namespace game
