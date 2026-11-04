#include "wasm_bot.hpp"
#include "abi.hpp"
#include "config.hpp"
#include "grid.hpp"
#include "snapshot.hpp"
#include <wasmtime.h>
#include <algorithm>
#include <cstring>
#include <sys/mman.h>
#include <fstream>
#include <stdexcept>

namespace runner {

// ---- Helpers ----------------------------------------------------------------

// Drain a wasmtime_error_t or wasm_trap_t into a string, then free both.
static std::string drain_error(wasmtime_error_t* err, wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_byte_vec_new_empty(&msg);
  if (err) {
    wasmtime_error_message(err, &msg);
    wasmtime_error_delete(err);
  } else if (trap) {
    wasm_trap_message(trap, &msg);
    wasm_trap_delete(trap);
  }
  std::string s(msg.data, msg.size);
  wasm_byte_vec_delete(&msg);
  return s;
}

// Classify a call. Must run before drain_error(), which deletes the trap. A refused
// over-cap grow wins even if the call then completed or trapped: it's the root cause
// (a bot whose allocation failed often crashes on the null pointer right after).
static game::TickFault classify_fault(wasmtime_error_t* err, wasm_trap_t* trap, bool over_cap) {
  if (over_cap)
    return game::TickFault::OutOfMemory;
  if (!err && !trap)
    return game::TickFault::None;
  wasmtime_trap_code_t code;
  if (trap && wasmtime_trap_code(trap, &code) && code == WASMTIME_TRAP_CODE_OUT_OF_FUEL)
    return game::TickFault::OutOfFuel;
  return game::TickFault::Trap;
}

// Maximum sizes for validation at load time.
static constexpr size_t MAX_SNAPSHOT_BYTES =
    sizeof(game::SnapshotHeader) +
    game::cfg::UNIT_HARD_CAP * sizeof(game::UnitView) +
    game::cfg::STRUCTURE_HARD_CAP * sizeof(game::StructureView) +
    game::cfg::UNIT_HARD_CAP * 2 * sizeof(game::EnemyView) +
    game::cfg::STRUCTURE_HARD_CAP * sizeof(game::EnemyStructureView) +
    game::cfg::MAP_MAX_W * game::cfg::MAP_MAX_H * sizeof(game::TileView);

static constexpr size_t MAX_COMMAND_BYTES =
    game::cfg::UNIT_HARD_CAP * sizeof(game::Command);

// Serialize a snapshot into the bot's snapshot buffer. The engine enforces the caps these counts
// are clamped to; the clamp is what guarantees the copy stays inside the bot's validated buffer.
static void write_snapshot(uint8_t* dest, const game::Snapshot& snap) {
  auto clamp = [](size_t n, size_t cap) { return static_cast<uint32_t>(std::min(n, cap)); };
  game::SnapshotHeader hdr = snap.header;
  hdr.my_unit_count       = clamp(snap.my_units.size(), game::cfg::UNIT_HARD_CAP);
  hdr.my_structure_count  = clamp(snap.my_structures.size(), game::cfg::STRUCTURE_HARD_CAP);
  hdr.visible_enemy_count = clamp(snap.visible_enemies.size(), game::cfg::UNIT_HARD_CAP * 2);
  hdr.visible_enemy_structure_count =
      clamp(snap.visible_enemy_structures.size(), game::cfg::STRUCTURE_HARD_CAP);
  hdr.visible_tile_count  =
      clamp(snap.visible_tiles.size(), game::cfg::MAP_MAX_W * game::cfg::MAP_MAX_H);
  hdr.my_units_off      = static_cast<uint32_t>(sizeof(game::SnapshotHeader));
  hdr.my_structures_off = hdr.my_units_off + hdr.my_unit_count * sizeof(game::UnitView);
  hdr.enemies_off = hdr.my_structures_off + hdr.my_structure_count * sizeof(game::StructureView);
  hdr.enemy_structures_off =
      hdr.enemies_off + hdr.visible_enemy_count * sizeof(game::EnemyView);
  hdr.tiles_off = hdr.enemy_structures_off +
                  hdr.visible_enemy_structure_count * sizeof(game::EnemyStructureView);

  std::memcpy(dest, &hdr, sizeof(hdr));
  auto copy = [&](uint32_t off, const auto& v, uint32_t n) {
    if (n) std::memcpy(dest + off, v.data(), n * sizeof(v[0]));
  };
  copy(hdr.my_units_off, snap.my_units, hdr.my_unit_count);
  copy(hdr.my_structures_off, snap.my_structures, hdr.my_structure_count);
  copy(hdr.enemies_off, snap.visible_enemies, hdr.visible_enemy_count);
  copy(hdr.enemy_structures_off, snap.visible_enemy_structures, hdr.visible_enemy_structure_count);
  copy(hdr.tiles_off, snap.visible_tiles, hdr.visible_tile_count);
}

// Terrain is public: before init the buffer holds the whole map as a snapshot of tiles only,
// one per tile in row order, with nothing else in it.
static game::Snapshot map_snapshot(const game::Map& map, uint32_t faction_id) {
  game::Snapshot s;
  s.header.my_faction_id = faction_id;
  s.header.map_w = static_cast<uint32_t>(map.width);
  s.header.map_h = static_cast<uint32_t>(map.height);
  for (int32_t y = 0; y < map.height; ++y)
    for (int32_t x = 0; x < map.width; ++x) {
      game::TileView tv{};
      tv.x = x;
      tv.y = y;
      tv.terrain = static_cast<uint16_t>(map.tile_at({x, y}).terrain);
      tv.resource_amount = map.tile_at({x, y}).resource_amount;
      s.visible_tiles.push_back(tv);
    }
  return s;
}

// ---- Free functions ---------------------------------------------------------

std::vector<uint8_t> wat_to_wasm(std::string_view wat) {
  wasm_byte_vec_t result;
  wasmtime_error_t* err = wasmtime_wat2wasm(wat.data(), wat.size(), &result);
  if (err) {
    std::string msg = drain_error(err, nullptr);
    throw std::runtime_error("wat2wasm failed: " + msg);
  }
  std::vector<uint8_t> out(result.data, result.data + result.size);
  wasm_byte_vec_delete(&result);
  return out;
}

std::vector<uint8_t> load_wasm_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot open wasm file: " + path);
  return {std::istreambuf_iterator<char>(f), {}};
}

