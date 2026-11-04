#pragma once
#include "bot_iface.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace runner {

// Compile WAT text to WASM binary using Wasmtime's built-in wat2wasm.
// Throws std::runtime_error on parse failure. Useful in tests.
std::vector<uint8_t> wat_to_wasm(std::string_view wat);

// Load a .wasm file from disk into a byte vector.
std::vector<uint8_t> load_wasm_file(const std::string& path);

// WASM bot runner — implements game::Bot by executing a compiled WASM module
// under Wasmtime with fuel metering and determinism flags.
//
// Lifecycle:
//   1. Construct with WASM bytes (check healthy() — false means load failure).
//   2. Call on_init() once (init-budget overrun → healthy() becomes false).
//   3. Call on_tick() each tick. A fuel, memory or trap fault returns no commands and leaves
//      the bot unhealthy: it loses the match (its state may be half-updated, so it is never
//      called again).
class WasmBot : public game::Bot {
public:
  explicit WasmBot(const std::vector<uint8_t>& wasm);
  ~WasmBot() override;

  WasmBot(const WasmBot&) = delete;
  WasmBot& operator=(const WasmBot&) = delete;

  void on_init(const game::Map& map, uint32_t faction_id) override;
  std::vector<game::Command> on_tick(const game::Snapshot& snap) override;
  bool healthy() const override { return healthy_; }
  game::BotMetrics last_metrics() const override { return last_metrics_; }

  // Human-readable message from the last failure (empty if none).
  const std::string& last_error() const { return last_error_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool healthy_{false};
  std::string last_error_;
  game::BotMetrics last_metrics_{};
};

} // namespace runner
