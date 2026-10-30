#pragma once
#include "snapshot.hpp"
#include "command.hpp"
#include <cstdint>
#include <vector>

namespace game {

struct Map;

// Why the most recent call was forfeited, if it was. A forfeited tick's commands are
// discarded: that side's units do nothing that tick. Stored per tick in replays, so
// values are append-only.
enum class TickFault : uint8_t {
  None,      // call completed
  OutOfFuel,   // fuel budget exhausted mid-call
  OutOfMemory, // tried to grow linear memory past cfg::MEMORY_CAP_MB (the grow is refused)
  Trap,        // any other trap or runtime error (unreachable, OOB access, ...)
};

inline const char* tick_fault_name(TickFault f) {
  switch (f) {
  case TickFault::None:        return "none";
  case TickFault::OutOfFuel:   return "fuel";
  case TickFault::OutOfMemory: return "memory";
  case TickFault::Trap:        return "trap";
  }
  return "?";
}

// Resource usage from the most recent on_init or on_tick call.
// WASM bots populate every field; scripted bots leave them zero / None.
struct BotMetrics {
  uint64_t  fuel_consumed{0};  // Wasmtime fuel units burned
  uint64_t  memory_bytes{0};   // linear memory size in bytes after the call
  TickFault fault{TickFault::None};
};

// Abstract interface implemented by both in-process scripted bots (tests)
// and the Wasmtime runner (Phase 4). The engine only talks to this interface.
class Bot {
public:
  virtual ~Bot() = default;

  // Called once before tick 0. Fat budget; bot allocates/precomputes here.
  virtual void on_init(const Map& map, uint32_t faction_id) = 0;

  // Called every tick. Returns raw commands; engine validates them.
  // Implementations MUST NOT retain a reference to `snap` after returning.
  virtual std::vector<Command> on_tick(const Snapshot& snap) = 0;

  // Whether the bot is still functional (false after init failure or
  // repeated OOM/trap — WASM runner sets this; scripted bots always true).
  virtual bool healthy() const { return true; }

  // Resource usage from the most recent call. Default impl returns zeros.
  virtual BotMetrics last_metrics() const { return {}; }
};

} // namespace game
