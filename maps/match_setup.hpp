#pragma once
#include "map_io.hpp"
#include "world.hpp"
#include <cstdint>

namespace maps {

// Standard match start: a Command Core on each spawn point plus a Drone,
// Interceptor and Frigate on free tiles next to it, biased toward the map centre.
game::World world_from_map(const MapData& md, uint64_t seed);

} // namespace maps
