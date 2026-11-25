#include "ncurses_out.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "render.hpp"
#include "replay.hpp"
#include "replay_io.hpp"
#include "version.hpp"

#include <ncurses.h>
#include <cstdio>

int main(int argc, char** argv) {
  if (game::print_version_if_asked(argc, argv, "ud_viz_nc")) return 0;
  if (argc < 2) {
    fprintf(stderr, "usage: ud_viz_nc <replay.ud> | --version\n");
    return 1;
  }

  game::ReplayLog log;
  try {
    log = game::read_replay_file(argv[1]);
  } catch (const game::ReplayVersionError& e) {
    fprintf(stderr, "error: %s\n       This replay requires a different engine build.\n",
            e.what());
    return 1;
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }

  fprintf(stderr, "Replay: %zu ticks, %dx%d map — re-simulating...\n",
          log.ticks.size(), log.map_w, log.map_h);

  uint64_t replayed = 0;
  game::Map map;
  std::vector<game::FrameState> frames;
  try {
    map    = game::replay_map(log);
    frames = game::replay_frames(log, &replayed);
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  if (replayed != log.expected_hash)
    fprintf(stderr, "warning: hash mismatch — replay may not reproduce the original match\n");
  if (frames.empty()) {
    fprintf(stderr, "error: empty replay\n");
    return 1;
  }

  fprintf(stderr, "Collected %zu frames.\n", frames.size());

  viz::init_ncurses();

  size_t idx  = 0;
  int    view = -2;

  auto show = [&] {
    auto rf = viz::build_render_frame(frames[idx], map, view, idx, frames.size());
    viz::draw_ncurses_frame(rf);
  };

  show();

  while (true) {
    int ch = getch();
    bool changed = true;
    switch (ch) {
      case 'q': case 'Q': goto done;
      case 'n': case ' ':
        if (idx + 1 < frames.size()) ++idx;
        break;
      case 'p':
        if (idx > 0) --idx;
        break;
      case 'u': view = -2; break;
      case 'g': view = -1; break;
      case '0': view = 0;  break;
      case '1': view = 1;  break;
      case KEY_RIGHT: case KEY_DOWN:
        if (idx + 1 < frames.size()) ++idx;
        break;
      case KEY_LEFT: case KEY_UP:
        if (idx > 0) --idx;
        break;
      default: changed = false; break;
    }
    if (changed) show();
  }

done:
  viz::cleanup_ncurses();
  return 0;
}
