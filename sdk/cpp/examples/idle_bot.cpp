#include <ud/types.hpp>

extern "C" {
  alignas(4) unsigned char SNAPSHOT_ADDR[ud::SNAPSHOT_BUFFER_SIZE];
  alignas(4) game::Command COMMAND_ADDR[game::cfg::UNIT_HARD_CAP];
}

extern "C" void    init(int32_t, int64_t) {}
extern "C" int32_t on_tick()     { return 0; }
