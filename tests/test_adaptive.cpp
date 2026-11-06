#include "adaptive_bot.hpp"
#include "config.hpp"
#include "expander_bot.hpp"
#include "frame.hpp"
#include "map_gen.hpp"
#include "map_io.hpp"
#include "match.hpp"
#include "match_setup.hpp"
#include "replay.hpp"
#include "turtle_bot.hpp"
#include <gtest/gtest.h>
#include <set>
#include <tuple>
#include <string>

// AdaptiveBot is the fixture that plays every system (Factories, counter-picked ships,
// scouting, nebula ambushes, claim sniping), so these tests are what put real combat on the
// board. The turtle and expander never fight back, so the adaptive bot should beat them by
// destroying their base on a full-size map; two adaptive bots should actually fight.

static const std::string kDataDir = MAPS_DATA_DIR;

namespace {

// Records what the bot fielded, from its own snapshots.
class Observed : public game::AdaptiveBot {
public:
  std::set<game::UnitType> types;
  std::set<game::StructureType> structures;
  bool hid_in_nebula{false};

  void on_init(const game::Map& map, uint32_t faction, uint64_t seed) override {
    map_ = map;
    AdaptiveBot::on_init(map, faction, seed);
  }

  std::vector<game::Command> on_tick(const game::Snapshot& snap) override {
    for (const auto& u : snap.my_units) {
      const auto type = static_cast<game::UnitType>(u.type);
      types.insert(type);
      if (type != game::UnitType::Drone &&
          map_.tile_at({u.x, u.y}).terrain == game::Terrain::Nebula)
        hid_in_nebula = true;
    }
    for (const auto& s : snap.my_structures)
      structures.insert(static_cast<game::StructureType>(s.type));
    return AdaptiveBot::on_tick(snap);
  }

private:
  game::Map map_;
};

maps::MapData full_size(uint64_t seed) {
  game::Rng rng{seed};
  maps::GenParams gp;
  gp.width = game::cfg::MAP_DEFAULT_W;
  gp.height = game::cfg::MAP_DEFAULT_H;
  return maps::generate_map(gp, rng);
}

template <class A, class B>
game::MatchRecord play(const maps::MapData& md, A& a, B& b, game::World* out = nullptr) {
  auto w = maps::world_from_map(md, 7);
  auto rec = game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);
  if (out) *out = std::move(w);
  return rec;
}

void expect_base_kill(const game::MatchRecord& rec, uint32_t winner, const std::string& where) {
  EXPECT_EQ(rec.outcome.reason, game::WinReason::BaseDestroyed) << where;
  EXPECT_EQ(rec.outcome.winner.value, winner) << where;
}

} // namespace

TEST(Adaptive, DestroysTheExpanderAndTheTurtleOnAFullSizeMap) {
  for (uint64_t seed : {9ULL}) {
    const auto md = full_size(seed);
    const std::string where = "seed " + std::to_string(seed);
    {
      game::AdaptiveBot a;
      game::ExpanderBot b;
      expect_base_kill(play(md, a, b), 0, where + " vs expander");
    }
    {
      game::ExpanderBot a;
      game::AdaptiveBot b;
      expect_base_kill(play(md, a, b), 1, where + " vs expander, as B");
    }
    {
      game::AdaptiveBot a;
      game::TurtleBot b;
      expect_base_kill(play(md, a, b), 0, where + " vs turtle");
    }
  }
}

// The bot is only useful as a combat fixture if it really does field everything.
TEST(Adaptive, FieldsEveryUnitAndStructureAndUsesNebulae) {
  const auto md = full_size(9);
  Observed a;
  game::ExpanderBot b;
  play(md, a, b);
  for (auto t : {game::UnitType::Drone, game::UnitType::Interceptor, game::UnitType::Frigate,
                 game::UnitType::Artillery, game::UnitType::Battleship})
    EXPECT_TRUE(a.types.contains(t)) << "never fielded unit type " << static_cast<int>(t);
  EXPECT_TRUE(a.structures.contains(game::StructureType::Factory));
  EXPECT_TRUE(a.structures.contains(game::StructureType::ClaimNode));
  EXPECT_TRUE(a.hid_in_nebula);
}

// Two adaptive bots contest the map, both build an army, and one destroys the other's base.
TEST(Adaptive, MirrorMatchIsAFight) {
  const auto md = maps::load_map(kDataDir + "/twin_peaks.json");
  Observed a, b;
  game::World w;
  const auto rec = play(md, a, b, &w);
  EXPECT_NE(rec.outcome.reason, game::WinReason::TickCap);
  EXPECT_LT(rec.ticks_played, game::cfg::TICK_CAP);
  EXPECT_TRUE(a.structures.contains(game::StructureType::Factory));
  EXPECT_TRUE(b.structures.contains(game::StructureType::Factory));
  EXPECT_EQ(w.command_core({1 - rec.outcome.winner.value}), nullptr) << "won by destroying it";
}

TEST(Adaptive, ReplayReproducesHash) {
  const auto md = maps::load_map(kDataDir + "/twin_peaks.json");
  game::AdaptiveBot a, b;
  const auto rec = play(md, a, b);
  EXPECT_EQ(game::replay(rec.replay), rec.replay.expected_hash);
  uint64_t fingerprint = 0;
  game::replay_frames(rec.replay, &fingerprint);
  EXPECT_EQ(fingerprint, rec.replay.expected_hash) << "viewers check the log with this alone";
}

// The bot plays a variation of its style drawn from its seed: a new match seed, another game;
// the same one, the same game again.
TEST(Adaptive, VariesItsPlayWithTheMatchSeed) {
  const auto md = maps::load_map(kDataDir + "/twin_peaks.json");
  auto game_with = [&](uint64_t seed) {
    game::AdaptiveBot a, b;
    auto w = maps::world_from_map(md, seed);
    const auto rec = game::run_match(w, a, 0, b, 1, game::cfg::TICK_CAP);
    return std::tuple{rec.replay.expected_hash, a.style().push_margin_pct, a.style().gather_pct};
  };
  std::set<uint64_t> hashes;
  std::set<std::pair<int32_t, int32_t>> styles;
  for (uint64_t seed = 1; seed <= 4; ++seed) {
    const auto [hash, margin, gather] = game_with(seed);
    hashes.insert(hash);
    styles.insert({margin, gather});
  }
  EXPECT_GE(hashes.size(), 3u);
  EXPECT_GE(styles.size(), 3u);
  EXPECT_EQ(std::get<0>(game_with(2)), std::get<0>(game_with(2)));

  game::AdaptiveStyle fixed;
  fixed.jitter_pct = 0;
  EXPECT_EQ(fixed.varied(123).push_margin_pct, fixed.push_margin_pct);
}
