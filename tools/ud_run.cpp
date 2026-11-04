// Runs a 1v1 match between two WASM bots and writes a replay file.
// Usage: ud_run <bot_a.wasm> <bot_b.wasm> [--map <path.json>] [--out <path.ud>]
//               [--seed N] [--ticks N]

#include "config.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include "map_gen.hpp"
#include "map_io.hpp"
#include "match_setup.hpp"
#include "match.hpp"
#include "replay.hpp"
#include "replay_io.hpp"
#include "rng.hpp"
#include "wasm_bot.hpp"
#include "wincheck.hpp"
#include "world.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

// ---- Helpers ----------------------------------------------------------------

static std::string basename_noext(const std::string& path) {
  auto slash = path.rfind('/');
  std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  auto dot = name.rfind('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  return name;
}

// ---- Argument parsing -------------------------------------------------------

struct Args {
  std::string bot_a;
  std::string bot_b;
  std::string map_path;
  std::string out_path;
  uint64_t    seed{0};
  bool        seed_set{false};
  uint32_t    ticks{static_cast<uint32_t>(game::cfg::TICK_CAP)};
};

[[noreturn]] static void usage(const char* prog, int code = 1) {
  FILE* f = code ? stderr : stdout;
  fprintf(f,
      "usage: %s <bot_a.wasm> <bot_b.wasm> [options]\n"
      "  --map  <path.json>   map file to play on (default: procedurally generated)\n"
      "  --out  <path.ud>     output replay path  (default: <a>_vs_<b>.ud)\n"
      "  --seed <N>           RNG seed            (default: time-based random)\n"
      "  --ticks <N>          tick cap            (default: %d)\n",
      prog, game::cfg::TICK_CAP);
  std::exit(code);
}

static Args parse_args(int argc, char** argv) {
  if (argc < 3) usage(argv[0]);

  Args a;
  a.bot_a = argv[1];
  a.bot_b = argv[2];

  for (int i = 3; i < argc; ++i) {
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s requires an argument\n", flag);
        usage(argv[0]);
      }
      return argv[++i];
    };

    if      (!strcmp(argv[i], "--map"))   a.map_path = need("--map");
    else if (!strcmp(argv[i], "--out"))   a.out_path = need("--out");
    else if (!strcmp(argv[i], "--seed"))  { a.seed = std::stoull(need("--seed")); a.seed_set = true; }
    else if (!strcmp(argv[i], "--ticks")) a.ticks = static_cast<uint32_t>(std::stoul(need("--ticks")));
    else if (!strcmp(argv[i], "--help"))  usage(argv[0], 0);
    else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); }
  }

  if (!a.seed_set)
    a.seed = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

  if (a.out_path.empty())
    a.out_path = basename_noext(a.bot_a) + "_vs_" + basename_noext(a.bot_b) + ".ud";

  return a;
}

// ---- Main -------------------------------------------------------------------

int main(int argc, char** argv) {
  const Args args = parse_args(argc, argv);

  // Load bots
  runner::WasmBot bot_a(runner::load_wasm_file(args.bot_a));
  if (!bot_a.healthy()) {
    fprintf(stderr, "error loading %s: %s\n", args.bot_a.c_str(), bot_a.last_error().c_str());
    return 1;
  }
  runner::WasmBot bot_b(runner::load_wasm_file(args.bot_b));
  if (!bot_b.healthy()) {
    fprintf(stderr, "error loading %s: %s\n", args.bot_b.c_str(), bot_b.last_error().c_str());
    return 1;
  }

  // Load or generate map
  maps::MapData md;
  try {
    if (!args.map_path.empty()) {
      md = maps::load_map(args.map_path);
    } else {
      game::Rng rng{args.seed};
      md = maps::generate_map({}, rng);
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }

  const std::string name_a = basename_noext(args.bot_a);
  const std::string name_b = basename_noext(args.bot_b);

  printf("Map:   %s (%dx%d)\n", md.name.c_str(), md.map.width, md.map.height);
  printf("Bot A: %s\n", name_a.c_str());
  printf("Bot B: %s\n", name_b.c_str());
  printf("Seed:  %llu\n", (unsigned long long)args.seed);
  printf("Ticks: %u cap\n\n", args.ticks);

  // Run match
  game::World w = maps::world_from_map(md, args.seed);
  auto rec = game::run_match(w, bot_a, 0, bot_b, 1, args.ticks, name_a, name_b);

  // Print result
  const auto& out = rec.outcome;
  std::array<game::TickFault, 2> faults{};
  if (!rec.replay.ticks.empty())
    faults = {rec.replay.ticks.back().fault_a, rec.replay.ticks.back().fault_b};
  const std::string why = game::result_reason(out, faults, {"A", "B"});
  if (faults[0] != game::TickFault::None || faults[1] != game::TickFault::None) {
    if (out.reason == game::WinReason::Draw)
      printf("Result: Draw — %s, at tick %u\n", why.c_str(), rec.ticks_played);
    else
      printf("Result: %s wins — %s, at tick %u\n", out.winner.value == 0 ? "A" : "B",
             why.c_str(), rec.ticks_played);
    // The runner's own message (a trap's backtrace, say), for the author to debug from.
    for (size_t f = 0; f < faults.size(); ++f)
      if (faults[f] != game::TickFault::None)
        printf("%s's error:\n%s\n", f == 0 ? "A" : "B",
               (f == 0 ? bot_a : bot_b).last_error().c_str());
  } else if (out.reason == game::WinReason::Draw) {
    printf("Result: Draw (tick cap, territory within %d%%)\n",
           game::cfg::DRAW_TERRITORY_MARGIN);
  } else {
    const char* side   = out.winner.value == 0 ? "A" : "B";
    const char* reason =
        out.reason == game::WinReason::BaseDestroyed      ? "base destroyed" :
        out.reason == game::WinReason::TerritoryThreshold ? "territory threshold" : "tick cap";
    printf("Result: %s wins — %s in %u ticks\n", side, reason, rec.ticks_played);
  }

  // Fuel and memory summary
  const auto& log = rec.replay;
  printf("\n  Fuel init:     A=%-10llu B=%llu\n",
      (unsigned long long)log.init_fuel_a,
      (unsigned long long)log.init_fuel_b);
  if (!log.ticks.empty()) {
    uint64_t peak_fa = 0, peak_fb = 0, peak_ma = 0, peak_mb = 0;
    for (const auto& t : log.ticks) {
      peak_fa = std::max(peak_fa, t.fuel_a);
      peak_fb = std::max(peak_fb, t.fuel_b);
      peak_ma = std::max(peak_ma, t.mem_bytes_a);
      peak_mb = std::max(peak_mb, t.mem_bytes_b);
    }
    printf("  Fuel peak/tick:A=%-10llu B=%llu\n",
        (unsigned long long)peak_fa, (unsigned long long)peak_fb);
    printf("  Memory peak:   A=%-10llu B=%llu bytes\n",
        (unsigned long long)peak_ma, (unsigned long long)peak_mb);

  }

  // Write replay
  printf("\n");
  try {
    game::write_replay_file(rec.replay, args.out_path);
    printf("Replay written: %s\n", args.out_path.c_str());
  } catch (const std::exception& e) {
    fprintf(stderr, "error writing replay: %s\n", e.what());
    return 1;
  }

  return 0;
}
