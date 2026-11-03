#pragma once
#include "command.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "ids.hpp"
#include <vector>

namespace game {

class FactionSight;
class Occupancy;

struct World;
struct Unit;

// Presentation-only record of what combat did, for replay viewers. Collected only
// when a caller passes a CombatLog; never read back by game logic, never hashed,
// never shown to bots — so it is not part of the ABI or the replay format.
struct ShotEvent {
  UnitId      attacker;        // null when a Claim Node fired
  FactionId   faction;
  UnitType    attacker_type;   // meaningless when a Claim Node fired
  Vec2        from;
  UnitId      target_unit;   // null when the target is a structure
  StructureId target_struct; // null when the target is a unit
  Vec2        to;            // target position; also the splash centre
  int32_t     damage;        // per entity hit
  bool        first_strike;
  bool        splash;
  StructureId attacker_struct{}; // the Claim Node that fired; null for a unit's shot
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
// Sub-phase A (first strike): Frigates fire first, simultaneously among themselves (Frigates
//   that kill each other both fire), drawing from the world RNG in ascending unit-id order.
//   Targets reduced to ≤0 hp are destroyed before sub-phase B.
//
// Sub-phase B (simultaneous): all other attackers (Interceptor, Artillery, Battleship)
//   compute damage against survivors, then apply it all at once.
//   Mutual kills both die. Artillery splash hits a radius-1 area, friendly
//   fire included, all simultaneous. Finished Claim Nodes fire here too
//   (CLAIM_ATTACK_DMG at the nearest enemy unit within CLAIM_ATTACK_RANGE).
//
// Targeting: explicit target if valid, in range and seen; else nearest enemy in range
//   (Manhattan) that the attacker's faction sees; else no attack. Nothing fires blind: a
//   target must be seen by some living unit or structure of the attacker's side (FactionSight),
//   checked again for sub-phase B, so a spotter killed in A no longer spots.
void run_combat(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b,
                CombatLog* log = nullptr);

// True if `unit` has anything it could attack right now: a living enemy unit or
// structure within its range that its faction can see. Movement uses this to
// halt MoveAttack units, so a unit stops at full range whether it is closing on a ship or a
// building.
bool target_in_range(const World& world, const Unit& unit, const FactionSight& sight,
                     const Occupancy& occ);
bool target_in_range(const World& world, const Unit& unit); // builds the other two itself

} // namespace game
