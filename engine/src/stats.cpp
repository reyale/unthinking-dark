#include "stats.hpp"
#include "entity.hpp"
#include "config.hpp"
#include <stdexcept>

namespace game {

static const UnitStats UNIT_STATS[] = {
  // Drone
  {cfg::DRONE_HP, cfg::DRONE_DMG, cfg::DRONE_RANGE, cfg::DRONE_SIGHT, cfg::DRONE_SPEED,
   false, false, 0, /*priority*/ 1},
  // Interceptor
  {cfg::INTERCEPTOR_HP, cfg::INTERCEPTOR_DMG, cfg::INTERCEPTOR_RANGE, cfg::INTERCEPTOR_SIGHT,
   cfg::INTERCEPTOR_SPEED, false, false, 0, /*priority*/ 3},
  // Frigate
  {cfg::FRIGATE_HP, cfg::FRIGATE_DMG, cfg::FRIGATE_RANGE, cfg::FRIGATE_SIGHT, cfg::FRIGATE_SPEED,
   /*first_strike*/ true, false, 0, /*priority*/ 1},
  // Artillery
  {cfg::ARTILLERY_HP, cfg::ARTILLERY_DMG, cfg::ARTILLERY_RANGE, cfg::ARTILLERY_SIGHT,
   cfg::ARTILLERY_SPEED, false, /*splash*/ true, cfg::ARTILLERY_SPLASH_RADIUS, /*priority*/ 1},
  // Battleship
  {cfg::BATTLESHIP_HP, cfg::BATTLESHIP_DMG, cfg::BATTLESHIP_RANGE, cfg::BATTLESHIP_SIGHT,
   cfg::BATTLESHIP_SPEED, false, false, 0, /*priority*/ 2},
};
static_assert(std::size(UNIT_STATS) == static_cast<std::size_t>(UnitType::Battleship) + 1,
              "UNIT_STATS out of sync with UnitType enum — add a row for the new type");

const UnitStats& stats_for(UnitType t) {
  return UNIT_STATS[static_cast<uint8_t>(t)];
}

int32_t structure_max_hp(StructureType t) {
  switch (t) {
  case StructureType::CommandCore: return cfg::CMD_CORE_HP;
  case StructureType::Factory:     return cfg::FACTORY_HP;
  case StructureType::ClaimNode:   return cfg::CLAIM_HP_BASE;
  }
  return 0;
}

int32_t placement_range(StructureType t) {
  return t == StructureType::ClaimNode ? cfg::CLAIM_DEPLOY_RANGE : cfg::BUILD_RANGE;
}

} // namespace game
