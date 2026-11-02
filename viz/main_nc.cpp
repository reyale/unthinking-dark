#include "ncurses_out.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "render.hpp"
#include "replay.hpp"
#include "replay_io.hpp"

#include <ncurses.h>
#include <cstdio>

static game::Map map_from_log(const game::ReplayLog& log) {
  game::Map map = game::Map::make(log.map_w, log.map_h);
  for (int32_t y = 0; y < log.map_h; ++y) {
    for (int32_t x = 0; x < log.map_w; ++x) {
      const auto& t = log.map_tiles[static_cast<size_t>(y * log.map_w + x)];
      if (t.terrain != game::Terrain::Open || t.resource_amount != 0)
        map.set_terrain({x, y}, t.terrain, t.resource_amount);
    }
  }
  map.recount_passable();
  return map;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: ud_viz_nc <replay.ud>\n");
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
  game::Map map = map_from_log(log);
  auto frames   = game::replay_frames(log, &replayed);
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
