#include "statehash.hpp"
#include "world.hpp"
#include "entity.hpp"

#include "xxhash.h" // XXH_INLINE_ALL defined via compile definition in CMake

namespace game {

namespace {

// Accumulate bytes into the state in a defined order.
struct Serializer {
  XXH3_state_t* state;

  void feed(const void* data, size_t len) { XXH3_64bits_update(state, data, len); }

  template <typename T>
  void write(const T& v) {
    feed(&v, sizeof(v));
  }
};

} // namespace

void StateHash::update(const World& world) {
  XXH3_state_t* state = XXH3_createState();
  XXH3_64bits_reset_withSeed(state, current); // chain from previous digest

  Serializer s{state};

  // Canonical order: tick, full RNG state (4 words) + draw_count.
  // Hashing the full state (not just draw_count) ensures different seeds
  // produce different hashes even when no RNG draws occur.
  s.write(world.tick);
  s.feed(world.rng.state_words(), 4 * sizeof(uint64_t));
  s.write(world.rng.draw_count());

  // Units in ascending id order (std::map guarantees this)
  for (const auto& [uid, unit] : world.units) {
    s.write(uid.value);
    s.write(static_cast<uint8_t>(unit.type));
    s.write(unit.faction.value);
    s.write(unit.pos.x);
    s.write(unit.pos.y);
    s.write(unit.hp);
    s.write(unit.shield);
    s.write(unit.shield_wait);
    s.write(static_cast<uint8_t>(unit.current_order));
    s.write(unit.cargo_energy);
    s.write(unit.cargo_alloy);
  }

  // Structures in ascending id order
  for (const auto& [sid, str] : world.structures) {
    s.write(sid.value);
    s.write(static_cast<uint8_t>(str.type));
    s.write(str.faction.value);
    s.write(str.pos.x);
    s.write(str.pos.y);
    s.write(str.hp);
    s.write(str.build_ticks_left);
  }

  // Resources per faction (fixed order 0, 1)
  for (int32_t i = 0; i < MAX_FACTIONS; ++i) {
    s.write(world.resources[static_cast<size_t>(i)].energy);
    s.write(world.resources[static_cast<size_t>(i)].alloy);
  }

  current = XXH3_64bits_digest(state);
  XXH3_freeState(state);

  per_tick.push_back(current);
}

uint64_t StateHash::fingerprint() const {
  return current;
}

} // namespace game
