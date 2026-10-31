// Rush bot: BFS pathfinding around obstacles.
// Combat units path toward the enemy starting corner.
// Drones path toward the nearest visible resource node.
#include <ud/types.hpp>
#include <cstdint>

extern "C" {
  alignas(4) unsigned char SNAPSHOT_ADDR[ud::SNAPSHOT_BUFFER_SIZE];
  alignas(4) game::Command COMMAND_ADDR[game::cfg::UNIT_HARD_CAP];
}

static constexpr int32_t MW = game::cfg::MAP_MAX_W;
static constexpr int32_t MH = game::cfg::MAP_MAX_H;

static uint32_t g_faction = 0;
static int32_t  g_enemy_x = 0;
static int32_t  g_enemy_y = 0;
static int32_t  g_map_w   = 0;
static int32_t  g_map_h   = 0;

// Persistent terrain knowledge. 0=open/unknown, 1=asteroid.
static uint8_t  g_terrain[MH][MW];

// BFS scratch — reused each run via a generation counter (no memset needed).
static uint32_t g_gen             = 0;
static uint32_t bfs_gen[MH][MW];   // bfs_gen[y][x] == g_gen → visited this run
static uint16_t bfs_dist[MH][MW];  // BFS distance from current target
static struct { int16_t x, y; } bfs_q[MH * MW];

static inline bool passable(int32_t x, int32_t y) {
  return x >= 0 && y >= 0 && x < g_map_w && y < g_map_h && g_terrain[y][x] == 0;
}

// Reverse BFS from (tx, ty) — fills bfs_dist and bfs_gen for all reachable cells.
static void bfs_from(int32_t tx, int32_t ty) {
  ++g_gen;
  // Clamp target into bounds; if it's an asteroid aim at the nearest open cell.
  // Simple fix: just mark the target as visited at dist=0 even if blocked.
  if (tx < 0) tx = 0;
  if (ty < 0) ty = 0;
  if (tx >= g_map_w) tx = g_map_w - 1;
  if (ty >= g_map_h) ty = g_map_h - 1;

  int head = 0, tail = 0;
  bfs_gen[ty][tx]  = g_gen;
  bfs_dist[ty][tx] = 0;
  bfs_q[tail++] = {(int16_t)tx, (int16_t)ty};

  static const int32_t DX[4] = {1, -1, 0,  0};
  static const int32_t DY[4] = {0,  0, 1, -1};

  while (head < tail) {
    auto [cx, cy] = bfs_q[head++];
    uint16_t nd = bfs_dist[cy][cx] + 1;
    for (int d = 0; d < 4; ++d) {
      int32_t nx = cx + DX[d], ny = cy + DY[d];
      if (!passable(nx, ny))            continue;
      if (bfs_gen[ny][nx] == g_gen)     continue;
      bfs_gen[ny][nx]  = g_gen;
      bfs_dist[ny][nx] = nd;
      bfs_q[tail++] = {(int16_t)nx, (int16_t)ny};
    }
  }
}

// Next step from (sx, sy) toward the target that was used in the last bfs_from() call.
// Returns false if already at target or unreachable.
static bool next_step(int32_t sx, int32_t sy, int32_t& ox, int32_t& oy) {
  if (bfs_gen[sy][sx] != g_gen) return false;  // source not reachable
  uint16_t cur = bfs_dist[sy][sx];
  if (cur == 0) return false;  // already at target

  static const int32_t DX[4] = {1, -1, 0,  0};
  static const int32_t DY[4] = {0,  0, 1, -1};

  uint16_t best = cur;
  ox = sx; oy = sy;
  for (int d = 0; d < 4; ++d) {
    int32_t nx = sx + DX[d], ny = sy + DY[d];
    if (!passable(nx, ny))            continue;
    if (bfs_gen[ny][nx] != g_gen)     continue;
    if (bfs_dist[ny][nx] < best) { best = bfs_dist[ny][nx]; ox = nx; oy = ny; }
  }
  return (ox != sx || oy != sy);
}

