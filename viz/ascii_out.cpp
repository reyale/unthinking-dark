#include "ascii_out.hpp"
#include <cstdio>

namespace viz {

namespace {

static const char* RESET  = "\033[0m";
static const char* CYAN   = "\033[36m";
static const char* RED    = "\033[31m";
static const char* GRAY   = "\033[90m";
static const char* YELLOW = "\033[33m";
static const char* CLEAR  = "\033[2J\033[H";

static const char* ansi_color(CellColor c) {
  switch (c) {
    case CellColor::Neutral:  return GRAY;
    case CellColor::FactionA: return CYAN;
    case CellColor::FactionB: return RED;
  }
  return RESET;
}

static const char* win_reason_str(game::WinReason r) {
  switch (r) {
    case game::WinReason::BaseDestroyed:      return "base destroyed";
    case game::WinReason::TerritoryThreshold: return "territory";
    case game::WinReason::TickCap:            return "tick cap";
    case game::WinReason::TieBreakLadder:     return "tie-break";
    case game::WinReason::Draw:               return "draw";
  }
  return "?";
}

} // namespace

void print_ascii_frame(const RenderFrame& rf) {
  printf("%s", CLEAR);

  const char* view_label = (rf.view == -1) ? "God"
                         : (rf.view == -2) ? "Union fog"
                         : (rf.view == 0)  ? "Faction 0 (cyan)"
                                           : "Faction 1 (red)";
  printf("Tick %-4u  %s view  [%zu/%zu]\n",
         rf.tick, view_label, rf.frame_idx + 1, rf.total_frames);
  printf("%sF0%s: %de %da   %sF1%s: %de %da\n",
         CYAN, RESET, rf.resources[0].energy, rf.resources[0].alloy,
         RED,  RESET, rf.resources[1].energy, rf.resources[1].alloy);

  printf("+");
  for (int32_t x = 0; x < rf.width; ++x) putchar('-');
  printf("+\n");

  for (int32_t y = 0; y < rf.height; ++y) {
    putchar('|');
    for (int32_t x = 0; x < rf.width; ++x) {
      const auto& cell = rf.at(x, y);
      if (!cell.visible) {
        putchar(' ');
      } else {
        printf("%s%c%s", ansi_color(cell.color), cell.glyph, RESET);
      }
    }
    printf("|\n");
  }

  printf("+");
  for (int32_t x = 0; x < rf.width; ++x) putchar('-');
  printf("+\n");

  if (rf.result) {
    if (rf.result->reason == game::WinReason::Draw)
      printf("%sGame over: Draw (%s)%s\n", YELLOW, win_reason_str(rf.result->reason), RESET);
    else
      printf("%sGame over: Faction %u wins (%s)%s\n", YELLOW,
             rf.result->winner.value, win_reason_str(rf.result->reason), RESET);
  } else {
    printf("\n");
  }

  printf("[n/space] next  [p] prev  [u] union  [g] god  [0] faction 0  [1] faction 1  [q] quit\n");
  printf("%sd%s drone (%sD%s carrying)  %si%s interceptor  %sf%s frigate  %sa%s artillery  "
         "%sC%s core  %sT%s factory  %sN%s claim (lower case: building)\n",
         CYAN, RESET, CYAN, RESET, CYAN, RESET, CYAN, RESET, CYAN, RESET,
         CYAN, RESET, CYAN, RESET, CYAN, RESET);
  fflush(stdout);
}

} // namespace viz
