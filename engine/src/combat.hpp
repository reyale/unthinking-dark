#pragma once
#include "command.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "ids.hpp"
#include <vector>

namespace game {

struct World;

// Presentation-only record of what combat did, for replay viewers. Collected only
// when a caller passes a CombatLog; never read back by game logic, never hashed,
// never shown to bots — so it is not part of the ABI or the replay format.
struct ShotEvent {
  UnitId      attacker;
  FactionId   faction;
  UnitType    attacker_type;
  Vec2        from;
  UnitId      target_unit;   // null when the target is a structure
  StructureId target_struct; // null when the target is a unit
  Vec2        to;            // target position; also the splash centre
  int32_t     damage;        // per entity hit
  bool        first_strike;
  bool        splash;
};

// Recorded when hp crosses to <= 0, before purge_dead() erases the entity, so
// splash victims (including friendly fire) get a position to explode at.
struct KillEvent {
  UnitId        unit;      // null for a structure
  StructureId   structure; // null for a unit
  FactionId     faction;
  UnitType      unit_type;
  StructureType structure_type;
  Vec2          pos;
};

struct CombatLog {
  std::vector<ShotEvent> shots;
  std::vector<KillEvent> kills;
};

// Phase 3 — combat against final positions.
//
// Sub-phase A (first-strike): Frigates fire first, iterating in ascending
//   unit-id order, drawing from the world RNG in that order.
//   Targets reduced to ≤0 hp are destroyed before sub-phase B.
//
// Sub-phase B (simultaneous): all other attackers (Interceptor, Artillery)
//   compute damage against survivors, then apply it all at once.
//   Mutual kills both die. Artillery splash hits a radius-1 area, friendly
//   fire included, all simultaneous.
//
// Targeting: explicit target if valid and in range; else nearest enemy in
//   range (Manhattan); else no attack.
void run_combat(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b,
                CombatLog* log = nullptr);

} // namespace game
