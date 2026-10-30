#include "frame.hpp"
#include "map_io.hpp"
#include "match.hpp"
#include "match_setup.hpp"
#include "econ_bot.hpp"
#include "idle_bot.hpp"
#include "rush_bot.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

static const std::string kDataDir = MAPS_DATA_DIR;

static std::vector<std::string> all_maps() {
  std::vector<std::string> paths;
  for (const auto& e : std::filesystem::directory_iterator(kDataDir))
    if (e.path().extension() == ".json")
      paths.push_back(e.path().string());
  std::sort(paths.begin(), paths.end());
  return paths;
}

// Units never share a tile with each other or with a structure.
static void expect_no_overlap(const std::vector<game::UnitFrame>& units,
                              const std::vector<game::StructureFrame>& structures,
                              const std::string& where) {
  std::map<game::Vec2, game::UnitId> seen;
  for (const auto& s : structures)
    for (const auto& u : units)
      EXPECT_FALSE(u.pos == s.pos) << where << ": unit " << u.id.value << " on structure "
                                   << s.id.value << " at " << u.pos.x << "," << u.pos.y;
  for (const auto& u : units) {
    auto [it, fresh] = seen.emplace(u.pos, u.id);
    EXPECT_TRUE(fresh) << where << ": units " << it->second.value << " and " << u.id.value
                       << " share " << u.pos.x << "," << u.pos.y;
  }
}

TEST(MatchSetup, StartingUnitsGetTheirOwnTiles) {
  const auto maps = all_maps();
  ASSERT_FALSE(maps.empty());
  for (const auto& path : maps) {
    auto w = maps::world_from_map(maps::load_map(path), 1);
    EXPECT_EQ(w.units.size(), 6u) << path;
    std::vector<game::UnitFrame> units;
    for (const auto& [id, u] : w.units)
      units.push_back({u.id, u.faction, u.type, u.pos, u.hp, u.hp});
    std::vector<game::StructureFrame> structs;
    for (const auto& [id, s] : w.structures)
      structs.push_back({s.id, s.faction, s.type, s.pos, s.hp, s.hp});
    expect_no_overlap(units, structs, path);
  }
}

// Every tick of real bot matches on every shipped map keeps one unit per tile.
TEST(MatchSetup, BotMatchesNeverShareTiles) {
  for (const auto& path : all_maps()) {
    const auto md = maps::load_map(path);
    auto check = [&](game::Bot& a, game::Bot& b, const char* name) {
      auto w = maps::world_from_map(md, 7);
      auto rec = game::run_match(w, a, 0, b, 1, 400);
      auto frames = game::replay_frames(rec.replay);
      for (const auto& fr : frames)
        expect_no_overlap(fr.units, fr.structures,
                          path + " " + name + " tick " + std::to_string(fr.tick));
    };
    { game::RushBot a, b; check(a, b, "rush-vs-rush"); }
    { game::RushBot a; game::IdleBot b; check(a, b, "rush-vs-idle"); }
    { game::EconBot a, b; check(a, b, "econ-vs-econ"); }
  }
}
