#include "movement.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "config.hpp"
#include "grid.hpp"
#include "occupancy.hpp"
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace game {

// Simple A→B path: one step toward target along the shorter axis first
// (no pathfinding in Phase 1 — bot is responsible for routing).
// Returns the next tile along the straight-line walk, or current pos if
// already at target.
static Vec2 step_toward(Vec2 from, Vec2 to) {
  if (from == to)
    return from;
  int32_t dx = to.x - from.x;
  int32_t dy = to.y - from.y;
  // Move along whichever axis has greater delta; ties favour x.
  if (std::abs(dx) >= std::abs(dy))
    return {from.x + (dx > 0 ? 1 : -1), from.y};
  return {from.x, from.y + (dy > 0 ? 1 : -1)};
}

namespace {

// A per-tile slot grid (-1 = empty) reset entry by entry, so reusing one across steps and
// ticks costs per entry written rather than per tile.
struct TileSlots {
  std::vector<int32_t> slot;
  std::vector<size_t> written;

  void reset(size_t tiles) {
    if (slot.size() != tiles) {
      slot.assign(tiles, -1);
    } else {
      for (size_t i : written)
        slot[i] = -1;
    }
    written.clear();
  }
  int32_t get(size_t i) const { return slot[i]; }
  void put(size_t i, int32_t v) {
    if (slot[i] < 0)
      written.push_back(i);
    slot[i] = v;
  }
};

} // namespace

void run_movement(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b) {
  struct Mover {
    Unit* unit;
    Vec2 target;
    int32_t steps_left;
    Vec2 next{};
  };

  std::vector<Mover> movers;
  for (const ValidatedCommands* cmds : {&cmds_a, &cmds_b}) {
    for (const auto& [uid, cmd] : *cmds) {
      if (static_cast<CommandKind>(cmd.kind) != CommandKind::Move)
        continue;
      Unit* u = world.find_unit(uid.unit());
      if (!u || !u->alive())
        continue;
      movers.push_back({u, {cmd.ax, cmd.ay}, u->stats().speed});
    }
  }
  std::sort(movers.begin(), movers.end(),
            [](const Mover& a, const Mover& b) { return a.unit->id < b.unit->id; });

  // Scratch reused across calls: only its allocation persists, every step refills it.
  thread_local Occupancy occ;
  thread_local TileSlots claim;  // tile → the mover winning it this step
  thread_local TileSlots winner; // tile → the winning mover standing there now
  const size_t tiles =
      static_cast<size_t>(world.map.width) * static_cast<size_t>(world.map.height);
  auto idx = [&](Vec2 p) {
    return static_cast<size_t>(p.y) * static_cast<size_t>(world.map.width) +
           static_cast<size_t>(p.x);
  };

  // Stepped simultaneous movement.
  const int32_t max_speed = cfg::INTERCEPTOR_SPEED; // largest speed in roster
  for (int32_t step = 0; step < max_speed && !movers.empty(); ++step) {
    occ.rebuild(world);
    claim.reset(tiles);
    winner.reset(tiles);

    // Proposals, in ascending unit id. A contested tile goes to the highest collision
    // priority, ties to the lower id.
    std::vector<int32_t> proposing;
    for (size_t i = 0; i < movers.size(); ++i) {
      Mover& mv = movers[i];
      if (mv.steps_left <= 0)
        continue;
      const Unit& u = *mv.unit;
      if (!u.alive())
        continue;

      const Vec2 next = step_toward(u.pos, mv.target);
      if (next == u.pos || !world.map.passable(next) || world.structure_by_pos.count(next)) {
        mv.steps_left = 0; // at target, or the way is shut
        continue;
      }
      mv.next = next;
      proposing.push_back(static_cast<int32_t>(i));
      const int32_t held = claim.get(idx(next));
      if (held < 0 ||
          u.stats().collision_priority >
              movers[static_cast<size_t>(held)].unit->stats().collision_priority)
        claim.put(idx(next), static_cast<int32_t>(i));
    }

    std::vector<int32_t> won;
    for (int32_t i : proposing) {
      const Mover& mv = movers[static_cast<size_t>(i)];
      if (claim.get(idx(mv.next)) != i)
        continue;
      won.push_back(i);
      if (winner.get(idx(mv.unit->pos)) < 0) // stacked units: the lowest id, as occ has
        winner.put(idx(mv.unit->pos), i);
    }

    // A winner moves if its tile is empty or the occupant moves out this step, so a column
    // advances together. A closed loop of movers (a swap, or a ring) is blocked whole, along
    // with every unit queued behind it: exchanging tiles is never allowed.
    enum class Fate : uint8_t { Unknown, Visiting, Moves, Stays };
    std::vector<Fate> fate(movers.size(), Fate::Unknown);
    std::vector<int32_t> chain;
    for (int32_t start : won) {
      chain.clear();
      Fate result = Fate::Stays;
      for (int32_t cur = start;;) {
        const Fate f = fate[static_cast<size_t>(cur)];
        if (f != Fate::Unknown) {
          result = f == Fate::Visiting ? Fate::Stays : f;
          break;
        }
        fate[static_cast<size_t>(cur)] = Fate::Visiting;
        chain.push_back(cur);
        const Vec2 to = movers[static_cast<size_t>(cur)].next;
        const UnitId occupant = occ.unit_at(to);
        if (occupant.is_null()) {
          result = Fate::Moves;
          break;
        }
        const int32_t next = winner.get(idx(to));
        if (next < 0 || movers[static_cast<size_t>(next)].unit->id != occupant)
          break; // the occupant isn't moving
        cur = next;
      }
      for (int32_t i : chain)
        fate[static_cast<size_t>(i)] = result;
    }

    for (int32_t i : won) {
      if (fate[static_cast<size_t>(i)] != Fate::Moves)
        continue;
      Mover& mv = movers[static_cast<size_t>(i)];
      mv.unit->pos = mv.next;
      --mv.steps_left;
    }

    // Remove movers that are done
    movers.erase(std::remove_if(movers.begin(), movers.end(),
                                [](const Mover& m) { return m.steps_left <= 0; }),
                 movers.end());
  }
}

} // namespace game
