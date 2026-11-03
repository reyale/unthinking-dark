#include "render.hpp"
#include "entity.hpp"
#include "config.hpp"
#include "vision.hpp"
#include <cstdlib>

namespace viz {

namespace {

char terrain_glyph(game::Terrain t) {
  switch (t) {
    case game::Terrain::Asteroid:     return '#';
    case game::Terrain::Nebula:       return '~';
    case game::Terrain::ResourceNode: return '*';
    default:                          return '.';
  }
}

char unit_glyph(game::UnitType t) {
  switch (t) {
    case game::UnitType::Drone:       return 'd';
    case game::UnitType::Interceptor: return 'i';
    case game::UnitType::Frigate:     return 'f';
    case game::UnitType::Artillery:   return 'a';
    case game::UnitType::Battleship:  return 'b';
  }
  return '?';
}

char structure_glyph(game::StructureType t) {
  switch (t) {
    case game::StructureType::CommandCore: return 'C';
    case game::StructureType::Factory:     return 'T';
    case game::StructureType::ClaimNode:   return 'N';
  }
  return '?';
}

CellColor faction_color(uint32_t faction) {
  return faction == 0 ? CellColor::FactionA : CellColor::FactionB;
}

} // namespace

RenderFrame build_render_frame(const game::FrameState& frame, const game::Map& map, int view,
                               size_t frame_idx, size_t total_frames) {
  int32_t W = map.width, H = map.height;

  RenderFrame rf;
  rf.width = W;
  rf.height = H;
  rf.tick = frame.tick;
  rf.view = view;
  rf.frame_idx = frame_idx;
  rf.total_frames = total_frames;
  rf.resources = frame.resources;
  rf.result = frame.result;
  rf.cells.resize(static_cast<size_t>(W * H));

  // Fill terrain
  for (int32_t y = 0; y < H; ++y) {
    for (int32_t x = 0; x < W; ++x) {
      auto& cell = rf.cells[static_cast<size_t>(y * W + x)];
      cell.glyph = terrain_glyph(map.tile_at({x, y}).terrain);
      cell.color = CellColor::Neutral;
      cell.visible = (view == -1); // god view: all visible initially
    }
  }

  // Compute fog visibility
  if (view != -1) {
    // Reveal tiles in sight of each relevant unit.
    for (const auto& uf : frame.units) {
      if (view >= 0 && static_cast<int>(uf.faction.value) != view)
        continue;
      int32_t sight = game::stats_for(uf.type).sight;
      for (int32_t dy = -sight; dy <= sight; ++dy) {
        for (int32_t dx = -sight; dx <= sight; ++dx) {
          if (std::abs(dx) + std::abs(dy) > sight)
            continue;
          int32_t nx = uf.pos.x + dx, ny = uf.pos.y + dy;
          if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
              game::sees_tile(map, uf.pos, sight, {nx, ny}))
            rf.cells[static_cast<size_t>(ny * W + nx)].visible = true;
        }
      }
    }
    // Own structures are always visible in their faction's view, and see around them.
    for (const auto& sf : frame.structures) {
      if (view >= 0 && static_cast<int>(sf.faction.value) != view)
        continue;
      rf.cells[static_cast<size_t>(sf.pos.y * W + sf.pos.x)].visible = true;
      if (sf.hp <= 0)
        continue;
      const int32_t sight = game::cfg::STRUCTURE_SIGHT;
      for (int32_t dy = -sight; dy <= sight; ++dy)
        for (int32_t dx = -sight; dx <= sight; ++dx) {
          const int32_t nx = sf.pos.x + dx, ny = sf.pos.y + dy;
          if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
              game::sees_tile(map, sf.pos, sight, {nx, ny}))
            rf.cells[static_cast<size_t>(ny * W + nx)].visible = true;
        }
    }
  }

  // Place structures, then units (units overwrite if colocated)
  for (const auto& sf : frame.structures) {
    size_t idx = static_cast<size_t>(sf.pos.y * W + sf.pos.x);
    if (!rf.cells[idx].visible)
      continue;
    const char g = structure_glyph(sf.type);
    rf.cells[idx].glyph = sf.build_ticks_left > 0 ? static_cast<char>(g - 'A' + 'a') : g;
    rf.cells[idx].color = faction_color(sf.faction.value);
  }
  for (const auto& uf : frame.units) {
    size_t idx = static_cast<size_t>(uf.pos.y * W + uf.pos.x);
    if (!rf.cells[idx].visible)
      continue;
    rf.cells[idx].glyph = uf.carrying() ? 'D' : unit_glyph(uf.type);
    rf.cells[idx].color = faction_color(uf.faction.value);
  }

  return rf;
}

} // namespace viz
