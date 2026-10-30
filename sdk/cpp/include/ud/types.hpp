#pragma once
// SDK-facing enum and constant definitions that supplement abi.hpp/config.hpp.
// Values must stay in sync with the corresponding engine headers.
// If any value changes, ABI_VERSION is bumped and this file must be updated too.
#include <abi.hpp>
#include <config.hpp>

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

namespace ud {

// Worst-case flat snapshot buffer size.  Must match MAX_SNAPSHOT_BYTES in
// runner/wasm_bot.cpp — the engine validates this at bot load time.
inline constexpr unsigned SNAPSHOT_BUFFER_SIZE =
    sizeof(game::SnapshotHeader)
    + (unsigned)game::cfg::UNIT_HARD_CAP * sizeof(game::UnitView)
    + (unsigned)game::cfg::UNIT_HARD_CAP * 2u * sizeof(game::EnemyView)
    + (unsigned)game::cfg::MAP_MAX_W * (unsigned)game::cfg::MAP_MAX_H
      * sizeof(game::TileView);

} // namespace ud

// Every bot binary must export this symbol. The engine rejects bots whose
// version doesn't match its own ABI_VERSION at load time.
extern "C" __attribute__((used)) inline int32_t abi_version() {
  return static_cast<int32_t>(game::cfg::ABI_VERSION);
}
