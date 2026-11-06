#include "profiler.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "match.hpp"
#include "snapshot.hpp"
#include "stats.hpp"
#include "wasm_bot.hpp"
#include <algorithm>
#include <cstdlib>

namespace profiler {

using game::FactionId;
using game::StructureType;
using game::UnitType;
using game::Vec2;
using game::World;

namespace {

constexpr FactionId kBot{0};
constexpr FactionId kOpponent{1};

int32_t max_weapon_range() {
  int32_t r = 0;
  for (auto t : {UnitType::Drone, UnitType::Interceptor, UnitType::Frigate, UnitType::Artillery,
                 UnitType::Battleship})
    r = std::max(r, game::stats_for(t).range);
  return r;
}

// One beyond the longest weapon range: nothing outside the ring can shoot in.
int32_t fortress_radius() { return max_weapon_range() + 1; }

World blank(int32_t w, int32_t h) {
  World world;
  world.map      = game::Map::make(w, h);
  world.rng      = game::Rng{1};
  world.rng_seed = 1;
  return world;
}

// The opponent's Core sits inside an asteroid ring out of every unit's range, so
// the bot can't end a scenario early by destroying it.
void fortified_enemy_core(World& w, Vec2 at) {
  const int32_t r = fortress_radius();
  for (int32_t dy = -r; dy <= r; ++dy)
    for (int32_t dx = -r; dx <= r; ++dx)
      if (std::max(std::abs(dx), std::abs(dy)) == r && w.map.in_bounds({at.x + dx, at.y + dy}))
        w.map.set_terrain({at.x + dx, at.y + dy}, game::Terrain::Asteroid, 0); // map edge walls the rest
  w.map.recount_passable();
  w.spawn_structure(kOpponent, StructureType::CommandCore, at);
}

bool inside_fortress(Vec2 p, Vec2 core) {
  return std::max(std::abs(p.x - core.x), std::abs(p.y - core.y)) <= fortress_radius();
}

constexpr UnitType kCombatRoster[] = {UnitType::Interceptor, UnitType::Frigate,
                                      UnitType::Artillery, UnitType::Battleship};

// Fill a block row by row with `count` combat units, skipping blocked tiles.
void place_block(World& w, FactionId f, Vec2 origin, int32_t cols, uint32_t count,
                 int32_t step = 1, std::function<bool(Vec2)> skip = {}) {
  uint32_t placed = 0;
  for (int32_t i = 0; placed < count && i < w.map.width * w.map.height; ++i) {
    Vec2 p{origin.x + (i % cols) * step, origin.y + (i / cols) * step};
    if (!w.map.in_bounds(p)) continue;
    if (!w.tile_free(p) || (skip && skip(p))) continue;
    w.spawn_unit(f, kCombatRoster[placed % 4], p);
    ++placed;
  }
}

World early_game() {
  World w = blank(32, 32);
  w.map.set_terrain({7, 3}, game::Terrain::ResourceNode, 500);
  w.map.set_terrain({3, 7}, game::Terrain::ResourceNode, 500);
  w.map.recount_passable();
  w.spawn_structure(kBot, StructureType::CommandCore, {3, 3});
  w.spawn_unit(kBot, UnitType::Drone, {4, 3});
  w.spawn_unit(kBot, UnitType::Interceptor, {3, 4});
  w.spawn_unit(kBot, UnitType::Frigate, {4, 4});
  fortified_enemy_core(w, {27, 27});
  w.spawn_unit(kOpponent, UnitType::Drone, {16, 15});
  w.spawn_unit(kOpponent, UnitType::Interceptor, {15, 16});
  w.spawn_unit(kOpponent, UnitType::Frigate, {16, 16});
  return w;
}

// Same scene at every size, on the largest map (the only one whose cap fits the biggest army):
// the bot's army in a block 10 wide, with 20 enemy Battleships in two rows right below it.
// Every enemy is within 3 tiles of a friendly unit (the shortest sight radius), so all 20 are
// in view at every army size.
World army(uint32_t n) {
  constexpr int32_t kCols = 10;
  World w = blank(game::cfg::MAP_MAX_W, game::cfg::MAP_MAX_H);
  w.spawn_structure(kBot, StructureType::CommandCore, {2, 2});
  fortified_enemy_core(w, {game::cfg::MAP_MAX_W - 9, game::cfg::MAP_MAX_H - 9});
  place_block(w, kBot, {8, 8}, kCols, n);
  const int32_t rows = static_cast<int32_t>((n + kCols - 1) / kCols);
  for (int32_t i = 0; i < 20; ++i)
    w.spawn_unit(kOpponent, UnitType::Battleship, {8 + i % kCols, 8 + rows + i / kCols});
  return w;
}

World dense_battle() {
  World w = blank(64, 64);
  w.spawn_structure(kBot, StructureType::CommandCore, {2, 2});
  fortified_enemy_core(w, {58, 58});
  // Checkerboard: every unit has enemies on all sides, everything in view.
  for (int32_t i = 0; i < 200; ++i) {
    Vec2 p{20 + i % 20, 20 + i / 20};
    w.spawn_unit((p.x + p.y) % 2 == 0 ? kBot : kOpponent, kCombatRoster[(i / 2) % 4], p);
  }
  return w;
}

// Battleships (longest sight) on a lattice so the bot sees most of the largest map: the widest
// spacing that still has room for the full unit cap.
void sight_lattice(World& w, Vec2 enemy_core) {
  int32_t step = game::cfg::MAP_MAX_W;
  while (step > 1 && ((w.map.width + step - 1) / step) * ((w.map.height + step - 1) / step) <
                         game::cfg::UNIT_HARD_CAP)
    --step;
  for (int32_t y = step / 2; y < w.map.height; y += step)
    for (int32_t x = step / 2; x < w.map.width; x += step) {
      Vec2 p{x, y};
      if (w.units.size() >= static_cast<size_t>(game::cfg::UNIT_HARD_CAP)) return;
      if (w.tile_free(p) && !inside_fortress(p, enemy_core))
        w.spawn_unit(kBot, UnitType::Battleship, p);
    }
}

World wide_visibility() {
  World w = blank(game::cfg::MAP_MAX_W, game::cfg::MAP_MAX_H);
  const Vec2 enemy_core{game::cfg::MAP_MAX_W - 9, game::cfg::MAP_MAX_H - 9};
  w.spawn_structure(kBot, StructureType::CommandCore, {1, 1});
  fortified_enemy_core(w, enemy_core);
  sight_lattice(w, enemy_core);
  place_block(w, kOpponent, {60, 60}, 5, 10);
  return w;
}

// The literal worst case the rules allow: largest map, full army, nearly all of it
// in view, a full set of structures, and a full enemy army (Battleships, so they survive a
// few volleys) standing next to it, with a full set of enemy structures in view too. The
// structures are Factories: Claim Nodes would hand either side territory and could end the
// scenario early.
World max_legal() {
  World w = blank(game::cfg::MAP_MAX_W, game::cfg::MAP_MAX_H);
  const Vec2 enemy_core{game::cfg::MAP_MAX_W - 9, game::cfg::MAP_MAX_H - 9};
  w.spawn_structure(kBot, StructureType::CommandCore, {1, 1});
  fortified_enemy_core(w, enemy_core);
  sight_lattice(w, enemy_core);
  for (int32_t i = 0, built = 1; built < game::cfg::STRUCTURE_HARD_CAP; ++i) {
    const Vec2 p{4 + 2 * (i % 32), 4 + 2 * (i / 32)}; // spaced so every unit can still move
    if (w.tile_free(p) && !inside_fortress(p, enemy_core)) {
      w.spawn_structure(kBot, StructureType::Factory, p);
      ++built;
    }
  }
  std::vector<Vec2> mine;
  for (const auto& [id, u] : w.units)
    if (u.faction == kBot) mine.push_back(u.pos);
  // Each enemy Factory goes beside one of the bot's units, so all of them are in view.
  int32_t enemy_built = 1; // the fortified Core
  for (const Vec2& p : mine)
    for (const Vec2& d : game::DIRS) {
      if (enemy_built >= game::cfg::STRUCTURE_HARD_CAP) break;
      if (w.tile_free(p + d) && !inside_fortress(p + d, enemy_core)) {
        w.spawn_structure(kOpponent, StructureType::Factory, p + d);
        ++enemy_built;
        break;
      }
    }
  uint32_t enemies = 0;
  for (const Vec2& p : mine)
    for (const Vec2& d : game::DIRS) {
      if (enemies >= static_cast<uint32_t>(game::cfg::UNIT_HARD_CAP)) break;
      if (w.tile_free(p + d) && !inside_fortress(p + d, enemy_core)) {
        w.spawn_unit(kOpponent, UnitType::Battleship, p + d);
        ++enemies;
        break;
      }
    }
  return w;
}

std::vector<Scenario> make_suite() {
  std::vector<Scenario> s;
  s.push_back({"early_game", "standard opening, 3 units; fixed per-tick overhead", 60, early_game});
  for (uint32_t n : {10u, 50u, 200u, static_cast<uint32_t>(game::cfg::UNIT_HARD_CAP)})
    s.push_back({"army_scaling/" + std::to_string(n),
                 std::to_string(n) + " units vs 20 visible enemies", 30, [n] { return army(n); }});
  s.push_back({"dense_battle", "100 vs 100 interleaved, all in view", 30, dense_battle});
  s.push_back({"wide_visibility", "largest map, Battleship lattice sees nearly all of it", 10,
               wide_visibility});
  s.push_back({"max_legal", "largest map, full army and bases, full enemy army and bases in view",
               10, max_legal});
  s.push_back({"long_running", "standard opening for 2000 ticks; watch memory for leaks", 2000,
               early_game});
  return s;
}

// Forwards to the real bot and keeps every call's metrics.
class Metered : public game::Bot {
public:
  explicit Metered(game::Bot& inner) : inner_(inner) {}
  void on_init(const game::Map& map, uint32_t faction, uint64_t seed) override {
    inner_.on_init(map, faction, seed);
    init = inner_.last_metrics();
  }
  std::vector<game::Command> on_tick(const game::Snapshot& snap) override {
    auto cmds = inner_.on_tick(snap);
    const auto m = inner_.last_metrics();
    ticks.push_back({m.fuel_consumed, m.memory_bytes, m.fault});
    return cmds;
  }
  bool healthy() const override { return inner_.healthy(); }
  game::BotMetrics last_metrics() const override { return inner_.last_metrics(); }

