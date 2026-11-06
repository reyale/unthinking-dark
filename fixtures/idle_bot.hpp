#pragma once
#include "bot_iface.hpp"

namespace game {

// Always returns empty commands. Used for determinism and baseline tests.
class IdleBot : public Bot {
public:
  void on_init(const Map&, uint32_t, uint64_t) override {}
  std::vector<Command> on_tick(const Snapshot&) override { return {}; }
};

} // namespace game
