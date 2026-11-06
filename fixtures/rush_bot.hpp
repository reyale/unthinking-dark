#pragma once
#include "bot_iface.hpp"
#include "bot_util.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "stats.hpp"
#include <climits>
#include <map>
#include <set>
#include <vector>

namespace game {

// The rush: kClaims claims by home (enough territory that the threshold can't end the game at
// once, and a Drone slot each), then a Factory, then Interceptors from it nonstop, sent in a
// wave every kWave ships. It never defends: it bets the game on tearing down the enemy's claims
// and base before the enemy's economy pulls ahead.
//
// Ships gather at a rally point kRallyDist out toward the enemy, clear of resource nodes where
// it can be so they don't block a harvest, until kWave are there. The wave then strikes the nearest enemy claim
// it knows of, unfinished first, and the Command Core (the mirrored spawn until the real one
// is seen) once none is left, shooting whatever comes into range on the way (focus_fire). Ships
// built after a wave leaves gather for the next.
class RushBot : public Bot {
public:
  static constexpr int32_t kSpacing = cfg::CLAIM_INFLUENCE / 2;
  static constexpr size_t kClaims = 2;
  static constexpr size_t kWave = 5;
  static constexpr int32_t kRallyDist = 5;
  static constexpr int32_t kGatherRadius = 2;
  static constexpr int32_t kApproach = cfg::INTERCEPTOR_RANGE;

  void on_init(const Map& map, uint32_t, uint64_t) override {
    map_ = map;
    router_.reset(map);
  }

  std::vector<Command> on_tick(const Snapshot& snap) override {
    if (!have_home_) init(snap);
    router_.observe(snap);
    out_.clear();
    busy_.clear();
    remember(snap);
    plan_economy(snap);
    plan_army(snap);
    return out_;
  }

  bool healthy() const override { return true; }

private:
  static constexpr int32_t kGoalNear = 0;

  Map map_;
  Router router_;
  ClaimLattice lattice_;
  bool have_home_{false};
  Vec2 home_{};
  Vec2 enemy_core_{};
  std::map<uint32_t, EnemyStructureView> enemy_structures_; // as last seen
  Vec2 rally_{};
  std::set<uint32_t> wave_; // the ships of the wave under way; empty while gathering

  std::vector<Command> out_;
  std::set<uint32_t> busy_;

  void issue(const Command& c) {
    out_.push_back(c);
    busy_.insert(c.entity_id);
  }

  void init(const Snapshot& snap) {
    have_home_ = true;
    home_ = core_pos(snap);
    enemy_core_ = mirrored(map_, home_);
    lattice_.init(map_, home_, kSpacing);
    // The gather radius, the tile beyond it a ship may wait on, and the tile beside a node it
    // is harvested from.
    rally_ = rally_point(map_, home_, enemy_core_, kRallyDist, kGatherRadius + 2);
  }

  // Enemy structures never move: one seen stays known until its tile is seen without it.
  void remember(const Snapshot& snap) {
    std::set<uint32_t> here;
    for (const auto& s : snap.visible_enemy_structures) {
      enemy_structures_[s.id] = s;
      here.insert(s.id);
      if (static_cast<StructureType>(s.type) == StructureType::CommandCore)
        enemy_core_ = {s.x, s.y};
    }
    std::set<Vec2> visible;
    for (const auto& t : snap.visible_tiles) visible.insert({t.x, t.y});
    std::erase_if(enemy_structures_, [&](const auto& kv) {
      return !here.contains(kv.first) && visible.contains({kv.second.x, kv.second.y});
    });
  }

