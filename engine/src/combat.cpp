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
#include <vector>

namespace game {

struct Hit {
  int32_t dmg;
  int32_t shield_pct; // how much of it the target's shield feels
};

// A combat target is either an enemy unit or an enemy structure.
struct CombatTarget {
  UnitId      unit_id{};
  StructureId struct_id{};
  bool is_null() const { return unit_id.is_null() && struct_id.is_null(); }
};

bool target_in_range(const World&, const Unit& unit, const FactionSight& sight,
                     const Occupancy& occ) {
  const int32_t range = unit.stats().range;
  if (range == 0)
    return false;
  return occ.any_within(unit.pos, range, [&](const auto& e) {
    return e.faction != unit.faction && sight.sees(unit.faction, e.pos);
  });
}

bool target_in_range(const World& world, const Unit& unit) {
  const Occupancy occ(world);
  return target_in_range(world, unit, FactionSight(world, occ), occ);
}

// Pick the best target for `attacker`, among enemies in range that its faction can see (see
// FactionSight). Priority order:
//   1. Explicit unit target from command (if valid and in range)
//   2. Explicit structure target from command (if valid and in range)
//   3. Nearest enemy unit in range (ties: ascending unit-id)
//   4. Nearest enemy structure in range (ties: ascending structure-id)
static CombatTarget pick_target(const World& world, const Unit& attacker, const Command* cmd,
                                const FactionSight& sight, const Occupancy& occ) {
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

static Hit hit_on(const UnitStats& weapon, const Unit& target) {
  return {weapon.dmg,
          weapon.pierces_shields ? cfg::FULL_SHIELD_DMG_PCT : target.stats().shield_dmg_pct};
}

// Splash damage centered on `center`, radius 1.  Hits all living units and
// structures (friendly fire applies to both).
static void apply_splash(const World& world, const Occupancy& occ, Vec2 center,
                         const UnitStats& weapon,
                         std::map<UnitId, std::vector<Hit>>& pending_units,
                         std::map<StructureId, int32_t>&     pending_structs) {
  const int32_t dmg = weapon.dmg;
  occ.each_unit_within(center, 1, [&](UnitId id) {
    pending_units[id].push_back(hit_on(weapon, *world.find_unit(id)));
  });
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

// The shield takes the hit first, feeling shield_pct of it, and the hull the rest in full. Any
// hit, even one the shield absorbs whole, restarts the wait before it regenerates.
static void take_damage(Unit& u, Hit h) {
  // Rounded up, so even a claim's 1-damage shot wears a hardened shield down.
  const int32_t on_shield =
      (h.dmg * h.shield_pct + cfg::FULL_SHIELD_DMG_PCT - 1) / cfg::FULL_SHIELD_DMG_PCT;
  if (on_shield <= u.shield) {
    u.shield -= on_shield;
  } else {
    // The part of the hit the shield used up, rounded up so a shield always costs something.
    const int32_t spent =
        (u.shield * cfg::FULL_SHIELD_DMG_PCT + h.shield_pct - 1) / h.shield_pct;
    u.shield = 0;
    u.hp -= h.dmg - spent;
  }
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

// A unit's hits are kept apart, in the order they were fired, because how much of each reaches
// the hull depends on how much shield the ones before it left.
struct Pending {
  std::map<UnitId, std::vector<Hit>> units;
  std::map<StructureId, int32_t> structs;
};

// Every living attacker of one sub-phase, in ascending id (the RNG draw order), picks its
// target against the same state and adds its damage to `hits`; none of it lands until land(),
// so a sub-phase's shots are simultaneous among themselves: attackers that kill each other
// both fire.
template <class GetCmd>
static void volley(World& world, bool first_strike, GetCmd& get_cmd, const FactionSight& sight,
                   const Occupancy& occ, Pending& hits, CombatLog* log) {
  for (auto& [uid, unit] : world.units) {
    if (!unit.alive() || unit.stats().first_strike != first_strike)
      continue;
    const Command* cmd = get_cmd(uid);
    auto kind = cmd ? static_cast<CommandKind>(cmd->kind) : CommandKind::Move;
    if (kind != CommandKind::Attack && kind != CommandKind::MoveAttack)
      continue;

    CombatTarget tgt = pick_target(world, unit, cmd, sight, occ);
    if (tgt.is_null())
      continue;

    world.rng.next(); // maintain consistent draw count
    const Vec2 at = tgt.struct_id.is_null() ? world.find_unit(tgt.unit_id)->pos
                                            : world.find_structure(tgt.struct_id)->pos;
    log_shot(log, unit, tgt, at, first_strike);
    if (unit.stats().splash)
      apply_splash(world, occ, at, unit.stats(), hits.units, hits.structs);
    else if (tgt.struct_id.is_null())
      hits.units[tgt.unit_id].push_back(hit_on(unit.stats(), *world.find_unit(tgt.unit_id)));
    else
      hits.structs[tgt.struct_id] += unit.stats().dmg;
  }
}

static void land(World& world, const Pending& hits, CombatLog* log) {
  for (const auto& [uid, unit_hits] : hits.units) {
    Unit* u = world.find_unit(uid);
    if (!u)
      continue;
    const bool was_alive = u->alive();
    for (const Hit& h : unit_hits)
      take_damage(*u, h);
    if (was_alive && !u->alive())
      log_kill(log, *u);
  }
  for (const auto& [sid, dmg] : hits.structs) {
    Structure* s = world.find_structure(sid);
    if (!s)
      continue;
    const bool was_alive = s->alive();
    s->hp -= dmg;
    if (was_alive && !s->alive())
      log_kill(log, *s);
  }
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

  // Who stands where, and so who sees what, from post-movement positions. The index's storage is
  // reused across calls; every rebuild refills it.
  thread_local Occupancy occ;
  occ.rebuild(world);

  // --- Sub-phase A: first strike (Frigates), simultaneous among themselves --------------------
  {
    const FactionSight sight(world, occ);
    Pending hits;
    volley(world, true, get_cmd, sight, occ, hits, log);
    land(world, hits, log);
  }
  world.purge_dead();
  occ.rebuild(world); // the purge freed what it pointed at

  // --- Sub-phase B: simultaneous, against the survivors (a spotter killed in A spots no more) --
  const FactionSight sight(world, occ);
  Pending hits;
  volley(world, false, get_cmd, sight, occ, hits, log);

  // Finished Claim Nodes fire last; their shots are simultaneous with the units' all the same.
  for (const auto& [sid, s] : world.structures) {
    if (!s.alive() || s.type != StructureType::ClaimNode || !s.complete())
      continue;
    const UnitId best = occ.nearest_unit(s.pos, cfg::CLAIM_ATTACK_RANGE, [&](const Unit& u) {
      return u.faction != s.faction && sight.sees(s.faction, u.pos);
    });
    if (best.is_null())
      continue;
    hits.units[best].push_back(
        {cfg::CLAIM_ATTACK_DMG, world.find_unit(best)->stats().shield_dmg_pct});
    if (log)
      log->shots.push_back({{}, s.faction, UnitType::Drone, s.pos, best, {},
                            world.find_unit(best)->pos, cfg::CLAIM_ATTACK_DMG, false, false, sid});
  }

  land(world, hits, log);
  world.purge_dead();
  regenerate_shields(world);
}

} // namespace game
