// The balance arena: equal-cost armies fight on an open field; prints who is worth their price.
#include "arena.hpp"
#include "config.hpp"
#include "stats.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Args {
  std::vector<int32_t> budgets;
  int32_t parts{4};
  arena::Setup setup;
  int32_t round_robin_budget{0};
  std::string replay_a, replay_b, replay_out;
};

void usage() {
  fprintf(stderr,
          "usage: ud_arena [--budget N]... [--parts K] [--ticks N] [--spacing N]\n"
          "                [--round-robin N] [--replay \"<army>\" \"<army>\" <out.ud>]\n"
          "  --budget N       army value for the pure-ship matrix (repeatable; default 600 1200 "
          "2400)\n"
          "  --round-robin N  army value for the all-mixes round robin (default: largest budget)\n"
          "  --parts K        mixes split the budget into K equal shares (default 4)\n"
          "  --ticks N        fight length limit (default 400)\n"
          "  --spacing N      tiles between neighbours in a block (default 1: packed)\n"
          "  --replay A B F   write the fight A vs B to replay F; armies as \"15I 6F 0A 2B\"\n"
          "value = energy + alloy * %d (alloy harvests %dx slower)\n",
          game::cfg::HARVEST_ENERGY / game::cfg::HARVEST_ALLOY,
          game::cfg::HARVEST_ENERGY / game::cfg::HARVEST_ALLOY);
}

bool parse_army(const std::string& s, arena::Army& out) {
  static constexpr char kLetter[] = "IFAB";
  const char* p = s.c_str();
  while (*p) {
    char* end = nullptr;
    const long n = strtol(p, &end, 10);
    if (end == p || n < 0) return false;
    const char* hit = strchr(kLetter, *end);
    if (!*end || !hit) return false;
    out.count[static_cast<size_t>(hit - kLetter)] = static_cast<int32_t>(n);
    p = end + 1;
    while (*p == ' ') ++p;
  }
  return true;
}

bool parse(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto num = [&](int32_t& v) {
      if (i + 1 >= argc) return false;
      v = atoi(argv[++i]);
      return v > 0;
    };
    int32_t v = 0;
    if (k == "--budget" && num(v)) a.budgets.push_back(v);
    else if (k == "--parts" && num(a.parts)) {
    } else if (k == "--ticks" && num(a.setup.tick_limit)) {
    } else if (k == "--spacing" && num(a.setup.spacing)) {
    } else if (k == "--round-robin" && num(a.round_robin_budget)) {
    } else if (k == "--replay" && i + 3 < argc) {
      a.replay_a = argv[++i];
      a.replay_b = argv[++i];
      a.replay_out = argv[++i];
    } else
      return false;
  }
  if (a.budgets.empty()) a.budgets = {600, 1200, 2400};
  if (a.round_robin_budget == 0)
    a.round_robin_budget = *std::max_element(a.budgets.begin(), a.budgets.end());
  return true;
}

const char* ship_name(game::UnitType t) {
  switch (t) {
  case game::UnitType::Interceptor: return "Interceptor";
  case game::UnitType::Frigate:     return "Frigate";
  case game::UnitType::Artillery:   return "Artillery";
  case game::UnitType::Battleship:  return "Battleship";
  default:                          return "?";
  }
}

void print_ships() {
  printf("%-12s %5s %5s %4s %4s %5s %5s %5s   %8s %8s\n", "ship", "value", "hull", "shld", "dmg",
         "range", "sight", "speed", "dmg/100v", "ehp/100v");
  for (auto t : arena::kShips) {
    const auto& s = game::stats_for(t);
    const int32_t v = arena::value(t);
    printf("%-12s %5d %5d %4d %4d %5d %5d %5d   %8.1f %8.1f\n", ship_name(t), v, s.hp, s.shield,
           s.dmg, s.range, s.sight, s.speed, 100.0 * s.dmg / v, 100.0 * (s.hp + s.shield) / v);
  }
}

void print_matrix(int32_t budget, const arena::Setup& setup) {
  printf("\nPure armies, value %d. Row's margin vs column (+1 = row loses nothing, column all):\n",
         budget);
  printf("%-12s", "");
  for (auto t : arena::kShips) printf(" %11s", ship_name(t));
  printf("   %s\n", "mean");
  for (auto x : arena::kShips) {
    const arena::Army ax = arena::pure(x, budget);
    double sum = 0;
    printf("%-12s", (std::to_string(ax.count[0] + ax.count[1] + ax.count[2] + ax.count[3]) +
                     " " + ship_name(x))
                        .c_str());
    for (auto y : arena::kShips) {
      const double s = x == y ? 0.0 : arena::score(ax, arena::pure(y, budget), setup);
      sum += s;
      printf(" %+11.2f", s);
    }
    printf("   %+.2f\n", sum / static_cast<double>(arena::kShips.size() - 1));
  }
  double side = 0;
  for (auto x : arena::kShips) {
    const arena::Army ax = arena::pure(x, budget);
    side += arena::fight(ax, ax, setup).margin();
  }
  printf("first-mover bias (mirror fights, faction 0's mean margin): %+.3f\n",
         side / static_cast<double>(arena::kShips.size()));
}

