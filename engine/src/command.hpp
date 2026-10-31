#pragma once
#include "abi.hpp"
#include "entity.hpp"
#include "ids.hpp"
#include <map>
#include <vector>

namespace game {

// ---- Validated result -------------------------------------------------------

// After Phase 1 validation: at most one command per issuer (a unit, or a Core or Factory
// producing), keyed by its id. std::map keeps iteration order deterministic (ascending id).
using ValidatedCommands = std::map<EntityId, Command>;

// ---- Validator --------------------------------------------------------------

// Filters `raw` commands against current world state for `faction_id`.
// Rules (§5 Phase 1):
//   - Unknown/dead/enemy issuer → rejected
//   - Produce comes from a Core or Factory (can_produce); every other kind from a unit
//   - Second command for same issuer → first listed wins
//   - Invalid target/range/cost → rejected
// Returns one valid command per surviving issuer (may be fewer than input).
// The Command Core produces Drones; a Factory produces combat ships.
bool can_produce(StructureType producer, UnitType what);

ValidatedCommands validate_commands(const struct World& world, uint32_t faction_id,
                                    const std::vector<Command>& raw);

} // namespace game
