// ud_profile — deterministic fuel/memory profile of a bot (DESIGN §10).
//
// Usage: ud_profile <bot.wasm> [--scenario NAME]... [--ticks] [--list]
//
// Runs the scenario suite through the same match code and pinned Wasmtime config
// as ranked play, so every number is exactly what the match meter would charge.
// Exit status: 0 = within budget, 1 = usage/load error, 2 = a forfeit (fuel, memory or trap).
#include "config.hpp"
#include "profiler.hpp"
#include "wasm_bot.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint64_t kMemCap = static_cast<uint64_t>(game::cfg::MEMORY_CAP_MB) * 1024 * 1024;

std::string grouped(uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return s;
}

std::string mib(uint64_t bytes) {
  char b[32];
  snprintf(b, sizeof b, "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return b;
}

std::string pct(uint64_t part, uint64_t whole) {
  char b[16];
  snprintf(b, sizeof b, "%.1f%%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
  return b;
}

bool g_tty = false;
std::string bold(const std::string& s) { return g_tty ? "\033[1m" + s + "\033[0m" : "*" + s + "*"; }
std::string red(const std::string& s) { return g_tty ? "\033[31m" + s + "\033[0m" : s; }

// Pads to `w` columns, measuring visible width only (ANSI codes and the * markers
// added by bold() shouldn't shift the columns).
std::string col(const std::string& s, int w, bool left = false) {
  int visible = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\033') { while (i < s.size() && s[i] != 'm') ++i; continue; }
    ++visible;
  }
  std::string pad(static_cast<size_t>(std::max(0, w - visible)), ' ');
  return left ? s + pad : pad + s;
}

void usage() {
  fprintf(stderr, "usage: ud_profile <bot.wasm> [--scenario NAME]... [--ticks] [--list]\n");
}

} // namespace

int main(int argc, char** argv) {
  g_tty = isatty(fileno(stdout));
  std::string path;
  std::vector<const profiler::Scenario*> chosen;
  bool per_tick = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--list")) {
      for (const auto& s : profiler::scenarios())
        printf("%-18s %5u ticks  %s\n", s.name.c_str(), s.ticks, s.what.c_str());
      return 0;
    } else if (!strcmp(argv[i], "--ticks")) {
      per_tick = true;
    } else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) {
      const auto* s = profiler::find_scenario(argv[++i]);
      if (!s) { fprintf(stderr, "unknown scenario '%s' (see --list)\n", argv[i]); return 1; }
      chosen.push_back(s);
    } else if (argv[i][0] == '-' || !path.empty()) {
      usage();
      return 1;
    } else {
      path = argv[i];
    }
  }
  if (path.empty()) { usage(); return 1; }
  if (chosen.empty())
    for (const auto& s : profiler::scenarios()) chosen.push_back(&s);

  std::vector<uint8_t> wasm;
  try {
    wasm = runner::load_wasm_file(path);
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }

  std::vector<profiler::Report> reports;
  for (const auto* s : chosen) {
    reports.push_back(profiler::run_scenario(wasm, *s));
    if (!reports.back().loaded) {
      fprintf(stderr, "error: %s: %s\n", path.c_str(), reports.back().error.c_str());
      return 1;
    }
  }

  printf("ud_profile %s\n", path.c_str());
  printf("budgets: %s fuel/tick, %s fuel at init, %s memory (pinned ranked-match config)\n",
         grouped(game::cfg::FUEL_PER_TICK).c_str(), grouped(game::cfg::FUEL_STARTUP).c_str(),
         mib(kMemCap).c_str());
  const auto& init = reports.front().init;
  printf("init:    %s fuel (%s), %s\n\n", grouped(init.fuel_consumed).c_str(),
         pct(init.fuel_consumed, game::cfg::FUEL_STARTUP).c_str(), mib(init.memory_bytes).c_str());

  size_t worst_i = 0, peak_i = 0;
  for (size_t i = 0; i < reports.size(); ++i) {
    if (reports[i].worst_fuel > reports[worst_i].worst_fuel) worst_i = i;
    if (reports[i].peak_memory > reports[peak_i].peak_memory) peak_i = i;
  }

  printf("%s%s%s%s%s%s%s%s%s%s\n", col("scenario", 18, true).c_str(), col("units", 6).c_str(),
         col("enemies", 8).c_str(), col("tiles", 7).c_str(), col("ticks", 6).c_str(),
         col("mean fuel", 13).c_str(), col("worst fuel", 13).c_str(), col("of budget", 10).c_str(),
         col("peak mem", 11).c_str(), col("forfeits", 22).c_str());
  bool breach = false;
  for (size_t i = 0; i < reports.size(); ++i) {
    const auto& r = reports[i];
    std::string worst = grouped(r.worst_fuel), peak = mib(r.peak_memory);
    std::string share = pct(r.worst_fuel, game::cfg::FUEL_PER_TICK);
    if (i == worst_i) { worst = bold(worst); share = bold(share); }
    if (i == peak_i) peak = bold(peak);
    std::string forfeits = "-";
    if (r.forfeits()) {
      std::string causes;
      for (auto [n, what] : {std::pair{r.fuel_forfeits, "fuel"}, std::pair{r.memory_forfeits, "memory"},
                             std::pair{r.trap_forfeits, "trap"}})
        if (n) causes += (causes.empty() ? "" : ", ") + std::to_string(n) + " " + what;
      forfeits = red(std::to_string(r.forfeits()) + " (" + causes + ")");
      breach = true;
    }
    printf("%s%s%s%s%s%s%s%s%s%s\n", col(r.name, 18, true).c_str(),
           col(std::to_string(r.my_units), 6).c_str(), col(std::to_string(r.visible_enemies), 8).c_str(),
           col(std::to_string(r.visible_tiles), 7).c_str(), col(std::to_string(r.ticks.size()), 6).c_str(),
           col(grouped(r.mean_fuel()), 13).c_str(), col(worst, 13).c_str(), col(share, 10).c_str(),
           col(peak, 11).c_str(), col(forfeits, 22).c_str());
  }

  // Memory over time for long runs: a leak shows as steady growth after warm-up.
  for (const auto& r : reports) {
    if (r.ticks.size() < 200) continue;
    printf("\n%s memory over time:\n", r.name.c_str());
    const size_t n = r.ticks.size(), points = 10;
    for (size_t k = 0; k <= points; ++k) {
      const size_t t = std::min(n - 1, k * (n - 1) / points);
      printf("  tick %5zu  %s\n", t, mib(r.ticks[t].memory).c_str());
    }
    const uint64_t warm = r.ticks[n / 10].memory, last = r.ticks[n - 1].memory;
    if (last > warm)
      printf("  %s\n", red("grew " + mib(last - warm) + " after warm-up (tick " +
                           std::to_string(n / 10) + " onward): possible leak").c_str());
    else
      printf("  flat after warm-up\n");
  }

  if (per_tick) {
    for (const auto& r : reports) {
      printf("\n%s per-tick fuel:\n", r.name.c_str());
      for (size_t t = 0; t < r.ticks.size(); ++t)
        printf("  %5zu  %13s  %s%s\n", t, grouped(r.ticks[t].fuel).c_str(),
               mib(r.ticks[t].memory).c_str(),
               r.ticks[t].fault == game::TickFault::None
                   ? ""
                   : ("  FORFEIT (" + std::string(game::tick_fault_name(r.ticks[t].fault)) + ")").c_str());
    }
  }
  return breach ? 2 : 0;
}