// ---- Capped linear memory ---------------------------------------------------
//
// The bot's linear memory is supplied by the runner through Wasmtime's host memory
// creator. That is the only C API hook that sees grow requests: a grow past
// cfg::MEMORY_CAP_MB is refused (memory.grow returns -1 to the bot, as at any limit)
// and the attempt is noted, so the bot forfeits even if it copes with the refusal. The
// memory never actually exceeds the cap.
//
// The full reservation Wasmtime asks for is mapped up front (PROT_NONE) and pages are
// made accessible as the memory grows, so the memory never moves and Wasmtime's
// bounds-check strategy (and therefore generated code) is unchanged.

static constexpr size_t kMemoryCapBytes = static_cast<size_t>(game::cfg::MEMORY_CAP_MB) << 20;

struct MemoryGate {
  bool over_cap = false; // set when a grow past the cap was refused; reset per call
};

struct CappedMemory {
  MemoryGate* gate;
  uint8_t*    base;
  size_t      mapped;   // reservation + guard, for munmap
  size_t      capacity; // bytes the memory may reach without moving
  size_t      size;
};

static uint8_t* capped_get(void* env, size_t* byte_size, size_t* byte_capacity) {
  auto* m = static_cast<CappedMemory*>(env);
  *byte_size     = m->size;
  *byte_capacity = m->capacity;
  return m->base;
}

