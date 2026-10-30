#include "tick.hpp"
#include "bot_iface.hpp"
#include "combat.hpp"
#include "command.hpp"
#include "economy.hpp"
#include "movement.hpp"
#include "snapshot.hpp"
#include "statehash.hpp"
#include "territory.hpp"
#include "wincheck.hpp"
#include "world.hpp"

namespace game {

std::optional<MatchResult> run_tick_phases(World& world, const std::vector<Command>& raw_a,
                                           uint32_t faction_a, const std::vector<Command>& raw_b,
                                           uint32_t faction_b, uint32_t tick_cap, StateHash& hash,
                                           CombatLog* combat_log) {
  // Phase 1 — Validate
  ValidatedCommands cmds_a = validate_commands(world, faction_a, raw_a);
  ValidatedCommands cmds_b = validate_commands(world, faction_b, raw_b);

  // Phase 2 — Movement
  run_movement(world, cmds_a, cmds_b);

  // Phase 3 — Combat
  run_combat(world, cmds_a, cmds_b, combat_log);

  // Phase 4 — Economy
  run_economy(world, cmds_a, cmds_b);

  // Phase 5 — Spawn / Upkeep (production handled inside economy)

  // Phase 6 — Territory & Win Check
  TerritoryState territory = run_territory(world, tick_cap);
  world.last_territory = territory;
  ++world.tick;

  auto result = run_wincheck(world, territory, tick_cap);
  hash.update(world);
  return result;
}

std::optional<MatchResult> run_tick(World& world, Bot& bot_a, uint32_t faction_a, Bot& bot_b,
                                    uint32_t faction_b, uint32_t tick_cap, StateHash& hash) {
  // Phase 0 — Snapshot & Decide
  Snapshot snap_a = build_snapshot(world, faction_a, tick_cap);
  Snapshot snap_b = build_snapshot(world, faction_b, tick_cap);

  std::vector<Command> raw_a = bot_a.healthy() ? bot_a.on_tick(snap_a) : std::vector<Command>{};
  std::vector<Command> raw_b = bot_b.healthy() ? bot_b.on_tick(snap_b) : std::vector<Command>{};

  return run_tick_phases(world, raw_a, faction_a, raw_b, faction_b, tick_cap, hash);
}

} // namespace game
