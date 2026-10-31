#pragma once
#include "abi.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "snapshot.hpp"
#include <algorithm>
#include <climits>
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
//
// The lower id has right of way: a moving unit with a higher id doesn't block a first step,
// because it treats ours as blocking and steps aside, and the engine lets us follow into the
// tile it leaves. Two units meeting head-on in a corridor would otherwise both detour every
// tick, forever; and since only one side plans through the other, they never swap (which the
// engine blocks).
class Router {
public:
  void reset(const Map& map) {
    map_ = map;
    const size_t n = static_cast<size_t>(map.width) * static_cast<size_t>(map.height);
    last_pos_.clear();
    enemy_structure_.assign(n, 0);
    own_unit_.assign(n, 0);
    own_id_.assign(n, 0);
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
      own_id_[idx(p)] = u.id;
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
    const uint32_t self = own_unit_[idx(from)] ? own_id_[idx(from)] : 0;
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
        if (own_unit_[i] == kParked) continue;
        if (p == from && own_unit_[i] == kMoving && own_id_[i] < self) continue;
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
  std::vector<uint32_t> own_id_; // id of the own unit on each tile marked in own_unit_
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

inline bool at_drop_off(const Snapshot& snap, Vec2 p) {
  for (const auto& s : snap.my_structures) {
    const auto type = static_cast<StructureType>(s.type);
    if ((type == StructureType::CommandCore || type == StructureType::ClaimNode) &&
        manhattan(p, {s.x, s.y}) <= cfg::DEPOSIT_RANGE)
      return true;
  }
  return false;
}

// A Drone's next step round its harvest loop: out to a resource node until its cargo is full,
// then back to a drop-off (own Command Core or Claim Node) to bank it.
inline Vec2 harvest_step(Router& router, const Map& map, const Snapshot& snap, const UnitView& d) {
  const Vec2 pos{d.x, d.y};
  if (d.cargo_energy >= cfg::DRONE_CARGO_ENERGY || d.cargo_alloy >= cfg::DRONE_CARGO_ALLOY)
    return router.next_step(pos, [&](Vec2 p) { return at_drop_off(snap, p); });
  return router.next_step(pos, [&](Vec2 p) { return near_resource(map, p); });
}

// A claim under construction only progresses while an own Drone stays within
// CLAIM_DEPLOY_RANGE, so each unfinished claim keeps the Drone nearest to it (ties: lower id)
// as its builder. Returns builder id → claim position.
inline std::map<uint32_t, Vec2> claim_builders(const Snapshot& snap) {
  std::map<uint32_t, Vec2> out;
  for (const auto& s : snap.my_structures) {
    if (static_cast<StructureType>(s.type) != StructureType::ClaimNode || s.build_ticks_left == 0)
      continue;
    const UnitView* best = nullptr;
    int32_t best_dist = INT32_MAX;
    for (const auto& u : snap.my_units) {
      if (static_cast<UnitType>(u.type) != UnitType::Drone || out.contains(u.id)) continue;
      const int32_t d = manhattan({u.x, u.y}, {s.x, s.y});
      if (d < best_dist) {
        best_dist = d;
        best = &u;
      }
    }
    if (best) out.emplace(best->id, Vec2{s.x, s.y});
  }
  return out;
}

// A builder's step: toward its claim until within CLAIM_DEPLOY_RANGE, then hold.
inline Vec2 builder_step(Router& router, const UnitView& d, Vec2 claim) {
  return router.next_step({d.x, d.y},
                          [&](Vec2 p) { return manhattan(p, claim) <= cfg::CLAIM_DEPLOY_RANGE; });
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
