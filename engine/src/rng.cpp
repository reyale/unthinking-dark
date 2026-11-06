#include "rng.hpp"

namespace {
constexpr uint64_t rotl(uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}
} // namespace

namespace game {

uint64_t Rng::splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

Rng::Rng(uint64_t seed) {
  s[0] = splitmix64(seed);
  s[1] = splitmix64(seed);
  s[2] = splitmix64(seed);
  s[3] = splitmix64(seed);
}

uint64_t Rng::next() {
  ++draw_count_;
  const uint64_t result = rotl(s[1] * 5, 7) * 9;
  const uint64_t t = s[1] << 17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = rotl(s[3], 45);
  return result;
}

uint32_t Rng::next_u32(uint32_t bound) {
  if (bound <= 1)
    return 0;
  // Fast-range (Lemire): slight bias acceptable for game use.
  return static_cast<uint32_t>(
    (static_cast<uint64_t>(static_cast<uint32_t>(next())) * uint64_t{bound}) >> 32);
}

uint64_t bot_seed(uint64_t match_seed, uint32_t faction) {
  uint64_t x = match_seed ^ (0xa0761d6478bd642full * (uint64_t{faction} + 1));
  return Rng::splitmix64(x);
}

} // namespace game
