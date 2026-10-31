// Unthinking Depths — C++ Bot Template
//
// Quick start:
//   make                    compiles this file → bot_template.wasm
//   make BOT=my_bot.cpp     compiles your file → my_bot.wasm
//
// The engine calls init() once before tick 0, then on_tick() every tick.
// It writes the snapshot into SNAPSHOT_ADDR before each call and reads
// up to (return value) Command structs from COMMAND_ADDR after each call.

#include <ud/types.hpp>
#include <cstdint>

// ---- ABI buffers -----------------------------------------------------------
// wasm-ld exports the linear-memory address of each symbol as a WASM i32
// global with the same name.  The engine reads those globals at load time.
extern "C" {
  alignas(4) unsigned char SNAPSHOT_ADDR[ud::SNAPSHOT_BUFFER_SIZE];
  alignas(4) game::Command COMMAND_ADDR[game::cfg::UNIT_HARD_CAP];
}

// ---- Snapshot accessors ----------------------------------------------------
static const game::SnapshotHeader* snap() {
  return reinterpret_cast<const game::SnapshotHeader*>(SNAPSHOT_ADDR);
}
static const game::UnitView* my_units() {
  return reinterpret_cast<const game::UnitView*>(SNAPSHOT_ADDR + snap()->my_units_off);
}
static const game::StructureView* my_structures() {
  return reinterpret_cast<const game::StructureView*>(SNAPSHOT_ADDR + snap()->my_structures_off);
}
static const game::EnemyView* enemies() {
  return reinterpret_cast<const game::EnemyView*>(SNAPSHOT_ADDR + snap()->enemies_off);
}
static const game::EnemyStructureView* enemy_structures() {
  return reinterpret_cast<const game::EnemyStructureView*>(SNAPSHOT_ADDR +
                                                           snap()->enemy_structures_off);
}
static const game::TileView* tiles() {
  return reinterpret_cast<const game::TileView*>(SNAPSHOT_ADDR + snap()->tiles_off);
}

// ---- Bot state -------------------------------------------------------------
static uint32_t g_faction = 0;

// ---- Bot implementation (edit from here down) ------------------------------

extern "C" void init(int32_t faction_id) {
  g_faction = static_cast<uint32_t>(faction_id);
}

extern "C" int32_t on_tick() {
  const game::SnapshotHeader* h = snap();
  const game::UnitView*       u = my_units();
  uint32_t n = 0;

  // Move every unit toward tile (10, 10) as a placeholder.
  for (uint32_t i = 0; i < h->my_unit_count && n < (uint32_t)game::cfg::UNIT_HARD_CAP; ++i) {
    game::Command& cmd = COMMAND_ADDR[n++];
    cmd            = {};
    cmd.unit_id    = u[i].id;
    cmd.kind       = static_cast<uint16_t>(game::CommandKind::Move);
    cmd.ax         = 10;
    cmd.ay         = 10;
  }

  return static_cast<int32_t>(n);
}
