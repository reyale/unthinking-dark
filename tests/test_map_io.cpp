#include "map_io.hpp"
#include "map_validate.hpp"
#include "map_gen.hpp"
#include "grid.hpp"
#include "rng.hpp"
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <string>

static const std::string kDataDir = MAPS_DATA_DIR;

TEST(MapIO, LoadSmallOpen) {
  auto data = maps::load_map(kDataDir + "/small_open.json");
  EXPECT_EQ(data.name, "small_open");
  EXPECT_EQ(data.resource_amount, 500);
  EXPECT_EQ(data.map.width, 18);
  EXPECT_EQ(data.map.height, 14);
  EXPECT_EQ(data.spawn[0].x, 1);
  EXPECT_EQ(data.spawn[0].y, 1);
  EXPECT_EQ(data.spawn[1].x, 16);
  EXPECT_EQ(data.spawn[1].y, 12);
  EXPECT_TRUE(data.map.passable(data.spawn[0]));
  EXPECT_EQ(data.map.tile_at({0, 0}).terrain, game::Terrain::Asteroid);
  EXPECT_EQ(data.map.tile_at({5, 1}).terrain, game::Terrain::ResourceNode);
}

TEST(MapIO, RoundTrip) {
  auto orig = maps::load_map(kDataDir + "/small_open.json");
  const std::string tmp = "/tmp/ud_map_roundtrip.json";
  maps::save_map(orig, tmp);
  auto loaded = maps::load_map(tmp);

  EXPECT_EQ(loaded.name, orig.name);
  EXPECT_EQ(loaded.resource_amount, orig.resource_amount);
  EXPECT_EQ(loaded.map.width, orig.map.width);
  EXPECT_EQ(loaded.map.height, orig.map.height);
  EXPECT_EQ(loaded.spawn[0].x, orig.spawn[0].x);
  EXPECT_EQ(loaded.spawn[0].y, orig.spawn[0].y);
  EXPECT_EQ(loaded.spawn[1].x, orig.spawn[1].x);
  EXPECT_EQ(loaded.spawn[1].y, orig.spawn[1].y);

  for (int32_t y = 0; y < orig.map.height; ++y)
    for (int32_t x = 0; x < orig.map.width; ++x)
      EXPECT_EQ(loaded.map.tile_at({x, y}).terrain, orig.map.tile_at({x, y}).terrain)
          << "at (" << x << "," << y << ")";
}

TEST(MapValidate, SmallOpenIsValid) {
  auto data = maps::load_map(kDataDir + "/small_open.json");
  auto errs = maps::validate_map(data);
  EXPECT_TRUE(errs.empty()) << errs[0];
}

TEST(MapValidate, TwinPeaksIsValid) {
  auto data = maps::load_map(kDataDir + "/twin_peaks.json");
  auto errs = maps::validate_map(data);
  EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs[0]);
}

TEST(MapValidate, RejectsNonSymmetric) {
  auto data = maps::load_map(kDataDir + "/small_open.json");
  // Set one tile but not its mirror
  data.map.set_terrain({3, 3}, game::Terrain::Asteroid);
  auto errs = maps::validate_map(data);
  ASSERT_FALSE(errs.empty());
  bool found = false;
  for (const auto& e : errs)
    if (e.find("symmetric") != std::string::npos)
      found = true;
  EXPECT_TRUE(found) << errs[0];
}

TEST(MapValidate, RejectsDisconnected) {
  auto data = maps::load_map(kDataDir + "/small_open.json");
  int32_t w = data.map.width;
  // Fill row 7 (middle) with asteroids to disconnect the map
  for (int32_t x = 1; x < w - 1; ++x)
    data.map.set_terrain({x, 7}, game::Terrain::Asteroid);
  data.map.recount_passable();
  auto errs = maps::validate_map(data);
  ASSERT_FALSE(errs.empty());
  bool found_reach = false;
  for (const auto& e : errs)
    if (e.find("reachable") != std::string::npos || e.find("isolated") != std::string::npos)
      found_reach = true;
  EXPECT_TRUE(found_reach) << errs[0];
}

