#pragma once
#include "abi.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "snapshot.hpp"
#include "stats.hpp"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <deque>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
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
    parent_.assign(n, Vec2{});
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
    const auto p = path(from, goal, 1);
    return p.empty() ? from : p.front();
  }

  // The first `max_len` tiles of that same path (empty if already at a goal or none is
  // reachable).
  template <class Goal> std::vector<Vec2> path(Vec2 from, Goal goal, size_t max_len) {
    std::vector<Vec2> out;
    for (Vec2 p = search(from, goal); p != from; p = parent_[idx(p)]) out.push_back(p);
    std::reverse(out.begin(), out.end());
    if (out.size() > max_len) out.resize(max_len);
    return out;
  }

private:
  // The goal tile a breadth-first search reaches first, or `from` if none (parent_ links it).
  template <class Goal> Vec2 search(Vec2 from, Goal goal) {
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
        parent_[i] = p;
        if (goal(n)) return n;
        q.push_back(n);
      }
    }
    return from;
  }

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
  std::vector<Vec2> parent_;   // the tile each tile was reached from by the current search
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

// Where a builder works: an unfinished structure, and how near its Drone must stay
// (placement_range).
struct BuildSite {
  Vec2 pos;
  int32_t range;
};

// A structure under construction only progresses while an own Drone stays within its
// placement range, so each one keeps the Drone nearest to it (ties: lower id) as its builder.
// Returns builder id → its site.
inline std::map<uint32_t, BuildSite> builders(const Snapshot& snap) {
  std::map<uint32_t, BuildSite> out;
  for (const auto& s : snap.my_structures) {
    if (s.build_ticks_left == 0) continue;
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
    if (best)
      out.emplace(best->id,
                  BuildSite{{s.x, s.y}, placement_range(static_cast<StructureType>(s.type))});
  }
  return out;
}

// A builder's step: toward its site until within range, then hold.
inline Vec2 builder_step(Router& router, const UnitView& d, const BuildSite& site) {
  return router.next_step({d.x, d.y},
                          [&](Vec2 p) { return manhattan(p, site.pos) <= site.range; });
}

// The engine's own walk: one tile toward the target along the longer axis (ties: x).
inline Vec2 straight_step(Vec2 from, Vec2 to) {
  if (from == to) return from;
  const int32_t dx = to.x - from.x;
  const int32_t dy = to.y - from.y;
  if (std::abs(dx) >= std::abs(dy)) return {from.x + (dx > 0 ? 1 : -1), from.y};
  return {from.x, from.y + (dy > 0 ? 1 : -1)};
}

// Where to send a unit of speed `speed` this tick: the furthest of its first `speed` path tiles
// that the engine's straight walk reaches along the path itself. Sending only the next tile
// would hold a fast unit to one step a tick; sending a far tile would walk it off the route.
template <class Goal> Vec2 waypoint(Router& router, Vec2 from, Goal goal, int32_t speed) {
  const auto p = router.path(from, goal, static_cast<size_t>(speed));
  if (p.empty()) return from;
  for (size_t k = p.size(); k > 1; --k) {
    Vec2 cur = from;
    bool on_route = true;
    for (size_t i = 0; i < k && on_route; ++i) {
      cur = straight_step(cur, p[k - 1]);
      on_route = cur == p[i];
    }
    if (on_route) return p[k - 1];
  }
  return p.front();
}

inline Command move_cmd(uint32_t unit_id, CommandKind kind, Vec2 to) {
  Command cmd{};
  cmd.entity_id = unit_id;
  cmd.kind = static_cast<uint16_t>(kind);
  cmd.ax = to.x;
  cmd.ay = to.y;
  return cmd;
}

// Claim sites `spacing` apart on a lattice anchored on home, so both sides of a point-symmetric
// map get mirror-image sites, handed out nearest home first. Each claimer Drone owns one site
// at a time.
class ClaimLattice {
public:
  static constexpr int32_t kMaxAttempts = 3; // deploys at one site before giving it up

  void init(const Map& map, Vec2 home, int32_t spacing) {
    sites_.clear();
    assigned_.clear();
    next_site_ = 0;
    for (int32_t y = 0; y < map.height; ++y)
      for (int32_t x = 0; x < map.width; ++x)
        if ((x - home.x) % spacing == 0 && (y - home.y) % spacing == 0 &&
            map.passable({x, y}) && Vec2{x, y} != home)
          sites_.push_back({x, y});
    std::stable_sort(sites_.begin(), sites_.end(), [&](Vec2 a, Vec2 b) {
      return manhattan(a, home) < manhattan(b, home);
    });
  }

  bool assigned(uint32_t drone) const { return assigned_.contains(drone); }
  size_t claimers() const { return assigned_.size(); }
  bool exhausted() const { return next_site_ >= sites_.size(); }
  void release(uint32_t drone) { assigned_.erase(drone); }

  std::optional<Vec2> site_of(uint32_t drone) const {
    auto it = assigned_.find(drone);
    if (it == assigned_.end()) return std::nullopt;
    return sites_[it->second.site];
  }

  // Walks the Drone to its site and deploys there, moving on to the next site when this one is
  // claimed, taken or unreachable. Deploys only while `alloy` covers a claim, and spends it.
  template <class Issue>
  void step(Router& router, const UnitView& d, const std::set<Vec2>& claimed,
            const std::set<Vec2>& taken, int32_t& alloy, Issue&& issue) {
    const Vec2 pos{d.x, d.y};
    for (;;) {
      auto it = assigned_.find(d.id);
      if (it == assigned_.end()) {
        if (exhausted()) return;
        it = assigned_.emplace(d.id, Assignment{next_site_++, 0}).first;
      }
      const Vec2 site = sites_[it->second.site];
      if (claimed.contains(site) || it->second.attempts >= kMaxAttempts) {
        assigned_.erase(it);
        continue;
      }
      if (manhattan(pos, site) <= cfg::CLAIM_DEPLOY_RANGE) {
        if (taken.contains(site)) {
          assigned_.erase(it);
          continue;
        }
        if (alloy >= cfg::CLAIM_COST_A) {
          issue(move_cmd(d.id, CommandKind::DeployClaim, site));
          alloy -= cfg::CLAIM_COST_A;
          ++it->second.attempts;
        }
        return;
      }
      const Vec2 step = router.next_step(
          pos, [&](Vec2 p) { return manhattan(p, site) <= cfg::CLAIM_DEPLOY_RANGE; });
      if (step == pos) { // unreachable from here
        assigned_.erase(it);
        continue;
      }
      issue(move_cmd(d.id, CommandKind::Move, step));
      return;
    }
  }

private:
  struct Assignment {
    size_t site;
    int32_t attempts;
  };
  std::vector<Vec2> sites_;
  size_t next_site_{0};
  std::map<uint32_t, Assignment> assigned_; // claimer Drone id → its site
};

} // namespace game
