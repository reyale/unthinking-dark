#pragma once
#include "abi.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "snapshot.hpp"
#include "stats.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
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
  // Tiles are kept on a grid padded with a closed border, so a step is an index offset with no
  // bounds check: these floods are most of a bot's fuel.
  void reset(const Map& map) {
    map_ = map;
    stride_ = map.width + 2;
    const size_t n = static_cast<size_t>(stride_) * static_cast<size_t>(map.height + 2);
    last_pos_.clear();
    enemy_structure_.assign(n, 0);
    enemy_cells_.clear();
    own_unit_.assign(n, 0);
    own_id_.assign(n, 0);
    marked_.clear();
    seen_.assign(n, 0);
    parent_.assign(n, 0);
    generation_ = 0;
    passable_.assign(n, 0);
    fixed_fields_.clear();
    structure_cells_.clear();
    tick_ = 0;
    nodes_.clear();
    for (int32_t y = 0; y < map.height; ++y)
      for (int32_t x = 0; x < map.width; ++x) {
        passable_[cell({x, y})] = map.passable({x, y});
        if (map.tile_at({x, y}).terrain == Terrain::ResourceNode) nodes_.push_back({x, y});
      }
    for (size_t k = 0; k < 4; ++k) step_[k] = DIRS[k].y * stride_ + DIRS[k].x;
    settled_ = passable_;
  }

  // Call once at the start of each tick, before next_step.
  void observe(const Snapshot& snap) {
    for (int32_t c : marked_) own_unit_[static_cast<size_t>(c)] = 0;
    marked_.clear();
    std::map<uint32_t, Vec2> now;
    for (const auto& u : snap.my_units) {
      const Vec2 p{u.x, u.y};
      now.emplace(u.id, p);
      auto it = last_pos_.find(u.id);
      const int32_t c = cell(p);
      own_unit_[static_cast<size_t>(c)] =
          it != last_pos_.end() && it->second == p ? kParked : kMoving;
      own_id_[static_cast<size_t>(c)] = u.id;
      marked_.push_back(c);
    }
    last_pos_ = std::move(now);
    for (const auto& t : snap.visible_tiles) {
      const int32_t c = cell({t.x, t.y});
      if (enemy_structure_[static_cast<size_t>(c)]) {
        enemy_structure_[static_cast<size_t>(c)] = 0;
        enemy_cells_.erase(c);
      }
    }
    for (const auto& s : snap.visible_enemy_structures) {
      enemy_structure_[static_cast<size_t>(cell({s.x, s.y}))] = 1;
      enemy_cells_.insert(cell({s.x, s.y}));
    }
    std::set<int32_t> now_cells = enemy_cells_;
    for (const auto& s : snap.my_structures) now_cells.insert(cell({s.x, s.y}));
    if (now_cells != structure_cells_) {
      std::vector<int32_t> changed;
      std::set_symmetric_difference(now_cells.begin(), now_cells.end(), structure_cells_.begin(),
                                    structure_cells_.end(), std::back_inserter(changed));
      for (int32_t c : changed) settled_[static_cast<size_t>(c)] = now_cells.contains(c) ? 0 : 1;
      structure_cells_ = std::move(now_cells);
      // A field the change didn't reach is still exact; one it did is started again.
      for (auto it = fixed_fields_.begin(); it != fixed_fields_.end();)
        it = touches(it->second, changed) ? fixed_fields_.erase(it) : std::next(it);
    }
    open_ = settled_;
    for (int32_t c : marked_)
      if (own_unit_[static_cast<size_t>(c)] == kParked) open_[static_cast<size_t>(c)] = 0;
    fields_.clear();
    flood_left_ = kFloodBudget;
    ++tick_;
    for (auto it = fixed_fields_.begin(); it != fixed_fields_.end();)
      it = it->second.used + 1 < tick_ ? fixed_fields_.erase(it) : std::next(it);
  }

  // Names a goal for path(key, ...): units given equal keys in one tick must be given the same
  // goal, since the first to ask starts the distance field the others reuse. Every goal tile
  // must lie within `radius` of `at`; only there is the goal tested.
  struct GoalKey {
    int32_t kind;
    Vec2 at;
    int32_t radius;
    uint32_t tag{0}; // anything else the goal depends on
    bool operator<(const GoalKey& o) const {
      return std::tie(kind, at.x, at.y, radius, tag) <
             std::tie(o.kind, o.at.x, o.at.y, o.radius, o.tag);
    }
  };
  // Kinds the helpers below use; a bot's own are 0 and up.
  static constexpr int32_t kGoalDropOff = -1;
  static constexpr int32_t kGoalResource = -2;
  static constexpr int32_t kGoalSite = -3; // within a structure's placement range of a site
  static constexpr GoalKey anywhere(int32_t kind, uint32_t tag = 0) {
    return {kind, {0, 0}, 1 << 20, tag};
  }

  const std::vector<Vec2>& resource_nodes() const { return nodes_; }

  // The same path as path(from, goal, max_len) would give, as short and under the same
  // blocking rules (ties may resolve differently), but read off a distance field to `goal`
  // shared by every unit given the same key this tick. A search from each unit floods the map
  // per unit, all of it when the goal can't be reached; a field grows out from the goal only
  // as far as its nearest asker's next step needs, and resumes for the next.
  template <class Goal>
  std::vector<Vec2> path(const GoalKey& key, Vec2 from, Goal goal, size_t max_len) {
    std::vector<Vec2> out;
    if (goal(from)) return out;
    const int32_t fc = cell(from);
    int32_t first[4];
    size_t n_first = 0;
    for (int32_t off : step_)
      if (first_step_ok(fc, fc + off)) first[n_first++] = fc + off;
    if (n_first == 0) return out; // boxed in this tick: growing could only flood the map
    auto [it, fresh] = fields_.try_emplace(key);
    Field& f = it->second;
    if (fresh) seed(f, key, goal, open_);
    if (!grow(f, first, n_first, open_)) return out;
    int32_t c = fc;
    int32_t best = kFar;
    for (size_t k = 0; k < n_first; ++k)
      if (f.dist[static_cast<size_t>(first[k])] < best) {
        best = f.dist[static_cast<size_t>(first[k])];
        c = first[k];
      }
    if (best == kFar) return out;
    out.push_back(at(c));
    while (out.size() < max_len && f.dist[static_cast<size_t>(c)] > 0)
      for (int32_t off : step_) {
        const int32_t n = c + off;
        if (open_[static_cast<size_t>(n)] &&
            f.dist[static_cast<size_t>(n)] == f.dist[static_cast<size_t>(c)] - 1) {
          c = n;
          out.push_back(at(n));
          break;
        }
      }
    return out;
  }

  // Like path(key, ...), for a goal tied to the map, not to where any unit stands this tick:
  // "within r of a point", say (a goal that moves with structures needs them in its key). Its
  // field crosses terrain and structures (enemy ones as remembered) alone, so it is kept from
  // tick to tick, growing as askers need it, and started again only when a structure it has
  // reached goes up or comes down. Units count where they bite: the first step must be open
  // and yield as everywhere, and the path stops short of a parked unit. A unit that can't get any
  // nearer that way searches for the nearest tile that is nearer, cut off after kDetourBudget
  // tiles: enough to step round a structure or a knot of parked units, and a detour much
  // longer isn't worth a flood of the map each tick, so it waits instead.
  //
  // `reachable`, if given, is set to whether terrain and structures let `from` reach the goal
  // at all: an empty path alone may only mean units are in the way this tick.
  template <class Goal>
  std::vector<Vec2> route(const GoalKey& key, Vec2 from, Goal goal, size_t max_len,
                          bool* reachable = nullptr) {
    return route(key, from, goal, max_len, reachable, nullptr);
  }

  // The same, for a goal spread over the map: `seeds(emit)` calls emit(p) for every tile that
  // might satisfy it, so that a new field needn't test the whole map.
  template <class Goal, class Seeds>
  std::vector<Vec2> route(const GoalKey& key, Vec2 from, Goal goal, size_t max_len,
                          bool* reachable, Seeds&& seeds) {
    std::vector<Vec2> out;
    if (reachable) *reachable = true;
    if (goal(from)) return out;
    const int32_t fc = cell(from);
    int32_t first[4];
    size_t n_first = 0;
    for (int32_t off : step_)
      if (first_step_ok(fc, fc + off)) first[n_first++] = fc + off;
    auto [it, fresh] = fixed_fields_.try_emplace(key);
    Field& f = it->second;
    f.used = tick_;
    if (fresh) {
      if constexpr (std::is_same_v<std::decay_t<Seeds>, std::nullptr_t>) {
        seed(f, key, goal, settled_);
      } else {
        f.dist.assign(settled_.size(), kFar);
        seeds([&](Vec2 p) {
          if (!map_.in_bounds(p)) return;
          const int32_t i = cell(p);
          if (!settled_[static_cast<size_t>(i)] || f.dist[static_cast<size_t>(i)] == 0 || !goal(p))
            return;
          f.dist[static_cast<size_t>(i)] = 0;
          f.queue.push_back(i);
        });
      }
    }
    if (reachable) {
      int32_t around[4];
      size_t n_around = 0;
      for (int32_t off : step_)
        if (settled_[static_cast<size_t>(fc + off)]) around[n_around++] = fc + off;
      *reachable = !grow(f, around, n_around, settled_); // not known yet: assume so
      for (size_t k = 0; k < n_around; ++k)
        *reachable = *reachable || f.dist[static_cast<size_t>(around[k])] != kFar;
    }
    if (n_first == 0 || !grow(f, first, n_first, settled_)) return out;
    int32_t best = kFar;
    for (size_t k = 0; k < n_first; ++k)
      best = std::min(best, f.dist[static_cast<size_t>(first[k])]);
    if (best == kFar) return out;
    // Grown until every tile nearer than `best` has its distance: unset means further. The
    // field can't see what stands in the way this tick, so a step that looks nearer may lead
    // into a dead end; one that does is passed over, or a unit could step in and back forever.
    const int32_t here = f.dist[static_cast<size_t>(fc)];
    int32_t c = fc;
    for (size_t k = 0; k < n_first && c == fc && best < here; ++k)
      if (f.dist[static_cast<size_t>(first[k])] == best && clear_ahead(f, first[k])) c = first[k];
    if (c == fc) {
      auto nearer = [&](Vec2 q) {
        const int32_t i = cell(q);
        return f.dist[static_cast<size_t>(i)] < here && clear_ahead(f, i);
      };
      for (int32_t d = search(fc, nearer, kDetourBudget); d != fc;
           d = parent_[static_cast<size_t>(d)])
        out.push_back(at(d));
      std::reverse(out.begin(), out.end());
      if (out.size() > max_len) out.resize(max_len);
      return out;
    }
    out.push_back(at(c));
    while (out.size() < max_len && f.dist[static_cast<size_t>(c)] > 0) {
      int32_t next = c;
      for (int32_t off : step_) {
        const auto i = static_cast<size_t>(c + off);
        if (open_[i] && f.dist[i] == f.dist[static_cast<size_t>(c)] - 1) {
          next = c + off;
          break;
        }
      }
      if (next == c) break;
      c = next;
      out.push_back(at(c));
    }
    return out;
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
    const int32_t fc = cell(from);
    for (int32_t c = search(fc, goal); c != fc; c = parent_[static_cast<size_t>(c)])
      out.push_back(at(c));
    std::reverse(out.begin(), out.end());
    if (out.size() > max_len) out.resize(max_len);
    return out;
  }

