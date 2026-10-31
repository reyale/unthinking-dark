#pragma once
#include "bot_iface.hpp"
#include "world.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Deterministic fuel/memory profiler (DESIGN §10). Every scenario is a real match
// played through game::run_match with a runner::WasmBot, i.e. the exact code and
// pinned Wasmtime config used for ranked matches, so the fuel it reports is the
// match meter's own number rather than an estimate.
namespace profiler {

struct Scenario {
  std::string name;
  std::string what;  // one-line description for the report
  uint32_t    ticks; // the match runs exactly this many ticks
  std::function<game::World()> build;
};

// The standard suite, in report order.
const std::vector<Scenario>& scenarios();
const Scenario* find_scenario(std::string_view name);

struct TickSample {
  uint64_t        fuel;
  uint64_t        memory;
  game::TickFault fault;
};

struct Report {
  std::string name, what;
  // Snapshot size the bot sees on the first tick.
  uint32_t my_units{0}, visible_enemies{0}, visible_tiles{0};

  bool        loaded{false};
  std::string error;            // load or init failure message
  game::BotMetrics init{};
  std::vector<TickSample> ticks;

  uint64_t worst_fuel{0};
  uint32_t worst_tick{0};
  uint64_t total_fuel{0};
  uint64_t peak_memory{0};
  uint32_t fuel_forfeits{0};
  uint32_t memory_forfeits{0};
  uint32_t trap_forfeits{0};

  uint64_t mean_fuel() const { return ticks.empty() ? 0 : total_fuel / ticks.size(); }
  uint32_t forfeits() const { return fuel_forfeits + memory_forfeits + trap_forfeits; }
};

Report run_scenario(const std::vector<uint8_t>& wasm, const Scenario& s);

// The passive opponent every scenario uses. Exposed so tests can replay a
// scenario as an ordinary match and compare meters.
class PassiveBot : public game::Bot {
public:
  void on_init(const game::Map&, uint32_t) override {}
  std::vector<game::Command> on_tick(const game::Snapshot&) override { return {}; }
};

} // namespace profiler
