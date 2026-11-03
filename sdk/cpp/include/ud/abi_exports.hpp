#pragma once
// What every bot module needs whatever API it is written against: the snapshot buffer's size
// and the abi_version export. Kept apart from types.hpp, whose enum mirrors would collide with
// the engine's own in a bot written against game::Bot (see bot_host.hpp).
#include <abi.hpp>
#include <config.hpp>
#include <cstdint>

namespace ud {

// Worst-case flat snapshot buffer size.  Must match MAX_SNAPSHOT_BYTES in
// runner/wasm_bot.cpp — the engine validates this at bot load time. Before init the same
// buffer holds the whole map, one TileView per tile, which this also bounds.
inline constexpr unsigned SNAPSHOT_BUFFER_SIZE =
    sizeof(game::SnapshotHeader)
    + (unsigned)game::cfg::UNIT_HARD_CAP * sizeof(game::UnitView)
    + (unsigned)game::cfg::STRUCTURE_HARD_CAP * sizeof(game::StructureView)
    + (unsigned)game::cfg::UNIT_HARD_CAP * 2u * sizeof(game::EnemyView)
    + (unsigned)game::cfg::STRUCTURE_HARD_CAP * sizeof(game::EnemyStructureView)
    + (unsigned)game::cfg::MAP_MAX_W * (unsigned)game::cfg::MAP_MAX_H
      * sizeof(game::TileView);

} // namespace ud

// Every bot binary must export this symbol. The engine rejects bots whose
// version doesn't match its own ABI_VERSION at load time.
extern "C" __attribute__((used)) inline int32_t abi_version() {
  return static_cast<int32_t>(game::cfg::ABI_VERSION);
}
