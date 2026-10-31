// Deliberately leaks memory: a profiler demonstration, not a strategy.
// Every tick it appends a record of what it saw to a history log that is never
// trimmed, growing linear memory a page at a time. Harmless for a short match;
// after thousands of ticks a real bot doing this runs out of memory. The
// long_running scenario's memory curve is how an author spots it.
#include <ud/types.hpp>
#include <cstdint>

extern "C" {
  alignas(4) unsigned char SNAPSHOT_ADDR[ud::SNAPSHOT_BUFFER_SIZE];
  alignas(4) game::Command COMMAND_ADDR[game::cfg::UNIT_HARD_CAP];
  extern unsigned char __heap_base; // first free byte after static data (wasm-ld)
}

static constexpr uint32_t kPage        = 65536;
static constexpr uint32_t kRecordBytes = 1024;

static uint32_t g_log_end = 0; // linear-memory offset of the end of the history log

extern "C" void init(int32_t) { g_log_end = (uint32_t)(uintptr_t)&__heap_base; }

extern "C" int32_t on_tick() {
  const auto* h     = reinterpret_cast<const game::SnapshotHeader*>(SNAPSHOT_ADDR);
  const auto* units = reinterpret_cast<const game::UnitView*>(SNAPSHOT_ADDR + h->my_units_off);

  const uint32_t need = g_log_end + kRecordBytes;
  const uint32_t have = (uint32_t)__builtin_wasm_memory_size(0) * kPage;
  if (need > have && __builtin_wasm_memory_grow(0, (need - have + kPage - 1) / kPage) < 0)
    return 0; // out of memory: stop recording, keep playing

  // The record: tick, resources, and as many of our unit positions as fit.
  auto* rec = reinterpret_cast<int32_t*>((uintptr_t)g_log_end);
  rec[0] = (int32_t)h->tick;
  rec[1] = h->energy;
  rec[2] = h->alloy;
  for (uint32_t i = 0; i < kRecordBytes / 4 - 3; ++i)
    rec[3 + i] = i < h->my_unit_count ? units[i].x * 1000 + units[i].y : 0;
  g_log_end = need;
  return 0;
}
