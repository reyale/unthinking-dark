#include <gtest/gtest.h>
#include <map>
#include <optional>
#include "frame.hpp"
#include "match.hpp"
#include "tick.hpp"
#include "world.hpp"
#include "stats.hpp"
#include "config.hpp"
#include "grid.hpp"
#include "../../fixtures/rush_bot.hpp"
#include "../../fixtures/econ_bot.hpp"
#include "../../fixtures/idle_bot.hpp"

// 20×20 map with resource nodes visible from each faction's starting drone
// (Manhattan distance = 2 from the drone's start, within sight radius 4).
// Point-symmetric: faction 0 top-left, faction 1 bottom-right.
static game::World make_bot_world(uint64_t seed = 42) {
  game::World w;
  w.map = game::Map::make(20, 20);
  w.rng = game::Rng{seed};
  w.rng_seed = seed;

  w.map.set_terrain({5,  2},  game::Terrain::ResourceNode, 1000);
  w.map.set_terrain({14, 17}, game::Terrain::ResourceNode, 1000);
  w.map.recount_passable();

  // Faction 0 — top-left
  w.spawn_structure({0}, game::StructureType::CommandCore, {2, 2});
  w.spawn_unit({0}, game::UnitType::Drone,       {3, 2});
  w.spawn_unit({0}, game::UnitType::Interceptor, {4, 2});
  w.spawn_unit({0}, game::UnitType::Frigate,     {4, 3});

  // Faction 1 — bottom-right
  w.spawn_structure({1}, game::StructureType::CommandCore, {17, 17});
  w.spawn_unit({1}, game::UnitType::Drone,       {16, 17});
  w.spawn_unit({1}, game::UnitType::Interceptor, {15, 17});
  w.spawn_unit({1}, game::UnitType::Frigate,     {15, 16});

  return w;
}

// ---- RushBot ---------------------------------------------------------------

TEST(RushBot, RunsFullMatch) {
  auto w = make_bot_world();
  game::RushBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);
  EXPECT_TRUE(a.healthy());
  EXPECT_TRUE(b.healthy());
}

TEST(RushBot, Deterministic) {
  auto run = [](uint64_t seed) {
    auto w = make_bot_world(seed);
    game::RushBot a, b;
    return game::run_match(w, a, 0, b, 1, 200).replay.expected_hash;
  };
  for (uint64_t seed : {1ULL, 42ULL, 99ULL})
    EXPECT_EQ(run(seed), run(seed)) << "non-deterministic at seed " << seed;
}

TEST(RushBot, ReplayReproducesHash) {
  auto w = make_bot_world(7);
  game::RushBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);
  EXPECT_EQ(game::replay(rec.replay), rec.replay.expected_hash);
}

// Both sides rush with Move, which fires on the way: their Frigates will clash in the middle.
// Verify the replay log has non-empty command buffers (bots actually issued orders).
TEST(RushBot, VsAggroIssuesCommands) {
  auto w = make_bot_world(13);
  game::RushBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);

  bool any_a = false, any_b = false;
  for (const auto& entry : rec.replay.ticks) {
    if (!entry.raw_a.empty()) any_a = true;
    if (!entry.raw_b.empty()) any_b = true;
  }
  EXPECT_TRUE(any_a);
  EXPECT_TRUE(any_b);
}

// ---- Combat log through replay frames -------------------------------------

// Collecting the combat log must not change the simulation: re-run the recorded
// commands on twin worlds, one with a log and one without, and compare per-tick hashes.
TEST(RushBot, CombatLogDoesNotChangeHash) {
  auto w = make_bot_world(13);
  game::RushBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);

  auto plain = make_bot_world(13);
  auto logged = make_bot_world(13);
  game::StateHash h_plain, h_logged;
  size_t total_shots = 0;
  for (const auto& entry : rec.replay.ticks) {
    game::CombatLog log;
    auto r1 = game::run_tick_phases(plain, entry.raw_a, rec.replay.faction_a, entry.raw_b,
                                    rec.replay.faction_b, rec.replay.tick_cap, h_plain);
    auto r2 = game::run_tick_phases(logged, entry.raw_a, rec.replay.faction_a, entry.raw_b,
                                    rec.replay.faction_b, rec.replay.tick_cap, h_logged, &log);
    total_shots += log.shots.size();
    ASSERT_EQ(h_plain.current, h_logged.current) << "diverged at tick " << logged.tick;
    if (r1 || r2) break;
  }
  EXPECT_EQ(h_logged.per_tick, rec.hash.per_tick);
  EXPECT_GT(total_shots, 0u) << "fixture never fought; test proves nothing";
}

