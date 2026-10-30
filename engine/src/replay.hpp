#pragma once
#include "bot_iface.hpp"
#include "command.hpp"
#include "grid.hpp"
#include "wincheck.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace game {

// Input-log replay model: the log stores the raw commands each bot returned
// each tick. Playback re-simulates deterministically — no game state is stored.
// Replays are tied to engine version; a config-hash mismatch means the log
// cannot be trusted to reproduce the original match.

struct TickEntry {
  uint32_t tick;
  std::vector<Command> raw_a;
  std::vector<Command> raw_b;
  // Resource usage observed from each bot this tick (0 for scripted bots).
  uint64_t fuel_a{0};
  uint64_t fuel_b{0};
  uint64_t mem_bytes_a{0};
  uint64_t mem_bytes_b{0};
  // Non-None when that bot forfeited this tick (its raw commands are then empty).
  TickFault fault_a{TickFault::None};
  TickFault fault_b{TickFault::None};
};

// Initial entity placement recorded in spawn order.
// Spawn order is critical: ids are assigned sequentially, so replay must
// recreate entities in the exact same order to reproduce the same ids.
struct EntityInit {
  uint32_t faction_id;
  uint8_t type; // UnitType or StructureType value
  bool is_structure;
  int32_t x, y;
};

struct ReplayLog {
  // Identity — must match the engine build that plays it back
  uint32_t abi_version;
  uint64_t seed;

  // Map (inline for v1; a map-id + external file is a later optimization)
  int32_t map_w;
  int32_t map_h;
  std::vector<Tile> map_tiles; // row-major

  // Initial world state — entities in spawn order
  std::vector<EntityInit> initial_entities;

  uint32_t faction_a;
  uint32_t faction_b;
  std::string name_a; // truncated to cfg::MAX_PLAYER_NAME_LEN at match start
  std::string name_b;
  uint32_t tick_cap;
  // Resource usage during on_init (0 for scripted bots).
  uint64_t init_fuel_a{0};
  uint64_t init_fuel_b{0};
  uint64_t init_mem_bytes_a{0};
  uint64_t init_mem_bytes_b{0};

  std::vector<TickEntry> ticks;

  // Final hash of the original run — replay must reproduce this exactly
  uint64_t expected_hash;
  MatchResult outcome;
};

// Re-simulate a replay log from stored commands (no bots needed).
// Returns the final hash. Caller asserts it equals log.expected_hash.
uint64_t replay(const ReplayLog& log);

} // namespace game
