#pragma once
// Runs a bot written against game::Bot (the interface the engine's own fixtures use) as a WASM
// module. The bot sees exactly what it would natively: on_init gets the Map, on_tick the
// Snapshot, both rebuilt from the buffers the runner fills, so one source builds for both:
//
//   #include "my_bot.hpp"
//   #include <ud/bot_host.hpp>
//   UD_WASM_BOT(my::Bot)
//
// Needs a C++ standard library for wasm32 (wasi-sdk); see fixtures/wasm_main.cpp.
#include "bot_iface.hpp"
#include "grid.hpp"
#include "snapshot.hpp"
#include <ud/abi_exports.hpp>
#include <algorithm>
#include <cstring>

namespace ud::host {

template <class T> std::vector<T> read_array(const unsigned char* buf, uint32_t off, uint32_t n) {
  std::vector<T> v(n);
  if (n) std::memcpy(v.data(), buf + off, n * sizeof(T));
  return v;
}

inline game::Snapshot read_snapshot(const unsigned char* buf) {
  game::Snapshot s;
  std::memcpy(&s.header, buf, sizeof(s.header));
  const game::SnapshotHeader& h = s.header;
  s.my_units = read_array<game::UnitView>(buf, h.my_units_off, h.my_unit_count);
  s.my_structures =
      read_array<game::StructureView>(buf, h.my_structures_off, h.my_structure_count);
  s.visible_enemies = read_array<game::EnemyView>(buf, h.enemies_off, h.visible_enemy_count);
  s.visible_enemy_structures = read_array<game::EnemyStructureView>(
      buf, h.enemy_structures_off, h.visible_enemy_structure_count);
  s.visible_tiles = read_array<game::TileView>(buf, h.tiles_off, h.visible_tile_count);
  return s;
}

// Before init the buffer holds the whole map as a snapshot of tiles only.
inline game::Map read_map(const unsigned char* buf) {
  const game::Snapshot s = read_snapshot(buf);
  game::Map m = game::Map::make(static_cast<int32_t>(s.header.map_w),
                                static_cast<int32_t>(s.header.map_h));
  for (const game::TileView& t : s.visible_tiles)
    m.set_terrain({t.x, t.y}, static_cast<game::Terrain>(t.terrain), t.resource_amount);
  m.recount_passable();
  return m;
}

} // namespace ud::host

extern "C" void __wasm_call_ctors(void);

// The bot object is built in init, not as a global: the runner calls no _initialize, so
// init runs the static constructors itself first.
#define UD_WASM_BOT(BotType)                                                                  \
  extern "C" {                                                                                \
  alignas(8) unsigned char SNAPSHOT_ADDR[ud::SNAPSHOT_BUFFER_SIZE];                           \
  alignas(8) game::Command COMMAND_ADDR[game::cfg::UNIT_HARD_CAP];                            \
  }                                                                                           \
  static BotType* ud_hosted_bot = nullptr;                                                    \
  extern "C" void init(int32_t faction_id) {                                                  \
    __wasm_call_ctors();                                                                      \
    ud_hosted_bot = new BotType();                                                            \
    ud_hosted_bot->on_init(ud::host::read_map(SNAPSHOT_ADDR),                                 \
                           static_cast<uint32_t>(faction_id));                                \
  }                                                                                           \
  extern "C" int32_t on_tick() {                                                              \
    const std::vector<game::Command> cmds =                                                   \
        ud_hosted_bot->on_tick(ud::host::read_snapshot(SNAPSHOT_ADDR));                        \
    const size_t n = std::min(cmds.size(), static_cast<size_t>(game::cfg::UNIT_HARD_CAP));    \
    if (n) std::memcpy(COMMAND_ADDR, cmds.data(), n * sizeof(game::Command));                 \
    return static_cast<int32_t>(n);                                                           \
  }
