#pragma once
#include "grid.hpp"
#include <cstdint>
#include <vector>

namespace game {

struct World;

// Tick phase 6 — Voronoi territory + Claim Node fragility (DESIGN §7).

struct TerritoryState {
  uint32_t pct_faction[2]{0, 0}; // owned share of passable tiles per faction (0..100)
};

struct ClaimAnchor {
  Vec2 pos;
  uint32_t faction;
};

// Owner of every tile (row-major, y * width + x): 0/1 for a faction, -1 for neutral, contested
// or impassable. The single definition of territory; the viewer draws zones with it.
std::vector<int8_t> territory_owners(const Map& map, const std::vector<ClaimAnchor>& claims);

// Per-faction share of passable tiles in an owner grid from territory_owners.
TerritoryState territory_pct(const Map& map, const std::vector<int8_t>& owners);

// Recompute Voronoi territory and update Claim Node effective hp from the
// game clock g = tick / tick_cap. Returns per-faction territory percentages.
TerritoryState run_territory(World& world, uint32_t tick_cap);

// Current win threshold for the descending threshold condition.
// threshold(g) = THRESH_A - THRESH_B * g^2, clamped to [THRESH_MIN, THRESH_A].
int32_t win_threshold(uint32_t tick, uint32_t tick_cap);

} // namespace game
