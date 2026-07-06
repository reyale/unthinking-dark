#pragma once
#include <compare>
#include <cstdint>
#include <vector>
#include <cassert>

namespace game {

// 4-connected (von Neumann) grid. Manhattan distance everywhere.
// Coordinates are signed to simplify neighbour arithmetic; valid range is
// [0, width) × [0, height) for a given Map.

struct Vec2 {
  int32_t x{0}, y{0};
  auto operator<=>(const Vec2&) const = default;

  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
};

constexpr int32_t manhattan(Vec2 a, Vec2 b) {
  auto dx = a.x - b.x;
  if (dx < 0)
    dx = -dx;
  auto dy = a.y - b.y;
  if (dy < 0)
    dy = -dy;
  return dx + dy;
}

// 4-connected neighbours in a fixed deterministic order (N, E, S, W).
constexpr Vec2 DIRS[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

enum class Terrain : uint8_t {
  Open = 0,
  Asteroid = 1,     // impassable
  Nebula = 2,       // passable, vision-blocking (v1: no vision effect yet)
  ResourceNode = 3, // passable, harvestable
};

constexpr bool is_passable(Terrain t) {
  return t != Terrain::Asteroid;
}

inline const char* terrain_type_name(Terrain t) {
  switch (t) {
  case Terrain::Open:         return "Open";
  case Terrain::Asteroid:     return "Asteroid";
  case Terrain::Nebula:       return "Nebula";
  case Terrain::ResourceNode: return "Resource Node";
  }
  return "?";
}

struct Tile {
  Terrain terrain{Terrain::Open};
  int32_t resource_amount{0}; // only meaningful for ResourceNode
};

struct Map {
  int32_t width{0};
  int32_t height{0};

  bool in_bounds(Vec2 p) const { return p.x >= 0 && p.x < width && p.y >= 0 && p.y < height; }

  bool passable(Vec2 p) const { return in_bounds(p) && is_passable(tile_at(p).terrain); }

  const Tile& tile_at(Vec2 p) const {
    assert(in_bounds(p));
    return tiles_[static_cast<size_t>(p.y) * static_cast<size_t>(width) + static_cast<size_t>(p.x)];
  }

  Tile& tile_at(Vec2 p) {
    assert(in_bounds(p));
    return tiles_[static_cast<size_t>(p.y) * static_cast<size_t>(width) + static_cast<size_t>(p.x)];
  }

  int32_t passable_tile_count() const { return passable_count_; }

  // Build helpers — used by the map loader.
  static Map make(int32_t w, int32_t h, Terrain fill = Terrain::Open);
  void set_terrain(Vec2 p, Terrain t, int32_t resource_amount = 0);
  void recount_passable();

private:
  std::vector<Tile> tiles_;
  int32_t passable_count_{0};
};

} // namespace game
