#include <gtest/gtest.h>
#include "config.hpp"
#include "file_io.hpp"
#include "frame.hpp"
#include "match.hpp"
#include "replay_io.hpp"
#include "snapshot.hpp"
#include "world.hpp"
#include "../fixtures/idle_bot.hpp"

// Build a small symmetric world: two Command Cores + two Drones each side.
static game::World make_test_world(uint64_t seed) {
  game::World w;
  w.map = game::Map::make(20, 20);
  w.rng = game::Rng{seed};
  w.rng_seed = seed;

  // Faction 0: top-left
  w.spawn_structure({0}, game::StructureType::CommandCore, {2, 2});
  w.spawn_unit({0}, game::UnitType::Drone, {3, 2});
  w.spawn_unit({0}, game::UnitType::Interceptor, {4, 2});

  // Faction 1: bottom-right
  w.spawn_structure({1}, game::StructureType::CommandCore, {17, 17});
  w.spawn_unit({1}, game::UnitType::Drone, {16, 17});
  w.spawn_unit({1}, game::UnitType::Interceptor, {15, 17});

  return w;
}

// ---- Determinism keystone ------------------------------------------------

TEST(Determinism, SameSeedSameHash) {
  auto run = [](uint64_t seed) {
    game::World w = make_test_world(seed);
    game::IdleBot a, b;
    return game::run_match(w, a, 0, b, 1, 200).hash.fingerprint();
  };

  uint64_t h1 = run(42);
  uint64_t h2 = run(42);
  EXPECT_EQ(h1, h2);
}

TEST(Determinism, DifferentSeedsDifferentHash) {
  auto run = [](uint64_t seed) {
    game::World w = make_test_world(seed);
    game::IdleBot a, b;
    return game::run_match(w, a, 0, b, 1, 200).hash.fingerprint();
  };

  EXPECT_NE(run(1), run(2));
}

TEST(Determinism, PerTickHashesMatch) {
  auto run = [](uint64_t seed) {
    game::World w = make_test_world(seed);
    game::IdleBot a, b;
    return game::run_match(w, a, 0, b, 1, 200).hash.per_tick;
  };

  auto h1 = run(99);
  auto h2 = run(99);
  ASSERT_EQ(h1.size(), h2.size());
  for (size_t i = 0; i < h1.size(); ++i)
    EXPECT_EQ(h1[i], h2[i]) << "diverged at tick " << i;
}

// ---- Replay keystone -----------------------------------------------------

TEST(Determinism, ReplayReproducesHash) {
  game::World w = make_test_world(7);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);

  uint64_t replayed = game::replay(rec.replay);
  EXPECT_EQ(replayed, rec.replay.expected_hash);
}

// ---- Fog-of-war correctness ----------------------------------------------

TEST(Fog, SnapshotNeverLeaksEnemyOutsideVision) {
  game::World w = make_test_world(0);
  // Place a stealth enemy unit far away from faction 0's sight
  w.spawn_unit({1}, game::UnitType::Drone, {19, 19});

  game::Snapshot snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);

  // Verify every visible enemy is actually within sight of some friendly unit
  for (const auto& ev : snap.visible_enemies) {
    game::Vec2 epos{ev.x, ev.y};
    bool in_sight = false;
    for (const auto& uv : snap.my_units) {
      game::Vec2 upos{uv.x, uv.y};
      int32_t sight = game::stats_for(static_cast<game::UnitType>(uv.type)).sight;
      if (game::manhattan(upos, epos) <= sight) {
        in_sight = true;
        break;
      }
    }
    EXPECT_TRUE(in_sight) << "enemy at (" << ev.x << "," << ev.y << ") leaked into snapshot";
  }
}

TEST(Fog, FarEnemyNotVisible) {
  game::World w;
  w.map = game::Map::make(40, 40);
  w.rng = game::Rng{0};

  // Faction 0 unit in one corner, faction 1 unit far away
  w.spawn_unit({0}, game::UnitType::Interceptor, {0, 0});
  w.spawn_unit({1}, game::UnitType::Interceptor, {39, 39});

  game::Snapshot snap = game::build_snapshot(w, 0, game::cfg::TICK_CAP);
  EXPECT_EQ(snap.visible_enemies.size(), 0u);
}

// ---- Replay file I/O --------------------------------------------------------

TEST(ReplayIO, RoundtripReproducesHash) {
  game::World w = make_test_world(42);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);

  game::MemoryWriter mw;
  game::write_replay(rec.replay, mw);

  game::MemoryReader mr(mw.data);
  game::ReplayLog log2 = game::read_replay(mr);

  uint64_t replayed = game::replay(log2);
  EXPECT_EQ(replayed, rec.replay.expected_hash);
}

TEST(ReplayIO, VersionMismatchThrows) {
  game::World w = make_test_world(1);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 10);
  rec.replay.abi_version = 999; // corrupt the version

  game::MemoryWriter mw;
  game::write_replay(rec.replay, mw);

  game::MemoryReader mr(mw.data);
  EXPECT_THROW(game::read_replay(mr), game::ReplayVersionError);
}