private:
  // A path may cross an open tile: passable, no structure (own, or enemy as remembered), no
  // parked own unit. The first step also yields to a moving own unit with a lower id.
  bool first_step_ok(int32_t from, int32_t n) const {
    const auto i = static_cast<size_t>(n);
    if (!open_[i]) return false;
    const auto f = static_cast<size_t>(from);
    const uint32_t self = own_unit_[f] ? own_id_[f] : 0;
    return !(own_unit_[i] == kMoving && own_id_[i] < self);
  }

  // The goal tile a breadth-first search reaches first, or `from` if none (parent_ links it).
  template <class Goal>
  int32_t search(int32_t from, Goal goal, size_t budget = SIZE_MAX) {
    if (goal(at(from))) return from;
    ++generation_;
    seen_[static_cast<size_t>(from)] = generation_;
    queue_.assign(1, from);
    for (size_t h = 0; h < queue_.size() && h < budget; ++h) {
      const int32_t c = queue_[h];
      for (int32_t off : step_) {
        const int32_t n = c + off;
        const auto i = static_cast<size_t>(n);
        if (!open_[i] || seen_[i] == generation_) continue;
        if (c == from && !first_step_ok(from, n)) continue;
        seen_[i] = generation_;
        parent_[i] = c;
        if (goal(at(n))) return n;
        queue_.push_back(n);
      }
    }
    return from;
  }

  // A breadth-first flood out from the goal tiles, paused once an asker has what it needs and
  // resumed for the next.
  struct Field {
    std::vector<int32_t> dist; // steps to the nearest goal tile; kFar: not reached (yet)
    std::vector<int32_t> queue;
    size_t head{0};
    uint32_t used{0}; // the last tick it was asked (kept fields only)
  };

  // Whether `f` has reached any of `cells` or a neighbour (so would read differently with it
  // opened or closed).
  bool touches(const Field& f, const std::vector<int32_t>& cells) const {
    for (int32_t c : cells) {
      if (f.dist[static_cast<size_t>(c)] != kFar) return true;
      for (int32_t off : step_)
        if (f.dist[static_cast<size_t>(c + off)] != kFar) return true;
    }
    return false;
  }

  // Whether the way down `f` from `c` stays open (no parked unit) for
  // kLookahead steps or to the goal.
  bool clear_ahead(const Field& f, int32_t c) const {
    for (int32_t k = 0; k < kLookahead && f.dist[static_cast<size_t>(c)] > 0; ++k) {
      int32_t next = c;
      for (int32_t off : step_) {
        const auto i = static_cast<size_t>(c + off);
        if (open_[i] && f.dist[i] == f.dist[static_cast<size_t>(c)] - 1) {
          next = c + off;
          break;
        }
      }
      if (next == c) return false;
      c = next;
    }
    return true;
  }

  template <class Goal>
  void seed(Field& f, const GoalKey& key, Goal goal, const std::vector<uint8_t>& open) {
    f.dist.assign(open.size(), kFar);
    const int32_t r = key.radius;
    for (int32_t y = std::max(0, key.at.y - r); y <= std::min(map_.height - 1, key.at.y + r); ++y)
      for (int32_t x = std::max(0, key.at.x - r); x <= std::min(map_.width - 1, key.at.x + r);
           ++x)
        if (open[static_cast<size_t>(cell({x, y}))] && goal(Vec2{x, y})) {
          f.dist[static_cast<size_t>(cell({x, y}))] = 0;
          f.queue.push_back(cell({x, y}));
        }
  }

  // Grows `f` over `open` until the nearest of the `first` tiles has its distance and every
  // tile as near has too. A flood reaches tiles in order of distance, so that is once the next
  // tile to expand is no nearer: the path beyond the first step only descends. Waiting for all
  // of them instead would flood the whole map whenever one can't reach the goal.
  //
  // False if this tick's kFloodBudget ran out first: the asker must wait, and a kept field
  // carries on next tick. A far new goal is spread over a few ticks rather than costing one
  // tick a flood of the map per field, which a fuel budget per tick can't afford.
  bool grow(Field& f, const int32_t* first, size_t n_first, const std::vector<uint8_t>& open) {
    for (;;) {
      int32_t best = kFar;
      for (size_t k = 0; k < n_first; ++k)
        best = std::min(best, f.dist[static_cast<size_t>(first[k])]);
      if (f.head == f.queue.size() ||
          (best != kFar && f.dist[static_cast<size_t>(f.queue[f.head])] >= best))
        return true;
      if (flood_left_ == 0) return false;
      --flood_left_;
      const int32_t c = f.queue[f.head++];
      const int32_t d = f.dist[static_cast<size_t>(c)] + 1;
      for (int32_t off : step_) {
        const auto i = static_cast<size_t>(c + off);
        if (!open[i] || f.dist[i] != kFar) continue;
        f.dist[i] = d;
        f.queue.push_back(c + off);
      }
    }
  }

  int32_t cell(Vec2 p) const { return (p.y + 1) * stride_ + p.x + 1; }
  Vec2 at(int32_t c) const { return {c % stride_ - 1, c / stride_ - 1}; }

  static constexpr int32_t kFar = INT32_MAX;
  static constexpr size_t kDetourBudget = 64; // tiles a blocked unit's detour search may visit
  static constexpr int32_t kLookahead = 4;    // steps clear_ahead checks
  static constexpr size_t kFloodBudget = 4000; // tiles all fields together may grow by a tick
  static constexpr uint8_t kMoving = 1;
  static constexpr uint8_t kParked = 2;

  Map map_;
  int32_t stride_{0};
  int32_t step_[4]{}; // DIRS as cell offsets
  std::map<uint32_t, Vec2> last_pos_; // own unit positions last tick
  std::vector<uint8_t> passable_;      // terrain alone, per cell; the border is closed
  std::vector<Vec2> nodes_;            // the map's resource nodes
  std::vector<uint8_t> settled_;       // passable_ less structures: route()'s fields' tiles
  std::set<int32_t> structure_cells_;  // own and remembered enemy ones: what settled_ lacks
  std::vector<uint8_t> open_;          // this tick's open tiles (first_step_ok)
  std::vector<uint8_t> enemy_structure_, own_unit_;
  std::set<int32_t> enemy_cells_;      // where enemy_structure_ is set
  std::vector<uint32_t> own_id_; // id of the own unit on each cell marked in own_unit_
  std::vector<int32_t> marked_;  // the cells own_unit_ marks this tick
  std::vector<uint32_t> seen_;   // == generation_ when visited by the current search
  std::vector<int32_t> parent_;  // the cell each cell was reached from by the current search
  std::vector<int32_t> queue_;
  uint32_t generation_{0};
  std::map<GoalKey, Field> fields_;       // this tick's, by goal (path(key, ...))
  std::map<GoalKey, Field> fixed_fields_; // kept across ticks, by goal (route())
  uint32_t tick_{0};
  size_t flood_left_{0}; // of this tick's kFloodBudget
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

