#pragma once
#include <cstdint>

namespace game {

struct World;

// Tick phase 6 — Voronoi territory + Claim Node fragility (DESIGN §7).

struct TerritoryState {
  uint32_t pct_faction[2]{0, 0}; // owned share of passable tiles per faction (0..100)
};

// Recompute Voronoi territory and update Claim Node effective hp from the
// game clock g = tick / tick_cap. Returns per-faction territory percentages.
TerritoryState run_territory(World& world, uint32_t tick_cap);

// Current win threshold for the descending threshold condition.
// threshold(g) = THRESH_A - THRESH_B * g^2, clamped to [THRESH_MIN, THRESH_A].
int32_t win_threshold(uint32_t tick, uint32_t tick_cap);

} // namespace game
