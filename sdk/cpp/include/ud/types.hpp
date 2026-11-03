#pragma once
// SDK-facing enum and constant definitions that supplement abi.hpp/config.hpp.
// Values must stay in sync with the corresponding engine headers.
// If any value changes, ABI_VERSION is bumped and this file must be updated too.
#include <abi.hpp>
#include <config.hpp>
#include <ud/abi_exports.hpp>

namespace game {

// Mirrors entity.hpp UnitType. Underlying type widened to uint16_t to match
// UnitView::type and EnemyView::type in the snapshot.
enum class UnitType : uint16_t {
  Drone       = 0,
  Interceptor = 1,
  Frigate     = 2,
  Artillery   = 3,
  Battleship  = 4,
};

// Mirrors entity.hpp StructureType.
enum class StructureType : uint16_t {
  CommandCore = 0,
  Factory     = 1,
  ClaimNode   = 2,
};

// Mirrors grid.hpp Terrain. Matches TileView::terrain in the snapshot.
enum class Terrain : uint16_t {
  Open         = 0,
  Asteroid     = 1,
  Nebula       = 2,
  ResourceNode = 3,
};

} // namespace game