TEST(MapValidate, RejectsNoResources) {
  maps::MapData data;
  data.name            = "no_resources";
  data.resource_amount = 500;
  data.map             = game::Map::make(8, 8, game::Terrain::Open);
  // Border
  for (int32_t x = 0; x < 8; ++x) {
    data.map.set_terrain({x, 0}, game::Terrain::Asteroid);
    data.map.set_terrain({x, 7}, game::Terrain::Asteroid);
  }
  for (int32_t y = 0; y < 8; ++y) {
    data.map.set_terrain({0, y}, game::Terrain::Asteroid);
    data.map.set_terrain({7, y}, game::Terrain::Asteroid);
  }
  data.spawn[0] = {1, 1};
  data.spawn[1] = {6, 6};
  data.map.recount_passable();

  auto errs = maps::validate_map(data);
  ASSERT_FALSE(errs.empty());
  bool found = false;
  for (const auto& e : errs)
    if (e.find("ResourceNode") != std::string::npos || e.find("resource") != std::string::npos)
      found = true;
  EXPECT_TRUE(found) << errs[0];
}

TEST(MapValidate, RejectsBadSpawnSymmetry) {
  auto data = maps::load_map(kDataDir + "/small_open.json");
  // Move spawn[1] off its symmetric position
  data.spawn[1] = {5, 5};
  auto errs = maps::validate_map(data);
  ASSERT_FALSE(errs.empty());
  bool found = false;
  for (const auto& e : errs)
    if (e.find("spawn symmetry") != std::string::npos ||
        e.find("Spawn symmetry") != std::string::npos ||
        e.find("symmetric") != std::string::npos ||
        e.find("mirror") != std::string::npos)
      found = true;
  EXPECT_TRUE(found) << errs[0];
}

TEST(MapGen, ProducesValidMap) {
  maps::GenParams p;
  p.width  = 20;
  p.height = 16;
  p.name   = "test_gen";

  game::Rng rng(42);
  auto data = maps::generate_map(p, rng);
  auto errs = maps::validate_map(data);
  EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs[0]);

  EXPECT_TRUE(data.map.passable(data.spawn[0]));
  EXPECT_TRUE(data.map.passable(data.spawn[1]));

  bool has_resource = false;
  for (int32_t y = 0; y < data.map.height; ++y)
    for (int32_t x = 0; x < data.map.width; ++x)
      if (data.map.tile_at({x, y}).terrain == game::Terrain::ResourceNode)
        has_resource = true;
  EXPECT_TRUE(has_resource);
}

TEST(MapIO, HashPopulatedOnLoad) {
  auto data = maps::load_map(kDataDir + "/small_open.json");
  EXPECT_NE(data.hash, 0u);
}

TEST(MapIO, HashStableAcrossLoads) {
  auto a = maps::load_map(kDataDir + "/small_open.json");
  auto b = maps::load_map(kDataDir + "/small_open.json");
  EXPECT_EQ(a.hash, b.hash);
}

TEST(MapIO, HashDiffersForDifferentMaps) {
  auto a = maps::load_map(kDataDir + "/small_open.json");
  auto b = maps::load_map(kDataDir + "/twin_peaks.json");
  EXPECT_NE(a.hash, b.hash);
}

TEST(MapIO, HashVerifiedOnRoundTrip) {
  auto orig = maps::load_map(kDataDir + "/small_open.json");
  const std::string tmp = "/tmp/ud_map_hash_roundtrip.json";
  maps::save_map(orig, tmp);
  // load_map will throw if the embedded hash doesn't match recomputed hash
  auto loaded = maps::load_map(tmp);
  EXPECT_EQ(loaded.hash, orig.hash);
}

TEST(MapIO, HashMismatchThrows) {
  auto orig = maps::load_map(kDataDir + "/small_open.json");
  maps::save_map(orig, "/tmp/ud_map_corrupt.json");

  // Tamper with the hash field directly
  {
    std::ifstream in("/tmp/ud_map_corrupt.json");
    nlohmann::json j = nlohmann::json::parse(in);
    j["hash"] = "0000000000000000";
    std::ofstream out("/tmp/ud_map_corrupt.json");
    out << j.dump(2) << '\n';
  }
  EXPECT_THROW(maps::load_map("/tmp/ud_map_corrupt.json"), std::runtime_error);
}

