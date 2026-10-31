#include "ncurses_out.hpp"
#include <ncurses.h>
#include <cstdio>
#include <cstring>

namespace viz {

// Color pair indices
static constexpr int CP_NEUTRAL  = 1; // dim white — terrain
static constexpr int CP_FOG      = 2; // very dim — fogged cells
static constexpr int CP_A        = 3; // cyan — faction 0
static constexpr int CP_B        = 4; // red  — faction 1
static constexpr int CP_HUD      = 5; // white bold — headers / borders
static constexpr int CP_WIN      = 6; // yellow — game-over banner

void init_ncurses() {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);

  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(CP_NEUTRAL, COLOR_WHITE,   -1);
    init_pair(CP_FOG,     COLOR_BLACK,   -1);
    init_pair(CP_A,       COLOR_CYAN,    -1);
    init_pair(CP_B,       COLOR_RED,     -1);
    init_pair(CP_HUD,     COLOR_WHITE,   -1);
    init_pair(CP_WIN,     COLOR_YELLOW,  -1);
  }
}

void cleanup_ncurses() {
  endwin();
}

static int color_attr(CellColor c) {
  switch (c) {
    case CellColor::FactionA: return COLOR_PAIR(CP_A) | A_BOLD;
    case CellColor::FactionB: return COLOR_PAIR(CP_B) | A_BOLD;
    default:                  return COLOR_PAIR(CP_NEUTRAL);
  }
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

void draw_ncurses_frame(const RenderFrame& rf) {
  erase();

  int screen_rows = LINES;
  int screen_cols = COLS;

  // ---- HUD: row 0 ----------------------------------------------------------
  attron(COLOR_PAIR(CP_HUD) | A_BOLD);
  const char* view_label = (rf.view < 0) ? "God"
                         : (rf.view == 0) ? "Faction 0"
                                          : "Faction 1";
  mvprintw(0, 0, "Tick %-5u  View: %-10s  Frame %zu/%zu",
           rf.tick, view_label, rf.frame_idx + 1, rf.total_frames);
  attroff(COLOR_PAIR(CP_HUD) | A_BOLD);

  // Resources row 1
  attron(COLOR_PAIR(CP_A) | A_BOLD);
  mvprintw(1, 0, "F0: %4de %4da", rf.resources[0].energy, rf.resources[0].alloy);
  attroff(COLOR_PAIR(CP_A) | A_BOLD);
  addstr("   ");
  attron(COLOR_PAIR(CP_B) | A_BOLD);
  printw("F1: %4de %4da", rf.resources[1].energy, rf.resources[1].alloy);
  attroff(COLOR_PAIR(CP_B) | A_BOLD);

  // ---- Map border: starts at row 2 -----------------------------------------
  const int map_top  = 2;
  const int map_left = 0;

  // Map fits within screen; clamp draw region
  int draw_w = std::min(rf.width,  screen_cols - map_left - 2); // -2 for borders
  int draw_h = std::min(rf.height, screen_rows - map_top  - 5); // -5 for HUD+legend

  if (draw_w <= 0 || draw_h <= 0) {
    mvprintw(map_top, 0, "Terminal too small (%dx%d) for %dx%d map",
             screen_cols, screen_rows, rf.width, rf.height);
    refresh();
    return;
  }

  // Top border
  attron(COLOR_PAIR(CP_HUD));
  mvaddch(map_top, map_left, '+');
  for (int x = 0; x < draw_w; ++x) addch('-');
  addch('+');
  attroff(COLOR_PAIR(CP_HUD));

  // Map rows
  for (int y = 0; y < draw_h; ++y) {
    int row = map_top + 1 + y;
    attron(COLOR_PAIR(CP_HUD));
    mvaddch(row, map_left, '|');
    attroff(COLOR_PAIR(CP_HUD));

    for (int x = 0; x < draw_w; ++x) {
      const auto& cell = rf.at(x, y);
      if (!cell.visible) {
        attron(COLOR_PAIR(CP_FOG));
        addch(' ');
        attroff(COLOR_PAIR(CP_FOG));
      } else {
        int attr = color_attr(cell.color);
        attron(attr);
        addch(static_cast<chtype>(cell.glyph));
        attroff(attr);
      }
    }

    attron(COLOR_PAIR(CP_HUD));
    addch('|');
    attroff(COLOR_PAIR(CP_HUD));
  }

  // Bottom border
  int bot_row = map_top + 1 + draw_h;
  attron(COLOR_PAIR(CP_HUD));
  mvaddch(bot_row, map_left, '+');
  for (int x = 0; x < draw_w; ++x) addch('-');
  addch('+');
  attroff(COLOR_PAIR(CP_HUD));

  // ---- Game-over banner ----------------------------------------------------
  if (rf.result) {
    attron(COLOR_PAIR(CP_WIN) | A_BOLD);
    if (rf.result->reason == game::WinReason::Draw)
      mvprintw(bot_row + 1, 0, "Game over: Draw (%s)", win_reason_str(rf.result->reason));
    else
      mvprintw(bot_row + 1, 0, "Game over: Faction %u wins (%s)",
               rf.result->winner.value, win_reason_str(rf.result->reason));
    attroff(COLOR_PAIR(CP_WIN) | A_BOLD);
  }

  // ---- Legend --------------------------------------------------------------
  int leg_row = screen_rows - 2;
  if (leg_row > bot_row + 2)
    leg_row = bot_row + 2;

  attron(COLOR_PAIR(CP_HUD));
  mvprintw(leg_row, 0,
    "[n/SPC] next  [p] prev  [g] god  [0] F0 fog  [1] F1 fog  [q] quit");
  mvprintw(leg_row + 1, 0,
    "d=drone D=carrying i=interceptor f=frigate a=artillery  C=core T=factory N=claim n=building");
  attroff(COLOR_PAIR(CP_HUD));

  refresh();
}

} // namespace viz