TEST(ReplayIO, OldFormatRefusedWithClearMessage) {
  game::World w = make_test_world(1);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 10);
  game::MemoryWriter mw;
  game::write_replay(rec.replay, mw);
  const char v1[] = "UDREPLAY"; // format 1, before forfeits were recorded
  std::copy(v1, v1 + 8, mw.data.begin());
  game::MemoryReader mr(mw.data);
  try {
    game::read_replay(mr);
    FAIL() << "format-1 replay should be refused";
  } catch (const game::ReplayVersionError& e) {
    EXPECT_NE(std::string(e.what()).find("re-record"), std::string::npos) << e.what();
  }
}

TEST(ReplayIO, ForfeitsSurviveRoundTrip) {
  game::World w = make_test_world(1);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 10);
  rec.replay.ticks[3].fault_a = game::TickFault::OutOfMemory;
  rec.replay.ticks[5].fault_b = game::TickFault::OutOfFuel;
  rec.replay.ticks[7].fault_a = game::TickFault::Trap;
  game::MemoryWriter mw;
  game::write_replay(rec.replay, mw);
  game::MemoryReader mr(mw.data);
  auto back = game::read_replay(mr);
  for (size_t i = 0; i < back.ticks.size(); ++i) {
    EXPECT_EQ(back.ticks[i].fault_a, rec.replay.ticks[i].fault_a) << i;
    EXPECT_EQ(back.ticks[i].fault_b, rec.replay.ticks[i].fault_b) << i;
  }
}

// ---- ReplayWriter streaming -------------------------------------------------

TEST(ReplayWriter, StreamingMatchOutputRoundtrips) {
  game::World w = make_test_world(77);
  game::IdleBot a, b;

  game::MemoryWriter mw;
  game::ReplayWriter rw(mw);
  auto rec = game::run_match(w, a, 0, b, 1, 200, "alice", "bob", &rw);

  game::MemoryReader mr(mw.data);
  game::ReplayLog log2 = game::read_replay(mr);

  EXPECT_EQ(game::replay(log2), rec.replay.expected_hash);
}

TEST(ReplayWriter, StreamingMatchPreservesNames) {
  game::World w = make_test_world(7);
  game::IdleBot a, b;

  game::MemoryWriter mw;
  game::ReplayWriter rw(mw);
  game::run_match(w, a, 0, b, 1, 50, "alice", "bob", &rw);

  game::MemoryReader mr(mw.data);
  game::ReplayLog log2 = game::read_replay(mr);

  EXPECT_EQ(log2.name_a, "alice");
  EXPECT_EQ(log2.name_b, "bob");
}

TEST(ReplayWriter, StreamingOutputMatchesBatchOutput) {
  game::World w1 = make_test_world(13);
  game::World w2 = make_test_world(13);
  game::IdleBot a1, b1, a2, b2;

  // Streaming write via run_match
  game::MemoryWriter stream_mw;
  game::ReplayWriter rw(stream_mw);
  game::run_match(w1, a1, 0, b1, 1, 100, "p0", "p1", &rw);

  // Batch write after the fact
  game::MemoryWriter batch_mw;
  auto rec = game::run_match(w2, a2, 0, b2, 1, 100, "p0", "p1");
  game::write_replay(rec.replay, batch_mw);

  EXPECT_EQ(stream_mw.data, batch_mw.data);
}

TEST(ReplayWriter, ManualBeginWriteTickFinish) {
  game::World w = make_test_world(3);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 30);

  // Replay it manually through ReplayWriter
  game::MemoryWriter mw;
  game::ReplayWriter rw(mw);
  rw.begin(rec.replay);
  for (const auto& entry : rec.replay.ticks)
    rw.write_tick(entry);
  rw.finish(rec.replay.expected_hash, rec.replay.outcome);

  game::MemoryReader mr(mw.data);
  game::ReplayLog log2 = game::read_replay(mr);

  EXPECT_EQ(game::replay(log2), rec.replay.expected_hash);
  EXPECT_EQ(log2.ticks.size(), rec.replay.ticks.size());
}

// ---- Frame collection -------------------------------------------------------

TEST(ReplayFrames, FrameCountMatchesTicks) {
  game::World w = make_test_world(5);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 200);

  auto frames = game::replay_frames(rec.replay);

  // One initial frame + one frame per tick played.
  EXPECT_EQ(frames.size(), rec.ticks_played + 1);
  EXPECT_TRUE(frames.back().result.has_value());
}

TEST(ReplayFrames, InitialFrameHasAllUnits) {
  game::World w = make_test_world(0);
  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 50);

  auto frames = game::replay_frames(rec.replay);

  // make_test_world spawns 2 units + 1 structure per faction = 4 units, 2 structures.
  EXPECT_EQ(frames[0].tick, 0u);
  EXPECT_EQ(frames[0].units.size(), 4u);
  EXPECT_EQ(frames[0].structures.size(), 2u);
}
