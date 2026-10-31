#pragma once
#include <cstdint>
#include <compare>
#include <limits>

// Strong-typed ids assigned in deterministic creation order.
// All tie-breaks must resolve to id (ascending), never pointer or hash-map order.
namespace game {

template <typename Tag>
struct StrongId {
  uint32_t value{std::numeric_limits<uint32_t>::max()};

  constexpr bool is_null() const { return value == std::numeric_limits<uint32_t>::max(); }
  auto operator<=>(const StrongId&) const = default;
};

struct UnitTag {};
struct StructureTag {};
struct FactionTag {};

using UnitId = StrongId<UnitTag>;
using StructureId = StrongId<StructureTag>;
using FactionId = StrongId<FactionTag>;

// Units and structures draw from one id sequence (World::next_entity_id), so an id names
// exactly one entity. A command's issuer is an EntityId: a unit, or a structure producing.
// Any UnitId or StructureId converts to one.
struct EntityId {
  uint32_t value{std::numeric_limits<uint32_t>::max()};

  constexpr EntityId() = default;
  constexpr explicit EntityId(uint32_t v) : value(v) {}
  constexpr EntityId(UnitId u) : value(u.value) {}
  constexpr EntityId(StructureId s) : value(s.value) {}

  constexpr UnitId unit() const { return {value}; }
  constexpr StructureId structure() const { return {value}; }
  auto operator<=>(const EntityId&) const = default;
};

} // namespace game
