#include "movement.hpp"
#include "combat.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "config.hpp"
#include "grid.hpp"
#include <map>
#include <algorithm>
#include <cstdlib>

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
    Unit* u = world.find_unit(uid);
    if (!u || !u->alive())
      continue;
    movers.push_back({uid, {cmd.ax, cmd.ay}, kind == CommandKind::MoveAttack, u->stats().speed});
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

    // Build current tile → unit-id map to detect conflicts.
    std::map<Vec2, UnitId> occupied;
    for (const auto& [uid, unit] : world.units)
      if (unit.alive())
        occupied.emplace(unit.pos, uid);

    // Resolve proposals: contested tile → highest collision priority wins,
    // ties → lower unit-id. Swap → both blocked.
    std::map<Vec2, UnitId> winners; // proposed tile → winning unit
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

    // Block swaps: if A→B and B→A both won their tiles, block both.
    for (auto& [tile, winner_id] : winners) {
      auto it = proposals.find(winner_id);
      if (it == proposals.end())
        continue;
      // Check if the unit currently on the target tile is moving to winner's tile
      auto occ_it = occupied.find(tile);
      if (occ_it == occupied.end())
        continue;
      UnitId blocker = occ_it->second;
      auto blocker_prop = proposals.find(blocker);
      if (blocker_prop == proposals.end())
        continue;
      Unit* winner_unit = world.find_unit(winner_id);
      if (winner_unit && blocker_prop->second == winner_unit->pos) {
        // Swap detected — mark by clearing both from winners map later.
        // We nullify the winner for this tile to signal a blocked swap.
        winner_id = UnitId{}; // null signals block
      }
    }

    // Apply valid moves.
    for (const auto& [tile, winner_id] : winners) {
      if (winner_id.is_null())
        continue;
      // Reject if target occupied by a unit that didn't move away
      auto occ_it = occupied.find(tile);
      if (occ_it != occupied.end() && occ_it->second != winner_id) {
        // The occupant isn't the winner — blocked
        continue;
      }
      Unit* u = world.find_unit(winner_id);
      if (!u)
        continue;
      u->pos = tile;

      // Decrement steps for this mover
      for (auto& mv : movers) {
        if (mv.id == winner_id) {
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
