#pragma once
#include "bot_iface.hpp"
#include "abi.hpp"
#include "entity.hpp"
#include "grid.hpp"
#include <climits>
#include <vector>

namespace game {

// Sends all combat units toward the enemy starting corner with MoveAttack.
// Moves drones toward the nearest visible resource node; idles when within
// harvest range (Manhattan distance ≤ 1) so auto-harvest kicks in.
//
// Does not issue Produce or Build commands.
class RushBot : public Bot {
public:
  void on_init(const Map& map, uint32_t faction_id) override {
    faction_id_ = faction_id;
    // Assume symmetric map: enemy starts at the opposite corner.
    enemy_corner_ = (faction_id == 0) ? Vec2{map.width - 3, map.height - 3}
                                      : Vec2{2, 2};
  }

  std::vector<Command> on_tick(const Snapshot& snap) override {
    std::vector<Command> out;
    out.reserve(snap.my_units.size());

    for (const auto& u : snap.my_units) {
      Command cmd{};
      cmd.entity_id = u.id;

      if (static_cast<UnitType>(u.type) == UnitType::Drone) {
        Vec2 res = nearest_resource(snap, {u.x, u.y});
        if (res.x < 0 || manhattan({u.x, u.y}, res) <= 1)
          continue; // no visible resource, or already in harvest range
        cmd.kind = static_cast<uint16_t>(CommandKind::Move);
        cmd.ax   = res.x;
        cmd.ay   = res.y;
      } else {
        cmd.kind = static_cast<uint16_t>(CommandKind::MoveAttack);
        cmd.ax   = enemy_corner_.x;
        cmd.ay   = enemy_corner_.y;
      }

      out.push_back(cmd);
    }

    return out;
  }

  bool healthy() const override { return true; }

private:
  uint32_t faction_id_{0};
  Vec2 enemy_corner_{};

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
