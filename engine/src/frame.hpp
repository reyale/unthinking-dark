#pragma once
#include "bot_iface.hpp"
#include "combat.hpp"
#include "entity.hpp"
#include "ids.hpp"
#include "wincheck.hpp"
#include "world.hpp"
#include <array>
#include <optional>
#include <vector>

namespace game {

struct ReplayLog;

struct UnitFrame {
  UnitId id;
  FactionId faction;
  UnitType type;
  Vec2 pos;
  int32_t hp;
  int32_t max_hp;
  int32_t cargo_energy{0}; // Drones: carried, not yet deposited
  int32_t cargo_alloy{0};
  int32_t shield{0};
  int32_t max_shield{0};

  bool carrying() const { return cargo_energy > 0 || cargo_alloy > 0; }
};

struct StructureFrame {
  StructureId id;
  FactionId faction;
  StructureType type;
  Vec2 pos;
  int32_t hp;
  int32_t max_hp;
  int32_t build_ticks_left{0}; // under construction (Factory or claim); 0 once finished
};

struct FrameState {
  uint32_t tick{0};
  std::vector<UnitFrame> units;
  std::vector<StructureFrame> structures;
  std::array<Resources, MAX_FACTIONS> resources{};
  std::optional<MatchResult> result; // non-empty on the final frame only
  // Combat during the tick that produced this frame (empty on the initial frame).
  std::vector<ShotEvent> shots;
  std::vector<KillEvent> kills;
  // Per faction: non-None if that side faulted (fuel, memory or a trap) and so forfeited the
  // match: only on the last frame, which then shows the board unchanged.
  std::array<TickFault, MAX_FACTIONS> forfeits{};
};

// Re-simulate a replay log and collect one FrameState per completed tick, plus
// the initial state before tick 1. The last frame has result set. `fingerprint`, if given,
// receives the same state hash replay() returns, so a viewer needn't simulate twice to check
// the log against its expected_hash.
std::vector<FrameState> replay_frames(const ReplayLog& log, uint64_t* fingerprint = nullptr);

} // namespace game
