#pragma once
#include "combat.hpp"
#include "command.hpp"
#include "wincheck.hpp"
#include "statehash.hpp"
#include <optional>
#include <vector>

namespace game {

struct World;
class Bot;

// Lower-level: run phases 1–6 with caller-supplied raw commands.
// The match runner uses this so it can record commands for the replay log
// before they are validated and consumed. Pass `combat_log` to collect shots and
// kills for presentation; it has no effect on the simulation.
std::optional<MatchResult> run_tick_phases(World& world, const std::vector<Command>& raw_a,
                                           uint32_t faction_a, const std::vector<Command>& raw_b,
                                           uint32_t faction_b, uint32_t tick_cap, StateHash& hash,
                                           CombatLog* combat_log = nullptr);

// Convenience wrapper: builds snapshots, calls bots, then calls run_tick_phases.
// Use this in tests that don't need replay recording.
std::optional<MatchResult> run_tick(World& world, Bot& bot_a, uint32_t faction_a, Bot& bot_b,
                                    uint32_t faction_b, uint32_t tick_cap, StateHash& hash);

} // namespace game
