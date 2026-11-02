#include "vision.hpp"
#include "world.hpp"

namespace game {

NebulaSight::NebulaSight(const World& world) : map_(&world.map) {
  const Map& map = world.map;
  const int32_t r = cfg::NEBULA_SIGHT;
  // Only the diamond itself: this runs for every entity, every movement step and combat.
  auto mark = [&](FactionId f, Vec2 c) {
    for (int32_t dy = -r; dy <= r; ++dy) {
      const int32_t w = r - (dy < 0 ? -dy : dy);
      for (int32_t dx = -w; dx <= w; ++dx) {
        const Vec2 p{c.x + dx, c.y + dy};
        if (map.in_bounds(p) && map.tile_at(p).terrain == Terrain::Nebula)
          seen_[f.value].insert(p);
      }
    }
  };
  for (const auto& [id, u] : world.units)
    if (u.alive())
      mark(u.faction, u.pos);
  for (const auto& [id, s] : world.structures)
    if (s.alive())
      mark(s.faction, s.pos);
}

bool NebulaSight::sees(FactionId f, Vec2 p) const {
  if (map_->tile_at(p).terrain != Terrain::Nebula)
    return true;
  return seen_[f.value].contains(p);
}

} // namespace game