// ---- load_map error paths -----------------------------------------------

static void write_tmp(const std::string& path, const std::string& content) {
  std::ofstream f(path);
  f << content;
}

TEST(MapIO, LoadMissingFileThrows) {
  EXPECT_THROW(maps::load_map("/tmp/ud_no_such_file_xyz.json"), std::runtime_error);
}

TEST(MapIO, LoadBadJsonThrows) {
  write_tmp("/tmp/ud_bad.json", "{not valid json}");
  EXPECT_THROW(maps::load_map("/tmp/ud_bad.json"), std::runtime_error);
}

TEST(MapIO, LoadMissingGridThrows) {
  write_tmp("/tmp/ud_nogrid.json", R"({"name":"t","resource_amount":500})");
  EXPECT_THROW(maps::load_map("/tmp/ud_nogrid.json"), std::runtime_error);
}

TEST(MapIO, LoadEmptyGridThrows) {
  write_tmp("/tmp/ud_emptygrid.json", R"({"name":"t","resource_amount":500,"grid":[]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_emptygrid.json"), std::runtime_error);
}

TEST(MapIO, LoadRowNotStringThrows) {
  write_tmp("/tmp/ud_notstr.json", R"({"name":"t","resource_amount":500,"grid":[42]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_notstr.json"), std::runtime_error);
}

