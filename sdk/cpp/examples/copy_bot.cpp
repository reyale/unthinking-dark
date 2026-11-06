// Copies, clears and shifts large buffers every tick: a check on the SDK runtime, not a strategy.
// Bots are built without bulk-memory instructions, so these compile to calls into runtime.cpp's
// memcpy/memset/memmove, which run as ordinary loops and are charged fuel per iteration. The bot
// traps if any result is wrong, which the runner reports as a forfeited tick.
#include <ud/types.hpp>
#include <cstdint>

extern "C" {
  alignas(4) unsigned char SNAPSHOT_ADDR[ud::SNAPSHOT_BUFFER_SIZE];
  alignas(4) game::Command COMMAND_ADDR[game::cfg::UNIT_HARD_CAP];
}

static constexpr uint32_t kCells = 16384; // 64 KiB per plan
static constexpr uint32_t kShift = 3;

struct Plan { int32_t cells[kCells]; };
static Plan g_now, g_prev;
static unsigned char g_bytes[kCells];

extern "C" void init(int32_t, int64_t) {}

extern "C" int32_t on_tick() {
  const auto* h = reinterpret_cast<const game::SnapshotHeader*>(SNAPSHOT_ADDR);
  const int32_t seed = (int32_t)h->tick;

  for (uint32_t i = 0; i < kCells; ++i) g_now.cells[i] = seed + (int32_t)i;
  g_prev = g_now;
  g_now = Plan{};
  for (uint32_t i = 0; i < kCells; ++i)
    if (g_now.cells[i] != 0 || g_prev.cells[i] != seed + (int32_t)i) __builtin_trap();

  // Overlapping, unaligned move: the case memcpy may not handle.
  for (uint32_t i = 0; i < kCells; ++i) g_bytes[i] = (unsigned char)(seed + i);
  __builtin_memmove(g_bytes + kShift, g_bytes + 1, kCells - kShift);
  for (uint32_t i = kShift; i < kCells; ++i)
    if (g_bytes[i] != (unsigned char)(seed + i - kShift + 1)) __builtin_trap();
  return 0;
}
