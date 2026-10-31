#pragma once
// Single source of truth for every struct that crosses the engine/bot boundary.
// Rules: fixed-width integers only, naturally aligned (no packing needed),
// plain C types so this header is valid in any language's C FFI.
// Never add std:: types, pointers, or virtual functions here.
//
// If you change any struct: update the offsetof/sizeof asserts below,
// bump cfg::ABI_VERSION in config.hpp, and update the SDK headers.
//
// TODO: Replace this hand-rolled layout with a schema-driven wire format
// (e.g. Protocol Buffers, FlatBuffers, or Cap'n Proto) so that alignment,
// endianness, and versioned field evolution are guaranteed by the toolchain
// rather than enforced manually by the static_asserts below.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace game {

// ---- Commands (bot → engine) ------------------------------------------------

enum class CommandKind : uint16_t {
  Move = 0,
  MoveAttack = 1,
  Attack = 2,
  Gather = 3,
  Build = 4,
  DeployClaim = 5,
  Produce = 6,
};

// Fields use plain integer types rather than CommandKind/UnitType enums so this
// struct is valid C and can be shared with bots written in any language.
struct Command {
  uint32_t entity_id; // who acts: a unit, or for Produce the Core or Factory producing
  uint16_t kind;      // CommandKind
  uint16_t arg_type;  // reserved / unit_type for Produce/Build
  int32_t ax, ay;     // target tile
  uint32_t target_id; // Attack target (unit or structure id)
  uint16_t aux;       // unit_type for Produce/Build
  uint16_t _pad;
};

// ---- Snapshot (engine → bot) ------------------------------------------------

struct SnapshotHeader {
  uint32_t tick;
  uint32_t my_faction_id;
  int32_t energy;
  int32_t alloy;
  uint32_t territory_pct;     // 0..100 integer
  uint32_t win_threshold;     // 0..100 integer
  uint32_t map_w;
  uint32_t map_h;
  uint32_t my_unit_count;
  uint32_t visible_enemy_count;
  uint32_t visible_tile_count;
  // byte offsets from start of flat buffer to each array
  uint32_t my_units_off;
  uint32_t enemies_off;
  uint32_t tiles_off;
  uint32_t my_structure_count;
  uint32_t my_structures_off;
  uint32_t visible_enemy_structure_count;
  uint32_t enemy_structures_off;
};

struct UnitView { // my units: full info
  uint32_t id;
  uint16_t type;
  uint16_t _pad;
  int32_t x, y;
  int32_t hp;
  uint16_t current_order;
  uint16_t _pad2;
};

// StructureView::producing when a Factory's queue is empty (and always, for other types).
inline constexpr uint16_t PRODUCING_NONE = 0xFFFF;

struct StructureView { // my structures: full info. All of them, regardless of fog.
  uint32_t id;         // Produce's entity_id when this is a Core or Factory
  uint16_t type;       // StructureType
  uint16_t producing;  // UnitType in the Factory's queue, or PRODUCING_NONE
  int32_t x, y;
  int32_t hp;
  uint32_t production_ticks_left; // 0 when not producing
};

struct EnemyView { // visible enemies: observable info only
  uint32_t id;
  uint16_t type;
  uint16_t _pad;
  int32_t x, y;
  int32_t hp;
};

struct EnemyStructureView { // enemy structures on fog-visible tiles: same rule as EnemyView
  uint32_t id;               // Attack's target_id
  uint16_t type;             // StructureType
  uint16_t _pad;
  int32_t x, y;
  int32_t hp;
};

struct TileView { // fog-visible tiles only
  int32_t x, y;
  uint16_t terrain;
  uint16_t occupant_faction; // 0=none, 1=me, 2=enemy
  int32_t resource_amount;
};

// ---- Layout assertions ------------------------------------------------------
// These catch any reordering, resizing, or implicit padding introduced by
// future edits. Update the expected values whenever the struct intentionally
// changes, and bump ABI_VERSION.

static_assert(sizeof(Command) == 24,                      "Command ABI size changed");
static_assert(offsetof(Command, entity_id) ==  0,         "Command layout changed");
static_assert(offsetof(Command, kind)      ==  4,         "Command layout changed");
static_assert(offsetof(Command, arg_type)  ==  6,         "Command layout changed");
static_assert(offsetof(Command, ax)        ==  8,         "Command layout changed");
static_assert(offsetof(Command, ay)        == 12,         "Command layout changed");
static_assert(offsetof(Command, target_id) == 16,         "Command layout changed");
static_assert(offsetof(Command, aux)       == 20,         "Command layout changed");
static_assert(offsetof(Command, _pad)      == 22,         "Command layout changed");

