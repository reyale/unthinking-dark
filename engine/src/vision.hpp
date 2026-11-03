#pragma once
#include "config.hpp"
#include "grid.hpp"
#include "ids.hpp"
#include "occupancy.hpp"
#include "world.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>

namespace game {

// Line of sight is Manhattan distance only, except into a nebula: a nebula tile, and whatever
// is on it, is seen only from within NEBULA_SIGHT. Units inside a nebula see out normally.
inline bool sees_tile(const Map& map, Vec2 from, int32_t sight, Vec2 p) {
  const int32_t d = manhattan(from, p);
  return d <= sight && (d <= cfg::NEBULA_SIGHT || map.tile_at(p).terrain != Terrain::Nebula);
}

inline int32_t sight_radius(const Unit& u) { return u.stats().sight; }
inline int32_t sight_radius(const Structure&) { return cfg::STRUCTURE_SIGHT; }

// The farthest anything sees: how far around a tile to look for whoever sees it.
inline constexpr int32_t MAX_SIGHT =
    std::max({cfg::DRONE_SIGHT, cfg::INTERCEPTOR_SIGHT, cfg::FRIGATE_SIGHT, cfg::ARTILLERY_SIGHT,
              cfg::BATTLESHIP_SIGHT, cfg::STRUCTURE_SIGHT});

// What each faction sees right now, for combat targeting and MoveAttack halts: a unit can only
// fire at what its side sees (any of its living units or structures, by sees_tile), so a
// long-range gun needs a spotter. The snapshot's fog uses the same rule.
//
// Asked tile by tile through the occupancy index, and remembered: a check looks only at the
// entities within MAX_SIGHT of the tile, where a per-tile grid would cost a pass over every
// entity's whole sight area each movement step. `occ` must hold the positions the checks are
// made at, so build this after rebuilding it, and again after anything moves or dies.
class FactionSight {
public:
  FactionSight(const World& world, const Occupancy& occ) : map_(world.map), occ_(occ) {}

  bool sees(FactionId f, Vec2 p) const {
    auto& known = seen_[f.value];
    if (auto it = known.find(p); it != known.end())
      return it->second;
    const bool seen = occ_.any_within(p, MAX_SIGHT, [&](const auto& e) {
      return e.faction == f && sees_tile(map_, e.pos, sight_radius(e), p);
    });
    known.emplace(p, seen);
    return seen;
  }

private:
  const Map& map_;
  const Occupancy& occ_;
  mutable std::array<std::map<Vec2, bool>, MAX_FACTIONS> seen_;
};

} // namespace game
