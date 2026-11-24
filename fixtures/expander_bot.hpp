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

// Expands over a lattice of claim sites, working outward from home, and never attacks: it can
// only win on territory, which makes it the opponent that proves a turtle loses to the threshold.
//
// The lattice is as tight as the structure cap allows: CLAIM_MIN_SPACING apart, widened until a
// lattice over the whole map fits under STRUCTURE_HARD_CAP, and never wider than CLAIM_INFLUENCE,
// the widest square lattice that leaves no tile out of reach of a claim. A tight lattice on a
// large map would run out of structures before half of it is held.
//
// Claims must be deployed by a Drone standing within CLAIM_DEPLOY_RANGE, so the bot needs
// Drones to spare: its Core produces one per kTilesPerDrone of map (at least kMinDrones, and
// within the Drone cap, one per claim plus one), one in kDronesPerHarvester of those it has
// ferries resources home and the rest walk to claim sites. A Drone stays by each claim it
// deploys until construction finishes. Combat units move kClearRadius out from
// home and hold there, so they can't box Drones in beside the base.
class ExpanderBot : public Bot {
public:
  static constexpr size_t kMinDrones = 4;
  static constexpr size_t kTilesPerDrone = 1600;
  static constexpr size_t kDronesPerHarvester = 4;
  static constexpr int32_t kClearRadius = 8;

  void on_init(const Map& map, uint32_t, uint64_t) override {
    map_ = map;
    router_.reset(map);
    const size_t area = static_cast<size_t>(map.width) * static_cast<size_t>(map.height);
    max_drones_ = std::max(kMinDrones, area / kTilesPerDrone);
    spacing_ = cfg::CLAIM_MIN_SPACING;
    while (spacing_ < cfg::CLAIM_INFLUENCE &&
           area > static_cast<size_t>(cfg::STRUCTURE_HARD_CAP) * spacing_ * spacing_)
      ++spacing_;
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
    if (core && core->producing == PRODUCING_NONE && drones.size() < max_drones_ &&
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

    const size_t harvesters = std::max<size_t>(1, drones.size() / kDronesPerHarvester);
    const auto builds = builders(snap);
    for (size_t i = 0; i < drones.size(); ++i) {
      const UnitView& d = *drones[i];
      if (busy.contains(d.id)) continue;
      const Vec2 pos{d.x, d.y};
      if (auto b = builds.find(d.id); b != builds.end()) {
        const Vec2 step = builder_step(router_, d, b->second);
        if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
        continue;
      }
      // A lone Drone (the cap is one until the first claim stands) harvests until it can
      // afford a claim, then goes to stake it.
      const bool lone_claimer = drones.size() == 1 && alloy >= cfg::CLAIM_COST_A;
      if (i < harvesters && !lone_claimer) {
        lattice_.release(d.id);
        const Vec2 step = harvest_step(router_, map_, snap, d);
        if (step != pos) issue(move_cmd(d.id, CommandKind::Move, step));
        continue;
      }
      lattice_.step(router_, d, claimed, taken, alloy, issue);
    }
    return out;
  }

  bool healthy() const override { return true; }

private:
  Map map_;
  Router router_;
  ClaimLattice lattice_;
  size_t max_drones_{kMinDrones};
  int32_t spacing_{cfg::CLAIM_MIN_SPACING};
  bool have_home_{false};
  Vec2 home_{};

  void init_sites(const Snapshot& snap) {
    have_home_ = true;
    home_ = core_pos(snap);
    lattice_.init(map_, home_, spacing_);
  }
};

} // namespace game
