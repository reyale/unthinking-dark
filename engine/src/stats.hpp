#pragma once
#include <cstddef>
#include <cstdint>

namespace game {

enum class UnitType : uint8_t;      // full definitions in entity.hpp
enum class StructureType : uint8_t;

struct UnitStats {
  int32_t hp;
  int32_t dmg;
  int32_t range;
  int32_t sight;
  int32_t speed;
  bool first_strike;
  bool splash;
  int32_t splash_radius;
  int32_t collision_priority;
};

const UnitStats& stats_for(UnitType t);

int32_t structure_max_hp(StructureType t); // a Claim Node's, before the fragility cap
// How near its Drone a structure is placed (Manhattan): BUILD_RANGE for a Factory,
// CLAIM_DEPLOY_RANGE for a Claim Node. The Drone must stay this near while it is built.
int32_t placement_range(StructureType t);

} // namespace game
