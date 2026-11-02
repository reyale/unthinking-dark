#pragma once
#include "bot_iface.hpp"
#include "bot_util.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "stats.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace game {

// Plays every system, so matches against it exercise real combat: it expands over a claim
// lattice, builds Factories, counter-picks ships against what it has seen, scouts, ambushes
// from nebulae and pushes when it has the stronger army.
//
// Economy: the Core makes Drones up to the cap. Drones harvest, except a builder beside each
// unfinished structure and up to kMaxClaimers claimers, sent out while alloy covers a claim.
// The first Factory comes once kClaimsBeforeFactory claims stand (or at once if the base is
// threatened), and the bot saves for it; later ones only from surplus.
//
// Army: the first Interceptor scouts the mirrored spawn and hides in a nebula by the enemy
// Core. The rest wait in an ambush: the nebula patch nearest home on the way to the enemy.
// Enemy ships near an own structure are always fought first. With an army worth
// kPushMarginPct% of the enemy army it has seen, it pushes in a wave (ships built meanwhile
// wait at home for the next one): it gathers out of sight
// (a nebula near the target if there is one), then strikes enemy claims, unfinished first, and
// finally the Command Core. It falls back to the ambush when the ships it meets outweigh it.
//
// A ship fires only on a tick it is ordered to Attack or MoveAttack, so every ship gets one:
// Attack aimed at a focus-fire target when something is in range, MoveAttack when moving.
// Focus fire: each ship (ascending id) takes the in-range enemy with the least hp + shield left
// after the shots already assigned, so damage lands on one target at a time and isn't wasted on
// a ship already dead.
class AdaptiveBot : public Bot {
public:
  static constexpr int32_t kSpacing = cfg::CLAIM_INFLUENCE / 2;
  static constexpr size_t kMaxClaimers = 2;
  static constexpr int32_t kClaimsBeforeFactory = 1;
  static constexpr int32_t kDefendRadius = 8;  // enemy units this near an own structure
  static constexpr int32_t kEngageRadius = 6;  // enemy ships this near a pushing ship
  static constexpr int32_t kAmbushRadius = 10; // nebula this near home can be the ambush
  static constexpr int32_t kRallyDist = 5;     // no such nebula: wait this far out
  static constexpr int32_t kStageMin = 3;      // gathering point's distance from the target
  static constexpr int32_t kStageMax = 9;
  static constexpr int32_t kGatherRadius = 3;
  static constexpr int32_t kGatherPct = 75;    // of the army gathered before it strikes
  static constexpr uint32_t kGatherTimeout = 60;
  static constexpr int32_t kPushMarginPct = 150;
  static constexpr int32_t kMinPushValue = 150;
  static constexpr int32_t kBlindPushValue = 250; // with no enemy ships seen
  static constexpr int32_t kRetreatPct = 60; // fall back below this share of the enemy met
  static constexpr uint32_t kRetreatTicks = 80;
  static constexpr uint32_t kForgetTicks = 200; // enemy ships unseen this long are forgotten
  static constexpr uint32_t kScoutCooldown = 400;
  static constexpr int32_t kScoutWatch = 3; // preferred hiding distance from the enemy Core
  static constexpr int32_t kScoutRange = 12;
  static constexpr int32_t kCounterWeight = 2;
  static constexpr int32_t kBattleshipSpareE = 120;
  static constexpr size_t kBattleshipMinArmy = 6;

  void on_init(const Map& map, uint32_t) override {
    map_ = map;
    router_.reset(map);
    for (int32_t y = 0; y < map.height; ++y)
      for (int32_t x = 0; x < map.width; ++x)
        if (map.tile_at({x, y}).terrain == Terrain::Nebula) nebulae_.push_back({x, y});
  }

  std::vector<Command> on_tick(const Snapshot& snap) override {
    tick_ = snap.header.tick;
    if (!have_home_) init(snap);
    router_.observe(snap);
    remember(snap);
    out_.clear();
    busy_.clear();
    sort_units(snap);
    find_threats(snap);
    plan_economy(snap);
    plan_scout(snap);
    plan_army(snap);
    return out_;
  }

