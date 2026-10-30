#include "frame.hpp"
#include "config.hpp"
#include "grid.hpp"
#include "replay.hpp"
#include "rng.hpp"
#include "statehash.hpp"
#include "tick.hpp"

namespace game {

namespace {

int32_t structure_max_hp(StructureType t) {
  switch (t) {
    case StructureType::CommandCore: return cfg::CMD_CORE_HP;
    case StructureType::Factory:     return cfg::FACTORY_HP;
    case StructureType::ClaimNode:   return cfg::CLAIM_HP_BASE;
  }
  return 0;
}

FrameState capture(const World& world, std::optional<MatchResult> result) {
  FrameState f;
  f.tick = world.tick;
  f.resources = world.resources;
  f.result = result;

  for (const auto& [uid, u] : world.units)
    f.units.push_back({uid, u.faction, u.type, u.pos, u.hp, stats_for(u.type).hp});

  for (const auto& [sid, s] : world.structures)
    f.structures.push_back({sid, s.faction, s.type, s.pos, s.hp, structure_max_hp(s.type)});

  return f;
}

} // namespace

std::vector<FrameState> replay_frames(const ReplayLog& log) {
  // Rebuild map — mirrors replay() in replay.cpp intentionally; both must stay in sync.
  Map map = Map::make(log.map_w, log.map_h);
  for (int32_t y = 0; y < log.map_h; ++y) {
    for (int32_t x = 0; x < log.map_w; ++x) {
      const auto& t = log.map_tiles[static_cast<size_t>(y * log.map_w + x)];
      if (t.terrain != Terrain::Open || t.resource_amount != 0)
        map.set_terrain({x, y}, t.terrain, t.resource_amount);
    }
  }
  map.recount_passable();

  World world;
  world.map = std::move(map);
  world.rng = Rng{log.seed};
  world.rng_seed = log.seed;

  for (const auto& e : log.initial_entities) {
    FactionId fid{e.faction_id};
    Vec2 pos{e.x, e.y};
    if (e.is_structure)
      world.spawn_structure(fid, static_cast<StructureType>(e.type), pos);
    else
      world.spawn_unit(fid, static_cast<UnitType>(e.type), pos);
  }

  std::vector<FrameState> frames;
  frames.push_back(capture(world, std::nullopt)); // initial state before first tick

  StateHash hash;
  for (const auto& entry : log.ticks) {
    CombatLog combat;
    auto result = run_tick_phases(world, entry.raw_a, log.faction_a, entry.raw_b, log.faction_b,
                                  log.tick_cap, hash, &combat);
    FrameState fs = capture(world, result);
    fs.shots = std::move(combat.shots);
    fs.kills = std::move(combat.kills);
    frames.push_back(std::move(fs));
    if (result)
      break;
  }

  return frames;
}

} // namespace game
