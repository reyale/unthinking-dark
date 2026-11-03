# runner/

Wasmtime host that executes untrusted `.wasm` bots inside the engine.

## What it does

`WasmBot` implements the `game::Bot` interface by loading a compiled WebAssembly
module and running it under [Wasmtime](https://wasmtime.dev/) with:

- **Fuel metering** — each tick is budgeted; exhaustion forfeits that tick
- **Memory cap** — `MEMORY_CAP_MB` enforced via a host memory creator; a grow past it is
  refused and forfeits that tick
- **Determinism flags** — threads off, SIMD off, relaxed SIMD off, NaN canonicalization on
- **Bulk memory off** — modules using `memory.copy`/`memory.fill`/`memory.init`/`data.drop`
  or the `table.*` equivalents are rejected at load (see below)
- **Buffer validation** — export addresses and memory size checked at load time

**Linux only.** The capped linear memory is reserved and committed with `mmap`/`mprotect`.
`runner/CMakeLists.txt` stops configuration on any other platform.

## Bot ABI contract

A valid bot module must export:

| Export          | Kind     | Description                                      |
|-----------------|----------|--------------------------------------------------|
| `memory`        | memory   | Linear memory; must be large enough (see below)  |
| `SNAPSHOT_ADDR` | global i32 | Byte offset where the engine writes the snapshot |
| `COMMAND_ADDR`  | global i32 | Byte offset where the bot writes commands        |
| `init`          | func (i32) → () | Called once before tick 0 with `faction_id`; the snapshot buffer then holds the whole map |
| `on_tick`       | func () → i32   | Called every tick; returns command count    |

The engine writes a flat snapshot buffer into `[SNAPSHOT_ADDR, SNAPSHOT_ADDR + N)`
and reads commands from `[COMMAND_ADDR, COMMAND_ADDR + count * 24)` after `on_tick`
returns. The bot must reserve enough linear memory to cover both regions for
worst-case v1 caps (`UNIT_HARD_CAP` units, the cap on the largest map; `STRUCTURE_HARD_CAP`
structures, 2× units of visible enemies, `STRUCTURE_HARD_CAP` visible enemy structures, the
whole largest map of tiles: ≈ 318 KiB snapshot + 15 KiB commands; 6 WASM pages suffices). Array counts are clamped to those caps when the snapshot is copied in, so the
copy can never run past the buffer the bot was validated to have.

The snapshot holds, in order: header, my units, my structures (all of them, fog or not),
visible enemy units, visible enemy structures, visible tiles. Use the header offsets rather than assuming the order.

Before `init` the same buffer holds the map, since terrain is public: a snapshot with only
tiles, one `TileView` per tile in row order (terrain, resource amount), and `map_w`, `map_h`,
`my_faction_id` in the header. `sdk/cpp/include/ud/bot_host.hpp` turns it back into a `Map`.

Copy-on-write memory initialization is off (`memory_init_cow`): it maps a module's data
segments straight into Wasmtime's own memory, which the capped memory creator replaces, and
the two together panic on any module with sizeable data (one linking libc, say).

See `engine/src/abi.hpp` for the exact struct layouts.

## Fuel budgets

| Phase  | Budget (fuel units)       |
|--------|---------------------------|
| `init` | `cfg::FUEL_STARTUP` (1 B) |
| tick   | `cfg::FUEL_PER_TICK` (10 M) |

Init overrun → bot permanently unhealthy (load failure, match continues with
empty commands every tick). Tick overrun → empty commands that tick, bot stays
healthy and is called again next tick.

## Bulk memory (disabled — open to review)

Wasmtime charges each instruction one unit of fuel, and bulk-memory instructions are single
instructions however many bytes they touch. Measured on this runner: an ordinary loop burns
10M fuel in ~2 ms, but 200 × `memory.fill` of 32 MiB cost 2,601 fuel and took ~600 ms. Fuel
would no longer bound CPU time.

So `wasmtime_config_wasm_bulk_memory_set(cfg, false)`: such modules fail to load with
`bulk memory support is not enabled` (the runner appends a hint pointing at the SDK). Without
the feature, compilers emit copies and fills as ordinary loops, or as calls to `memcpy`/`memset`,
which are metered per iteration like any other code. Memory size, loads/stores, the memory cap
and fuel are otherwise unaffected.

The C++ SDK builds with `-mno-bulk-memory` and links `sdk/cpp/runtime.cpp`, which provides
loop-based `memcpy`/`memset`/`memmove` (the build is `-nostdlib`, so nothing else would).
`sdk/cpp/examples/copy_bot.cpp` and `SdkLifecycle.LargeCopiesRunAsMeteredLoops` check it.

**To review:** this pushes a burden onto every non-SDK toolchain (clang 19+, recent Rust and
Zig enable bulk memory by default). Alternatives if it proves a problem: rewrite modules at load
to charge bulk instructions in proportion to their length, or adopt per-byte fuel if Wasmtime
adds it. A wall-clock timeout is *not* an option for forfeits: it is machine-dependent and would
break determinism (at most a Phase 6 infrastructure abort for a hung match).

## Building bots

Bots are compiled to `wasm32` via clang (not the engine's GCC toolchain):

```bash
clang++ --target=wasm32-unknown-unknown -nostdlib -O2 -mno-bulk-memory \
        -Wl,--export=init,--export=on_tick \
        -Wl,--export=SNAPSHOT_ADDR,--export=COMMAND_ADDR \
        -Wl,--no-entry \
        -o my_bot.wasm my_bot.cpp sdk/cpp/runtime.cpp
```

The C++ SDK template in `sdk/cpp/` (Phase 5) provides scaffolding and the
shared ABI headers so you don't have to manage exports manually.