  bool healthy() const override { return true; }

private:
  enum class Mode : uint8_t { Hold, Stage, Strike, Retreat };

  struct SeenUnit {
    UnitType type;
    Vec2 pos;
    uint32_t tick;
  };
  struct SeenStructure {
    StructureType type;
    Vec2 pos;
    bool building;
  };

  Map map_;
  Router router_;
  ClaimLattice lattice_;
  std::vector<Vec2> nebulae_;
  bool have_home_{false};
  Vec2 home_{};
  Vec2 enemy_core_{}; // the mirrored spawn until the real one is seen
  uint32_t tick_{0};

  std::map<uint32_t, SeenUnit> enemy_units_;
  std::map<uint32_t, SeenStructure> enemy_structures_;
  std::set<uint32_t> doomed_; // enemies last tick's focus fire should have killed
  std::vector<uint8_t> visible_;

  std::vector<Vec2> ambush_tiles_; // the home nebula patch, or empty
  Vec2 ambush_{};

  uint32_t scout_{0};
  uint32_t scout_ready_{0};
  bool scout_hidden_{false};
  Vec2 scout_hide_{};

  Mode mode_{Mode::Hold};
  uint32_t mode_since_{0};
  uint32_t target_id_{0}; // 0: the enemy Core's guessed tile
  Vec2 target_{};
  Vec2 stage_{};
  std::set<uint32_t> squad_; // the ships a push started with; later ones wait at home

  uint32_t factory_builder_{0};

  // Per tick.
  std::vector<Command> out_;
  std::set<uint32_t> busy_;
  std::vector<const UnitView*> drones_, army_;
  std::vector<const EnemyView*> home_threats_, threats_;
  std::map<uint32_t, uint32_t> focus_; // own ship → what it shoots

  static int32_t value(UnitType t) {
    switch (t) {
    case UnitType::Interceptor: return cfg::INTERCEPTOR_COST_E + cfg::INTERCEPTOR_COST_A;
    case UnitType::Frigate: return cfg::FRIGATE_COST_E + cfg::FRIGATE_COST_A;
    case UnitType::Artillery: return cfg::ARTILLERY_COST_E + cfg::ARTILLERY_COST_A;
    case UnitType::Battleship: return cfg::BATTLESHIP_COST_E + cfg::BATTLESHIP_COST_A;
    default: return 0;
    }
  }

  static std::array<int32_t, 2> cost(UnitType t) {
    switch (t) {
    case UnitType::Interceptor: return {cfg::INTERCEPTOR_COST_E, cfg::INTERCEPTOR_COST_A};
    case UnitType::Frigate: return {cfg::FRIGATE_COST_E, cfg::FRIGATE_COST_A};
    case UnitType::Artillery: return {cfg::ARTILLERY_COST_E, cfg::ARTILLERY_COST_A};
    case UnitType::Battleship: return {cfg::BATTLESHIP_COST_E, cfg::BATTLESHIP_COST_A};
    default: return {cfg::DRONE_COST_E, cfg::DRONE_COST_A};
    }
  }

  size_t idx(Vec2 p) const {
    return static_cast<size_t>(p.y) * static_cast<size_t>(map_.width) + static_cast<size_t>(p.x);
  }
  bool nebula(Vec2 p) const { return map_.tile_at(p).terrain == Terrain::Nebula; }
  bool in_ambush(Vec2 p) const {
    return std::find(ambush_tiles_.begin(), ambush_tiles_.end(), p) != ambush_tiles_.end();
  }

  void issue(const Command& c) {
    out_.push_back(c);
    busy_.insert(c.entity_id);
  }

  void attack(uint32_t id, uint32_t target) {
    Command c = move_cmd(id, CommandKind::Attack, {0, 0});
    c.target_id = target;
    issue(c);
  }

  template <class Goal> void go(const UnitView& u, Goal goal, CommandKind kind) {
    const Vec2 pos{u.x, u.y};
    const Vec2 to = waypoint(router_, pos, goal, stats_for(static_cast<UnitType>(u.type)).speed);
    if (to != pos) issue(move_cmd(u.id, kind, to));
    else if (kind != CommandKind::Move) attack(u.id, 0);
  }

