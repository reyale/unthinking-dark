#pragma once
#include "entity.hpp"
#include "grid.hpp"
#include "ids.hpp"
#include "world.hpp"
#include <climits>
#include <cstdint>
#include <vector>

namespace game {

// Who stands on each tile, as flat per-tile grids, so a range query visits only the tiles
// in range instead of every entity. A snapshot of one moment: rebuild it where positions are
// fixed (each movement step, combat) and again after anything moves or is purged, since it
// points into World's maps. Entities that die after a rebuild are skipped: every lookup checks
// hp.
//
// Queries give exactly what scanning every entity in id order would: nearest first, ties to
// the lower id. Real play never puts two units (or two structures) on one tile, or anything
// off the map, but a hand-built world can; then the queries fall back to that scan.
class Occupancy {
public:
  Occupancy() = default;
  explicit Occupancy(const World& world) { rebuild(world); }

  // Clears only the tiles the last rebuild filled, so reusing one Occupancy across ticks
  // costs per entity, not per tile. Every non-null slot is one of those, so this is exact
  // whatever world the last rebuild saw.
  void rebuild(const World& world);

  UnitId unit_at(Vec2 p) const { // the lowest-id living unit there, or null
    const Unit* u = units_[idx(p)];
    return u ? u->id : UnitId{};
  }

  // Nearest unit/structure within `range` of `c` (Manhattan) that `ok` accepts.
  template <class Ok> UnitId nearest_unit(Vec2 c, int32_t range, Ok&& ok) const {
    if (stacked_) return scan_nearest(world_->units, c, range, ok);
    for (int32_t d = 0; d <= range; ++d) {
      const Unit* best = nullptr;
      ring(c, d, [&](size_t i) {
        const Unit* u = units_[i];
        if (u && u->alive() && (!best || u->id < best->id) && ok(*u)) best = u;
      });
      if (best) return best->id;
    }
    return {};
  }

  template <class Ok> StructureId nearest_structure(Vec2 c, int32_t range, Ok&& ok) const {
    if (stacked_) return scan_nearest(world_->structures, c, range, ok);
    for (int32_t d = 0; d <= range; ++d) {
      const Structure* best = nullptr;
      ring(c, d, [&](size_t i) {
        const Structure* s = structures_[i];
        if (s && s->alive() && (!best || s->id < best->id) && ok(*s)) best = s;
      });
      if (best) return best->id;
    }
    return {};
  }

  // Both of the above in one walk over the tiles in range: the structure is only wanted when no
  // unit qualifies, and finding it costs nothing extra on the way out to the full range.
  struct Nearest {
    UnitId unit;
    StructureId structure;
  };
  template <class Ok> Nearest nearest_target(Vec2 c, int32_t range, Ok&& ok) const {
    if (stacked_)
      return {scan_nearest(world_->units, c, range, ok),
              scan_nearest(world_->structures, c, range, ok)};
    const Structure* first_s = nullptr; // nearest ring holding one, lowest id in it
    for (int32_t d = 0; d <= range; ++d) {
      const Unit* best = nullptr;
      const Structure* ring_s = nullptr;
      ring(c, d, [&](size_t i) {
        const Unit* u = units_[i];
        if (u && u->alive() && (!best || u->id < best->id) && ok(*u)) best = u;
        const Structure* s = structures_[i];
        if (!first_s && s && s->alive() && (!ring_s || s->id < ring_s->id) && ok(*s))
          ring_s = s;
      });
      if (best) return {best->id, {}};
      if (!first_s) first_s = ring_s;
    }
    return {{}, first_s ? first_s->id : StructureId{}};
  }

  // Whether any unit or structure within `range` of `c` is accepted by `ok`.
  template <class Ok> bool any_within(Vec2 c, int32_t range, Ok&& ok) const {
    if (stacked_)
      return !scan_nearest(world_->units, c, range, ok).is_null() ||
             !scan_nearest(world_->structures, c, range, ok).is_null();
    bool found = false;
    for (int32_t d = 0; d <= range && !found; ++d)
      ring(c, d, [&](size_t i) {
        if (found) return;
        const Unit* u = units_[i];
        const Structure* s = structures_[i];
        found = (u && u->alive() && ok(*u)) || (s && s->alive() && ok(*s));
      });
    return found;
  }

  // Calls f(id) for every living unit/structure within `range` of `c`, in no fixed order:
  // callers must only accumulate.
  template <class F> void each_unit_within(Vec2 c, int32_t range, F&& f) const {
    if (stacked_) {
      for (const auto& [id, u] : world_->units)
        if (u.alive() && manhattan(c, u.pos) <= range) f(id);
      return;
    }
    for (int32_t d = 0; d <= range; ++d)
      ring(c, d, [&](size_t i) {
        if (const Unit* u = units_[i]; u && u->alive()) f(u->id);
      });
  }

  template <class F> void each_structure_within(Vec2 c, int32_t range, F&& f) const {
    if (stacked_) {
      for (const auto& [id, s] : world_->structures)
        if (s.alive() && manhattan(c, s.pos) <= range) f(id);
      return;
    }
    for (int32_t d = 0; d <= range; ++d)
      ring(c, d, [&](size_t i) {
        if (const Structure* s = structures_[i]; s && s->alive()) f(s->id);
      });
  }

private:
  template <class Entities, class Ok>
  static auto scan_nearest(const Entities& all, Vec2 c, int32_t range, Ok& ok) {
    typename Entities::key_type best{};
    int32_t best_d = INT32_MAX;
    for (const auto& [id, e] : all) {
      const int32_t d = manhattan(c, e.pos);
      if (e.alive() && d <= range && d < best_d && ok(e)) {
        best_d = d;
        best = id;
      }
    }
    return best;
  }

  // The in-bounds tiles at exactly Manhattan distance d from c, by grid index.
  template <class F> void ring(Vec2 c, int32_t d, F&& f) const {
    const Map& map = world_->map;
    if (d == 0) {
      if (map.in_bounds(c)) f(idx(c));
      return;
    }
    for (int32_t dx = -d; dx <= d; ++dx) {
      const int32_t dy = d - (dx < 0 ? -dx : dx);
      const Vec2 a{c.x + dx, c.y + dy};
      if (map.in_bounds(a)) f(idx(a));
      if (dy != 0) {
        const Vec2 b{c.x + dx, c.y - dy};
        if (map.in_bounds(b)) f(idx(b));
      }
    }
  }

  size_t idx(Vec2 p) const {
    return static_cast<size_t>(p.y) * static_cast<size_t>(width_) + static_cast<size_t>(p.x);
  }

  const World* world_{nullptr};
  int32_t width_{0};
  std::vector<const Unit*> units_; // per tile; null = none
  std::vector<const Structure*> structures_;
  std::vector<size_t> filled_; // the tiles the last rebuild set
  bool stacked_{false};
};

} // namespace game