  game::BotMetrics        init{};
  std::vector<TickSample> ticks;

private:
  game::Bot& inner_;
};

} // namespace

const std::vector<Scenario>& scenarios() {
  static const std::vector<Scenario> suite = make_suite();
  return suite;
}

const Scenario* find_scenario(std::string_view name) {
  for (const auto& s : scenarios())
    if (s.name == name) return &s;
  return nullptr;
}

Report run_scenario(const std::vector<uint8_t>& wasm, const Scenario& s) {
  Report r;
  r.name = s.name;
  r.what = s.what;

  World world = s.build();
  const game::Snapshot first = game::build_snapshot(world, kBot.value, s.ticks);
  r.my_units        = static_cast<uint32_t>(first.my_units.size());
  r.visible_enemies = static_cast<uint32_t>(first.visible_enemies.size());
  r.visible_tiles   = static_cast<uint32_t>(first.visible_tiles.size());

  runner::WasmBot bot(wasm);
  if (!bot.healthy()) {
    r.error = bot.last_error();
    return r;
  }
  Metered metered(bot);
  PassiveBot opponent;
  game::run_match(world, metered, kBot.value, opponent, kOpponent.value, s.ticks);

  r.init  = metered.init;
  r.ticks = std::move(metered.ticks);
  if (!bot.healthy() && r.ticks.empty()) {
    r.error = "init failed: " + bot.last_error();
    return r;
  }
  r.loaded      = true;
  r.peak_memory = r.init.memory_bytes;
  for (uint32_t i = 0; i < r.ticks.size(); ++i) {
    const auto& t = r.ticks[i];
    r.total_fuel += t.fuel;
    if (t.fuel > r.worst_fuel) { r.worst_fuel = t.fuel; r.worst_tick = i; }
    r.peak_memory = std::max(r.peak_memory, t.memory);
    switch (t.fault) {
    case game::TickFault::None:        break;
    case game::TickFault::OutOfFuel:   ++r.fuel_forfeits; break;
    case game::TickFault::OutOfMemory: ++r.memory_forfeits; break;
    case game::TickFault::Trap:        ++r.trap_forfeits; break;
    }
  }
  return r;
}

} // namespace profiler