  // ---- Knowledge ----------------------------------------------------------------------------

  void init(const Snapshot& snap) {
    have_home_ = true;
    home_ = core_pos(snap);
    enemy_core_ = {map_.width - 1 - home_.x, map_.height - 1 - home_.y};
    lattice_.init(map_, home_, kSpacing);
    choose_ambush();
  }

  // The nebula tile nearest home, among those within kAmbushRadius, on the shortest way to the
  // enemy; its whole patch is the ambush. Without one, a tile kRallyDist out toward the enemy.
  void choose_ambush() {
    int32_t best = INT32_MAX;
    for (Vec2 n : nebulae_) {
      if (manhattan(n, home_) > kAmbushRadius) continue;
      const int32_t d = manhattan(n, home_) + manhattan(n, enemy_core_);
      if (d < best) {
        best = d;
        ambush_ = n;
      }
    }
    if (best != INT32_MAX) {
      std::set<Vec2> seen{ambush_};
      std::vector<Vec2> q{ambush_};
      for (size_t i = 0; i < q.size(); ++i)
        for (const Vec2& d : DIRS) {
          const Vec2 n = q[i] + d;
          if (map_.in_bounds(n) && nebula(n) && seen.insert(n).second) q.push_back(n);
        }
      ambush_tiles_ = q;
      return;
    }
    best = INT32_MAX;
    for (int32_t y = 0; y < map_.height; ++y)
      for (int32_t x = 0; x < map_.width; ++x) {
        const Vec2 p{x, y};
        if (!map_.passable(p) || manhattan(p, home_) != kRallyDist) continue;
        if (manhattan(p, enemy_core_) < best) {
          best = manhattan(p, enemy_core_);
          ambush_ = p;
        }
      }
  }

  void remember(const Snapshot& snap) {
    visible_.assign(static_cast<size_t>(map_.width) * static_cast<size_t>(map_.height), 0);
    for (const auto& t : snap.visible_tiles) visible_[idx({t.x, t.y})] = 1;

    std::set<uint32_t> now;
    for (const auto& e : snap.visible_enemies) {
      enemy_units_[e.id] = {static_cast<UnitType>(e.type), {e.x, e.y}, tick_};
      now.insert(e.id);
    }
    for (auto it = enemy_units_.begin(); it != enemy_units_.end();) {
      const bool gone = !now.contains(it->first) &&
                        (doomed_.contains(it->first) || tick_ - it->second.tick > kForgetTicks);
      it = gone ? enemy_units_.erase(it) : std::next(it);
    }
    doomed_.clear();

    std::set<uint32_t> here;
    for (const auto& s : snap.visible_enemy_structures) {
      const auto type = static_cast<StructureType>(s.type);
      enemy_structures_[s.id] = {type, {s.x, s.y}, s.building != 0};
      here.insert(s.id);
      if (type == StructureType::CommandCore) enemy_core_ = {s.x, s.y};
    }
    for (auto it = enemy_structures_.begin(); it != enemy_structures_.end();) {
      const bool gone = !here.contains(it->first) && visible_[idx(it->second.pos)];
      it = gone ? enemy_structures_.erase(it) : std::next(it);
    }
  }

  int32_t enemy_army_value() const {
    int32_t v = 0;
    for (const auto& [id, e] : enemy_units_) v += value(e.type);
    return v;
  }

  void sort_units(const Snapshot& snap) {
    drones_.clear();
    army_.clear();
    bool scout_alive = false;
    for (const auto& u : snap.my_units) {
      if (static_cast<UnitType>(u.type) == UnitType::Drone) drones_.push_back(&u);
      else if (u.id == scout_) scout_alive = true;
      else army_.push_back(&u);
    }
    if (scout_ != 0 && !scout_alive) {
      scout_ = 0;
      scout_ready_ = tick_ + kScoutCooldown;
    }
  }

