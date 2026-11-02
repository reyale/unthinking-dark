#include "occupancy.hpp"

namespace game {

void Occupancy::rebuild(const World& world) {
  const size_t n = static_cast<size_t>(world.map.width) * static_cast<size_t>(world.map.height);
  if (units_.size() != n) {
    units_.assign(n, nullptr);
    structures_.assign(n, nullptr);
  } else {
    for (size_t i : filled_) {
      units_[i] = nullptr;
      structures_[i] = nullptr;
    }
  }
  world_ = &world;
  width_ = world.map.width;
  filled_.clear();
  stacked_ = false;

  for (const auto& [id, u] : world.units) {
    if (!u.alive()) continue;
    if (!world.map.in_bounds(u.pos)) {
      stacked_ = true; // only the full scan sees it
      continue;
    }
    const size_t i = idx(u.pos);
    if (units_[i]) {
      stacked_ = true; // ascending ids: the one already there is the lowest
      continue;
    }
    units_[i] = &u;
    filled_.push_back(i);
  }
  for (const auto& [id, s] : world.structures) {
    if (!s.alive()) continue;
    if (!world.map.in_bounds(s.pos)) {
      stacked_ = true;
      continue;
    }
    const size_t i = idx(s.pos);
    if (structures_[i]) {
      stacked_ = true;
      continue;
    }
    structures_[i] = &s;
    filled_.push_back(i);
  }
}

} // namespace game
