#pragma once
#include "bot_iface.hpp"
#include "abi.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include <climits>
#include <vector>

namespace game {

// Moves drones toward the nearest visible resource node and harvests passively.
// Non-drone units hold position — the combat engine auto-engages enemies that
// wander into range, so they still fight defensively.
//
// Does not issue Produce or Build commands.
class EconBot : public Bot {
public:
  void on_init(const Map&, uint32_t) override {}

  std::vector<Command> on_tick(const Snapshot& snap) override {
    std::vector<Command> out;

    for (const auto& u : snap.my_units) {
      if (static_cast<UnitType>(u.type) != UnitType::Drone)
        continue;
      Vec2 res = nearest_resource(snap, {u.x, u.y});
      if (res.x < 0 || manhattan({u.x, u.y}, res) <= 1)
        continue; // no visible resource, or already in harvest range
      Command cmd{};
      cmd.entity_id = u.id;
      cmd.kind    = static_cast<uint16_t>(CommandKind::Move);
      cmd.ax      = res.x;
      cmd.ay      = res.y;
      out.push_back(cmd);
    }

    return out;
  }

  bool healthy() const override { return true; }

private:
  static Vec2 nearest_resource(const Snapshot& snap, Vec2 from) {
    Vec2 best{-1, -1};
    int32_t best_dist = INT32_MAX;
    for (const auto& tv : snap.visible_tiles) {
      if (static_cast<Terrain>(tv.terrain) != Terrain::ResourceNode)
        continue;
      int32_t d = manhattan(from, {tv.x, tv.y});
      if (d < best_dist) {
        best_dist = d;
        best = {tv.x, tv.y};
      }
    }
    return best;
  }
};

} // namespace game
