#include "match.hpp"
#include "bot_iface.hpp"
#include "config.hpp"
#include "grid.hpp"
#include "rng.hpp"
#include "snapshot.hpp"
#include "tick.hpp"
#include "world.hpp"
#include <map>

namespace game {

MatchRecord run_match(World& world, Bot& bot_a, uint32_t faction_a, Bot& bot_b, uint32_t faction_b,
                      uint32_t tick_cap, std::string_view name_a, std::string_view name_b,
                      ReplayWriter* writer) {
  MatchRecord rec;

  // Populate replay header
  rec.replay.abi_version = cfg::ABI_VERSION;
  rec.replay.seed        = world.rng_seed;
  rec.replay.map_w       = world.map.width;
  rec.replay.map_h       = world.map.height;
  rec.replay.faction_a   = faction_a;
  rec.replay.faction_b   = faction_b;
  auto resolve_name = [](std::string_view name, uint32_t faction) {
    auto s = name.empty() ? "player_" + std::to_string(faction) : std::string(name);
    if (s.size() > cfg::MAX_PLAYER_NAME_LEN) s.resize(cfg::MAX_PLAYER_NAME_LEN);
    return s;
  };
  rec.replay.name_a  = resolve_name(name_a, faction_a);
  rec.replay.name_b  = resolve_name(name_b, faction_b);
  rec.replay.tick_cap = tick_cap;

  // Snapshot map tiles
  rec.replay.map_tiles.reserve(static_cast<size_t>(world.map.width) *
                               static_cast<size_t>(world.map.height));
  for (int32_t y = 0; y < world.map.height; ++y)
    for (int32_t x = 0; x < world.map.width; ++x)
      rec.replay.map_tiles.push_back(world.map.tile_at({x, y}));

  // Record initial entity placement in id order (= spawn order). Units and structures share
  // one id sequence, so merge the two maps: re-spawning in this order reproduces every id.
  std::map<uint32_t, EntityInit> by_id;
  for (const auto& [uid, u] : world.units)
    by_id.emplace(uid.value,
                  EntityInit{u.faction.value, static_cast<uint8_t>(u.type), false, u.pos.x, u.pos.y});
  for (const auto& [sid, s] : world.structures)
    by_id.emplace(sid.value,
                  EntityInit{s.faction.value, static_cast<uint8_t>(s.type), true, s.pos.x, s.pos.y});
  for (const auto& [id, e] : by_id)
    rec.replay.initial_entities.push_back(e);

  if (writer)
    writer->begin(rec.replay);

  // Init bots
  bot_a.on_init(world.map, faction_a, bot_seed(world.rng_seed, faction_a));
  rec.replay.init_fuel_a     = bot_a.last_metrics().fuel_consumed;
  rec.replay.init_mem_bytes_a = bot_a.last_metrics().memory_bytes;
  bot_b.on_init(world.map, faction_b, bot_seed(world.rng_seed, faction_b));
  rec.replay.init_fuel_b     = bot_b.last_metrics().fuel_consumed;
  rec.replay.init_mem_bytes_b = bot_b.last_metrics().memory_bytes;

  // Tick loop
  std::optional<MatchResult> result;
  while (!result) {
    Snapshot snap_a = build_snapshot(world, faction_a, tick_cap);
    Snapshot snap_b = build_snapshot(world, faction_b, tick_cap);

    // A bot that is unhealthy (it failed to load or init) isn't called: it faults at tick 0.
    auto call = [](Bot& bot, const Snapshot& snap, BotMetrics& m) {
      if (!bot.healthy()) {
        m = {};
        m.fault = bot.last_metrics().fault != TickFault::None ? bot.last_metrics().fault
                                                              : TickFault::Trap;
        return std::vector<Command>{};
      }
      auto cmds = bot.on_tick(snap);
      m = bot.last_metrics();
      return cmds;
    };
    BotMetrics ma, mb;
    std::vector<Command> raw_a = call(bot_a, snap_a, ma);
    std::vector<Command> raw_b = call(bot_b, snap_b, mb);

    TickEntry entry;
    entry.tick        = world.tick;
    entry.raw_a       = raw_a;
    entry.raw_b       = raw_b;
    entry.fuel_a      = ma.fuel_consumed;
    entry.fuel_b      = mb.fuel_consumed;
    entry.mem_bytes_a = ma.memory_bytes;
    entry.mem_bytes_b = mb.memory_bytes;
    entry.fault_a     = ma.fault;
    entry.fault_b     = mb.fault;
    rec.replay.ticks.push_back(entry);
    if (writer)
      writer->write_tick(entry);

    result = fault_result(faction_a, ma.fault, faction_b, mb.fault);
    if (!result)
      result = run_tick_phases(world, raw_a, faction_a, raw_b, faction_b, tick_cap, rec.hash);
  }

  rec.outcome               = *result;
  rec.ticks_played          = world.tick;
  rec.replay.expected_hash  = rec.hash.fingerprint();
  rec.replay.outcome        = *result;

  if (writer)
    writer->finish(rec.replay.expected_hash, rec.replay.outcome);

  return rec;
}

} // namespace game
