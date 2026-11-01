#include "vision.hpp"
#include "world.hpp"

namespace game {

NebulaSight::NebulaSight(const World& world) : map_(&world.map) {
  const int32_t W = world.map.width;
  const int32_t H = world.map.height;
  for (auto& s : seen_)
    s.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
  const int32_t r = cfg::NEBULA_SIGHT;
  auto mark = [&](FactionId f, Vec2 c) {
    auto& seen = seen_[f.value];
    for (int32_t y = std::max(0, c.y - r); y <= std::min(H - 1, c.y + r); ++y)
      for (int32_t x = std::max(0, c.x - r); x <= std::min(W - 1, c.x + r); ++x)
        if (manhattan(c, {x, y}) <= r)
          seen[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = 1;
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
  return seen_[f.value][static_cast<size_t>(p.y) * static_cast<size_t>(map_->width) +
                        static_cast<size_t>(p.x)] != 0;
}

} // namespace game
