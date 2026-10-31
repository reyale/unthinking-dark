#pragma once
#include "abi.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "snapshot.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace game {

// Routing for fixture bots. The engine moves a unit one straight-line step and leaves it put
// if that step is blocked (DESIGN: no retry), so a bot that wants to get anywhere has to plan
// its own path.
//
// Routes around our own structures and every enemy structure seen so far (they never move, so
// one stays an obstacle after it drops out of view, until it is seen gone). An own unit that
// didn't move last tick is treated as parked and routed around; one that is moving blocks only
// the first step, since it will likely have gone by the time we get there. Planning through a
// parked unit makes a bot walk up to it, turn back and try again forever. Enemy units are not
// avoided.
class Router {
public:
  void reset(const Map& map) {
    map_ = map;
    const size_t n = static_cast<size_t>(map.width) * static_cast<size_t>(map.height);
    last_pos_.clear();
    enemy_structure_.assign(n, 0);
    own_unit_.assign(n, 0);
    own_structure_.assign(n, 0);
    seen_.assign(n, 0);
    first_.assign(n, Vec2{});
    generation_ = 0;
  }

  // Call once at the start of each tick, before next_step.
  void observe(const Snapshot& snap) {
    std::fill(own_unit_.begin(), own_unit_.end(), 0);
    std::fill(own_structure_.begin(), own_structure_.end(), 0);
    std::map<uint32_t, Vec2> now;
    for (const auto& u : snap.my_units) {
      const Vec2 p{u.x, u.y};
      now.emplace(u.id, p);
      auto it = last_pos_.find(u.id);
      own_unit_[idx(p)] = it != last_pos_.end() && it->second == p ? kParked : kMoving;
    }
    last_pos_ = std::move(now);
    for (const auto& s : snap.my_structures) own_structure_[idx({s.x, s.y})] = 1;
    for (const auto& t : snap.visible_tiles) enemy_structure_[idx({t.x, t.y})] = 0;
    for (const auto& s : snap.visible_enemy_structures) enemy_structure_[idx({s.x, s.y})] = 1;
  }

  // First step of a shortest 4-connected path from `from` to any tile satisfying `goal`, or
  // `from` itself if already at a goal or none is reachable. Breadth-first in DIRS order, so
  // ties between equally short paths always resolve the same way.
  template <class Goal> Vec2 next_step(Vec2 from, Goal goal) {
    if (goal(from)) return from;
    ++generation_;
    seen_[idx(from)] = generation_;
    std::deque<Vec2> q{from};
    while (!q.empty()) {
      const Vec2 p = q.front();
      q.pop_front();
      for (const Vec2& d : DIRS) {
        const Vec2 n = p + d;
        if (!map_.passable(n)) continue;
        const size_t i = idx(n);
        if (seen_[i] == generation_ || enemy_structure_[i] || own_structure_[i]) continue;
        if (own_unit_[i] == kParked || (p == from && own_unit_[i] == kMoving)) continue;
        seen_[i] = generation_;
        first_[i] = p == from ? n : first_[idx(p)];
        if (goal(n)) return first_[i];
        q.push_back(n);
      }
    }
    return from;
  }

private:
  size_t idx(Vec2 p) const {
    return static_cast<size_t>(p.y) * static_cast<size_t>(map_.width) + static_cast<size_t>(p.x);
  }

  static constexpr uint8_t kMoving = 1;
  static constexpr uint8_t kParked = 2;

  Map map_;
  std::map<uint32_t, Vec2> last_pos_; // own unit positions last tick
  std::vector<uint8_t> enemy_structure_, own_unit_, own_structure_;
  std::vector<uint32_t> seen_; // == generation_ when visited by the current search
  std::vector<Vec2> first_;    // the first step of the path that reached each tile
  uint32_t generation_{0};
};

inline const StructureView* find_structure(const Snapshot& snap, StructureType type) {
  for (const auto& s : snap.my_structures)
    if (static_cast<StructureType>(s.type) == type) return &s;
  return nullptr;
}

inline Vec2 core_pos(const Snapshot& snap) {
  const StructureView* core = find_structure(snap, StructureType::CommandCore);
  return core ? Vec2{core->x, core->y} : Vec2{0, 0};
}

inline bool near_resource(const Map& map, Vec2 p) {
  for (int32_t dy = -1; dy <= 1; ++dy)
    for (int32_t dx = -1; dx <= 1; ++dx) {
      if (dx != 0 && dy != 0) continue;
      const Vec2 q{p.x + dx, p.y + dy};
      if (map.in_bounds(q) && map.tile_at(q).terrain == Terrain::ResourceNode) return true;
    }
  return false;
}

inline Command move_cmd(uint32_t unit_id, CommandKind kind, Vec2 to) {
  Command cmd{};
  cmd.entity_id = unit_id;
  cmd.kind = static_cast<uint16_t>(kind);
  cmd.ax = to.x;
  cmd.ay = to.y;
  return cmd;
}

} // namespace game