  void find_threats(const Snapshot& snap) {
    home_threats_.clear();
    threats_.clear();
    for (const auto& e : snap.visible_enemies) {
      bool near_home = false, near_any = false;
      for (const auto& s : snap.my_structures) {
        if (manhattan({e.x, e.y}, {s.x, s.y}) > kDefendRadius) continue;
        near_any = true;
        if (static_cast<StructureType>(s.type) != StructureType::ClaimNode) near_home = true;
      }
      if (near_home) home_threats_.push_back(&e);
      if (near_any) threats_.push_back(&e);
    }
  }

  // ---- Economy ------------------------------------------------------------------------------

  UnitType choose_ship(int32_t energy, size_t army_size) const {
    if (army_size >= kBattleshipMinArmy &&
        energy >= cfg::BATTLESHIP_COST_E + kBattleshipSpareE)
      return UnitType::Battleship;
    std::map<UnitType, int32_t> seen;
    for (const auto& [id, e] : enemy_units_) ++seen[e.type];
    // Interceptor > Frigate > Artillery > Interceptor.
    const std::array<UnitType, 3> kinds{UnitType::Interceptor, UnitType::Frigate,
                                        UnitType::Artillery};
    std::array<int32_t, 3> want{2 + kCounterWeight * seen[UnitType::Frigate],
                                2 + kCounterWeight * seen[UnitType::Artillery],
                                1 + kCounterWeight * seen[UnitType::Interceptor]};
    std::array<int32_t, 3> have{};
    for (const UnitView* u : army_)
      for (size_t k = 0; k < kinds.size(); ++k)
        if (static_cast<UnitType>(u->type) == kinds[k]) ++have[k];
    int32_t want_total = 0, have_total = 0;
    for (size_t k = 0; k < kinds.size(); ++k) {
      want_total += want[k];
      have_total += have[k];
    }
    size_t best = 0;
    int64_t best_gap = INT64_MIN;
    for (size_t k = 0; k < kinds.size(); ++k) {
      const int64_t gap = int64_t{want[k]} * (have_total + 1) - int64_t{have[k]} * want_total;
      if (gap > best_gap) {
        best_gap = gap;
        best = k;
      }
    }
    return kinds[best];
  }

  // A free open tile 2–4 from the Core, clear of resource nodes and with all four sides
  // passable, so the Factory blocks no harvest and always has room to launch ships.
  Vec2 factory_site(const std::set<Vec2>& taken) const {
    Vec2 best{-1, -1};
    int32_t best_d = INT32_MAX;
    for (int32_t y = home_.y - 4; y <= home_.y + 4; ++y)
      for (int32_t x = home_.x - 4; x <= home_.x + 4; ++x) {
        const Vec2 p{x, y};
        const int32_t d = manhattan(p, home_);
        if (d < 2 || d > 4 || !map_.passable(p) || taken.contains(p)) continue;
        if (map_.tile_at(p).terrain != Terrain::Open || near_resource(map_, p)) continue;
        bool open = true;
        for (const Vec2& dir : DIRS) open = open && map_.passable(p + dir);
        if (!open) continue;
        const int32_t score = d * 1000 + manhattan(p, enemy_core_);
        if (score < best_d) {
          best_d = score;
          best = p;
        }
      }
    return best;
  }