// Whether a resource node lies within `r` of `p` (Manhattan).
inline bool resource_within(const Map& map, Vec2 p, int32_t r) {
  for (int32_t y = p.y - r; y <= p.y + r; ++y)
    for (int32_t x = p.x - r; x <= p.x + r; ++x)
      if (map.in_bounds({x, y}) && manhattan(p, {x, y}) <= r &&
          map.tile_at({x, y}).terrain == Terrain::ResourceNode)
        return true;
  return false;
}

// Where ships wait: a passable tile `dist` from home, toward `enemy`, preferring one with no
// resource node within `clear` so waiting ships never stand where a Drone harvests.
inline Vec2 rally_point(const Map& map, Vec2 home, Vec2 enemy, int32_t dist, int32_t clear) {
  Vec2 best_p{home};
  int32_t best = INT32_MAX;
  for (int32_t y = 0; y < map.height; ++y)
    for (int32_t x = 0; x < map.width; ++x) {
      const Vec2 p{x, y};
      if (!map.passable(p) || manhattan(p, home) != dist) continue;
      const int32_t score = (resource_within(map, p, clear) ? 1'000'000 : 0) + manhattan(p, enemy);
      if (score < best) {
        best = score;
        best_p = p;
      }
    }
  return best_p;
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
// The drop-offs as a GoalKey: they come and go with claims, and a kept field must know which.
inline Router::GoalKey drop_off_key(const Snapshot& snap) {
  uint32_t sig = 0;
  for (const auto& s : snap.my_structures) {
    const auto type = static_cast<StructureType>(s.type);
    if (type != StructureType::CommandCore && type != StructureType::ClaimNode) continue;
    sig = sig * 1000003u + static_cast<uint32_t>(s.y * 4096 + s.x + 1);
  }
  return Router::anywhere(Router::kGoalDropOff, sig);
}

inline Vec2 harvest_step(Router& router, const Map& map, const Snapshot& snap, const UnitView& d) {
  const Vec2 pos{d.x, d.y};
  const bool full =
      d.cargo_energy >= cfg::DRONE_CARGO_ENERGY || d.cargo_alloy >= cfg::DRONE_CARGO_ALLOY;
  auto around = [](Vec2 c, auto& emit) {
    emit(c);
    for (const Vec2& dir : DIRS) emit(c + dir);
  };
  static_assert(cfg::DEPOSIT_RANGE == 1, "drop-off seeds assume DEPOSIT_RANGE 1");
  const auto p =
      full ? router.route(drop_off_key(snap), pos, [&](Vec2 q) { return at_drop_off(snap, q); },
                          1, nullptr,
                          [&](auto&& emit) {
                            for (const auto& s : snap.my_structures) around({s.x, s.y}, emit);
                          })
           : router.route(Router::anywhere(Router::kGoalResource), pos,
                          [&](Vec2 q) { return near_resource(map, q); }, 1, nullptr,
                          [&](auto&& emit) {
                            for (Vec2 n : router.resource_nodes()) around(n, emit);
                          });
  return p.empty() ? pos : p.front();
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
inline Vec2 site_step(Router& router, Vec2 from, Vec2 site, int32_t range,
                      bool* reachable = nullptr) {
  const auto p = router.route({Router::kGoalSite, site, range}, from,
                              [&](Vec2 q) { return manhattan(q, site) <= range; }, 1, reachable);
  return p.empty() ? from : p.front();
}

inline Vec2 builder_step(Router& router, const UnitView& d, const BuildSite& site) {
  return site_step(router, {d.x, d.y}, site.pos, site.range);
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
inline Vec2 waypoint(Vec2 from, const std::vector<Vec2>& p) {
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

template <class Goal> Vec2 waypoint(Router& router, Vec2 from, Goal goal, int32_t speed) {
  return waypoint(from, router.path(from, goal, static_cast<size_t>(speed)));
}

template <class Goal>
Vec2 waypoint(Router& router, const Router::GoalKey& key, Vec2 from, Goal goal, int32_t speed) {
  return waypoint(from, router.path(key, from, goal, static_cast<size_t>(speed)));
}

// As above, for a goal that depends on the map alone (Router::route).
template <class Goal>
Vec2 route_waypoint(Router& router, const Router::GoalKey& key, Vec2 from, Goal goal,
                    int32_t speed) {
  return waypoint(from, router.route(key, from, goal, static_cast<size_t>(speed)));
}

inline Command move_cmd(uint32_t unit_id, CommandKind kind, Vec2 to) {
  Command cmd{};
  cmd.entity_id = unit_id;
  cmd.kind = static_cast<uint16_t>(kind);
  cmd.ax = to.x;
  cmd.ay = to.y;
  return cmd;
}

inline std::array<int32_t, 2> unit_cost(UnitType t) {
  switch (t) {
  case UnitType::Interceptor: return {cfg::INTERCEPTOR_COST_E, cfg::INTERCEPTOR_COST_A};
  case UnitType::Frigate: return {cfg::FRIGATE_COST_E, cfg::FRIGATE_COST_A};
  case UnitType::Artillery: return {cfg::ARTILLERY_COST_E, cfg::ARTILLERY_COST_A};
  case UnitType::Battleship: return {cfg::BATTLESHIP_COST_E, cfg::BATTLESHIP_COST_A};
  default: return {cfg::DRONE_COST_E, cfg::DRONE_COST_A};
  }
}

// Total cost: how much a ship is worth when weighing armies.
inline int32_t unit_value(UnitType t) {
  if (t == UnitType::Drone) return 0;
  const auto [e, a] = unit_cost(t);
  return e + a;
}

// The enemy's spawn on a point-symmetric map: every shipped map is one.
inline Vec2 mirrored(const Map& map, Vec2 p) { return {map.width - 1 - p.x, map.height - 1 - p.y}; }

inline Command produce_cmd(uint32_t producer, UnitType t) {
  Command c = move_cmd(producer, CommandKind::Produce, {0, 0});
  c.aux = static_cast<uint16_t>(t);
  return c;
}

// A free open tile 2–4 from the Core, clear of resource nodes and with all four sides
// passable, so the Factory blocks no harvest and always has room to launch ships. Nearest the
// Core first, then nearest the enemy. {-1, -1} if there is none.
inline Vec2 factory_site(const Map& map, Vec2 home, Vec2 enemy, const std::set<Vec2>& taken) {
  Vec2 best{-1, -1};
  int32_t best_d = INT32_MAX;
  for (int32_t y = home.y - 4; y <= home.y + 4; ++y)
    for (int32_t x = home.x - 4; x <= home.x + 4; ++x) {
      const Vec2 p{x, y};
      const int32_t d = manhattan(p, home);
      if (d < 2 || d > 4 || !map.passable(p) || taken.contains(p)) continue;
      if (map.tile_at(p).terrain != Terrain::Open || near_resource(map, p)) continue;
      bool open = true;
      for (const Vec2& dir : DIRS) open = open && map.passable(p + dir);
      if (!open) continue;
      const int32_t score = d * 1000 + manhattan(p, enemy);
      if (score < best_d) {
        best_d = score;
        best = p;
      }
    }
  return best;
}

// Focus fire. Each shooter (ascending id) takes the in-range enemy with the least hp + shield
// left after the shots already assigned, Drones last, so damage lands on one target at a time
// and isn't wasted on a ship already dead. With no unit in range, an enemy structure:
// `prefer` first, then unfinished ones, then the weakest. Returns shooter id → target id; the
// enemies it expects to die go in `doomed`.
inline std::map<uint32_t, uint32_t> focus_fire(const Snapshot& snap,
                                               std::vector<const UnitView*> shooters,
                                               uint32_t prefer = 0,
                                               std::set<uint32_t>* doomed = nullptr) {
  std::map<uint32_t, uint32_t> focus;
  std::map<uint32_t, int32_t> left;
  for (const auto& e : snap.visible_enemies) left[e.id] = e.hp + e.shield;
  std::sort(shooters.begin(), shooters.end(),
            [](const UnitView* a, const UnitView* b) { return a->id < b->id; });
  auto unit_key = [&](const EnemyView& e) {
    return std::tuple{left[e.id] <= 0, static_cast<UnitType>(e.type) == UnitType::Drone,
                      left[e.id], e.id};
  };
  auto structure_key = [&](const EnemyStructureView& s) {
    return std::tuple{s.id != prefer, s.building == 0, s.hp, s.id};
  };
  for (const UnitView* u : shooters) {
    const UnitStats& st = stats_for(static_cast<UnitType>(u->type));
    const EnemyView* best = nullptr;
    for (const auto& e : snap.visible_enemies) {
      if (manhattan({u->x, u->y}, {e.x, e.y}) > st.range) continue;
      if (!best || unit_key(e) < unit_key(*best)) best = &e;
    }
    if (best) {
      focus[u->id] = best->id;
      left[best->id] -= st.dmg;
      if (doomed && left[best->id] <= 0) doomed->insert(best->id);
      continue;
    }
    const EnemyStructureView* sbest = nullptr;
    for (const auto& s : snap.visible_enemy_structures) {
      if (manhattan({u->x, u->y}, {s.x, s.y}) > st.range) continue;
      if (!sbest || structure_key(s) < structure_key(*sbest)) sbest = &s;
    }
    if (sbest) focus[u->id] = sbest->id;
  }
  return focus;
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
      bool reachable = true;
      const Vec2 step = site_step(router, pos, site, cfg::CLAIM_DEPLOY_RANGE, &reachable);
      if (!reachable) {
        assigned_.erase(it);
        continue;
      }
      if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
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
