#pragma once
#include "abi.hpp"
#include <vector>

namespace game {

struct World; // forward — full type in world.hpp

// ---- Engine-side snapshot (owns the data) -----------------------------------

struct Snapshot {
  SnapshotHeader header{};
  std::vector<UnitView> my_units;
  std::vector<StructureView> my_structures;
  std::vector<EnemyView> visible_enemies;
  std::vector<EnemyStructureView> visible_enemy_structures;
  std::vector<TileView> visible_tiles;
};

// ---- Builder ----------------------------------------------------------------

// Build a fog-masked snapshot for `faction` from the current world state.
// LoS rule: sight radius only (Manhattan distance), no terrain blocking.
Snapshot build_snapshot(const World& world, uint32_t faction_id, uint32_t tick_cap);

} // namespace game
