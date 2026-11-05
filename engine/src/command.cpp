#include "command.hpp"
#include "world.hpp"
#include "entity.hpp"
#include "grid.hpp"

namespace game {

bool can_produce(StructureType producer, UnitType what) {
  switch (producer) {
  case StructureType::CommandCore:
    return what == UnitType::Drone;
  case StructureType::Factory:
    return what == UnitType::Interceptor || what == UnitType::Frigate ||
           what == UnitType::Artillery || what == UnitType::Battleship;
  default:
    return false;
  }
}

ValidatedCommands validate_commands(const World& world, uint32_t faction_id,
                                    const std::vector<Command>& raw) {
  FactionId my_faction{faction_id};
  ValidatedCommands out;

  for (const auto& cmd : raw) {
    const EntityId eid{cmd.entity_id};

    // One command per issuer per tick: the first listed wins.
    if (out.contains(eid))
      continue;

    const auto kind = static_cast<CommandKind>(cmd.kind);

    // Structures issue one command, Produce: the Core makes Drones, a Factory makes ships.
    if (kind == CommandKind::Produce) {
      const Structure* s = world.find_structure(eid.structure());
      if (!s || !s->alive() || s->faction != my_faction)
        continue;
      if (!can_produce(s->type, static_cast<UnitType>(cmd.aux)))
        continue;
      out.emplace(eid, cmd);
      continue;
    }

    // Everything else is a unit's own order.
    const Unit* unit = world.find_unit(eid.unit());
    if (!unit || !unit->alive() || unit->faction != my_faction)
      continue;

    // Per-kind validation
    switch (kind) {
    case CommandKind::MoveAttack:
      continue; // reserved: a Move fires already, and nothing halts it
    case CommandKind::Move: {
      Vec2 target{cmd.ax, cmd.ay};
      if (!world.map.in_bounds(target))
        continue;
      break;
    }
    case CommandKind::Attack: {
      if (cmd.target_id != 0) {
        // An enemy unit or an enemy structure; ids are unique across both.
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
    case CommandKind::Gather:
      continue; // reserved: harvesting is automatic for a Drone beside a node
    case CommandKind::Build:
    case CommandKind::DeployClaim: {
      if (unit->type != UnitType::Drone)
        continue; // only Drones build (DESIGN §6)
      Vec2 target{cmd.ax, cmd.ay};
      if (!world.map.passable(target))
        continue;
      break;
    }
    case CommandKind::Produce:
      continue; // a unit can't produce
    }

    out.emplace(eid, cmd);
  }

  return out;
}

} // namespace game
