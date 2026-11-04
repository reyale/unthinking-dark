#pragma once
#include "bot_iface.hpp"
#include "ids.hpp"
#include "territory.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace game {

struct World;

enum class WinReason : uint8_t {
  BaseDestroyed,
  TerritoryThreshold,
  TickCap,
  TieBreakLadder,
  Draw,    // tick cap with territory difference within DRAW_TERRITORY_MARGIN, or see below
  Forfeit, // the other bot faulted (ran out of fuel, over the memory cap, trapped)
};

struct MatchResult {
  FactionId winner;
  WinReason reason;
};

// Phase 6 win checks, in fixed order (§5 Phase 6):
//   1. Base destroyed this tick? Both destroyed → Draw.
//   2. Territory ≥ current threshold? Both at once → tie-break ladder.
//   3. Tick cap reached? Territory difference ≤ DRAW_TERRITORY_MARGIN → Draw.
// Returns the match result, or nullopt if the game continues. Draws are a legitimate
// outcome (DESIGN §7), not a gap in the ladder.
std::optional<MatchResult> run_wincheck(const World& world, const TerritoryState& territory,
                                        uint32_t tick_cap);

// A bot that faults loses there and then, before the tick is played (DESIGN §2.4); both in
// the same tick is a Draw. Nullopt if neither faulted.
inline std::optional<MatchResult> fault_result(uint32_t faction_a, TickFault a, uint32_t faction_b,
                                               TickFault b) {
  if (a == TickFault::None && b == TickFault::None) return std::nullopt;
  if (a != TickFault::None && b != TickFault::None)
    return MatchResult{FactionId{}, WinReason::Draw};
  return MatchResult{FactionId{a != TickFault::None ? faction_b : faction_a}, WinReason::Forfeit};
}

// What decided the match, in words: "base destroyed", "territory", ..., and for a forfeit who
// faulted and why ("B forfeited: ran out of fuel"). `faults` (the last tick's) and `names` are
// indexed by faction id.
std::string result_reason(const MatchResult& r, const std::array<TickFault, 2>& faults,
                          const std::array<const char*, 2>& names);

} // namespace game
