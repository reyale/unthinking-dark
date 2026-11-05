#pragma once
#include "bot_iface.hpp"
#include "bot_util.hpp"
#include "config.hpp"
#include "entity.hpp"
#include "stats.hpp"
#include <climits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace game {

// The boom: expands over the claim lattice as fast as alloy allows, fills every Drone slot the
// claims open, adds Factories as the cap rises, and builds ships only to defend. It never
// attacks, so it wins on territory or not at all; unlike the expander, it fights for its claims.
//
// Drones: a builder stays by each unfinished structure, the newest stake claims (kMinClaimers,
// or one Drone in kDronesPerClaimer if more) while alloy covers every claim in flight, and the
// rest harvest. The first Factory waits for kClaimsBeforeFactory finished claims; it and later
// ones, up to kMaxFactories, are built only from what claims leave.
// Factories make Frigates (first strike, the heaviest hull for the energy), with an Artillery
// once alloy runs past kAlloySpare, up to kBaseArmy ships plus one per finished claim: more
// would only crowd the map (and the unit cap) the Drones need.
//
// Ships go to whichever structure has an enemy within kDefendRadius, or is losing hp to one it
// can't see (a Frigate outranges a structure's sight), nearest first. Otherwise
// they spread out over the finished claims beyond kClearRadius of home, ship k (by id) beside
// claim k mod n (by id): parked round the Core they would wall in the Drones and the
// Factories' launch tiles. With no such claim yet they wait kClearRadius out toward the enemy.
class EconBot : public Bot {
public:
  static constexpr int32_t kSpacing = cfg::CLAIM_INFLUENCE / 2;
  static constexpr size_t kMinClaimers = 2;
  static constexpr size_t kDronesPerClaimer = 4;
  static constexpr int32_t kClaimsBeforeFactory = 2;
  static constexpr int32_t kMaxFactories = 3;
  static constexpr int32_t kAlloySpare = cfg::ARTILLERY_COST_A;
  static constexpr int32_t kBaseArmy = 12;
  static constexpr int32_t kDefendRadius = 8;
  static constexpr int32_t kGuardRadius = 2;
  static constexpr int32_t kClearRadius = 8;
  static constexpr int32_t kPostRadius = 3;

  void on_init(const Map& map, uint32_t) override {
    map_ = map;
    router_.reset(map);
  }