  void plan_economy(const Snapshot& snap) {
    int32_t energy = snap.header.energy;
    int32_t alloy = snap.header.alloy;

    std::set<Vec2> taken, claimed;
    for (const auto& u : snap.my_units) taken.insert({u.x, u.y});
    for (const auto& e : snap.visible_enemies) taken.insert({e.x, e.y});
    for (const auto& s : snap.visible_enemy_structures) taken.insert({s.x, s.y});
    int32_t finished_claims = 0, factories = 0;
    bool factory_building = false;
    for (const auto& s : snap.my_structures) {
      taken.insert({s.x, s.y});
      const auto type = static_cast<StructureType>(s.type);
      if (type == StructureType::ClaimNode) {
        claimed.insert({s.x, s.y});
        if (s.build_ticks_left == 0) ++finished_claims;
      } else if (type == StructureType::Factory) {
        ++factories;
        if (s.build_ticks_left != 0) factory_building = true;
      }
    }

    const int32_t cap = std::max(finished_claims / cfg::CLAIMS_PER_FACTORY, cfg::MIN_FACTORY_CAP);
    bool want_factory = false;
    int32_t reserve_e = 0, reserve_a = 0;
    if (factories < cap && !factory_building) {
      if (factories == 0) {
        want_factory = finished_claims >= kClaimsBeforeFactory || !home_threats_.empty();
        if (want_factory) {
          reserve_e = cfg::FACTORY_COST_E;
          reserve_a = cfg::FACTORY_COST_A;
        }
      } else {
        want_factory = energy >= cfg::FACTORY_COST_E + cfg::FRIGATE_COST_E &&
                       alloy >= cfg::FACTORY_COST_A + cfg::CLAIM_COST_A;
      }
    }

    const StructureView* core = find_structure(snap, StructureType::CommandCore);
    if (core && core->producing == PRODUCING_NONE && energy >= cfg::DRONE_COST_E) {
      Command c = move_cmd(core->id, CommandKind::Produce, {0, 0});
      c.aux = static_cast<uint16_t>(UnitType::Drone);
      issue(c); // refused at no cost while at the Drone cap
    }

    for (const auto& s : snap.my_structures) {
      if (static_cast<StructureType>(s.type) != StructureType::Factory || s.build_ticks_left != 0 ||
          s.producing != PRODUCING_NONE)
        continue;
      const UnitType t = choose_ship(energy, army_.size());
      const auto [ce, ca] = cost(t);
      if (energy - reserve_e < ce || alloy - reserve_a < ca) break;
      Command c = move_cmd(s.id, CommandKind::Produce, {0, 0});
      c.aux = static_cast<uint16_t>(t);
      issue(c);
      energy -= ce;
      alloy -= ca;
    }

    plan_drones(snap, want_factory, energy, alloy, alloy, claimed, taken);
  }

  void plan_drones(const Snapshot& snap, bool want_factory, int32_t energy, int32_t free_alloy,
                   int32_t alloy, const std::set<Vec2>& claimed, const std::set<Vec2>& taken) {
    const auto builds = builders(snap);
    std::set<uint32_t> alive;
    for (const UnitView* d : drones_) alive.insert(d->id);

    const bool afford_factory = energy >= cfg::FACTORY_COST_E && alloy >= cfg::FACTORY_COST_A;
    if (!want_factory || !afford_factory || !alive.contains(factory_builder_) ||
        builds.contains(factory_builder_))
      factory_builder_ = 0;
    if (want_factory && afford_factory && factory_builder_ == 0) {
      int32_t best = INT32_MAX;
      for (const UnitView* d : drones_) {
        if (builds.contains(d->id) || lattice_.assigned(d->id)) continue;
        const int32_t dist = manhattan({d->x, d->y}, home_);
        if (dist < best) {
          best = dist;
          factory_builder_ = d->id;
        }
      }
    }

    for (const UnitView* d : drones_) {
      if (!lattice_.assigned(d->id)) continue;
      const auto site = lattice_.site_of(d->id);
      if (builds.contains(d->id) || (site && claimed.contains(*site))) lattice_.release(d->id);
    }
    // Claimers are the newest Drones, sent only while alloy covers every claim in flight.
    for (auto it = drones_.rbegin(); it != drones_.rend(); ++it) {
      const UnitView* d = *it;
      if (lattice_.exhausted() || lattice_.claimers() >= kMaxClaimers) break;
      const int32_t in_flight = static_cast<int32_t>(lattice_.claimers()) + 1;
      if (free_alloy < cfg::CLAIM_COST_A * in_flight) break;
      if (lattice_.assigned(d->id) || builds.contains(d->id) || d->id == factory_builder_)
        continue;
      lattice_.step(router_, *d, claimed, taken, free_alloy,
                    [&](const Command& c) { issue(c); });
    }

    for (const UnitView* d : drones_) {
      if (busy_.contains(d->id)) continue;
      const Vec2 pos{d->x, d->y};
      if (auto b = builds.find(d->id); b != builds.end()) {
        const Vec2 step = builder_step(router_, *d, b->second);
        if (step != pos) issue(move_cmd(d->id, CommandKind::Move, step));
        continue;
      }
      if (d->id == factory_builder_) {
        const Vec2 site = factory_site(taken);
        if (site.x < 0) continue;
        if (manhattan(pos, site) <= cfg::BUILD_RANGE) {
          Command c = move_cmd(d->id, CommandKind::Build, site);
          c.aux = static_cast<uint16_t>(StructureType::Factory);
          issue(c);
          continue;
        }
        const Vec2 step = router_.next_step(
            pos, [&](Vec2 p) { return manhattan(p, site) <= cfg::BUILD_RANGE; });
        if (step != pos) issue(move_cmd(d->id, CommandKind::Move, step));
        continue;
      }
      if (lattice_.assigned(d->id)) {
        lattice_.step(router_, *d, claimed, taken, free_alloy,
                      [&](const Command& c) { issue(c); });
        continue;
      }
      const Vec2 step = harvest_step(router_, map_, snap, *d);
      if (step != pos) issue(move_cmd(d->id, CommandKind::Move, step));
    }
  }

