#pragma once
#include "bot_iface.hpp"
#include "bot_util.hpp"
#include "config.hpp"
#include "entity.hpp"
#include <algorithm>
#include <climits>
#include <set>
#include <vector>

namespace game {

// The turtle: holds one Claim Node beside its base and never reaches for more. Drones
// harvest, combat units stay within kGuardRadius of home and only engage enemies that come
// inside it. Attrition can't beat it, so the descending territory threshold has to
// (IMPLEMENTATION_PLAN Phase 3).
class TurtleBot : public Bot {
public:
  static constexpr int32_t kGuardRadius = 6;
  static constexpr int32_t kHomeRadius = 2;

  void on_init(const Map& map, uint32_t) override {
    map_ = map;
    router_.reset(map);
  }

  std::vector<Command> on_tick(const Snapshot& snap) override {
    if (!have_home_) init_home(snap);
    router_.observe(snap);
    std::vector<Command> out;

    std::set<Vec2> own;
    for (const auto& u : snap.my_units) own.insert({u.x, u.y});
    for (const auto& s : snap.my_structures) own.insert({s.x, s.y});

    for (const auto& s : snap.visible_enemy_structures) own.insert({s.x, s.y});
    for (const auto& e : snap.visible_enemies) own.insert({e.x, e.y});

    // Replace the home claim whenever it is missing: not yet placed or destroyed. A Drone
    // deploys it on the free tile within reach that is closest to home, and skips its usual
    // order that tick. Drones work beside home, so that is close enough to count as home.
    uint32_t deployer = 0;
    const bool have_claim = find_structure(snap, StructureType::ClaimNode) != nullptr;
    if (!have_claim && snap.header.alloy >= cfg::CLAIM_COST_A) {
      for (const auto& u : snap.my_units) {
        if (static_cast<UnitType>(u.type) != UnitType::Drone) continue;
        const Vec2 site = claim_site({u.x, u.y}, own);
        if (site.x < 0) continue;
        deployer = u.id;
        out.push_back(move_cmd(deployer, CommandKind::DeployClaim, site));
        break;
      }
    }

    for (const auto& u : snap.my_units) {
      if (u.id == deployer) continue;
      const Vec2 pos{u.x, u.y};
      if (static_cast<UnitType>(u.type) == UnitType::Drone) {
        const Vec2 step =
            router_.next_step(pos, [&](Vec2 p) { return near_resource(map_, p); });
        if (step != pos) out.push_back(move_cmd(u.id, CommandKind::Move, step));
        continue;
      }
      const Vec2 target = intruder(snap);
      if (target.x >= 0) {
        out.push_back(move_cmd(u.id, CommandKind::MoveAttack, target));
        continue;
      }
      const Vec2 step =
          router_.next_step(pos, [&](Vec2 p) { return manhattan(p, home_) <= kHomeRadius; });
      if (step != pos) out.push_back(move_cmd(u.id, CommandKind::Move, step));
    }
    return out;
  }

  bool healthy() const override { return true; }

private:
  Map map_;
  Router router_;
  bool have_home_{false};
  Vec2 home_{};

  void init_home(const Snapshot& snap) {
    have_home_ = true;
    home_ = core_pos(snap);
  }

  Vec2 claim_site(Vec2 drone, const std::set<Vec2>& taken) const {
    Vec2 best{-1, -1};
    int32_t best_dist = INT32_MAX;
    const int32_t r = cfg::CLAIM_DEPLOY_RANGE;
    for (int32_t y = drone.y - r; y <= drone.y + r; ++y)
      for (int32_t x = drone.x - r; x <= drone.x + r; ++x) {
        const Vec2 p{x, y};
        if (manhattan(p, drone) > r || !map_.passable(p) || taken.contains(p)) continue;
        if (map_.tile_at(p).terrain == Terrain::ResourceNode) continue;
        const int32_t d = manhattan(p, home_);
        if (d < best_dist) {
          best_dist = d;
          best = p;
        }
      }
    return best;
  }

  Vec2 intruder(const Snapshot& snap) const {
    Vec2 best{-1, -1};
    int32_t best_dist = INT32_MAX;
    for (const auto& e : snap.visible_enemies) {
      const int32_t d = manhattan({e.x, e.y}, home_);
      if (d <= kGuardRadius && d < best_dist) {
        best_dist = d;
        best = {e.x, e.y};
      }
    }
    return best;
  }
};

} // namespace game
