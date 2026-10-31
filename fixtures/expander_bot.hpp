#pragma once
#include "bot_iface.hpp"
#include "bot_util.hpp"
#include "config.hpp"
#include "entity.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace game {

// Expands over a lattice of claim sites kSpacing apart, working outward from home, and never
// attacks: it can only win on territory, which makes it the opponent that proves a turtle loses
// to the threshold.
//
// Claims must be deployed by a Drone standing within CLAIM_DEPLOY_RANGE, so the bot needs
// Drones to spare: its Core produces up to kMaxDrones (and the Drone cap, one per claim plus
// one), the first kHarvesters ferry resources home and the rest walk to claim sites. A Drone
// stays by each claim it deploys until construction finishes. Combat units move kClearRadius out from
// home and hold there, so they can't box Drones in beside the base.
class ExpanderBot : public Bot {
public:
  static constexpr int32_t kSpacing = cfg::CLAIM_INFLUENCE / 2;
  static constexpr size_t kHarvesters = 1;
  static constexpr size_t kMaxDrones = 4;
  static constexpr int32_t kMaxAttempts = 3; // deploys at one site before giving it up
  static constexpr int32_t kClearRadius = 8;

  void on_init(const Map& map, uint32_t) override {
    map_ = map;
    router_.reset(map);
  }

  std::vector<Command> on_tick(const Snapshot& snap) override {
    if (!have_home_) init_sites(snap);
    router_.observe(snap);
    std::vector<Command> out;
    std::set<uint32_t> busy;
    auto issue = [&](const Command& c) {
      out.push_back(c);
      busy.insert(c.entity_id);
    };

    std::set<Vec2> taken; // tiles a claim can't go on, as far as we can see
    std::set<Vec2> claimed;
    for (const auto& u : snap.my_units) taken.insert({u.x, u.y});
    for (const auto& s : snap.my_structures) {
      taken.insert({s.x, s.y});
      if (static_cast<StructureType>(s.type) == StructureType::ClaimNode)
        claimed.insert({s.x, s.y});
    }
    for (const auto& e : snap.visible_enemies) taken.insert({e.x, e.y});
    for (const auto& s : snap.visible_enemy_structures) taken.insert({s.x, s.y});

    std::vector<const UnitView*> drones;
    for (const auto& u : snap.my_units)
      if (static_cast<UnitType>(u.type) == UnitType::Drone) drones.push_back(&u);

    int32_t alloy = snap.header.alloy;
    const StructureView* core = find_structure(snap, StructureType::CommandCore);
    if (core && core->producing == PRODUCING_NONE && drones.size() < kMaxDrones &&
        snap.header.energy >= cfg::DRONE_COST_E) {
      Command c = move_cmd(core->id, CommandKind::Produce, {0, 0});
      c.aux = static_cast<uint16_t>(UnitType::Drone);
      issue(c);
    }

    for (const auto& u : snap.my_units) {
      if (busy.contains(u.id) || static_cast<UnitType>(u.type) == UnitType::Drone) continue;
      const Vec2 pos{u.x, u.y};
      const Vec2 step =
          router_.next_step(pos, [&](Vec2 p) { return manhattan(p, home_) >= kClearRadius; });
      if (step != pos) issue(move_cmd(u.id, CommandKind::Move, step));
    }

    const auto builders = claim_builders(snap);
    for (size_t i = 0; i < drones.size(); ++i) {
      const UnitView& d = *drones[i];
      if (busy.contains(d.id)) continue;
      const Vec2 pos{d.x, d.y};
      if (auto b = builders.find(d.id); b != builders.end()) {
        const Vec2 step = builder_step(router_, d, b->second);
        if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
        continue;
      }
      // A lone Drone (the cap is one until the first claim stands) harvests until it can
      // afford a claim, then goes to stake it.
      const bool lone_claimer = drones.size() == 1 && alloy >= cfg::CLAIM_COST_A;
      if (i < kHarvesters && !lone_claimer) {
        assigned_.erase(d.id);
        const Vec2 step = harvest_step(router_, map_, snap, d);
        if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
        continue;
      }
      claim_with(d, claimed, taken, alloy, issue);
    }
    return out;
  }

  bool healthy() const override { return true; }

private:
  struct Assignment {
    size_t site;
    int32_t attempts;
  };
  Map map_;
  Router router_;
  bool have_home_{false};
  Vec2 home_{};
  std::vector<Vec2> sites_;
  size_t next_site_{0};
  std::map<uint32_t, Assignment> assigned_; // claimer drone id → its site

  template <class Issue>
  void claim_with(const UnitView& d, const std::set<Vec2>& claimed, const std::set<Vec2>& taken,
                  int32_t& alloy, Issue&& issue) {
    const Vec2 pos{d.x, d.y};
    for (;;) {
      auto it = assigned_.find(d.id);
      if (it == assigned_.end()) {
        if (next_site_ >= sites_.size()) return;
        it = assigned_.emplace(d.id, Assignment{next_site_++, 0}).first;
      }
      const Vec2 site = sites_[it->second.site];
      if (claimed.contains(site) || it->second.attempts >= kMaxAttempts) {
        assigned_.erase(it);
        continue;
      }
      if (manhattan(pos, site) <= cfg::CLAIM_DEPLOY_RANGE) {
        if (taken.contains(site)) {
          assigned_.erase(it);
          continue;
        }
        if (alloy >= cfg::CLAIM_COST_A) {
          issue(move_cmd(d.id, CommandKind::DeployClaim, site));
          alloy -= cfg::CLAIM_COST_A;
          ++it->second.attempts;
        }
        return;
      }
      const Vec2 step = router_.next_step(
          pos, [&](Vec2 p) { return manhattan(p, site) <= cfg::CLAIM_DEPLOY_RANGE; });
      if (step == pos) { // unreachable from here
        assigned_.erase(it);
        continue;
      }
      issue(move_cmd(d.id, CommandKind::Move, step));
      return;
    }
  }

  void init_sites(const Snapshot& snap) {
    have_home_ = true;
    home_ = core_pos(snap);
    // Anchored on home so both sides of a point-symmetric map get mirror-image sites.
    for (int32_t y = 0; y < map_.height; ++y)
      for (int32_t x = 0; x < map_.width; ++x)
        if ((x - home_.x) % kSpacing == 0 && (y - home_.y) % kSpacing == 0 &&
            map_.passable({x, y}) && Vec2{x, y} != home_)
          sites_.push_back({x, y});
    std::stable_sort(sites_.begin(), sites_.end(), [&](Vec2 a, Vec2 b) {
      return manhattan(a, home_) < manhattan(b, home_);
    });
  }
};

} // namespace game
