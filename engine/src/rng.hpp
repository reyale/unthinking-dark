#pragma once
#include <cstdint>

// xoshiro256** — fast, high-quality 64-bit PRNG.
// One instance per match, seeded from the match seed. All draws MUST happen
// in a fixed, deterministic order (combat: ascending unit-id). draw_count()
// is included in the state hash so any ordering violation is detectable.
//
// Why not std::mt19937_64?
// The engine type (std::mt19937_64) is fully specified by the standard and
// would be deterministic. The problem is std::uniform_int_distribution: the
// standard does not mandate an algorithm for it, so the same seed produces
// different sequences across libstdc++, libc++, and MSVC. We would still need
// a hand-rolled bounded-draw function regardless. xoshiro256** is also ~2.5x
// faster than MT19937 and uses 32 bytes of state vs 2.5 KB.
namespace game {

class Rng {
public:
  Rng() : Rng(0) {}
  explicit Rng(uint64_t seed);

  uint64_t next();

  // Uniform draw in [0, bound). Callers must draw in ascending unit-id order.
  uint32_t next_u32(uint32_t bound);

  uint64_t draw_count() const { return draw_count_; }

  // Expose raw state for canonical hashing — different seeds = different words.
  const uint64_t* state_words() const { return s; }

private:
  uint64_t s[4];
  uint64_t draw_count_{0};

  static uint64_t splitmix64(uint64_t& x);
  friend uint64_t bot_seed(uint64_t match_seed, uint32_t faction);
};

// The seed a bot gets at init (Bot::on_init): its own, drawn from the match seed and its
// faction, so a replay's seed gives the same bots the same seeds, and the two sides of a
// mirror match don't get one between them. Bots may prime their own RNG with it, or not.
// Nothing in the engine draws from it.
uint64_t bot_seed(uint64_t match_seed, uint32_t faction);

} // namespace game