  // ---- Scout --------------------------------------------------------------------------------

  void plan_scout(const Snapshot& snap) {
    if (scout_ == 0 && tick_ >= scout_ready_) {
      for (const UnitView* u : army_)
        if (static_cast<UnitType>(u->type) == UnitType::Interceptor) {
          scout_ = u->id;
          scout_hidden_ = false;
          army_.erase(std::find(army_.begin(), army_.end(), u));
          break;
        }
    }
    const UnitView* s = nullptr;
    for (const auto& u : snap.my_units)
      if (u.id == scout_) s = &u;
    if (!s) return;

    bool core_seen = false;
    for (const auto& [id, e] : enemy_structures_)
      core_seen = core_seen || e.type == StructureType::CommandCore;
    if (core_seen && !scout_hidden_) {
      scout_hidden_ = true;
      scout_hide_ = {s->x, s->y};
      int32_t best = INT32_MAX;
      for (Vec2 n : nebulae_) {
        const int32_t d = manhattan(n, enemy_core_);
        if (d < 2 || d > kScoutRange) continue;
        const int32_t score = std::abs(d - kScoutWatch);
        if (score < best) {
          best = score;
          scout_hide_ = n;
        }
      }
    }
    const Vec2 to = scout_hidden_ ? scout_hide_ : enemy_core_;
    const int32_t near = scout_hidden_ ? 0 : 2;
    go(*s, [&](Vec2 p) { return manhattan(p, to) <= near; }, CommandKind::Move);
    if (!busy_.contains(s->id)) attack(s->id, 0);
  }

  // ---- Army ---------------------------------------------------------------------------------

  void set_mode(Mode m) {
    mode_ = m;
    mode_since_ = tick_;
  }

  static Vec2 centroid(const std::vector<const UnitView*>& units) {
    int64_t x = 0, y = 0;
    for (const UnitView* u : units) {
      x += u->x;
      y += u->y;
    }
    const auto n = static_cast<int64_t>(units.size());
    return {static_cast<int32_t>(x / n), static_cast<int32_t>(y / n)};
  }

  // Next push target: enemy claims, unfinished first, then the Command Core; nearest the army
  // first. Before the Core is seen, its guessed tile (id 0).
  void choose_target(Vec2 from) {
    int32_t best = INT32_MAX;
    target_id_ = 0;
    target_ = enemy_core_;
    for (const auto& [id, s] : enemy_structures_) {
      int32_t rank = 2;
      if (s.type == StructureType::ClaimNode) rank = s.building ? 0 : 1;
      else if (s.type != StructureType::CommandCore) continue;
      const int32_t score = rank * 100000 + manhattan(from, s.pos);
      if (score < best) {
        best = score;
        target_id_ = id;
        target_ = s.pos;
      }
    }
  }

  // Where to gather before striking `target_`: kStageMin–kStageMax from it, a nebula if
  // possible, and the least detour from where the army is.
  Vec2 choose_stage(Vec2 from) const {
    Vec2 best{from};
    int64_t best_score = INT64_MAX;
    for (int32_t y = target_.y - kStageMax; y <= target_.y + kStageMax; ++y)
      for (int32_t x = target_.x - kStageMax; x <= target_.x + kStageMax; ++x) {
        const Vec2 p{x, y};
        const int32_t d = manhattan(p, target_);
        if (d < kStageMin || d > kStageMax || !map_.passable(p)) continue;
        const int64_t score = (nebula(p) ? 0 : 1'000'000) + manhattan(from, p) + d;
        if (score < best_score) {
          best_score = score;
          best = p;
        }
      }
    return best;
  }

