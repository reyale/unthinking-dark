#pragma once
#include "ids.hpp"
#include "grid.hpp"
#include "stats.hpp"
#include <cstdint>
#include <optional>
#include <type_traits>

namespace game {

// ---- Enums ----------------------------------------------------------------

enum class UnitType : uint8_t {
  Drone = 0,
  Interceptor = 1,
  Frigate = 2,
  Artillery = 3,
  Battleship = 4,
};
static_assert(std::is_same_v<std::underlying_type_t<UnitType>, uint8_t>,
              "UnitType underlying type changed — update Command::aux consumers");

enum class StructureType : uint8_t {
  CommandCore = 0,
  Factory = 1,
  ClaimNode = 2,
};
static_assert(std::is_same_v<std::underlying_type_t<StructureType>, uint8_t>,
              "StructureType underlying type changed — update Command::aux consumers");

enum class OrderType : uint8_t {
  Idle = 0,
  Move = 1,
  MoveAttack = 2,
  Attack = 3,
  Gather = 4,
  Build = 5,
  DeployClaim = 6,
};


// ---- Unit -----------------------------------------------------------------

struct Unit {
  UnitId id;
  FactionId faction;
  UnitType type;
  Vec2 pos;
  int32_t hp;

  OrderType current_order{OrderType::Idle};
  Vec2 order_target{};    // tile target for Move/MoveAttack/Attack
  UnitId attack_target{}; // explicit attack target (may be null)

  int32_t move_steps_remaining{0}; // steps left this tick's movement phase

  bool alive() const { return hp > 0; }
  const UnitStats& stats() const { return stats_for(type); }
};

// ---- Structure ------------------------------------------------------------

struct Structure {
  StructureId id;
  FactionId faction;
  StructureType type;
  Vec2 pos;
  int32_t hp;

  // Factory: production queue (single slot, v1)
  std::optional<UnitType> production_queue{};
  int32_t production_ticks_left{0};

  bool alive() const { return hp > 0; }
};

// ---- Resources per faction ------------------------------------------------

struct Resources {
  int32_t energy{0};
  int32_t alloy{0};
};

} // namespace game
