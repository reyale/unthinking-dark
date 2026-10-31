#include "command.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "grid.hpp"

namespace game {

ValidatedCommands validate_commands(const World& world, uint32_t faction_id,
                                    const std::vector<Command>& raw) {
  FactionId my_faction{faction_id};
  ValidatedCommands out;

  for (const auto& cmd : raw) {
    UnitId uid{cmd.unit_id};

    // Reject if already have a command for this unit (first listed wins)
    if (out.contains(uid))
      continue;

    // Reject if unit doesn't exist, is dead, or belongs to enemy
    const Unit* unit = world.find_unit(uid);
    if (!unit || !unit->alive() || unit->faction != my_faction)
      continue;

    const auto kind = static_cast<CommandKind>(cmd.kind);

    // Per-kind validation
    switch (kind) {
    case CommandKind::Move:
    case CommandKind::MoveAttack: {
      Vec2 target{cmd.ax, cmd.ay};
      if (!world.map.in_bounds(target))
        continue;
      break;
    }
    case CommandKind::Attack: {
      if (cmd.target_id != 0) {
        // Accept valid enemy unit OR valid enemy structure; unit takes priority
        // when IDs coincide (both counters start at 1).
        UnitId uid{cmd.target_id};
        const Unit* u = world.find_unit(uid);
        if (u && u->alive() && u->faction != my_faction)
          break;
        StructureId sid{cmd.target_id};
        const Structure* s = world.find_structure(sid);
        if (!s || !s->alive() || s->faction == my_faction)
          continue; // explicit target provided but invalid
      }
      break;
    }
    case CommandKind::Gather: {
      Vec2 node{cmd.ax, cmd.ay};
      if (!world.map.in_bounds(node))
        continue;
      if (world.map.tile_at(node).terrain != Terrain::ResourceNode)
        continue;
      break;
    }
    case CommandKind::Build:
    case CommandKind::DeployClaim: {
      if (unit->type != UnitType::Drone)
        continue; // only Drones build (DESIGN §6)
      Vec2 target{cmd.ax, cmd.ay};
      if (!world.map.passable(target))
        continue;
      break;
    }
    case CommandKind::Produce: {
      // Issuer must be a Factory owned by this faction
      StructureId sid{cmd.target_id};
      const Structure* fac = world.find_structure(sid);
      if (!fac || !fac->alive() || fac->faction != my_faction)
        continue;
      if (fac->type != StructureType::Factory)
        continue;
      break;
    }
    }

    out.emplace(uid, cmd);
  }

  return out;
}

} // namespace game