  std::vector<Command> on_tick(const Snapshot& snap) override {
    if (!have_home_) init(snap);
    router_.observe(snap);
    out_.clear();
    busy_.clear();
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
  Vec2 enemy_spawn_{};
  Vec2 post_{};
  uint32_t factory_builder_{0};
  std::map<uint32_t, int32_t> last_hp_; // own Core and Factories, by id, as of last tick

  std::vector<Command> out_;
  std::set<uint32_t> busy_;

  void issue(const Command& c) {
    out_.push_back(c);
    busy_.insert(c.entity_id);
  }

  void init(const Snapshot& snap) {
    have_home_ = true;
    home_ = core_pos(snap);
    enemy_spawn_ = mirrored(map_, home_);
    post_ = rally_point(map_, home_, enemy_spawn_, kClearRadius, kPostRadius + 2);
    lattice_.init(map_, home_, kSpacing);
  }

  void plan_economy(const Snapshot& snap) {
    int32_t energy = snap.header.energy;
    int32_t alloy = snap.header.alloy;
    std::set<Vec2> taken, claimed;
    for (const auto& u : snap.my_units) taken.insert({u.x, u.y});
    for (const auto& e : snap.visible_enemies) taken.insert({e.x, e.y});
    for (const auto& s : snap.visible_enemy_structures) taken.insert({s.x, s.y});
    int32_t finished_claims = 0, factories = 0;
    bool building = false;
    for (const auto& s : snap.my_structures) {
      taken.insert({s.x, s.y});
      const auto type = static_cast<StructureType>(s.type);
      if (type == StructureType::ClaimNode) {
        claimed.insert({s.x, s.y});
        if (s.build_ticks_left == 0) ++finished_claims;
      } else if (type == StructureType::Factory) {
        ++factories;
        building = building || s.build_ticks_left != 0;
      }
    }
    const int32_t cap = std::max(finished_claims / cfg::CLAIMS_PER_FACTORY, cfg::MIN_FACTORY_CAP);
    size_t drones = 0;
    for (const auto& u : snap.my_units) drones += static_cast<UnitType>(u.type) == UnitType::Drone;
    const size_t max_claimers = std::max(kMinClaimers, drones / kDronesPerClaimer);
    const int32_t claim_reserve =
        lattice_.exhausted() ? 0 : cfg::CLAIM_COST_A * static_cast<int32_t>(max_claimers);
    const bool want_factory = factories < std::min(cap, kMaxFactories) && !building &&
                              finished_claims >= kClaimsBeforeFactory &&
                              energy >= cfg::FACTORY_COST_E &&
                              alloy >= cfg::FACTORY_COST_A + claim_reserve;

    const StructureView* core = find_structure(snap, StructureType::CommandCore);
    if (core && core->producing == PRODUCING_NONE && energy >= cfg::DRONE_COST_E) {
      issue(produce_cmd(core->id, UnitType::Drone)); // refused at no cost while at the cap
      energy -= cfg::DRONE_COST_E;
    }
    int32_t ships = 0;
    for (const auto& u : snap.my_units) ships += static_cast<UnitType>(u.type) != UnitType::Drone;
    const int32_t reserve_e = want_factory ? cfg::FACTORY_COST_E : 0;
    const int32_t reserve_a = (want_factory ? cfg::FACTORY_COST_A : 0) + claim_reserve;
    for (const auto& s : snap.my_structures) {
      if (static_cast<StructureType>(s.type) != StructureType::Factory ||
          s.build_ticks_left != 0 || s.producing != PRODUCING_NONE)
        continue;
      if (ships >= kBaseArmy + finished_claims) break;
      const UnitType t =
          alloy - reserve_a >= kAlloySpare ? UnitType::Artillery : UnitType::Frigate;
      const auto [ce, ca] = unit_cost(t);
      if (energy - reserve_e < ce || alloy - reserve_a < ca) break;
      issue(produce_cmd(s.id, t));
      ++ships;
      energy -= ce;
      alloy -= ca;
    }

    plan_drones(snap, want_factory, max_claimers, alloy, claimed, taken);
  }

  void plan_drones(const Snapshot& snap, bool want_factory, size_t max_claimers, int32_t alloy,
                   const std::set<Vec2>& claimed, const std::set<Vec2>& taken) {
    std::vector<const UnitView*> drones;
    for (const auto& u : snap.my_units)
      if (static_cast<UnitType>(u.type) == UnitType::Drone) drones.push_back(&u);
    const auto builds = builders(snap);

    std::set<uint32_t> alive;
    for (const UnitView* d : drones) alive.insert(d->id);
    if (!want_factory || !alive.contains(factory_builder_) || builds.contains(factory_builder_))
      factory_builder_ = 0;
    if (want_factory && factory_builder_ == 0)
      for (const UnitView* d : drones)
        if (!builds.contains(d->id) && !lattice_.assigned(d->id)) {
          factory_builder_ = d->id;
          break;
        }

    for (const UnitView* d : drones) {
      const auto site = lattice_.site_of(d->id);
      if (builds.contains(d->id) || (site && claimed.contains(*site))) lattice_.release(d->id);
    }
    // Claimers are the newest Drones; the oldest stay on the harvest loop they already run.
    for (auto it = drones.rbegin(); it != drones.rend(); ++it) {
      const UnitView& d = **it;
      if (lattice_.exhausted() || lattice_.claimers() >= max_claimers) break;
      const int32_t in_flight = static_cast<int32_t>(lattice_.claimers()) + 1;
      if (alloy < cfg::CLAIM_COST_A * in_flight) break;
      if (lattice_.assigned(d.id) || builds.contains(d.id) || d.id == factory_builder_) continue;
      lattice_.step(router_, d, claimed, taken, alloy, [&](const Command& c) { issue(c); });
    }

    for (const UnitView* d : drones) {
      if (busy_.contains(d->id)) continue;
      const Vec2 pos{d->x, d->y};
      if (auto b = builds.find(d->id); b != builds.end()) {
        const Vec2 step = builder_step(router_, *d, b->second);
        if (step != pos) issue(move_cmd(d->id, CommandKind::Move, step));
        continue;
      }
      if (d->id == factory_builder_) {
        const Vec2 site = factory_site(map_, home_, enemy_spawn_, taken);
        if (site.x >= 0 && manhattan(pos, site) <= cfg::BUILD_RANGE) {
          Command c = move_cmd(d->id, CommandKind::Build, site);
          c.aux = static_cast<uint16_t>(StructureType::Factory);
          issue(c);
          continue;
        }
        if (site.x >= 0) {
          const Vec2 step = site_step(router_, pos, site, cfg::BUILD_RANGE);
          if (step != pos) issue(move_cmd(d->id, CommandKind::Move, step));
          continue;
        }
      }
      if (lattice_.assigned(d->id)) {
        lattice_.step(router_, *d, claimed, taken, alloy, [&](const Command& c) { issue(c); });
        continue;
      }
      const Vec2 step = harvest_step(router_, map_, snap, *d);
      if (step != pos) issue(move_cmd(d->id, CommandKind::Move, step));
    }
  }

  void go_near(const UnitView& u, Vec2 at, int32_t radius) {
    const Vec2 pos{u.x, u.y};
    const Vec2 to = route_waypoint(router_, {kGoalNear, at, radius}, pos,
                                   [&](Vec2 p) { return manhattan(p, at) <= radius; },
                                   stats_for(static_cast<UnitType>(u.type)).speed);
    issue(to != pos ? move_cmd(u.id, CommandKind::Move, to)
                    : move_cmd(u.id, CommandKind::Attack, {0, 0}));
  }

  // The own structure nearest `from` with an enemy unit within kDefendRadius, or that lost hp
  // since last tick (`hit`), or {-1, -1}.
  static Vec2 threatened(const Snapshot& snap, Vec2 from, const std::set<uint32_t>& hit) {
    Vec2 best{-1, -1};
    int32_t best_d = INT32_MAX;
    for (const auto& s : snap.my_structures) {
      bool threat = hit.contains(s.id);
      for (const auto& e : snap.visible_enemies)
        threat = threat || manhattan({e.x, e.y}, {s.x, s.y}) <= kDefendRadius;
      if (threat && manhattan(from, {s.x, s.y}) < best_d) {
        best_d = manhattan(from, {s.x, s.y});
        best = {s.x, s.y};
      }
    }
    return best;
  }

  void plan_army(const Snapshot& snap) {
    std::vector<const UnitView*> ships;
    for (const auto& u : snap.my_units)
      if (static_cast<UnitType>(u.type) != UnitType::Drone) ships.push_back(&u);
    std::vector<Vec2> posts;
    for (const auto& s : snap.my_structures)
      if (static_cast<StructureType>(s.type) == StructureType::ClaimNode &&
          s.build_ticks_left == 0 && manhattan({s.x, s.y}, home_) > kClearRadius)
        posts.push_back({s.x, s.y});
    // Claims are left out: territory moves their hp cap, so a drop needn't be an attack.
    std::set<uint32_t> hit;
    std::map<uint32_t, int32_t> hp_now;
    for (const auto& s : snap.my_structures) {
      if (static_cast<StructureType>(s.type) == StructureType::ClaimNode) continue;
      hp_now[s.id] = s.hp;
      if (auto it = last_hp_.find(s.id); it != last_hp_.end() && s.hp < it->second)
        hit.insert(s.id);
    }
    last_hp_ = std::move(hp_now);
    const auto focus = focus_fire(snap, ships);
    size_t k = 0;
    for (const UnitView* u : ships) {
      const size_t mine = k++;
      if (auto f = focus.find(u->id); f != focus.end()) {
        Command c = move_cmd(u->id, CommandKind::Attack, {0, 0});
        c.target_id = f->second;
        issue(c);
        continue;
      }
      const Vec2 guard = threatened(snap, {u->x, u->y}, hit);
      if (guard.x >= 0) go_near(*u, guard, kGuardRadius);
      else if (posts.empty()) go_near(*u, post_, kPostRadius);
      else go_near(*u, posts[mine % posts.size()], kGuardRadius);
    }
  }
};

} // namespace game
