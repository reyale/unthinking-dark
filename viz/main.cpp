#include "ascii_out.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "render.hpp"
#include "replay.hpp"
#include "replay_io.hpp"
#include "version.hpp"

#include <cstdio>
#include <termios.h>
#include <unistd.h>

// ---- Terminal raw mode ------------------------------------------------------

struct RawMode {
  termios saved{};
  RawMode() {
    tcgetattr(STDIN_FILENO, &saved);
    termios t = saved;
    t.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
  }
  ~RawMode() { tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
};

// ---- Main -------------------------------------------------------------------

int main(int argc, char** argv) {
  if (game::print_version_if_asked(argc, argv, "ud_viz")) return 0;
  if (argc < 2) {
    fprintf(stderr, "usage: ud_viz <replay.ud> | --version\n");
    fprintf(stderr, "  Glyphs: d=drone  i=interceptor  f=frigate  a=artillery\n");
    fprintf(stderr, "          C=core   T=factory      N=claim\n");
    fprintf(stderr, "  Cyan = faction 0, Red = faction 1\n");
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

  fprintf(stderr, "Replay: %zu ticks, %dx%d map\n", log.ticks.size(), log.map_w, log.map_h);
  fprintf(stderr, "Re-simulating...\n");

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
  fprintf(stderr, "Ready (%zu ticks). Starting viewer...\n", frames.size() - 1);

  RawMode raw;
  size_t idx = 0;
  int view = -2;

  auto show = [&] {
    auto rf = viz::build_render_frame(frames[idx], map, view, idx, frames.size());
    viz::print_ascii_frame(rf);
  };

  show();
  while (true) {
    int ch = getchar();
    if (ch == EOF)
      break;
    bool changed = true;
    switch (ch) {
      case 'q': goto done;
      case 'n': case ' ': if (idx + 1 < frames.size()) ++idx; break;
      case 'p':           if (idx > 0) --idx; break;
      case 'u': view = -2; break;
      case 'g': view = -1; break;
      case '0': view = 0; break;
      case '1': view = 1; break;
      default:  changed = false; break;
    }
    if (changed)
      show();
  }

done:
  printf("\033[2J\033[H");
  return 0;
}
