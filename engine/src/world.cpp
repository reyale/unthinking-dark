#include "world.hpp"
#include "config.hpp"
#include <stdexcept>

namespace game {

Unit& World::spawn_unit(FactionId faction, UnitType type, Vec2 pos) {
  UnitId id{next_entity_id++};
  Unit u;
  u.id = id;
  u.faction = faction;
  u.type = type;
  u.pos = pos;
  u.hp = stats_for(type).hp;
  auto [it, ok] = units.emplace(id, u);
  (void)ok;
  return it->second;
}

Structure& World::spawn_structure(FactionId faction, StructureType type, Vec2 pos) {
  StructureId id{next_entity_id++};
  Structure s;
  s.id = id;
  s.faction = faction;
  s.type = type;
  s.pos = pos;
  switch (type) {
  case StructureType::CommandCore:
    s.hp = cfg::CMD_CORE_HP;
    break;
  case StructureType::Factory:
    s.hp = cfg::FACTORY_HP;
    break;
  case StructureType::ClaimNode:
    s.hp = cfg::CLAIM_HP_BASE;
    break;
  }
  auto [it, ok] = structures.emplace(id, s);
  (void)ok;
  structure_by_pos.emplace(pos, id);
  return it->second;
}

Unit* World::find_unit(UnitId id) {
  auto it = units.find(id);
  return it != units.end() ? &it->second : nullptr;
}

const Unit* World::find_unit(UnitId id) const {
  auto it = units.find(id);
  return it != units.end() ? &it->second : nullptr;
}

Structure* World::find_structure(StructureId id) {
  auto it = structures.find(id);
  return it != structures.end() ? &it->second : nullptr;
}

const Structure* World::find_structure(StructureId id) const {
  auto it = structures.find(id);
  return it != structures.end() ? &it->second : nullptr;
}

Structure* World::command_core(FactionId f) {
  for (auto& [id, s] : structures)
    if (s.faction == f && s.type == StructureType::CommandCore && s.alive())
      return &s;
  return nullptr;
}

const Structure* World::command_core(FactionId f) const {
  for (const auto& [id, s] : structures)
    if (s.faction == f && s.type == StructureType::CommandCore && s.alive())
      return &s;
  return nullptr;
}

bool World::tile_free(Vec2 pos) const {
  if (!map.passable(pos) || structure_by_pos.count(pos))
    return false;
  for (const auto& [uid, u] : units)
    if (u.alive() && u.pos == pos)
      return false;
  return true;
}

void World::purge_dead() {
  for (auto it = units.begin(); it != units.end();) {
    it = it->second.alive() ? std::next(it) : units.erase(it);
  }
  for (auto it = structures.begin(); it != structures.end();) {
    if (it->second.alive()) {
      it = std::next(it);
    } else {
      structure_by_pos.erase(it->second.pos);
      it = structures.erase(it);
    }
  }
}

} // namespace game
