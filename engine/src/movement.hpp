#pragma once
#include "command.hpp"

namespace game {

struct World;

// Phase 2 — stepped simultaneous movement.
// All units with Move commands advance up to their speed in
// micro-steps. Each step is resolved simultaneously:
//   - Contested tile: higher collision priority wins; ties → lower unit-id.
//   - Chains: a unit may enter a tile its occupant leaves in the same step.
//   - Swaps and any other closed loop of movers: all blocked, and everyone queued behind.
//   - Blocked unit: stays put for the step.
//   - Nothing halts a Move short of its tile: where a ship stops is the bot's call.
// Units with Attack/Idle/other commands do not move.
void run_movement(World& world, const ValidatedCommands& cmds_a, const ValidatedCommands& cmds_b);

} // namespace game
