#include "territory.hpp"
#include "world.hpp"
#include "config.hpp"
#include "entity.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace game {

// Voronoi: each tile goes to the nearest living Claim Node by squared Euclidean distance.
// Ties between different factions → neutral. Tiles beyond CLAIM_INFLUENCE radius → neutral.
// Pure integer math (no sqrt needed for comparison).
TerritoryState run_territory(World& world, uint32_t tick_cap) {
  // Step 1 — Claim Node fragility: cap hp to eff_hp(g) = BASE*(1-g) + FLOOR
  uint32_t tick = world.tick;
  for (auto& [sid, s] : world.structures) {
    if (s.type != StructureType::ClaimNode || !s.alive())
      continue;
    int32_t eff_hp;
    if (tick_cap == 0 || tick >= tick_cap) {
      eff_hp = cfg::CLAIM_HP_FLOOR;
    } else {
      int64_t remaining = static_cast<int64_t>(tick_cap - tick);
      eff_hp = static_cast<int32_t>(
        static_cast<int64_t>(cfg::CLAIM_HP_BASE) * remaining / tick_cap
      ) + cfg::CLAIM_HP_FLOOR;
    }
    if (s.hp > eff_hp)
      s.hp = eff_hp;
  }

  // Step 2 — Voronoi partition
  struct NodeEntry { Vec2 pos; uint32_t faction; };
  std::vector<NodeEntry> nodes;
  for (const auto& [sid, s] : world.structures) {
    if (s.type == StructureType::ClaimNode && s.alive())
      nodes.push_back({s.pos, s.faction.value});
  }

  TerritoryState ts;
  if (nodes.empty())
    return ts;

  const int64_t influence_sq =
    static_cast<int64_t>(cfg::CLAIM_INFLUENCE) * cfg::CLAIM_INFLUENCE;

  // Each claim stamps only the tiles within its influence radius (tiles beyond every claim's
  // radius are neutral anyway), so the cost is claims × influence area rather than
  // claims × map. A tile's owner is the faction of its nearest claim, or neutral if the
  // nearest claims belong to different factions; that doesn't depend on stamping order.
  const int32_t W = world.map.width;
  const int32_t H = world.map.height;
  std::vector<int64_t> best_sq(static_cast<size_t>(W) * static_cast<size_t>(H), INT64_MAX);
  std::vector<int32_t> owner(best_sq.size(), -1); // -1 = neutral / contested
  const int32_t r = cfg::CLAIM_INFLUENCE;
  for (const auto& node : nodes) {
    for (int32_t y = std::max(0, node.pos.y - r); y <= std::min(H - 1, node.pos.y + r); ++y) {
      for (int32_t x = std::max(0, node.pos.x - r); x <= std::min(W - 1, node.pos.x + r); ++x) {
        const int64_t dx = x - node.pos.x;
        const int64_t dy = y - node.pos.y;
        const int64_t dist_sq = dx * dx + dy * dy;
        if (dist_sq > influence_sq)
          continue;
        const size_t i = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
        const auto faction = static_cast<int32_t>(node.faction);
        if (dist_sq < best_sq[i]) {
          best_sq[i] = dist_sq;
          owner[i] = faction;
        } else if (dist_sq == best_sq[i] && faction != owner[i]) {
          owner[i] = -1; // equidistant from different factions
        }
      }
    }
  }

  // Territory is a share of passable tiles (§7.2): asteroids can't be held, so counting them
  // would cap every faction below the opening threshold on asteroid-heavy maps.
  uint32_t tile_count[2]{0, 0};
  uint32_t total_tiles = 0;
  for (int32_t y = 0; y < H; ++y) {
    for (int32_t x = 0; x < W; ++x) {
      if (!world.map.passable({x, y}))
        continue;
      ++total_tiles;
      const int32_t o =
          owner[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)];
      if (o >= 0)
        tile_count[static_cast<uint32_t>(o)]++;
    }
  }

  if (total_tiles > 0) {
    ts.pct_faction[0] = (tile_count[0] * 100) / total_tiles;
    ts.pct_faction[1] = (tile_count[1] * 100) / total_tiles;
  }
  return ts;
}

int32_t win_threshold(uint32_t tick, uint32_t tick_cap) {
  if (tick_cap == 0)
    return cfg::THRESH_A;
  int64_t g2 =
    (static_cast<int64_t>(tick) * tick * 10000) / (static_cast<int64_t>(tick_cap) * tick_cap);
  int32_t drop = static_cast<int32_t>(static_cast<int64_t>(cfg::THRESH_B) * g2 / 10000);
  int32_t threshold = cfg::THRESH_A - drop;
  return threshold < cfg::THRESH_MIN ? cfg::THRESH_MIN : threshold;
}

} // namespace game
