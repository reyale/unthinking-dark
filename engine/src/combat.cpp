#include "combat.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "occupancy.hpp"
#include "vision.hpp"
#include "config.hpp"
#include <algorithm>
#include <map>
#include <limits>

namespace game {

// A combat target is either an enemy unit or an enemy structure.
struct CombatTarget {
  UnitId      unit_id{};
  StructureId struct_id{};
  bool is_null() const { return unit_id.is_null() && struct_id.is_null(); }
};

bool target_in_range(const World&, const Unit& unit, const NebulaSight& sight,
                     const Occupancy& occ) {
  const int32_t range = unit.stats().range;
  if (range == 0)
    return false;
  return occ.any_within(unit.pos, range, [&](const auto& e) {
    return e.faction != unit.faction && sight.sees(unit.faction, e.pos);
  });
}

bool target_in_range(const World& world, const Unit& unit) {
  return target_in_range(world, unit, NebulaSight(world), Occupancy(world));
}

// Pick the best target for `attacker`, among enemies in range that its faction can see (only
// a nebula hides anything; see NebulaSight). Priority order:
//   1. Explicit unit target from command (if valid and in range)
//   2. Explicit structure target from command (if valid and in range)
//   3. Nearest enemy unit in range (ties: ascending unit-id)
//   4. Nearest enemy structure in range (ties: ascending structure-id)
static CombatTarget pick_target(const World& world, const Unit& attacker, const Command* cmd,
                                const NebulaSight& sight, const Occupancy& occ) {
  const int32_t range = attacker.stats().range;
  if (range == 0)
    return {};
  auto targetable = [&](Vec2 p) {
    return manhattan(attacker.pos, p) <= range && sight.sees(attacker.faction, p);
  };

  if (cmd && cmd->target_id != 0) {
    // Try as a unit first
    UnitId uid{cmd->target_id};
    const Unit* u = world.find_unit(uid);
    if (u && u->alive() && u->faction != attacker.faction && targetable(u->pos))
      return {uid, {}};

    // Try as a structure
    StructureId sid{cmd->target_id};
    const Structure* s = world.find_structure(sid);
    if (s && s->alive() && s->faction != attacker.faction && targetable(s->pos))
      return {{}, sid};
  }

  // Auto: nearest enemy unit, then nearest enemy structure; ties to the lower id.
  auto enemy_seen = [&](const auto& e) {
    return e.faction != attacker.faction && sight.sees(attacker.faction, e.pos);
  };
  const auto [unit, structure] = occ.nearest_target(attacker.pos, range, enemy_seen);
  if (!unit.is_null())
    return {unit, {}};
  return {{}, structure};
}

// Splash damage centered on `center`, radius 1.  Hits all living units and
// structures (friendly fire applies to both).
static void apply_splash(const Occupancy& occ, Vec2 center, int32_t dmg,
                         std::map<UnitId, int32_t>&      pending_units,
                         std::map<StructureId, int32_t>& pending_structs) {
  occ.each_unit_within(center, 1, [&](UnitId id) { pending_units[id] += dmg; });
  occ.each_structure_within(center, 1, [&](StructureId id) { pending_structs[id] += dmg; });
}

static void log_shot(CombatLog* log, const Unit& attacker, const CombatTarget& tgt, Vec2 to,
                     bool first_strike) {
  if (!log)
    return;
  const UnitStats& st = attacker.stats();
  log->shots.push_back({attacker.id, attacker.faction, attacker.type, attacker.pos,
                        tgt.unit_id, tgt.struct_id, to, st.dmg, first_strike, st.splash});
}

// The shield takes the hit first and the hull the overflow. Any hit, even one the shield
// absorbs whole, restarts the wait before it regenerates.
static void take_damage(Unit& u, int32_t dmg) {
  const int32_t absorbed = std::min(u.shield, dmg);
  u.shield -= absorbed;
  u.hp -= dmg - absorbed;
  u.shield_wait = cfg::SHIELD_REGEN_DELAY;
}

// After the tick's damage: a unit hit this tick (or within the delay) only counts down.
static void regenerate_shields(World& world) {
  for (auto& [uid, u] : world.units) {
    if (u.shield_wait > 0) {
      --u.shield_wait;
      continue;
    }
    u.shield = std::min(u.shield + u.stats().shield_regen, u.stats().shield);
  }
}

static void log_kill(CombatLog* log, const Unit& u) {
  if (log)
    log->kills.push_back({u.id, {}, u.faction, u.type, StructureType::CommandCore, u.pos});
}

static void log_kill(CombatLog* log, const Structure& s) {
  if (log)
    log->kills.push_back({{}, s.id, s.faction, UnitType::Drone, s.type, s.pos});
}

void run_combat(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b,
                CombatLog* log) {
  // A unit belongs to one faction, so at most one side holds its command.
  auto get_cmd = [&](UnitId uid) -> const Command* {
    if (auto it = cmds_a.find(uid); it != cmds_a.end())
      return &it->second;
    auto it = cmds_b.find(uid);
    return it != cmds_b.end() ? &it->second : nullptr;
  };

  // Who sees into which nebula, and who stands where, are fixed at the start of combat, from
  // post-movement positions; entities killed meanwhile drop out through their hp. The index's
  // storage is reused across calls; every rebuild refills it.
  const NebulaSight sight(world);
  thread_local Occupancy occ;
  occ.rebuild(world);

  // --- Sub-phase A: first-strike (Frigates only) ---------------------------
  for (auto& [uid, unit] : world.units) {
    if (!unit.alive() || !unit.stats().first_strike)
      continue;
    const Command* cmd = get_cmd(uid);
    auto kind = cmd ? static_cast<CommandKind>(cmd->kind) : CommandKind::Move;
    if (kind != CommandKind::Attack && kind != CommandKind::MoveAttack)
      continue;

    CombatTarget tgt = pick_target(world, unit, cmd, sight, occ);
    if (tgt.is_null())
      continue;

    world.rng.next(); // maintain consistent draw count
    if (!tgt.struct_id.is_null()) {
      Structure* s = world.find_structure(tgt.struct_id);
      if (s) {
        log_shot(log, unit, tgt, s->pos, true);
        s->hp -= unit.stats().dmg;
        if (!s->alive())
          log_kill(log, *s);
      }
    } else {
      Unit* t = world.find_unit(tgt.unit_id);
      if (t) {
        log_shot(log, unit, tgt, t->pos, true);
        take_damage(*t, unit.stats().dmg);
        if (!t->alive()) {
          t->hp = 0;
          log_kill(log, *t);
        }
      }
    }
  }
  world.purge_dead();
  occ.rebuild(world); // the purge freed what it pointed at

  // --- Sub-phase B: simultaneous -------------------------------------------
  std::map<UnitId, int32_t>      pending_units;
  std::map<StructureId, int32_t> pending_structs;

  for (auto& [uid, unit] : world.units) {
    if (!unit.alive() || unit.stats().first_strike)
      continue;
    const Command* cmd = get_cmd(uid);
    auto kind = cmd ? static_cast<CommandKind>(cmd->kind) : CommandKind::Move;
    if (kind != CommandKind::Attack && kind != CommandKind::MoveAttack)
      continue;

    CombatTarget tgt = pick_target(world, unit, cmd, sight, occ);
    if (tgt.is_null())
      continue;

    world.rng.next();

    if (!tgt.struct_id.is_null()) {
      Structure* s = world.find_structure(tgt.struct_id);
      if (!s)
        continue;
      log_shot(log, unit, tgt, s->pos, false);
      if (unit.stats().splash)
        apply_splash(occ, s->pos, unit.stats().dmg, pending_units, pending_structs);
      else
        pending_structs[tgt.struct_id] += unit.stats().dmg;
    } else {
      Unit* t = world.find_unit(tgt.unit_id);
      if (!t)
        continue;
      log_shot(log, unit, tgt, t->pos, false);
      if (unit.stats().splash)
        apply_splash(occ, t->pos, unit.stats().dmg, pending_units, pending_structs);
      else
        pending_units[tgt.unit_id] += unit.stats().dmg;
    }
  }

  // Finished Claim Nodes fire last; their shots are simultaneous with the units' all the same.
  for (const auto& [sid, s] : world.structures) {
    if (!s.alive() || s.type != StructureType::ClaimNode || !s.complete())
      continue;
    const UnitId best = occ.nearest_unit(s.pos, cfg::CLAIM_ATTACK_RANGE, [&](const Unit& u) {
      return u.faction != s.faction && sight.sees(s.faction, u.pos);
    });
    if (best.is_null())
      continue;
    pending_units[best] += cfg::CLAIM_ATTACK_DMG;
    if (log)
      log->shots.push_back({{}, s.faction, UnitType::Drone, s.pos, best, {},
                            world.find_unit(best)->pos, cfg::CLAIM_ATTACK_DMG, false, false, sid});
  }

  for (auto& [uid, dmg] : pending_units) {
    Unit* u = world.find_unit(uid);
    if (!u)
      continue;
    const bool was_alive = u->alive();
    take_damage(*u, dmg);
    if (was_alive && !u->alive())
      log_kill(log, *u);
  }
  for (auto& [sid, dmg] : pending_structs) {
    Structure* s = world.find_structure(sid);
    if (!s)
      continue;
    const bool was_alive = s->alive();
    s->hp -= dmg;
    if (was_alive && !s->alive())
      log_kill(log, *s);
  }
  world.purge_dead();
  regenerate_shields(world);
}

} // namespace game