// The fewest of `x` that hold their own (margin >= 0) against `y`: more always fights at least
// as well, so a binary search finds it. 0 if even kMaxWorth times the price can't.
int32_t break_even(game::UnitType x, const arena::Army& y, const arena::Setup& setup) {
  constexpr int32_t kMaxWorth = 4;
  int32_t lo = 1, hi = kMaxWorth * y.value() / arena::value(x) + 1, found = 0;
  while (lo <= hi) {
    const int32_t mid = (lo + hi) / 2;
    arena::Army a;
    for (size_t k = 0; k < arena::kShips.size(); ++k)
      if (arena::kShips[k] == x) a.count[k] = mid;
    if (arena::score(a, y, setup) >= 0) {
      found = mid;
      hi = mid - 1;
    } else
      lo = mid + 1;
  }
  return found;
}

// What each ship is worth against each other ship: the budget over how many of it break even
// against a pure army of that budget. Above the ship's price means it is cheap for that fight.
void print_worth(int32_t budget, const arena::Setup& setup) {
  printf("\nWorth, value %d: what one row ship is worth against the column ship (its price in "
         "brackets; '-' = can't break even at 4x):\n",
         budget);
  printf("%-18s", "");
  for (auto t : arena::kShips) printf(" %11s", ship_name(t));
  printf("\n");
  for (auto x : arena::kShips) {
    printf("%-12s (%3d)", ship_name(x), arena::value(x));
    for (auto y : arena::kShips) {
      if (x == y) {
        printf(" %11s", "");
        continue;
      }
      const int32_t n = break_even(x, arena::pure(y, budget), setup);
      if (n == 0) printf(" %11s", "-");
      else printf(" %11d", arena::pure(y, budget).value() / n);
    }
    printf("\n");
  }
}

void round_robin(int32_t budget, int32_t parts, const arena::Setup& setup) {
  const auto armies = arena::compositions(budget, parts);
  const size_t n = armies.size();
  std::vector<double> total(n, 0.0);
  int32_t unfinished = 0, fights = 0;
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j) {
      const auto there = arena::fight(armies[i], armies[j], setup);
      const auto back = arena::fight(armies[j], armies[i], setup);
      const double s = (there.margin() - back.margin()) / 2; // as arena::score
      total[i] += s;
      total[j] -= s;
      fights += 2;
      unfinished += (there.ticks >= setup.tick_limit) + (back.ticks >= setup.tick_limit);
    }
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return total[a] > total[b]; });

  printf("\nRound robin, value %d, %zu mixes in %d shares (mean margin vs the field; %d of %d "
         "fights hit the tick limit):\n",
         budget, n, parts, unfinished, fights);
  for (size_t r = 0; r < n; ++r) {
    const size_t i = order[r];
    printf("  %2zu. %-18s value %5d   %+.3f\n", r + 1, armies[i].name().c_str(),
           armies[i].value(), total[i] / static_cast<double>(n - 1));
  }

  // Each ship's share of an army's budget, weighed against how that army did.
  printf("\nShare-weighted mean margin (how armies heavy in each ship did):\n");
  for (size_t k = 0; k < arena::kShips.size(); ++k) {
    double num = 0, den = 0;
    for (size_t i = 0; i < n; ++i) {
      const int32_t spent = armies[i].count[k] * arena::value(arena::kShips[k]);
      const double share = static_cast<double>(spent) / armies[i].value();
      num += share * total[i] / static_cast<double>(n - 1);
      den += share;
    }
    printf("  %-12s %+.3f\n", ship_name(arena::kShips[k]), num / den);
  }
}

} // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse(argc, argv, args)) {
    usage();
    return 1;
  }
  if (!args.replay_out.empty()) {
    arena::Army a, b;
    if (!parse_army(args.replay_a, a) || !parse_army(args.replay_b, b)) {
      usage();
      return 1;
    }
    arena::write_replay(a, b, args.setup, args.replay_out);
    const auto o = arena::fight(a, b, args.setup);
    printf("%s (%d) vs %s (%d): %d ticks, left %d vs %d, margin %+.2f -> %s\n",
           a.name().c_str(), o.start_a, b.name().c_str(), o.start_b, o.ticks, o.left_a, o.left_b,
           o.margin(), args.replay_out.c_str());
    return 0;
  }
  print_ships();
  for (int32_t b : args.budgets) print_matrix(b, args.setup);
  print_worth(args.round_robin_budget, args.setup);
  round_robin(args.round_robin_budget, args.parts, args.setup);
  return 0;
}
