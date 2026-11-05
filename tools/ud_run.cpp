// Runs a 1v1 match between two WASM bots and writes a replay file.
// Usage: ud_run <bot_a.wasm> <bot_b.wasm> [--map <path.json>] [--out <path.ud>]
//               [--seed N] [--ticks N] [--json]
//
// --json prints one JSON object on stdout instead of the summary, for the tournament harness:
// keys are append-only, as the harness stores them. Exit status: 0 = the match was played
// (whatever its result), 1 = it couldn't be (the object then has an "error").

#include "config.hpp"
#include "entity.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "map_gen.hpp"
#include "map_io.hpp"
#include "match_setup.hpp"
#include "match.hpp"
#include "replay.hpp"
#include "replay_io.hpp"
#include "rng.hpp"
#include "territory.hpp"
#include "wasm_bot.hpp"
#include "wincheck.hpp"
#include "world.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
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
  bool        json{false};
};

[[noreturn]] static void usage(const char* prog, int code = 1) {
  FILE* f = code ? stderr : stdout;
  fprintf(f,
      "usage: %s <bot_a.wasm> <bot_b.wasm> [options]\n"
      "  --map  <path.json>   map file to play on (default: procedurally generated)\n"
      "  --out  <path.ud>     output replay path  (default: <a>_vs_<b>.ud)\n"
      "  --seed <N>           RNG seed            (default: time-based random)\n"
      "  --ticks <N>          tick cap            (default: %d)\n"
      "  --json               print the result as one JSON object\n"
      "  --version            print the build version and exit\n",
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
    else if (!strcmp(argv[i], "--json"))  a.json = true;
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

// ---- Match statistics ---------------------------------------------------------

using Json = nlohmann::ordered_json;

struct SideStats {
  uint32_t units_built{0}, peak_units{0}, units_lost{0}, structures_lost{0}, peak_claims{0};
  uint32_t peak_territory_pct{0}, final_territory_pct{0};
};

// Replays the match frame by frame (only the engine can simulate it, so the harness gets these
// from here), and checks the replay reproduces the match's hash.
static std::array<SideStats, 2> side_stats(const game::MatchRecord& rec, const game::Map& map,
                                           bool& replay_ok) {
  uint64_t hash = 0;
  const auto frames = game::replay_frames(rec.replay, &hash);
  replay_ok = hash == rec.replay.expected_hash;

  std::array<SideStats, 2> st{};
  std::array<std::set<uint32_t>, 2> seen;
  std::vector<game::ClaimAnchor> claims, last_claims;
  game::TerritoryState terr{};
  for (size_t f = 0; f < frames.size(); ++f) {
    const auto& fr = frames[f];
    std::array<uint32_t, 2> units{}, finished{};
    for (const auto& u : fr.units) {
      if (u.faction.value > 1) continue;
      ++units[u.faction.value];
      if (seen[u.faction.value].insert(u.id.value).second && f > 0)
        ++st[u.faction.value].units_built;
    }
    claims.clear();
    for (const auto& s : fr.structures)
      if (s.type == game::StructureType::ClaimNode && s.hp > 0 && s.build_ticks_left == 0 &&
          s.faction.value <= 1) {
        claims.push_back({s.pos, s.faction.value});
        ++finished[s.faction.value];
      }
    if (f == 0 || claims != last_claims) {
      terr = game::territory_pct(map, game::territory_owners(map, claims));
      last_claims = claims;
    }
    for (const auto& k : fr.kills) {
      if (k.faction.value > 1) continue;
      ++(!k.unit.is_null() ? st[k.faction.value].units_lost : st[k.faction.value].structures_lost);
    }
    for (size_t i = 0; i < 2; ++i) {
      st[i].peak_units = std::max(st[i].peak_units, units[i]);
      st[i].peak_claims = std::max(st[i].peak_claims, finished[i]);
      st[i].peak_territory_pct = std::max(st[i].peak_territory_pct, terr.pct_faction[i]);
      st[i].final_territory_pct = terr.pct_faction[i];
    }
  }
  return st;
}

static const char* reason_key(game::WinReason r) {
  switch (r) {
  case game::WinReason::BaseDestroyed:      return "base_destroyed";
  case game::WinReason::TerritoryThreshold: return "territory";
  case game::WinReason::TickCap:            return "tick_cap";
  case game::WinReason::TieBreakLadder:     return "tie_break";
  case game::WinReason::Draw:               return "draw";
  case game::WinReason::Forfeit:            return "forfeit";
  }
  return "?";
}

static Json json_header() {
  return Json{{"engine_version", game::build_version()}, {"abi", game::cfg::ABI_VERSION}};
}

// side: which bot failed to load, or -1 for neither (the map, the replay file).
static int fail(const Args& args, const std::string& stage, int side, const std::string& msg) {
  if (args.json) {
    Json j = json_header();
    j["error"] = {{"stage", stage}, {"side", side < 0 ? Json(nullptr) : Json(side)},
                  {"message", msg}};
    printf("%s\n", j.dump().c_str());
  } else {
    fprintf(stderr, "error: %s\n", msg.c_str());
  }
  return 1;
}

static std::unique_ptr<runner::WasmBot> load_bot(const std::string& path, std::string& err) {
  try {
    auto bot = std::make_unique<runner::WasmBot>(runner::load_wasm_file(path));
    if (bot->healthy()) return bot;
    err = path + ": " + bot->last_error();
  } catch (const std::exception& e) {
    err = e.what();
  }
  return nullptr;
}

// ---- Main -------------------------------------------------------------------

int main(int argc, char** argv) {
  if (game::print_version_if_asked(argc, argv, "ud_run")) return 0;
  const Args args = parse_args(argc, argv);

  std::string err;
  auto bot_a_ptr = load_bot(args.bot_a, err);
  if (!bot_a_ptr) return fail(args, "load", 0, err);
  auto bot_b_ptr = load_bot(args.bot_b, err);
  if (!bot_b_ptr) return fail(args, "load", 1, err);
  runner::WasmBot& bot_a = *bot_a_ptr;
  runner::WasmBot& bot_b = *bot_b_ptr;

  maps::MapData md;
  try {
    if (!args.map_path.empty()) {
      md = maps::load_map(args.map_path);
    } else {
      game::Rng rng{args.seed};
      md = maps::generate_map({}, rng);
      md.hash = maps::compute_map_hash(md);
    }
  } catch (const std::exception& e) {
    return fail(args, "map", -1, e.what());
  }

  const std::string name_a = basename_noext(args.bot_a);
  const std::string name_b = basename_noext(args.bot_b);

  if (!args.json) {
    printf("Map:   %s (%dx%d)\n", md.name.c_str(), md.map.width, md.map.height);
    printf("Bot A: %s\n", name_a.c_str());
    printf("Bot B: %s\n", name_b.c_str());
    printf("Seed:  %llu\n", (unsigned long long)args.seed);
    printf("Ticks: %u cap\n\n", args.ticks);
  }

  game::World w = maps::world_from_map(md, args.seed);
  auto rec = game::run_match(w, bot_a, 0, bot_b, 1, args.ticks, name_a, name_b);

  const auto& out = rec.outcome;
  std::array<game::TickFault, 2> faults{};
  if (!rec.replay.ticks.empty())
    faults = {rec.replay.ticks.back().fault_a, rec.replay.ticks.back().fault_b};
  const std::string why = game::result_reason(out, faults, {"A", "B"});

  const auto& log = rec.replay;
  std::array<uint64_t, 2> peak_fuel{}, peak_mem{log.init_mem_bytes_a, log.init_mem_bytes_b};
  for (const auto& t : log.ticks) {
    peak_fuel = {std::max(peak_fuel[0], t.fuel_a), std::max(peak_fuel[1], t.fuel_b)};
    peak_mem = {std::max(peak_mem[0], t.mem_bytes_a), std::max(peak_mem[1], t.mem_bytes_b)};
  }

  if (args.json) {
    try {
      game::write_replay_file(rec.replay, args.out_path);
    } catch (const std::exception& e) {
      return fail(args, "replay", -1, std::string("writing replay: ") + e.what());
    }
    bool replay_ok = false;
    const auto st = side_stats(rec, w.map, replay_ok);
    char hash[17], map_hash[17];
    snprintf(hash, sizeof hash, "%016llx", (unsigned long long)log.expected_hash);
    snprintf(map_hash, sizeof map_hash, "%016llx", (unsigned long long)md.hash);
    Json j = json_header();
    j["map"] = {{"name", md.name}, {"hash", map_hash}, {"width", md.map.width},
                {"height", md.map.height}};
    j["seed"] = args.seed;
    j["tick_cap"] = args.ticks;
    j["result"] = {{"winner", out.reason == game::WinReason::Draw ? Json(nullptr)
                                                                  : Json(out.winner.value)},
                   {"reason", reason_key(out.reason)},
                   {"text", why}};
    j["ticks"] = rec.ticks_played;
    j["state_hash"] = hash;
    j["replay"] = args.out_path;
    j["replay_verified"] = replay_ok;
    const std::array<uint64_t, 2> init_fuel{log.init_fuel_a, log.init_fuel_b};
    for (size_t i = 0; i < 2; ++i) {
      const auto& s = st[i];
      j["sides"].push_back(
          {{"path", i == 0 ? args.bot_a : args.bot_b},
           {"fault", game::tick_fault_name(faults[i])},
           {"error", faults[i] == game::TickFault::None ? ""
                                                        : (i == 0 ? bot_a : bot_b).last_error()},
           {"fuel_init", init_fuel[i]},
           {"fuel_peak", peak_fuel[i]},
           {"mem_peak", peak_mem[i]},
           {"units_built", s.units_built},
           {"peak_units", s.peak_units},
           {"units_lost", s.units_lost},
           {"structures_lost", s.structures_lost},
           {"peak_claims", s.peak_claims},
           {"peak_territory_pct", s.peak_territory_pct},
           {"final_territory_pct", s.final_territory_pct}});
    }
    printf("%s\n", j.dump().c_str());
    return 0;
  }
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

  printf("\n  Fuel init:     A=%-10llu B=%llu\n",
      (unsigned long long)log.init_fuel_a,
      (unsigned long long)log.init_fuel_b);
  if (!log.ticks.empty()) {
    printf("  Fuel peak/tick:A=%-10llu B=%llu\n",
        (unsigned long long)peak_fuel[0], (unsigned long long)peak_fuel[1]);
    printf("  Memory peak:   A=%-10llu B=%llu bytes\n",
        (unsigned long long)peak_mem[0], (unsigned long long)peak_mem[1]);
  }

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
