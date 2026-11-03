#include "arena.hpp"
#include "bot_iface.hpp"
#include "bot_util.hpp"
#include "config.hpp"
#include "match.hpp"
#include "stats.hpp"
#include "statehash.hpp"
#include "tick.hpp"
#include <algorithm>
#include <array>
#include <climits>

namespace arena {

using game::FactionId;
using game::UnitType;
using game::Vec2;

namespace {

constexpr int32_t kRanks = 16; // block width, across the line of advance
constexpr int32_t kGap = 12;   // between the blocks: beyond every sight and weapon range
constexpr int32_t kPad = 8;    // behind each block, to the map edge
constexpr int32_t kSide = 6;   // beside each block, to the map edge

int32_t ships(const Army& a) {
  int32_t n = 0;
  for (int32_t c : a.count) n += c;
  return n;
}

int32_t columns(const Army& a) { return (ships(a) + kRanks - 1) / kRanks; }

// Sends every ship at the enemy and shoots with focus fire: the same orders for both sides, so
// only the armies differ. Ships move down one distance field to the nearest enemy ship, in which
// our ships standing to fire are walls: the rest flow around the firing line to open positions,
// so a swarm surrounds instead of queueing behind its own front rank. The arena tests fighting,
// not scouting, so the commander is told where every enemy ship is (read from the world at the
// start of the tick, the state its snapshot shows). Faction 1 stands on the mirror image of
// faction 0's field and breaks step ties in that mirrored frame, so neither side is favoured.
class Commander : public game::Bot {
public:
  explicit Commander(const game::World& world) : world_(world) {}

  void on_init(const game::Map& map, uint32_t faction) override {
    // Forward (toward the enemy's side) first, then across, then back.
    const int32_t fwd = faction == 0 ? 1 : -1;
    dirs_ = {Vec2{fwd, 0}, Vec2{0, 1}, Vec2{0, -1}, Vec2{-fwd, 0}};
    w_ = map.width;
    h_ = map.height;
  }

  std::vector<game::Command> on_tick(const game::Snapshot& snap) override {
    std::vector<const game::UnitView*> mine;
    for (const auto& u : snap.my_units)
      if (static_cast<UnitType>(u.type) != UnitType::Drone) mine.push_back(&u);
    std::vector<Vec2> enemies;
    for (const auto& [id, u] : world_.units)
      if (u.alive() && u.faction.value != snap.header.my_faction_id && u.type != UnitType::Drone)
        enemies.push_back(u.pos);
    if (mine.empty() || enemies.empty()) return {};

    const auto focus = game::focus_fire(snap, mine);
    std::vector<bool> wall(static_cast<size_t>(w_ * h_), false);
    for (const auto& [id, s] : world_.structures) wall[idx(s.pos)] = true;
    for (const game::UnitView* u : mine)
      if (focus.contains(u->id)) wall[idx({u->x, u->y})] = true;
    const auto dist = distances(enemies, wall);

    std::vector<game::Command> out;
    for (const game::UnitView* u : mine) {
      if (auto f = focus.find(u->id); f != focus.end()) {
        game::Command c = game::move_cmd(u->id, game::CommandKind::Attack, {0, 0});
        c.target_id = f->second;
        out.push_back(c);
        continue;
      }
      const Vec2 pos{u->x, u->y};
      Vec2 p = pos;
      for (int32_t i = 0; i < game::stats_for(static_cast<UnitType>(u->type)).speed; ++i) {
        Vec2 best = p;
        for (const Vec2& d : dirs_) {
          const Vec2 n{p.x + d.x, p.y + d.y};
          if (in_bounds(n) && dist[idx(n)] > 0 && dist[idx(n)] < dist[idx(best)]) best = n;
        }
        if (best == p) break;
        p = best;
      }
      out.push_back(p != pos ? game::move_cmd(u->id, game::CommandKind::MoveAttack, p)
                             : game::move_cmd(u->id, game::CommandKind::Attack, {0, 0}));
    }
    return out;
  }

private:
  static constexpr int32_t kFar = INT32_MAX;

  const game::World& world_;
  std::array<Vec2, 4> dirs_{};
  int32_t w_{0}, h_{0};

  bool in_bounds(Vec2 p) const { return p.x >= 0 && p.y >= 0 && p.x < w_ && p.y < h_; }
  size_t idx(Vec2 p) const { return static_cast<size_t>(p.y * w_ + p.x); }

