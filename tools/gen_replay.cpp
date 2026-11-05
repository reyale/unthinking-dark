// Generates a demo replay file for manual visualizer testing.
// Usage: gen_replay <output.ud>
#include "entity.hpp"
#include "grid.hpp"
#include "match.hpp"
#include "replay_io.hpp"
#include "world.hpp"
#include "../../fixtures/idle_bot.hpp"
#include "version.hpp"

#include <cstdio>

int main(int argc, char** argv) {
  if (game::print_version_if_asked(argc, argv, "gen_example_replay")) return 0;
  const char* path = (argc >= 2) ? argv[1] : "demo.ud";

  game::World w;
  w.map = game::Map::make(20, 20);
  w.rng = game::Rng{42};
  w.rng_seed = 42;

  // Faction 0 — top-left
  w.spawn_structure({0}, game::StructureType::CommandCore, {2, 2});
  w.spawn_unit({0}, game::UnitType::Drone, {3, 2});
  w.spawn_unit({0}, game::UnitType::Interceptor, {4, 2});
  w.spawn_unit({0}, game::UnitType::Frigate, {5, 2});

  // Faction 1 — bottom-right
  w.spawn_structure({1}, game::StructureType::CommandCore, {17, 17});
  w.spawn_unit({1}, game::UnitType::Drone, {16, 17});
  w.spawn_unit({1}, game::UnitType::Interceptor, {15, 17});
  w.spawn_unit({1}, game::UnitType::Artillery, {14, 17});

  // Some terrain
  w.map.set_terrain({10, 8},  game::Terrain::Asteroid);
  w.map.set_terrain({10, 9},  game::Terrain::Asteroid);
  w.map.set_terrain({10, 10}, game::Terrain::Asteroid);
  w.map.set_terrain({10, 11}, game::Terrain::Asteroid);
  w.map.set_terrain({10, 12}, game::Terrain::Asteroid);
  w.map.set_terrain({8,  10}, game::Terrain::Nebula);
  w.map.set_terrain({5,  14}, game::Terrain::ResourceNode, 500);
  w.map.set_terrain({14, 5},  game::Terrain::ResourceNode, 500);
  w.map.recount_passable();

  game::IdleBot a, b;
  auto rec = game::run_match(w, a, 0, b, 1, 100, "player_a", "player_b");
  printf("Match: %u ticks, faction %u wins\n", rec.ticks_played, rec.outcome.winner.value);

  try {
    game::write_replay_file(rec.replay, path);
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  printf("Written: %s\n", path);
  return 0;
}