static_assert(std::is_same_v<std::underlying_type_t<CommandKind>, decltype(Command::kind)>,
              "CommandKind underlying type must match Command::kind");

static_assert(sizeof(SnapshotHeader) == 72,                          "SnapshotHeader ABI size changed");
static_assert(offsetof(SnapshotHeader, tick)                 ==  0,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, my_faction_id)        ==  4,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, energy)               ==  8,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, alloy)                == 12,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, territory_pct)        == 16,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, win_threshold)        == 20,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, map_w)                == 24,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, map_h)                == 28,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, my_unit_count)        == 32,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, visible_enemy_count)  == 36,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, visible_tile_count)   == 40,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, my_units_off)         == 44,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, enemies_off)          == 48,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, tiles_off)            == 52,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, my_structure_count)   == 56,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, my_structures_off)    == 60,  "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, visible_enemy_structure_count) == 64, "SnapshotHeader layout changed");
static_assert(offsetof(SnapshotHeader, enemy_structures_off) == 68,  "SnapshotHeader layout changed");

static_assert(sizeof(UnitView) == 24,                    "UnitView ABI size changed");
static_assert(offsetof(UnitView, id)            ==  0,   "UnitView layout changed");
static_assert(offsetof(UnitView, type)          ==  4,   "UnitView layout changed");
static_assert(offsetof(UnitView, _pad)          ==  6,   "UnitView layout changed");
static_assert(offsetof(UnitView, x)             ==  8,   "UnitView layout changed");
static_assert(offsetof(UnitView, y)             == 12,   "UnitView layout changed");
static_assert(offsetof(UnitView, hp)            == 16,   "UnitView layout changed");
static_assert(offsetof(UnitView, current_order) == 20,   "UnitView layout changed");
static_assert(offsetof(UnitView, _pad2)         == 22,   "UnitView layout changed");

static_assert(sizeof(StructureView) == 24,                             "StructureView ABI size changed");
static_assert(offsetof(StructureView, id)                    ==  0,   "StructureView layout changed");
static_assert(offsetof(StructureView, type)                  ==  4,   "StructureView layout changed");
static_assert(offsetof(StructureView, producing)             ==  6,   "StructureView layout changed");
static_assert(offsetof(StructureView, x)                     ==  8,   "StructureView layout changed");
static_assert(offsetof(StructureView, y)                     == 12,   "StructureView layout changed");
static_assert(offsetof(StructureView, hp)                    == 16,   "StructureView layout changed");
static_assert(offsetof(StructureView, production_ticks_left) == 20,   "StructureView layout changed");

static_assert(sizeof(EnemyView) == 20,               "EnemyView ABI size changed");
static_assert(offsetof(EnemyView, id)   ==  0,       "EnemyView layout changed");
static_assert(offsetof(EnemyView, type) ==  4,       "EnemyView layout changed");
static_assert(offsetof(EnemyView, _pad) ==  6,       "EnemyView layout changed");
static_assert(offsetof(EnemyView, x)    ==  8,       "EnemyView layout changed");
static_assert(offsetof(EnemyView, y)    == 12,       "EnemyView layout changed");
static_assert(offsetof(EnemyView, hp)   == 16,       "EnemyView layout changed");

static_assert(sizeof(EnemyStructureView) == 20,          "EnemyStructureView ABI size changed");
static_assert(offsetof(EnemyStructureView, id)   ==  0,  "EnemyStructureView layout changed");
static_assert(offsetof(EnemyStructureView, type) ==  4,  "EnemyStructureView layout changed");
static_assert(offsetof(EnemyStructureView, _pad) ==  6,  "EnemyStructureView layout changed");
static_assert(offsetof(EnemyStructureView, x)    ==  8,  "EnemyStructureView layout changed");
static_assert(offsetof(EnemyStructureView, y)    == 12,  "EnemyStructureView layout changed");
static_assert(offsetof(EnemyStructureView, hp)   == 16,  "EnemyStructureView layout changed");

static_assert(sizeof(TileView) == 16,                          "TileView ABI size changed");
static_assert(offsetof(TileView, x)                 ==  0,    "TileView layout changed");
static_assert(offsetof(TileView, y)                 ==  4,    "TileView layout changed");
static_assert(offsetof(TileView, terrain)           ==  8,    "TileView layout changed");
static_assert(offsetof(TileView, occupant_faction)  == 10,    "TileView layout changed");
static_assert(offsetof(TileView, resource_amount)   == 12,    "TileView layout changed");

} // namespace game
