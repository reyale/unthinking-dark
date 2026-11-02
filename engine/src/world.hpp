#pragma once
#include "grid.hpp"
#include "entity.hpp"
#include "ids.hpp"
#include "rng.hpp"
#include "territory.hpp"
#include <map>
#include <array>
#include <cstdint>

namespace game {

// Maximum factions in v1.
inline constexpr int32_t MAX_FACTIONS = 2;

// Authoritative game state. All containers are ordered (std::map keyed by id)
// so iteration order is deterministic — never use unordered containers here.
struct World {
  Map map;
  Rng rng;
  uint64_t rng_seed{0}; // seed used to initialize rng; stored for replay
  uint32_t tick{0};

  std::map<UnitId, Unit> units;
  std::map<StructureId, Structure> structures;
  std::map<Vec2, StructureId> structure_by_pos; // kept in sync with structures
  std::array<Resources, MAX_FACTIONS> resources{};

  // Id counters — assigned in creation order, never reused.
  uint32_t next_entity_id{1}; // shared by units and structures: ids never collide

  // Last territory partition (updated each tick after run_territory).
  TerritoryState last_territory{};
  // The finished claims last_territory was computed from. Territory depends only on these and
  // the static map, so run_territory skips the partition while they are unchanged. Derived
  // state: not hashed.
  std::vector<ClaimAnchor> territory_claims{};

  // ---- Factory helpers --------------------------------------------------

  Unit& spawn_unit(FactionId faction, UnitType type, Vec2 pos);
  Structure& spawn_structure(FactionId faction, StructureType type, Vec2 pos);

  // Passable terrain with no structure and no living unit on it.
  bool tile_free(Vec2 pos) const;

  // ---- Accessors --------------------------------------------------------

  // Returns nullptr if dead or nonexistent.
  Unit* find_unit(UnitId id);
  Structure* find_structure(StructureId id);

  const Unit* find_unit(UnitId id) const;
  const Structure* find_structure(StructureId id) const;

  Resources& res(FactionId f) { return resources[f.value]; }
  const Resources& res(FactionId f) const { return resources[f.value]; }

  // Command Core for a faction (nullptr if destroyed).
  Structure* command_core(FactionId f);
  const Structure* command_core(FactionId f) const;

  // Remove dead entities — called at end of combat phase.
  void purge_dead();
};

} // namespace game