TEST(MapIO, LoadInconsistentWidthThrows) {
  write_tmp("/tmp/ud_width.json",
    R"({"name":"t","resource_amount":500,"grid":["###1#","###"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_width.json"), std::runtime_error);
}

TEST(MapIO, LoadNebulaTerrain) {
  write_tmp("/tmp/ud_nebula.json",
    R"({"name":"t","resource_amount":100,"grid":["####","#1~#","#~2#","####"]})");
  auto data = maps::load_map("/tmp/ud_nebula.json");
  EXPECT_EQ(data.map.tile_at({2, 1}).terrain, game::Terrain::Nebula);
}

TEST(MapIO, LoadDuplicateSpawn1Throws) {
  write_tmp("/tmp/ud_dup1.json",
    R"({"name":"t","resource_amount":100,"grid":["#####","#1.1#","#..2#","#####"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_dup1.json"), std::runtime_error);
}

TEST(MapIO, LoadDuplicateSpawn2Throws) {
  write_tmp("/tmp/ud_dup2.json",
    R"({"name":"t","resource_amount":100,"grid":["#####","#1.2#","#..2#","#####"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_dup2.json"), std::runtime_error);
}

TEST(MapIO, LoadUnknownCharThrows) {
  write_tmp("/tmp/ud_unk.json",
    R"({"name":"t","resource_amount":100,"grid":["####","#1X#","#.2#","####"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_unk.json"), std::runtime_error);
}

TEST(MapIO, LoadMissingSpawn1Throws) {
  write_tmp("/tmp/ud_nos1.json",
    R"({"name":"t","resource_amount":100,"grid":["####","#..#","#.2#","####"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_nos1.json"), std::runtime_error);
}

TEST(MapIO, LoadMissingSpawn2Throws) {
  write_tmp("/tmp/ud_nos2.json",
    R"({"name":"t","resource_amount":100,"grid":["####","#1.#","#..#","####"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_nos2.json"), std::runtime_error);
}

TEST(MapIO, SaveNebulaChar) {
  maps::MapData data;
  data.name            = "nebula_save";
  data.resource_amount = 100;
  data.map             = game::Map::make(4, 4, game::Terrain::Open);
  data.map.set_terrain({1, 1}, game::Terrain::Nebula);
  data.map.set_terrain({2, 2}, game::Terrain::Nebula);
  data.spawn[0] = {1, 0};
  data.spawn[1] = {2, 3};
  data.map.recount_passable();
  maps::save_map(data, "/tmp/ud_nebula_save.json");

  std::ifstream f("/tmp/ud_nebula_save.json");
  nlohmann::json j = nlohmann::json::parse(f);
  bool found_tilde = false;
  for (const auto& row : j["grid"])
    if (row.get<std::string>().find('~') != std::string::npos)
      found_tilde = true;
  EXPECT_TRUE(found_tilde);
}

TEST(MapIO, SaveToUnwritablePathThrows) {
  auto orig = maps::load_map(kDataDir + "/small_open.json");
  EXPECT_THROW(maps::save_map(orig, "/nonexistent_dir/map.json"), std::runtime_error);
}

// ---- validate_map error paths -------------------------------------------

TEST(MapValidate, RejectsTooSmallDimensions) {
  maps::MapData data;
  data.map = game::Map::make(3, 3, game::Terrain::Open);
  auto errs = maps::validate_map(data);
  ASSERT_FALSE(errs.empty());
  bool found = false;
  for (const auto& e : errs)
    if (e.find("dimensions") != std::string::npos || e.find("out of range") != std::string::npos)
      found = true;
  EXPECT_TRUE(found);
}

TEST(MapValidate, RejectsImpassableSpawn) {
  maps::MapData data;
  data.name            = "t";
  data.resource_amount = 500;
  data.map             = game::Map::make(8, 8, game::Terrain::Open);
  data.map.set_terrain({1, 1}, game::Terrain::Asteroid);
  data.spawn[0] = {1, 1}; // on an asteroid
  data.spawn[1] = {6, 6};
  data.map.recount_passable();
  auto errs = maps::validate_map(data);
  ASSERT_FALSE(errs.empty());
  bool found = false;
  for (const auto& e : errs)
    if (e.find("passable") != std::string::npos)
      found = true;
  EXPECT_TRUE(found);
}

// ---- generate_map exhaustion --------------------------------------------

TEST(MapGen, ThrowsAfterExhausted) {
  maps::GenParams p;
  p.width  = 20;
  p.height = 16;
  p.name   = "exhaust";
  game::Rng rng(1);
  EXPECT_THROW(maps::generate_map(p, rng, 0), std::runtime_error);
}

TEST(MapGen, SameSeedSameOutput) {
  maps::GenParams p;
  p.width  = 20;
  p.height = 16;
  p.name   = "test_seed";

  game::Rng rng_a(7);
  auto data_a = maps::generate_map(p, rng_a);
  maps::save_map(data_a, "/tmp/ud_map_seed_a.json");

  game::Rng rng_b(7);
  auto data_b = maps::generate_map(p, rng_b);
  maps::save_map(data_b, "/tmp/ud_map_seed_b.json");

  auto a = maps::load_map("/tmp/ud_map_seed_a.json");
  auto b = maps::load_map("/tmp/ud_map_seed_b.json");

  ASSERT_EQ(a.map.width, b.map.width);
  ASSERT_EQ(a.map.height, b.map.height);

  for (int32_t y = 0; y < a.map.height; ++y)
    for (int32_t x = 0; x < a.map.width; ++x)
      EXPECT_EQ(a.map.tile_at({x, y}).terrain, b.map.tile_at({x, y}).terrain)
          << "mismatch at (" << x << "," << y << ")";
}

TEST(MapIO, LoadEmptyRowThrows) {
  write_tmp("/tmp/ud_emptyrow.json", R"({"name":"t","resource_amount":500,"grid":[""]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_emptyrow.json"), std::runtime_error);
}

TEST(MapIO, LoadOversizedMapThrows) {
  std::string big_row(129, '.');
  big_row[1] = '1';
  big_row[2] = '2';
  write_tmp("/tmp/ud_bigrow.json",
            std::string(R"({"name":"t","resource_amount":500,"grid":[")") + big_row + R"("]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_bigrow.json"), std::runtime_error);
}

TEST(MapIO, LoadNegativeResourceThrows) {
  write_tmp("/tmp/ud_negres.json",
            R"({"name":"t","resource_amount":-5,"grid":["####","#1.#","#.2#","####"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_negres.json"), std::runtime_error);
}

TEST(MapIO, LoadMalformedHashThrows) {
  write_tmp("/tmp/ud_badhash.json",
            R"({"name":"t","resource_amount":500,"hash":"1234zzzz","grid":["####","#1.#","#.2#","####"]})");
  EXPECT_THROW(maps::load_map("/tmp/ud_badhash.json"), std::runtime_error);
}
