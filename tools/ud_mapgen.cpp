#include "map_gen.hpp"
#include "map_io.hpp"
#include "rng.hpp"
#include "version.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>

int main(int argc, char* argv[]) {
  if (game::print_version_if_asked(argc, argv, "ud_mapgen")) return 0;
  maps::GenParams params;
  uint64_t    seed     = 0;
  std::string out_path;

  for (int i = 1; i < argc; ++i) {
    auto next_arg = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << argv[i] << "\n";
        std::exit(1);
      }
      return argv[++i];
    };

    if (std::strcmp(argv[i], "--width") == 0) {
      params.width = std::atoi(next_arg());
    } else if (std::strcmp(argv[i], "--height") == 0) {
      params.height = std::atoi(next_arg());
    } else if (std::strcmp(argv[i], "--seed") == 0) {
      seed = static_cast<uint64_t>(std::strtoull(next_arg(), nullptr, 10));
    } else if (std::strcmp(argv[i], "--resource-amount") == 0) {
      params.resource_amount = std::atoi(next_arg());
    } else if (std::strcmp(argv[i], "--name") == 0) {
      params.name = next_arg();
    } else if (std::strcmp(argv[i], "--out") == 0) {
      out_path = next_arg();
    } else {
      std::cerr << "unknown argument: " << argv[i] << "\n";
      return 1;
    }
  }

  if (out_path.empty()) {
    std::cerr << "usage: ud_mapgen [--width W] [--height H] [--seed N] "
                 "[--resource-amount R] [--name NAME] [--version] --out FILE\n";
    return 1;
  }

  game::Rng rng(seed);
  maps::MapData result;
  try {
    result = maps::generate_map(params, rng);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  maps::save_map(result, out_path);

  int32_t resource_count = 0;
  for (int32_t y = 0; y < result.map.height; ++y)
    for (int32_t x = 0; x < result.map.width; ++x)
      if (result.map.tile_at({x, y}).terrain == game::Terrain::ResourceNode)
        ++resource_count;

  std::cout << "Generated " << result.name << " (" << params.width << "x" << params.height
            << ", " << result.map.passable_tile_count() << " passable tiles, "
            << resource_count << " resource nodes)\n";
  return 0;
}