  void start_push(Vec2 c) {
    squad_.clear();
    for (const UnitView* u : army_) squad_.insert(u->id);
    choose_target(c);
    stage_ = choose_stage(c);
    set_mode(Mode::Stage);
  }

  // Assigns each ship with an enemy in range its focus-fire target (see the class comment);
  // enemy structures only when no unit is in range, the push target first.
  void plan_focus(const Snapshot& snap) {
    focus_.clear();
    std::map<uint32_t, int32_t> left;
    for (const auto& e : snap.visible_enemies) left[e.id] = e.hp + e.shield;
    std::vector<const UnitView*> shooters = army_;
    for (const auto& u : snap.my_units)
      if (u.id == scout_) shooters.push_back(&u);
    std::sort(shooters.begin(), shooters.end(),
              [](const UnitView* a, const UnitView* b) { return a->id < b->id; });
    for (const UnitView* u : shooters) {
      const UnitStats& st = stats_for(static_cast<UnitType>(u->type));
      const EnemyView* best = nullptr;
      for (const auto& e : snap.visible_enemies) {
        if (manhattan({u->x, u->y}, {e.x, e.y}) > st.range) continue;
        const bool e_drone = static_cast<UnitType>(e.type) == UnitType::Drone;
        const auto key = std::tuple{left[e.id] <= 0, e_drone, left[e.id], e.id};
        if (!best || key < std::tuple{left[best->id] <= 0,
                                      static_cast<UnitType>(best->type) == UnitType::Drone,
                                      left[best->id], best->id})
          best = &e;
      }
      if (best) {
        focus_[u->id] = best->id;
        left[best->id] -= st.dmg;
        if (left[best->id] <= 0) doomed_.insert(best->id);
        continue;
      }
      const EnemyStructureView* sbest = nullptr;
      for (const auto& s : snap.visible_enemy_structures) {
        if (manhattan({u->x, u->y}, {s.x, s.y}) > st.range) continue;
        const auto key = std::tuple{s.id != target_id_, s.building == 0, s.hp, s.id};
        if (!sbest ||
            key < std::tuple{sbest->id != target_id_, sbest->building == 0, sbest->hp, sbest->id})
          sbest = &s;
      }
      if (sbest) focus_[u->id] = sbest->id;
    }
  }

