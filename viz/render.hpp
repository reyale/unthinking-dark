#pragma once
#include "frame.hpp"
#include "grid.hpp"
#include "wincheck.hpp"
#include "world.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace viz {

enum class CellColor : uint8_t {
  Neutral  = 0, // terrain, empty tiles
  FactionA = 1, // faction 0 units/structures
  FactionB = 2, // faction 1 units/structures
};

struct RenderCell {
  char glyph{'.'};
  CellColor color{CellColor::Neutral};
  bool visible{true}; // false = out of sight: glyph is the (public) terrain, nothing on it
};

struct RenderFrame {
  int32_t width{0};
  int32_t height{0};
  std::vector<RenderCell> cells; // row-major: cells[y * width + x]

  uint32_t tick{0};
  int view{-2}; // -2 = union fog, -1 = god (all visible), 0/1 = faction fog
  size_t frame_idx{0};
  size_t total_frames{0};

  std::array<game::Resources, game::MAX_FACTIONS> resources{};
  std::optional<game::MatchResult> result;
  std::array<game::TickFault, game::MAX_FACTIONS> forfeits{};

  const RenderCell& at(int32_t x, int32_t y) const {
    return cells[static_cast<size_t>(y * width + x)];
  }
};

// Pure function: map game state + view choice → renderable frame data.
// No I/O. Safe to call from tests.
RenderFrame build_render_frame(const game::FrameState& frame, const game::Map& map, int view,
                               size_t frame_idx, size_t total_frames);

} // namespace viz