  // The wave's target: the nearest known enemy claim, unfinished first, else the Core.
  std::pair<uint32_t, Vec2> target(Vec2 from) const {
    std::pair<uint32_t, Vec2> best{0, enemy_core_};
    int64_t best_score = INT64_MAX;
    for (const auto& [id, s] : enemy_structures_) {
      const auto type = static_cast<StructureType>(s.type);
      int64_t rank = 2;
      if (type == StructureType::ClaimNode) rank = s.building ? 0 : 1;
      else if (type != StructureType::CommandCore) continue;
      const int64_t score = rank * 1'000'000 + manhattan(from, {s.x, s.y});
      if (score < best_score) {
        best_score = score;
        best = {id, {s.x, s.y}};
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
    bool have_factory = false;
    for (const auto& s : snap.my_structures) {
      taken.insert({s.x, s.y});
      const auto type = static_cast<StructureType>(s.type);
      if (type == StructureType::ClaimNode) claimed.insert({s.x, s.y});
      if (type == StructureType::Factory) have_factory = true;
    }

    const StructureView* core = find_structure(snap, StructureType::CommandCore);
    if (core && core->producing == PRODUCING_NONE && energy >= cfg::DRONE_COST_E)
      issue(produce_cmd(core->id, UnitType::Drone)); // refused at no cost while at the cap
    const bool claiming = claimed.size() < kClaims && !lattice_.exhausted();
    const int32_t reserve_a = claiming ? cfg::CLAIM_COST_A : 0;
    for (const auto& s : snap.my_structures) {
      if (static_cast<StructureType>(s.type) != StructureType::Factory ||
          s.build_ticks_left != 0 || s.producing != PRODUCING_NONE)
        continue;
      if (energy < cfg::INTERCEPTOR_COST_E || alloy - reserve_a < cfg::INTERCEPTOR_COST_A) break;
      issue(produce_cmd(s.id, UnitType::Interceptor));
      energy -= cfg::INTERCEPTOR_COST_E;
      alloy -= cfg::INTERCEPTOR_COST_A;
    }

    // Claims first, one claimer at a time; then the oldest free Drone builds the Factory.
    const auto builds = builders(snap);
    bool factory_builder_chosen = false;
    for (const auto& d : snap.my_units) {
      const auto site = lattice_.site_of(d.id);
      if (builds.contains(d.id) || (site && claimed.contains(*site))) lattice_.release(d.id);
    }
    for (const auto& d : snap.my_units) {
      if (static_cast<UnitType>(d.type) != UnitType::Drone || busy_.contains(d.id)) continue;
      const Vec2 pos{d.x, d.y};
      if (auto b = builds.find(d.id); b != builds.end()) {
        const Vec2 step = builder_step(router_, d, b->second);
        if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
        continue;
      }
      if (claiming && (lattice_.assigned(d.id) || lattice_.claimers() == 0) &&
          alloy >= cfg::CLAIM_COST_A) {
        lattice_.step(router_, d, claimed, taken, alloy, [&](const Command& c) { issue(c); });
        if (busy_.contains(d.id)) continue;
      }
      lattice_.release(d.id);
      if (!claiming && !have_factory && !factory_builder_chosen &&
          energy >= cfg::FACTORY_COST_E && alloy >= cfg::FACTORY_COST_A) {
        factory_builder_chosen = true;
        if (build_factory(d, taken)) continue;
      }
      const Vec2 step = harvest_step(router_, map_, snap, d);
      if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
    }
  }

  // Walks the Drone to the Factory site and builds there; false if there is no site.
  bool build_factory(const UnitView& d, const std::set<Vec2>& taken) {
    const Vec2 site = factory_site(map_, home_, enemy_core_, taken);
    if (site.x < 0) return false;
    const Vec2 pos{d.x, d.y};
    if (manhattan(pos, site) <= cfg::BUILD_RANGE) {
      Command c = move_cmd(d.id, CommandKind::Build, site);
      c.aux = static_cast<uint16_t>(StructureType::Factory);
      issue(c);
      return true;
    }
    const Vec2 step = site_step(router_, pos, site, cfg::BUILD_RANGE);
    if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
    return true;
  }

  void go_near(const UnitView& u, Vec2 at, int32_t radius) {
    const Vec2 pos{u.x, u.y};
    const Vec2 to = route_waypoint(router_, {kGoalNear, at, radius}, pos,
                                   [&](Vec2 p) { return manhattan(p, at) <= radius; },
                                   stats_for(static_cast<UnitType>(u.type)).speed);
    issue(to != pos ? move_cmd(u.id, CommandKind::Move, to)
                    : move_cmd(u.id, CommandKind::Attack, {0, 0}));
  }

  void plan_army(const Snapshot& snap) {
    std::vector<const UnitView*> ships;
    for (const auto& u : snap.my_units)
      if (static_cast<UnitType>(u.type) != UnitType::Drone) ships.push_back(&u);

    std::set<uint32_t> alive;
    for (const UnitView* u : ships) alive.insert(u->id);
    std::erase_if(wave_, [&](uint32_t id) { return !alive.contains(id); });
    if (wave_.empty()) {
      size_t gathered = 0;
      for (const UnitView* u : ships)
        if (manhattan({u->x, u->y}, rally_) <= kGatherRadius + 1) ++gathered;
      if (gathered >= kWave)
        for (const UnitView* u : ships) wave_.insert(u->id);
    }

    Vec2 c{rally_};
    if (!wave_.empty()) {
      int64_t x = 0, y = 0;
      for (const UnitView* u : ships)
        if (wave_.contains(u->id)) {
          x += u->x;
          y += u->y;
        }
      const auto n = static_cast<int64_t>(wave_.size());
      c = {static_cast<int32_t>(x / n), static_cast<int32_t>(y / n)};
    }
    const auto [target_id, target_at] = target(c);
    const auto focus = focus_fire(snap, ships, target_id);
    for (const UnitView* u : ships) {
      if (auto f = focus.find(u->id); f != focus.end()) {
        Command c = move_cmd(u->id, CommandKind::Attack, {0, 0});
        c.target_id = f->second;
        issue(c);
        continue;
      }
      if (wave_.contains(u->id)) go_near(*u, target_at, kApproach);
      else go_near(*u, rally_, kGatherRadius);
    }
  }
};

} // namespace game