// Every hp change in the replay frames is explained by that frame's shots.
TEST(RushBot, ReplayFrameShotsAccountForDamage) {
  auto w = make_bot_world(13);
  game::RushBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);
  auto frames = game::replay_frames(rec.replay);

  ASSERT_FALSE(frames.empty());
  EXPECT_TRUE(frames[0].shots.empty());
  EXPECT_TRUE(frames[0].kills.empty());

  size_t total_shots = 0, total_kills = 0;
  for (size_t f = 1; f < frames.size(); ++f) {
    const auto& prev = frames[f - 1];
    const auto& cur = frames[f];
    total_shots += cur.shots.size();
    total_kills += cur.kills.size();
    bool any_splash = false;
    // A unit's hits in firing order, each as (damage, attacker type or none for a claim).
    std::map<game::UnitId, std::vector<std::pair<int32_t, std::optional<game::UnitType>>>>
        unit_hits;
    std::map<game::StructureId, int32_t> struct_dmg;
    for (const auto& s : cur.shots) {
      any_splash |= s.splash;
      std::optional<game::UnitType> by;
      if (!s.attacker.is_null()) by = s.attacker_type;
      if (!s.target_unit.is_null()) unit_hits[s.target_unit].push_back({s.damage, by});
      else struct_dmg[s.target_struct] += s.damage;
    }
    if (any_splash) continue; // splash damage isn't attributed per victim

    // A unit hit this tick doesn't regenerate, so its hull and shield are exactly what its hits
    // leave (worked through here independently of the engine); an unhit one can only gain shield.
    std::map<game::UnitId, std::pair<int32_t, int32_t>> cur_hp; // hull, shield
    for (const auto& u : cur.units) cur_hp[u.id] = {u.hp, u.shield};
    for (const auto& u : prev.units) {
      int32_t hull = u.hp, shield = u.shield;
      for (const auto& [dmg, by] : unit_hits[u.id]) {
        const int32_t pct = by && game::stats_for(*by).pierces_shields
                                ? game::cfg::FULL_SHIELD_DMG_PCT
                                : game::stats_for(u.type).shield_dmg_pct;
        const int32_t felt =
            (dmg * pct + game::cfg::FULL_SHIELD_DMG_PCT - 1) / game::cfg::FULL_SHIELD_DMG_PCT;
        if (felt <= shield) {
          shield -= felt;
          continue;
        }
        hull -= dmg - (shield * game::cfg::FULL_SHIELD_DMG_PCT + pct - 1) / pct;
        shield = 0;
      }
      auto it = cur_hp.find(u.id);
      if (it != cur_hp.end()) {
        if (!unit_hits[u.id].empty()) {
          EXPECT_EQ(it->second.first, hull) << "unit " << u.id.value << " frame " << f;
          EXPECT_EQ(it->second.second, shield) << "unit " << u.id.value << " frame " << f;
        } else {
          EXPECT_GE(it->second.first + it->second.second, u.hp + u.shield)
              << "unit " << u.id.value << " frame " << f;
        }
      } else {
        bool logged = false;
        for (const auto& k : cur.kills) logged |= (k.unit == u.id);
        EXPECT_TRUE(logged) << "unit " << u.id.value << " vanished without a kill event";
        EXPECT_LE(hull, 0) << "unit " << u.id.value << " frame " << f;
      }
    }
    for (const auto& ps : prev.structures) {
      if (ps.type == game::StructureType::ClaimNode) continue; // territory caps claim hp
      if (ps.build_ticks_left > 0) continue;                    // hp rises while it is built
      for (const auto& cs : cur.structures) {
        if (cs.id == ps.id) {
          EXPECT_EQ(ps.hp - cs.hp, struct_dmg[ps.id]) << "structure " << ps.id.value;
        }
      }
    }
  }
  EXPECT_GT(total_shots, 0u);
  EXPECT_GT(total_kills, 0u);
}

