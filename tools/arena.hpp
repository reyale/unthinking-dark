#pragma once
#include "entity.hpp"
#include "world.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The balance arena: two armies of equal cost meet on an open field and fight to the last ship,
// both sides played by the same simple commander (advance, focus fire). What survives, weighed
// by cost, says which ships and mixes are worth their price.
namespace arena {

inline constexpr std::array<game::UnitType, 4> kShips = {
    game::UnitType::Interceptor, game::UnitType::Frigate, game::UnitType::Artillery,
    game::UnitType::Battleship};

// A ship's price as one number: energy plus alloy weighted by how much scarcer alloy is to
// harvest (HARVEST_ENERGY / HARVEST_ALLOY per Drone per tick).
int32_t value(game::UnitType t);

struct Army {
  std::array<int32_t, 4> count{}; // per kShips
  int32_t value() const;
  std::string name() const; // e.g. "15I 6F 0A 2B"
};

// Every way to split `budget` over the four ships in `parts` equal shares (a share buys as
// many of one ship as it affords), so all mixes cost about the same.
std::vector<Army> compositions(int32_t budget, int32_t parts);
Army pure(game::UnitType t, int32_t budget);

struct Setup {
  int32_t tick_limit{400};
  int32_t spacing{1}; // between neighbours in a block: 1 packs them tight; splash likes that
};

// The battlefield: open, mirrored left to right, armies `kGap` apart in blocks `kRanks` wide,
// shortest range at the front. Each side's Command Core sits in a corner behind its army, out of
// the fight, only so the match can't end on base destruction.
game::World battlefield(const Army& a, const Army& b, const Setup& s);

struct Outcome {
  int32_t start_a{0}, start_b{0}; // army value
  int32_t left_a{0}, left_b{0};   // value still alive, each ship scaled by hull left
  int32_t ticks{0};
  // Share of A's army left minus share of B's, in [-1, 1]: > 0 means A won.
  double margin() const;
};

// Fights until one side has no ships, or the tick limit.
Outcome fight(const Army& a, const Army& b, const Setup& s);

// Both ways round (each army once as faction 0, which spawns first and so gets the lower ids:
// they decide movement's right of way), as `x`'s margin, averaged.
double score(const Army& x, const Army& y, const Setup& s);

// The same fight as `fight`, written as a replay the viewers can play.
void write_replay(const Army& a, const Army& b, const Setup& s, const std::string& path);

} // namespace arena