  // Steps from every tile to the nearest enemy ship (0 on their tiles), around walls.
  std::vector<int32_t> distances(const std::vector<Vec2>& from,
                                 const std::vector<bool>& wall) const {
    std::vector<int32_t> d(static_cast<size_t>(w_ * h_), kFar);
    std::vector<Vec2> frontier;
    for (const Vec2& p : from) {
      d[idx(p)] = 0;
      frontier.push_back(p);
    }
    for (size_t i = 0; i < frontier.size(); ++i) {
      const Vec2 p = frontier[i];
      for (const Vec2& dir : dirs_) {
        const Vec2 n{p.x + dir.x, p.y + dir.y};
        if (!in_bounds(n) || wall[idx(n)] || d[idx(n)] != kFar) continue;
        d[idx(n)] = d[idx(p)] + 1;
        frontier.push_back(n);
      }
    }
    return d;
  }
};

struct Field {
  int32_t w, h;
  int32_t depth; // in tiles, of the deeper block
  int32_t spacing;
};

Field field(const Army& a, const Army& b, const Setup& s) {
  const int32_t d = (std::max(columns(a), columns(b)) - 1) * s.spacing + 1;
  return {2 * kPad + 2 * d + kGap, (kRanks - 1) * s.spacing + 1 + 2 * kSide, d, s.spacing};
}

// Faction 0's block, front column first (nearest the enemy), shortest range at the front.
std::vector<std::pair<UnitType, Vec2>> deploy(const Army& a, const Field& f) {
  std::vector<UnitType> order;
  for (size_t i = 0; i < kShips.size(); ++i)
    for (int32_t k = 0; k < a.count[i]; ++k) order.push_back(kShips[i]);
  std::stable_sort(order.begin(), order.end(), [](UnitType x, UnitType y) {
    return game::stats_for(x).range < game::stats_for(y).range;
  });
  std::vector<std::pair<UnitType, Vec2>> out;
  const int32_t front = kPad + f.depth - 1;
  for (size_t i = 0; i < order.size(); ++i) {
    const auto n = static_cast<int32_t>(i);
    out.push_back({order[i], {front - n / kRanks * f.spacing, kSide + n % kRanks * f.spacing}});
  }
  return out;
}

// Faction 1's side is faction 0's reflected across the middle column.
Vec2 mirror(const Field& f, Vec2 p) { return {f.w - 1 - p.x, p.y}; }

int32_t standing(const game::World& w, FactionId side) {
  int64_t v = 0;
  for (const auto& [id, u] : w.units)
    if (u.alive() && u.faction == side && u.type != UnitType::Drone)
      v += static_cast<int64_t>(value(u.type)) * u.hp / u.stats().hp;
  return static_cast<int32_t>(v);
}

bool has_ships(const game::World& w, FactionId side) {
  for (const auto& [id, u] : w.units)
    if (u.alive() && u.faction == side && u.type != UnitType::Drone) return true;
  return false;
}

} // namespace

int32_t value(UnitType t) {
  const auto [e, a] = game::unit_cost(t);
  return e + a * game::cfg::HARVEST_ENERGY / game::cfg::HARVEST_ALLOY;
}

int32_t Army::value() const {
  int32_t v = 0;
  for (size_t i = 0; i < kShips.size(); ++i) v += count[i] * arena::value(kShips[i]);
  return v;
}

std::string Army::name() const {
  static constexpr char kLetter[] = {'I', 'F', 'A', 'B'};
  std::string s;
  for (size_t i = 0; i < kShips.size(); ++i) {
    if (!s.empty()) s += ' ';
    s += std::to_string(count[i]) + kLetter[i];
  }
  return s;
}

std::vector<Army> compositions(int32_t budget, int32_t parts) {
  std::vector<Army> out;
  const int32_t share = budget / parts;
  for (int32_t i = 0; i <= parts; ++i)
    for (int32_t f = 0; i + f <= parts; ++f)
      for (int32_t a = 0; i + f + a <= parts; ++a) {
        const int32_t b = parts - i - f - a;
        const std::array<int32_t, 4> shares{i, f, a, b};
        Army army;
        for (size_t k = 0; k < kShips.size(); ++k)
          army.count[k] = shares[k] * share / value(kShips[k]);
        out.push_back(army);
      }
  return out;
}

Army pure(UnitType t, int32_t budget) {
  Army a;
  for (size_t k = 0; k < kShips.size(); ++k)
    if (kShips[k] == t) a.count[k] = budget / value(t);
  return a;
}

game::World battlefield(const Army& a, const Army& b, const Setup& s) {
  const Field f = field(a, b, s);
  game::World w;
  w.map = game::Map::make(f.w, f.h);
  w.rng = game::Rng{1};
  w.rng_seed = 1;
  w.spawn_structure(FactionId{0}, game::StructureType::CommandCore, {0, 0});
  w.spawn_structure(FactionId{1}, game::StructureType::CommandCore, mirror(f, {0, 0}));
  for (const auto& [t, p] : deploy(a, f)) w.spawn_unit(FactionId{0}, t, p);
  for (const auto& [t, p] : deploy(b, f)) w.spawn_unit(FactionId{1}, t, mirror(f, p));
  return w;
}

double Outcome::margin() const {
  const double a = start_a > 0 ? static_cast<double>(left_a) / start_a : 0.0;
  const double b = start_b > 0 ? static_cast<double>(left_b) / start_b : 0.0;
  return a - b;
}

Outcome fight(const Army& a, const Army& b, const Setup& s) {
  game::World w = battlefield(a, b, s);
  Commander ca{w}, cb{w};
  ca.on_init(w.map, 0);
  cb.on_init(w.map, 1);
  Outcome o;
  o.start_a = standing(w, FactionId{0});
  o.start_b = standing(w, FactionId{1});
  game::StateHash hash;
  const auto cap = static_cast<uint32_t>(s.tick_limit);
  while (o.ticks < s.tick_limit && has_ships(w, FactionId{0}) && has_ships(w, FactionId{1})) {
    game::run_tick(w, ca, 0, cb, 1, cap, hash);
    ++o.ticks;
  }
  o.left_a = standing(w, FactionId{0});
  o.left_b = standing(w, FactionId{1});
  return o;
}

double score(const Army& x, const Army& y, const Setup& s) {
  return (fight(x, y, s).margin() - fight(y, x, s).margin()) / 2;
}

void write_replay(const Army& a, const Army& b, const Setup& s, const std::string& path) {
  const int32_t ticks = fight(a, b, s).ticks;
  game::World w = battlefield(a, b, s);
  Commander ca{w}, cb{w};
  const auto rec = game::run_match(w, ca, 0, cb, 1, static_cast<uint32_t>(ticks), a.name(),
                                   b.name());
  game::write_replay_file(rec.replay, path);
}

} // namespace arena