  void plan_army(const Snapshot& snap) {
    plan_focus(snap);
    // A push is a wave: only the squad it started with goes. Ships built meanwhile hold at
    // home, so reinforcements don't cross the map one at a time into the enemy base.
    std::vector<const UnitView*> squad;
    for (const UnitView* u : army_)
      if (mode_ == Mode::Hold || squad_.contains(u->id)) squad.push_back(u);
    if (squad.empty()) {
      if (mode_ != Mode::Hold) set_mode(Mode::Hold);
      squad = army_;
    }
    if (squad.empty()) return;
    const Vec2 c = centroid(squad);
    int32_t mine = 0;
    for (const UnitView* u : squad) mine += value(static_cast<UnitType>(u->type));
    int32_t met = 0;
    for (const auto& e : snap.visible_enemies) {
      bool near = false;
      for (const UnitView* u : squad)
        near = near || manhattan({u->x, u->y}, {e.x, e.y}) <= kEngageRadius;
      if (near) met += value(static_cast<UnitType>(e.type));
    }

    if (mode_ == Mode::Stage || mode_ == Mode::Strike) {
      if (!home_threats_.empty()) set_mode(Mode::Hold);
      else if (int64_t{mine} * 100 < int64_t{met} * kRetreatPct) set_mode(Mode::Retreat);
      else if (target_id_ == 0 || !enemy_structures_.contains(target_id_)) {
        const uint32_t was = target_id_;
        choose_target(c);
        if (target_id_ == 0 && manhattan(c, target_) <= 2) {
          set_mode(Mode::Hold); // nothing at the guessed spawn
        } else if (target_id_ != was && manhattan(c, target_) > kStageMax) {
          stage_ = choose_stage(c);
          set_mode(Mode::Stage);
        }
      }
    }
    if (mode_ == Mode::Retreat &&
        (manhattan(c, ambush_) <= kGatherRadius * 2 || tick_ - mode_since_ > kRetreatTicks))
      set_mode(Mode::Hold);
    if (mode_ == Mode::Hold) {
      squad_.clear();
      if (threats_.empty()) {
        const int32_t theirs = enemy_army_value();
        const bool stronger =
            theirs == 0 ? mine >= kBlindPushValue
                        : mine >= kMinPushValue &&
                              int64_t{mine} * 100 >= int64_t{theirs} * kPushMarginPct;
        if (stronger) start_push(c);
      }
    }
    if (mode_ == Mode::Stage) {
      int32_t gathered = 0;
      for (const UnitView* u : squad)
        if (manhattan({u->x, u->y}, stage_) <= kGatherRadius + 1) ++gathered;
      if (gathered * 100 >= static_cast<int32_t>(squad.size()) * kGatherPct ||
          tick_ - mode_since_ > kGatherTimeout)
        set_mode(Mode::Strike);
    }

    std::set<Vec2> occupied;
    for (const auto& u : snap.my_units) occupied.insert({u.x, u.y});
    for (const UnitView* u : army_) {
      if (busy_.contains(u->id)) continue;
      const Mode mode = squad_.contains(u->id) ? mode_ : Mode::Hold;
      if (mode == Mode::Retreat) {
        go(*u, [&](Vec2 p) { return manhattan(p, ambush_) <= kGatherRadius; }, CommandKind::Move);
        continue;
      }
      if (auto f = focus_.find(u->id); f != focus_.end()) {
        attack(u->id, f->second);
        continue;
      }
      const int32_t range = stats_for(static_cast<UnitType>(u->type)).range;
      const Vec2 pos{u->x, u->y};
      // Enemy ships first: threats to our structures, and while pushing any met on the way.
      const EnemyView* foe = nullptr;
      int32_t foe_d = INT32_MAX;
      auto consider = [&](const EnemyView* e) {
        const int32_t d = manhattan(pos, {e->x, e->y});
        if (d < foe_d) {
          foe_d = d;
          foe = e;
        }
      };
      const auto& defend = mode == Mode::Hold ? threats_ : home_threats_;
      for (const EnemyView* e : defend) consider(e);
      if (!foe && mode != Mode::Hold)
        for (const auto& e : snap.visible_enemies)
          if (static_cast<UnitType>(e.type) != UnitType::Drone &&
              manhattan(pos, {e.x, e.y}) <= kEngageRadius)
            consider(&e);
      if (foe) {
        const Vec2 at{foe->x, foe->y};
        go(*u, [&](Vec2 p) { return manhattan(p, at) <= range; }, CommandKind::MoveAttack);
        continue;
      }
      if (mode == Mode::Stage) {
        go(*u, [&](Vec2 p) { return manhattan(p, stage_) <= kGatherRadius; },
           CommandKind::MoveAttack);
      } else if (mode == Mode::Strike) {
        go(*u, [&](Vec2 p) { return manhattan(p, target_) <= range; }, CommandKind::MoveAttack);
      } else if (!ambush_tiles_.empty() && !in_ambush(pos)) {
        const bool room = std::any_of(ambush_tiles_.begin(), ambush_tiles_.end(),
                                      [&](Vec2 t) { return !occupied.contains(t); });
        go(*u,
           [&](Vec2 p) {
             return room ? in_ambush(p) && !occupied.contains(p)
                         : manhattan(p, ambush_) <= kGatherRadius;
           },
           CommandKind::MoveAttack);
      } else if (ambush_tiles_.empty()) {
        go(*u, [&](Vec2 p) { return manhattan(p, ambush_) <= kGatherRadius; },
           CommandKind::MoveAttack);
      } else {
        attack(u->id, 0);
      }
    }
  }
};

} // namespace game
