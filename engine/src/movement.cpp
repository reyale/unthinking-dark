#include "movement.hpp"
#include "combat.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "config.hpp"
#include "grid.hpp"
#include <map>
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

void run_movement(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b) {
  // Merge both factions' commands into one lookup.
  // std::map ordering is by UnitId so this is deterministic.
  ValidatedCommands all_cmds;
  for (const auto& [k, v] : cmds_a)
    all_cmds.emplace(k, v);
  for (const auto& [k, v] : cmds_b)
    all_cmds.emplace(k, v);

  // Track which units are still moving and their targets this phase.
  struct Mover {
    UnitId id;
    Vec2 target;
    bool move_attack;
    int32_t steps_left;
  };

  std::vector<Mover> movers;
  for (const auto& [uid, cmd] : all_cmds) {
    const auto kind = static_cast<CommandKind>(cmd.kind);
    if (kind != CommandKind::Move && kind != CommandKind::MoveAttack)
      continue;
    Unit* u = world.find_unit(uid.unit());
    if (!u || !u->alive())
      continue;
    movers.push_back({uid.unit(), {cmd.ax, cmd.ay}, kind == CommandKind::MoveAttack, u->stats().speed});
  }

  // Stepped simultaneous movement.
  const int32_t max_speed = cfg::INTERCEPTOR_SPEED; // largest speed in roster
  for (int32_t step = 0; step < max_speed && !movers.empty(); ++step) {
    // Proposed moves: unit_id → proposed tile
    std::map<UnitId, Vec2> proposals;

    for (auto& mv : movers) {
      if (mv.steps_left <= 0)
        continue;
      Unit* u = world.find_unit(mv.id);
      if (!u || !u->alive())
        continue;

      // MoveAttack: halt if enemy now in range
      if (mv.move_attack && target_in_range(world, *u)) {
        mv.steps_left = 0;
        continue;
      }

      Vec2 next = step_toward(u->pos, mv.target);
      if (next == u->pos) {
        mv.steps_left = 0;
        continue;
      } // at target
      if (!world.map.passable(next)) {
        mv.steps_left = 0;
        continue;
      }
      if (world.structure_by_pos.count(next)) {
        mv.steps_left = 0;
        continue;
      }
      proposals.emplace(mv.id, next);
    }

    std::map<Vec2, UnitId> occupied;
    for (const auto& [uid, unit] : world.units)
      if (unit.alive())
        occupied.emplace(unit.pos, uid);

    // Contested tile → highest collision priority wins, ties → lower unit-id.
    std::map<Vec2, UnitId> winners;
    for (const auto& [uid, tile] : proposals) {
      auto it = winners.find(tile);
      if (it == winners.end()) {
        winners.emplace(tile, uid);
      } else {
        const Unit* a = world.find_unit(uid);
        const Unit* b = world.find_unit(it->second);
        int32_t pa = a ? a->stats().collision_priority : 0;
        int32_t pb = b ? b->stats().collision_priority : 0;
        if (pa > pb || (pa == pb && uid < it->second))
          it->second = uid;
      }
    }
    std::map<UnitId, Vec2> won;
    for (const auto& [tile, uid] : winners)
      won.emplace(uid, tile);

    // A winner moves if its tile is empty or the occupant moves out this step, so a column
    // advances together. A closed loop of movers (a swap, or a ring) is blocked whole, along
    // with every unit queued behind it: exchanging tiles is never allowed.
    enum class Fate : uint8_t { Visiting, Moves, Stays };
    std::map<UnitId, Fate> fate;
    for (const auto& [start, tile] : won) {
      std::vector<UnitId> chain;
      Fate result = Fate::Stays;
      for (UnitId cur = start;;) {
        if (auto f = fate.find(cur); f != fate.end()) {
          result = f->second == Fate::Visiting ? Fate::Stays : f->second;
          break;
        }
        fate[cur] = Fate::Visiting;
        chain.push_back(cur);
        auto occ = occupied.find(won.at(cur));
        if (occ == occupied.end()) {
          result = Fate::Moves;
          break;
        }
        if (!won.contains(occ->second))
          break;
        cur = occ->second;
      }
      for (UnitId id : chain)
        fate[id] = result;
    }

    for (const auto& [uid, tile] : won) {
      if (fate.at(uid) != Fate::Moves)
        continue;
      world.find_unit(uid)->pos = tile;
      for (auto& mv : movers) {
        if (mv.id == uid) {
          --mv.steps_left;
          break;
        }
      }
    }

    // Remove movers that are done
    movers.erase(std::remove_if(movers.begin(), movers.end(),
                                [](const Mover& m) { return m.steps_left <= 0; }),
                 movers.end());
  }
}

} // namespace game