// ---- EconBot ----------------------------------------------------------------

// EconBot never attacks: against a side that doesn't play it wins on territory alone, and the
// enemy Core ends the match untouched.
TEST(EconBot, WinsOnTerritoryWithoutAttacking) {
  auto w = make_bot_world();
  game::EconBot a;
  game::IdleBot b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);
  EXPECT_EQ(rec.outcome.winner, game::FactionId{0});
  EXPECT_EQ(rec.outcome.reason, game::WinReason::TerritoryThreshold);
  for (const auto& [id, s] : w.structures) {
    if (s.faction == game::FactionId{1} && s.type == game::StructureType::CommandCore) {
      EXPECT_EQ(s.hp, game::cfg::CMD_CORE_HP);
    }
  }
}

TEST(EconBot, Deterministic) {
  auto run = [](uint64_t seed) {
    auto w = make_bot_world(seed);
    game::EconBot a, b;
    return game::run_match(w, a, 0, b, 1, 200).replay.expected_hash;
  };
  for (uint64_t seed : {2ULL, 55ULL, 88ULL})
    EXPECT_EQ(run(seed), run(seed)) << "non-deterministic at seed " << seed;
}

TEST(EconBot, ReplayReproducesHash) {
  auto w = make_bot_world(3);
  game::EconBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);
  EXPECT_EQ(game::replay(rec.replay), rec.replay.expected_hash);
}

// ---- RushBot vs EconBot ----------------------------------------------------

TEST(RushVsEcon, Deterministic) {
  auto run = [](uint64_t seed) {
    auto w = make_bot_world(seed);
    game::RushBot a;
    game::EconBot b;
    return game::run_match(w, a, 0, b, 1, 200).replay.expected_hash;
  };
  for (uint64_t seed : {10ULL, 20ULL, 30ULL})
    EXPECT_EQ(run(seed), run(seed)) << "non-deterministic at seed " << seed;
}

TEST(RushVsEcon, ReplayReproducesHash) {
  auto w = make_bot_world(50);
  game::RushBot a;
  game::EconBot b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);
  EXPECT_EQ(game::replay(rec.replay), rec.replay.expected_hash);
}

// RushBot issues Move commands every tick; IdleBot never does.
// The replay log should reflect this asymmetry.
TEST(RushVsEcon, AggroIssuedCommandsIdleDidNot) {
  auto w = make_bot_world(77);
  game::RushBot a;
  game::IdleBot b;
  auto rec = game::run_match(w, a, 0, b, 1, 50);

  bool a_issued = false, b_issued = false;
  for (const auto& entry : rec.replay.ticks) {
    if (!entry.raw_a.empty()) a_issued = true;
    if (!entry.raw_b.empty()) b_issued = true;
  }
  EXPECT_TRUE(a_issued);
  EXPECT_FALSE(b_issued);
}

// Each bot gets a seed of its own at init, drawn from the match seed: the same match seed gives
// the same seeds again, another gives others, and the two sides never share one.
TEST(BotSeed, EachSideGetsItsOwnFromTheMatchSeed) {
  struct Seeded : game::IdleBot {
    uint64_t seed = 0;
    void on_init(const game::Map& m, uint32_t f, uint64_t s) override {
      seed = s;
      IdleBot::on_init(m, f, s);
    }
  };
  auto seeds = [](uint64_t match_seed) {
    auto w = make_bot_world(match_seed);
    Seeded a, b;
    game::run_match(w, a, 0, b, 1, 1);
    return std::pair{a.seed, b.seed};
  };
  const auto [a, b] = seeds(42);
  EXPECT_EQ(a, game::bot_seed(42, 0));
  EXPECT_EQ(b, game::bot_seed(42, 1));
  EXPECT_NE(a, b);
  EXPECT_EQ(seeds(42), std::pair(a, b));
  EXPECT_NE(seeds(43).first, a);
}