static wasmtime_error_t* capped_grow(void* env, size_t new_size) {
  auto* m = static_cast<CappedMemory*>(env);
  if (new_size > kMemoryCapBytes) {
    m->gate->over_cap = true;
    return wasmtime_error_new("linear memory would exceed MEMORY_CAP_MB");
  }
  if (new_size > m->size &&
      mprotect(m->base + m->size, new_size - m->size, PROT_READ | PROT_WRITE) != 0)
    return wasmtime_error_new("mprotect failed while growing linear memory");
  m->size = new_size;
  return nullptr;
}

static void capped_free(void* env) {
  auto* m = static_cast<CappedMemory*>(env);
  munmap(m->base, m->mapped);
  delete m;
}

static wasmtime_error_t* capped_new(void* env, const wasm_memorytype_t*, size_t minimum,
                                    size_t /*maximum*/, size_t reserved, size_t guard,
                                    wasmtime_linear_memory_t* out) {
  if (minimum > kMemoryCapBytes)
    return wasmtime_error_new("initial linear memory exceeds MEMORY_CAP_MB");
  const size_t capacity = std::max(reserved, kMemoryCapBytes);
  const size_t mapped   = capacity + guard;
  void* base = mmap(nullptr, mapped, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (base == MAP_FAILED)
    return wasmtime_error_new("could not reserve address space for linear memory");
  if (minimum > 0 && mprotect(base, minimum, PROT_READ | PROT_WRITE) != 0) {
    munmap(base, mapped);
    return wasmtime_error_new("mprotect failed for initial linear memory");
  }
  auto* m = new CappedMemory{static_cast<MemoryGate*>(env), static_cast<uint8_t*>(base), mapped,
                             capacity, minimum};
  out->env         = m;
  out->get_memory  = capped_get;
  out->grow_memory = capped_grow;
  out->finalizer   = capped_free;
  return nullptr;
}

// ---- WasmBot::Impl ----------------------------------------------------------

struct WasmBot::Impl {
  wasm_engine_t*     engine{nullptr};
  wasmtime_store_t*  store{nullptr};
  wasmtime_linker_t* linker{nullptr};
  wasmtime_module_t* module{nullptr};
  wasmtime_instance_t instance{};
  wasmtime_memory_t   memory{};
  wasmtime_func_t     fn_init{};
  wasmtime_func_t     fn_on_tick{};
  uint32_t snapshot_addr{0};
  uint32_t command_addr{0};
  MemoryGate memory_gate; // outlives the store: members are destroyed after ~Impl's body

  wasmtime_context_t* ctx() { return wasmtime_store_context(store); }

  ~Impl() {
    if (module) wasmtime_module_delete(module);
    if (linker) wasmtime_linker_delete(linker);
    if (store)  wasmtime_store_delete(store);
    if (engine) wasm_engine_delete(engine);
  }
};

// ---- WasmBot ----------------------------------------------------------------

WasmBot::WasmBot(const std::vector<uint8_t>& wasm)
    : impl_(std::make_unique<Impl>()) {
  // Engine with determinism flags and fuel metering.
  wasm_config_t* cfg = wasm_config_new();
  wasmtime_config_consume_fuel_set(cfg, true);
  wasmtime_config_wasm_threads_set(cfg, false);
  wasmtime_config_wasm_simd_set(cfg, false);
  wasmtime_config_wasm_relaxed_simd_set(cfg, false); // relaxed SIMD requires SIMD
  // memory.fill/memory.copy cost one fuel unit whatever their length, so a bot could burn
  // unbounded CPU inside its budget. Without them the compiler emits ordinary metered loops.
  wasmtime_config_wasm_bulk_memory_set(cfg, false);
  wasmtime_config_cranelift_nan_canonicalization_set(cfg, true);
  // Copy-on-write data initialization maps a module's data segments straight into Wasmtime's
  // own mmap'd memory, which a host memory creator replaces; with both, Wasmtime panics on any
  // module with sizeable data (a bot linking libc, say). Segments are then copied in instead.
  wasmtime_config_memory_init_cow_set(cfg, false);
  wasmtime_memory_creator_t creator{&impl_->memory_gate, capped_new, nullptr};
  wasmtime_config_host_memory_creator_set(cfg, &creator); // copied into the config
  impl_->engine = wasm_engine_new_with_config(cfg); // config ownership transferred

  impl_->store  = wasmtime_store_new(impl_->engine, nullptr, nullptr);
  impl_->linker = wasmtime_linker_new(impl_->engine);

  // Compile.
  wasmtime_error_t* err = wasmtime_module_new(
      impl_->engine, wasm.data(), wasm.size(), &impl_->module);
  if (err) {
    last_error_ = drain_error(err, nullptr);
    if (last_error_.find("bulk memory support is not enabled") != std::string::npos)
      last_error_ += "\n(bots must be built with -mno-bulk-memory; see sdk/cpp/Makefile and "
                     "sdk/cpp/runtime.cpp)";
    return;
  }

  // Instantiate (no host imports required for v1 bots).
  wasm_trap_t* trap = nullptr;
  err = wasmtime_linker_instantiate(
      impl_->linker, impl_->ctx(), impl_->module, &impl_->instance, &trap);
  if (err || trap) { last_error_ = drain_error(err, trap); return; }

  // Extract and validate required exports.
  auto* ctx = impl_->ctx();
  auto& inst = impl_->instance;
  wasmtime_extern_t item;

  auto get_export = [&](const char* name, uint8_t expected_kind) -> bool {
    if (!wasmtime_instance_export_get(ctx, &inst, name, strlen(name), &item)
        || item.kind != expected_kind) {
      last_error_ = std::string("missing or wrong-type export: ") + name;
      return false;
    }
    return true;
  };

  if (!get_export("memory",   WASMTIME_EXTERN_MEMORY)) return;
  impl_->memory = item.of.memory;

  if (!get_export("init",     WASMTIME_EXTERN_FUNC)) return;
  impl_->fn_init = item.of.func;

  if (!get_export("on_tick",  WASMTIME_EXTERN_FUNC)) return;
  impl_->fn_on_tick = item.of.func;

  if (!get_export("SNAPSHOT_ADDR", WASMTIME_EXTERN_GLOBAL)) return;
  {
    wasmtime_val_t val;
    wasmtime_global_get(ctx, &item.of.global, &val);
    if (val.kind != WASMTIME_I32) { last_error_ = "SNAPSHOT_ADDR not i32"; return; }
    impl_->snapshot_addr = static_cast<uint32_t>(val.of.i32);
  }

  if (!get_export("COMMAND_ADDR", WASMTIME_EXTERN_GLOBAL)) return;
  {
    wasmtime_val_t val;
    wasmtime_global_get(ctx, &item.of.global, &val);
    if (val.kind != WASMTIME_I32) { last_error_ = "COMMAND_ADDR not i32"; return; }
    impl_->command_addr = static_cast<uint32_t>(val.of.i32);
  }

  // Validate buffer regions fit in the bot's linear memory.
  size_t mem_bytes = wasmtime_memory_data_size(ctx, &impl_->memory);
  if (impl_->snapshot_addr + MAX_SNAPSHOT_BYTES > mem_bytes) {
    last_error_ = "SNAPSHOT_ADDR buffer too small for worst-case snapshot";
    return;
  }
  if (impl_->command_addr + MAX_COMMAND_BYTES > mem_bytes) {
    last_error_ = "COMMAND_ADDR buffer too small for max commands";
    return;
  }

  if (!get_export("abi_version", WASMTIME_EXTERN_FUNC)) return;
  {
    wasmtime_func_t fn_abi = item.of.func;
    wasmtime_context_set_fuel(ctx, 1000);
    wasmtime_val_t abi_result{};
    wasm_trap_t* trap2   = nullptr;
    wasmtime_error_t* err2 =
        wasmtime_func_call(ctx, &fn_abi, nullptr, 0, &abi_result, 1, &trap2);
    if (err2 || trap2) {
      last_error_ = "abi_version() failed: " + drain_error(err2, trap2);
      return;
    }
    if (static_cast<uint32_t>(abi_result.of.i32) != game::cfg::ABI_VERSION) {
      last_error_ = "ABI version mismatch: bot=" + std::to_string(abi_result.of.i32)
                    + " engine=" + std::to_string(game::cfg::ABI_VERSION);
      return;
    }
  }

  healthy_ = true;
}

WasmBot::~WasmBot() = default;

void WasmBot::on_init(const game::Map& map, uint32_t faction_id) {
  if (!healthy_) return;

  auto* ctx = impl_->ctx();
  write_snapshot(wasmtime_memory_data(ctx, &impl_->memory) + impl_->snapshot_addr,
                 map_snapshot(map, faction_id));
  wasmtime_context_set_fuel(ctx, game::cfg::FUEL_STARTUP);
  impl_->memory_gate.over_cap = false;

  wasmtime_val_t arg{};
  arg.kind   = WASMTIME_I32;
  arg.of.i32 = static_cast<int32_t>(faction_id);

  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &impl_->fn_init, &arg, 1, nullptr, 0, &trap);

  uint64_t remaining = 0;
  wasmtime_context_get_fuel(ctx, &remaining);
  last_metrics_.fuel_consumed = game::cfg::FUEL_STARTUP - remaining;
  last_metrics_.memory_bytes  = wasmtime_memory_data_size(ctx, &impl_->memory);
  last_metrics_.fault         = classify_fault(err, trap, impl_->memory_gate.over_cap);

  // Any init overrun, memory included, is a load failure: the bot cannot play.
  if (last_metrics_.fault != game::TickFault::None) {
    std::string detail = drain_error(err, trap); // also frees err/trap
    last_error_ = last_metrics_.fault == game::TickFault::OutOfMemory
                      ? "init exceeded the memory cap"
                      : detail;
    healthy_ = false;
  }
}