static int32_t mdist(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
  int32_t dx = ax - bx; if (dx < 0) dx = -dx;
  int32_t dy = ay - by; if (dy < 0) dy = -dy;
  return dx + dy;
}

extern "C" void init(int32_t faction_id) {
  g_faction = static_cast<uint32_t>(faction_id);
  g_enemy_x = (faction_id == 0) ? game::cfg::MAP_MAX_W - 3 : 2;
  g_enemy_y = (faction_id == 0) ? game::cfg::MAP_MAX_H - 3 : 2;
}

extern "C" int32_t on_tick() {
  const auto* h     = reinterpret_cast<const game::SnapshotHeader*>(SNAPSHOT_ADDR);
  const auto* units = reinterpret_cast<const game::UnitView*>(SNAPSHOT_ADDR + h->my_units_off);
  const auto* tvs   = reinterpret_cast<const game::TileView*>(SNAPSHOT_ADDR + h->tiles_off);

  if (h->map_w > 0 && h->map_h > 0) {
    g_map_w   = (int32_t)h->map_w;
    g_map_h   = (int32_t)h->map_h;
    g_enemy_x = (g_faction == 0) ? g_map_w - 3 : 2;
    g_enemy_y = (g_faction == 0) ? g_map_h - 3 : 2;
  }

  // Absorb newly-visible terrain into our persistent map.
  for (uint32_t t = 0; t < h->visible_tile_count; ++t) {
    int32_t x = tvs[t].x, y = tvs[t].y;
    if (x >= 0 && y >= 0 && x < MW && y < MH)
      g_terrain[y][x] = (tvs[t].terrain == (uint16_t)game::Terrain::Asteroid) ? 1 : 0;
  }

  // One BFS from the enemy corner serves all combat units this tick.
  uint32_t combat_gen = 0;
  if (g_map_w > 0) {
    bfs_from(g_enemy_x, g_enemy_y);
    combat_gen = g_gen;
  }

  uint32_t n = 0;
  for (uint32_t i = 0; i < h->my_unit_count && n < (uint32_t)game::cfg::UNIT_HARD_CAP; ++i) {
    const game::UnitView& u = units[i];
    game::Command cmd{};
    cmd.entity_id = u.id;

    if (u.type == (uint16_t)game::UnitType::Drone) {
      // Find nearest visible resource node.
      int32_t best = 0x7fffffff, bx = -1, by = -1;
      for (uint32_t t = 0; t < h->visible_tile_count; ++t) {
        if (tvs[t].terrain != (uint16_t)game::Terrain::ResourceNode) continue;
        int32_t d = mdist(u.x, u.y, tvs[t].x, tvs[t].y);
        if (d < best) { best = d; bx = tvs[t].x; by = tvs[t].y; }
      }
      if (bx < 0 || best <= 1) continue;
      bfs_from(bx, by);
      int32_t nx, ny;
      if (!next_step(u.x, u.y, nx, ny)) continue;
      cmd.kind = (uint16_t)game::CommandKind::Move;
      cmd.ax = nx; cmd.ay = ny;
    } else {
      // Restore the shared combat BFS result, then find next step.
      // (bfs_from may have been overwritten by a drone BFS above)
      if (g_gen != combat_gen) {
        bfs_from(g_enemy_x, g_enemy_y);
        combat_gen = g_gen;
      }
      int32_t nx = u.x, ny = u.y;
      if (!next_step(u.x, u.y, nx, ny)) {
        if (bfs_gen[u.y][u.x] != g_gen) continue;  // unreachable
        // dist=0: stay in place and keep attacking
      }
      cmd.kind = (uint16_t)game::CommandKind::MoveAttack;
      cmd.ax = nx; cmd.ay = ny;
    }

    COMMAND_ADDR[n++] = cmd;
  }
  return (int32_t)n;
}
