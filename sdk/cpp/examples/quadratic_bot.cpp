// Deliberately O(n^2) per tick: a profiler demonstration, not a strategy.
// Every unit checks every other friendly unit (crowding) and every visible enemy
// (target choice), so fuel grows with the square of the army: cheap with ten units,
// several percent of the budget at fifty, and a large share of it at the unit cap.
// That curve is what the army_scaling scenarios are there to show.
#include <ud/types.hpp>
#include <cstdint>

extern "C" {
  alignas(4) unsigned char SNAPSHOT_ADDR[ud::SNAPSHOT_BUFFER_SIZE];
  alignas(4) game::Command COMMAND_ADDR[game::cfg::UNIT_HARD_CAP];
}

static int32_t mdist(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
  int32_t dx = ax - bx; if (dx < 0) dx = -dx;
  int32_t dy = ay - by; if (dy < 0) dy = -dy;
  return dx + dy;
}

extern "C" void init(int32_t) {}

extern "C" int32_t on_tick() {
  const auto* h       = reinterpret_cast<const game::SnapshotHeader*>(SNAPSHOT_ADDR);
  const auto* units   = reinterpret_cast<const game::UnitView*>(SNAPSHOT_ADDR + h->my_units_off);
  const auto* enemies = reinterpret_cast<const game::EnemyView*>(SNAPSHOT_ADDR + h->enemies_off);
  const uint32_t n = h->my_unit_count, m = h->visible_enemy_count;

  uint32_t out = 0;
  for (uint32_t i = 0; i < n && out < (uint32_t)game::cfg::UNIT_HARD_CAP; ++i) {
    const auto& u = units[i];
    if (u.type == (uint16_t)game::UnitType::Drone) continue;

    // O(n): how crowded is this unit? Weighted so nearer friends count more.
    int32_t crowd = 0;
    for (uint32_t j = 0; j < n; ++j) {
      if (j == i) continue;
      int32_t d = mdist(u.x, u.y, units[j].x, units[j].y);
      crowd += (d < 8) ? (8 - d) * (8 - d) : 0;
    }

    // O(m): nearest visible enemy.
    int32_t best = 0x7fffffff;
    uint32_t target = 0;
    int32_t tx = u.x, ty = u.y;
    for (uint32_t k = 0; k < m; ++k) {
      int32_t d = mdist(u.x, u.y, enemies[k].x, enemies[k].y);
      if (d < best) { best = d; target = enemies[k].id; tx = enemies[k].x; ty = enemies[k].y; }
    }

    game::Command cmd{};
    cmd.entity_id = u.id;
    if (target != 0) {
      cmd.kind      = (uint16_t)game::CommandKind::MoveAttack;
      cmd.ax        = tx;
      cmd.ay        = ty;
      cmd.target_id = target;
    } else if (crowd > 40) {
      // Too crowded and nothing to shoot: spread out by stepping away from the pack.
      cmd.kind = (uint16_t)game::CommandKind::Move;
      cmd.ax   = u.x + ((u.id & 1) ? 1 : -1);
      cmd.ay   = u.y + ((u.id & 2) ? 1 : -1);
    } else {
      continue;
    }
    COMMAND_ADDR[out++] = cmd;
  }
  return (int32_t)out;
}
