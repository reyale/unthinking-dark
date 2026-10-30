# runner/

Wasmtime host that executes untrusted `.wasm` bots inside the engine.

## What it does

`WasmBot` implements the `game::Bot` interface by loading a compiled WebAssembly
module and running it under [Wasmtime](https://wasmtime.dev/) with:

- **Fuel metering** — each tick is budgeted; exhaustion forfeits that tick
- **Memory cap** — `MEMORY_CAP_MB` enforced via a host memory creator; a grow past it is
  refused and forfeits that tick
- **Determinism flags** — threads off, SIMD off, relaxed SIMD off, NaN canonicalization on

**Linux only.** The capped linear memory is reserved and committed with `mmap`/`mprotect`.
`runner/CMakeLists.txt` stops configuration on any other platform.
- **Buffer validation** — export addresses and memory size checked at load time

## Bot ABI contract

A valid bot module must export:

| Export          | Kind     | Description                                      |
|-----------------|----------|--------------------------------------------------|
| `memory`        | memory   | Linear memory; must be large enough (see below)  |
| `SNAPSHOT_ADDR` | global i32 | Byte offset where the engine writes the snapshot |
| `COMMAND_ADDR`  | global i32 | Byte offset where the bot writes commands        |
| `init`          | func (i32) → () | Called once before tick 0 with `faction_id` |
| `on_tick`       | func () → i32   | Called every tick; returns command count    |

The engine writes a flat snapshot buffer into `[SNAPSHOT_ADDR, SNAPSHOT_ADDR + N)`
and reads commands from `[COMMAND_ADDR, COMMAND_ADDR + count * 24)` after `on_tick`
returns. The bot must reserve enough linear memory to cover both regions for
worst-case v1 caps (≈ 280 KB snapshot + 6 KB commands; 6 WASM pages suffices).

See `engine/src/abi.hpp` for the exact struct layouts.

## Fuel budgets

| Phase  | Budget (fuel units)       |
|--------|---------------------------|
| `init` | `cfg::FUEL_STARTUP` (1 B) |
| tick   | `cfg::FUEL_PER_TICK` (10 M) |

Init overrun → bot permanently unhealthy (load failure, match continues with
empty commands every tick). Tick overrun → empty commands that tick, bot stays
healthy and is called again next tick.

## Building bots

Bots are compiled to `wasm32` via clang (not the engine's GCC toolchain):

```bash
clang++ --target=wasm32-unknown-unknown -nostdlib -O2 \
        -Wl,--export=init,--export=on_tick \
        -Wl,--export=SNAPSHOT_ADDR,--export=COMMAND_ADDR \
        -Wl,--no-entry \
        -o my_bot.wasm my_bot.cpp
```

The C++ SDK template in `sdk/cpp/` (Phase 5) provides scaffolding and the
shared ABI headers so you don't have to manage exports manually.