std::vector<game::Command> WasmBot::on_tick(const game::Snapshot& snap) {
  if (!healthy_) return {};

  auto* ctx = impl_->ctx();
  uint8_t* mem = wasmtime_memory_data(ctx, &impl_->memory);
  write_snapshot(mem + impl_->snapshot_addr, snap);

  // Refuel and call on_tick.
  wasmtime_context_set_fuel(ctx, game::cfg::FUEL_PER_TICK);
  impl_->memory_gate.over_cap = false;

  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  wasmtime_error_t* err =
      wasmtime_func_call(ctx, &impl_->fn_on_tick, nullptr, 0, &result, 1, &trap);

  uint64_t remaining = 0;
  wasmtime_context_get_fuel(ctx, &remaining);
  last_metrics_.fuel_consumed = game::cfg::FUEL_PER_TICK - remaining;
  last_metrics_.memory_bytes  = wasmtime_memory_data_size(ctx, &impl_->memory);
  last_metrics_.fault         = classify_fault(err, trap, impl_->memory_gate.over_cap);

  if (last_metrics_.fault != game::TickFault::None) {
    // The bot forfeits the match: commands are discarded, even if the call completed after a
    // refused grow.
    last_error_ = (err || trap) ? drain_error(err, trap) : "memory cap exceeded";
    healthy_ = false;
    return {};
  }

  auto cmd_count = static_cast<uint32_t>(result.of.i32);
  cmd_count = std::min(cmd_count, static_cast<uint32_t>(game::cfg::UNIT_HARD_CAP));

  std::vector<game::Command> cmds(cmd_count);
  if (cmd_count > 0)
    std::memcpy(cmds.data(), mem + impl_->command_addr,
                cmd_count * sizeof(game::Command));
  return cmds;
}

} // namespace runner
