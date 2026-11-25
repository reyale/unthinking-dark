#include "map_io.hpp"
#include "config.hpp"
#include <nlohmann/json.hpp>
#include <xxhash.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace maps {

using json = nlohmann::json;

static std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

uint64_t compute_map_hash(const MapData& data) {
  const int32_t w = data.map.width;
  const int32_t h = data.map.height;

  std::vector<uint8_t> buf;
  buf.reserve(static_cast<size_t>(7 * 4 + w * h * 5));

  auto push32 = [&](int32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    buf.insert(buf.end(), b, b + 4);
  };

  push32(w);
  push32(h);
  push32(data.spawn[0].x);
  push32(data.spawn[0].y);
  push32(data.spawn[1].x);
  push32(data.spawn[1].y);
  push32(data.resource_amount);

  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const auto& tile = data.map.tile_at({x, y});
      buf.push_back(static_cast<uint8_t>(tile.terrain));
      uint8_t b[4];
      std::memcpy(b, &tile.resource_amount, 4);
      buf.insert(buf.end(), b, b + 4);
    }
  }

  return XXH3_64bits(buf.data(), buf.size());
}

MapData load_map(const std::string& path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("cannot open map file: " + path);

  json j;
  try {
    j = json::parse(f);
  } catch (const json::exception& e) {
    throw std::runtime_error(std::string("JSON parse error in ") + path + ": " + e.what());
  }

  MapData data;
  data.name            = j.value("name", std::string{});
  data.resource_amount = j.value("resource_amount", 500);
  if (data.resource_amount < 0)
    throw std::runtime_error("negative resource_amount in map: " + path);

  if (!j.contains("grid") || !j["grid"].is_array())
    throw std::runtime_error("map file missing 'grid' array: " + path);

  const auto& grid = j["grid"];
  if (grid.empty())
    throw std::runtime_error("grid is empty: " + path);
  if (grid.size() > static_cast<size_t>(game::cfg::MAP_MAX_H))
    throw std::runtime_error("map height exceeds maximum in: " + path);

  int32_t h = static_cast<int32_t>(grid.size());
  int32_t w = -1;

  bool spawn0_set = false;
  bool spawn1_set = false;

  for (int32_t y = 0; y < h; ++y) {
    if (!grid[static_cast<size_t>(y)].is_string())
      throw std::runtime_error("grid row is not a string at row " + std::to_string(y));
    std::string row = grid[static_cast<size_t>(y)].get<std::string>();

    if (row.empty() || row.size() > static_cast<size_t>(game::cfg::MAP_MAX_W))
      throw std::runtime_error("map width out of bounds at row " + std::to_string(y) + " in: " +
                               path);
    int32_t row_w = static_cast<int32_t>(row.size());
    if (w == -1) {
      w = row_w;
      data.map = game::Map::make(w, h, game::Terrain::Open);
    } else if (row_w != w) {
      throw std::runtime_error(
          "inconsistent row width at row " + std::to_string(y) + ": expected " +
          std::to_string(w) + ", got " + std::to_string(row_w));
    }

    for (int32_t x = 0; x < w; ++x) {
      char c = row[static_cast<size_t>(x)];
      game::Vec2 p{x, y};
      switch (c) {
        case '.': break;
        case '#': data.map.set_terrain(p, game::Terrain::Asteroid); break;
        case '~': data.map.set_terrain(p, game::Terrain::Nebula); break;
        case 'R':
          data.map.set_terrain(p, game::Terrain::ResourceNode, data.resource_amount);
          break;
        case '1':
          if (spawn0_set)
            throw std::runtime_error("duplicate spawn '1' in map: " + path);
          spawn0_set   = true;
          data.spawn[0] = p;
          break;
        case '2':
          if (spawn1_set)
            throw std::runtime_error("duplicate spawn '2' in map: " + path);
          spawn1_set   = true;
          data.spawn[1] = p;
          break;
        default:
          throw std::runtime_error(
              std::string("unknown character '") + c + "' at (" + std::to_string(x) + "," +
              std::to_string(y) + ") in: " + path);
      }
    }
  }

  if (!spawn0_set)
    throw std::runtime_error("missing spawn '1' in map: " + path);
  if (!spawn1_set)
    throw std::runtime_error("missing spawn '2' in map: " + path);

  data.map.recount_passable();
  data.hash = compute_map_hash(data);

  if (j.contains("hash")) {
    if (!j["hash"].is_string())
      throw std::runtime_error("map 'hash' must be a hex string: " + path);
    const std::string hs = j["hash"].get<std::string>();
    size_t parsed = 0;
    uint64_t stored = 0;
    try {
      stored = std::stoull(hs, &parsed, 16);
    } catch (const std::exception&) {
      throw std::runtime_error("invalid map 'hash' format: " + path);
    }
    if (hs.empty() || parsed != hs.size())
      throw std::runtime_error("invalid map 'hash' format: " + path);
    if (stored != data.hash)
      throw std::runtime_error("map hash mismatch (file may be corrupted): " + path);
  }

  return data;
}

void save_map(const MapData& data, const std::string& path) {
  int32_t w = data.map.width;
  int32_t h = data.map.height;

  std::vector<std::string> grid;
  grid.reserve(static_cast<size_t>(h));

  for (int32_t y = 0; y < h; ++y) {
    std::string row;
    row.reserve(static_cast<size_t>(w));
    for (int32_t x = 0; x < w; ++x) {
      game::Vec2 p{x, y};
      if (p == data.spawn[0]) {
        row += '1';
      } else if (p == data.spawn[1]) {
        row += '2';
      } else {
        switch (data.map.tile_at(p).terrain) {
          case game::Terrain::Open:         row += '.'; break;
          case game::Terrain::Asteroid:     row += '#'; break;
          case game::Terrain::Nebula:       row += '~'; break;
          case game::Terrain::ResourceNode: row += 'R'; break;
        }
      }
    }
    grid.push_back(std::move(row));
  }

  json j;
  j["name"]            = data.name;
  j["resource_amount"] = data.resource_amount;
  j["hash"]            = hex64(compute_map_hash(data));
  j["grid"]            = grid;

  std::ofstream f(path);
  if (!f)
    throw std::runtime_error("cannot write map file: " + path);
  f << j.dump(2) << '\n';
}

} // namespace maps
