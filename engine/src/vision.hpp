#pragma once
#include "config.hpp"
#include "grid.hpp"
#include "ids.hpp"
#include "world.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace game {

// Line of sight is Manhattan distance only, except into a nebula: a nebula tile, and whatever
// is on it, is seen only from within NEBULA_SIGHT. Units inside a nebula see out normally.
inline bool sees_tile(const Map& map, Vec2 from, int32_t sight, Vec2 p) {
  const int32_t d = manhattan(from, p);
  return d <= sight && (d <= cfg::NEBULA_SIGHT || map.tile_at(p).terrain != Terrain::Nebula);
}

// Which nebula tiles each faction sees into right now, for combat targeting: a unit hidden in a
// nebula can't be attacked by a side that can't see it. Every other tile counts as seen (combat
// is limited by weapon range, not sight). Build it from the positions the check is made at.
class NebulaSight {
public:
  explicit NebulaSight(const World& world);
  bool sees(FactionId f, Vec2 p) const;

private:
  const Map* map_;
  std::array<std::vector<uint8_t>, MAX_FACTIONS> seen_;
};

} // namespace game
