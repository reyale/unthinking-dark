# Dev Agent Guide

Read this before touching any code. It covers what exists, what invariants must never break,
and what is explicitly out of scope.

Full game rules and ABI spec: `docs/DESIGN.md`.
Build order and acceptance criteria: `docs/IMPLEMENTATION_PLAN.md`.

---

## Current State

**Phases 1, 2, and 4 complete. Phase 5 partially complete. 188/188 tests passing.**

| Phase | Status | Notes |
|-------|--------|-------|
| 1 — Engine | ✅ Done | Deterministic core, full test suite |
| 2 — Visualizer | ✅ Done | Terminal, ncurses, Dear ImGui (isometric), browser WASM |
| 3 — Territory | 🔲 Stubbed | `territory.cpp` returns zeros; threshold/fragility not implemented |
| 4 — WASM runner | ✅ Done | Wasmtime C API in `runner/`; `ud_run` CLI in `tools/` |
| 5 — Bot SDK | 🔲 Partial | C++ template + ABI headers in `sdk/cpp/`; no profiler yet |
| 6 — Tournament | 🔲 Not started | |
| 7 — Calibration | 🔲 Not started | |

Additional work:
- `WinReason::Draw` — tick-cap result when territory difference ≤ `cfg::DRAW_TERRITORY_MARGIN` (5%).
- `ABI_VERSION` is 5; bumping it invalidates all existing replay files.

---

## Build & Test

```bash
~/.local/bin/cmake -B build -DCMAKE_BUILD_TYPE=Debug   # once, or when CMakeLists.txt changes
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

Requires CMake ≥ 3.20 (system cmake is 3.16; use `~/.local/bin/cmake` which is 4.x).

ASan + UBSan are enabled in Debug builds. Never suppress them — a sanitizer failure is a
determinism bug, not a false positive.

Coverage (requires `pip install gcovr`; separate build dir, no sanitizers):
```bash
cmake -B build-cov -DCMAKE_BUILD_TYPE=Coverage
cmake --build build-cov --target coverage
```

For LSP support: `ln -s build/compile_commands.json compile_commands.json`

---

## What Is Built

### engine/src/ — deterministic simulation

| File | Role |
|------|------|
| `abi.hpp` | **Single source of truth for all bot/engine wire structs.** Locked with `offsetof`/`sizeof` static_asserts. Do not define ABI structs anywhere else. |
| `config.hpp` | Every balance constant and hard cap. No magic numbers anywhere else. |
| `ids.hpp` | Strong typed `UnitId`, `StructureId`, `FactionId`. Never raw `uint32_t` for ids. |
| `rng.hpp/.cpp` | xoshiro256** PRNG. Hand-rolled — `std::uniform_int_distribution` is not portable. |
| `entity.hpp` | `UnitType`, `StructureType`, `OrderType` enums; `UnitStats` table; core structs. |
| `world.hpp/.cpp` | Authoritative game state. `std::map` for units/structures (never unordered). |
| `snapshot.hpp/.cpp` | Fog-masked snapshot builder. LoS = Manhattan distance ≤ sight_radius only. |
| `command.hpp/.cpp` | `ValidatedCommands` and `validate_commands()`. |
| `movement.hpp/.cpp` | Stepped simultaneous movement. Blocked unit stays put, no retry. |
| `combat.hpp/.cpp` | Sub-phase A: first-strike (Frigates). Sub-phase B: simultaneous; splash has friendly fire. |
| `economy.hpp/.cpp` | Harvest (Drones adjacent to ResourceNode), spend, production. |
| `territory.hpp/.cpp` | **Stubbed.** Returns `TerritoryState{}` with zeros. |
| `wincheck.hpp/.cpp` | Fixed-order checks: base destruction → territory threshold → tick cap. |
| `statehash.hpp/.cpp` | Rolling xxh3 via xxhash (`XXH_INLINE_ALL`). |
| `tick.hpp/.cpp` | Phase pipeline. `run_tick_phases()` (raw commands) and `run_tick()` (calls bots). |
| `replay.hpp/.cpp` | Input-log model. `replay()` re-simulates; `replay_frames()` builds `FrameState` vector. |
| `frame.hpp/.cpp` | `FrameState` (renderable snapshot) and `replay_frames()`. |
| `match.hpp/.cpp` | `run_match()`: drives tick loop, records replay log. |
| `file_io.cpp` | Raw + Zstd file I/O. Zstd sections are `#ifdef`'d out under `__EMSCRIPTEN__`. |

`fixtures/idle_bot.hpp` — `IdleBot`: always returns empty commands. Used by determinism tests.

### runner/ — Wasmtime WASM bot host

`WasmBot` implements `game::Bot`. Loads `.wasm` files via Wasmtime C API, enforces fuel limits,
validates ABI version before first tick. `load_wasm_file()` reads bytes; `WasmBot::healthy()`
must be true before use.

### tools/ — CLI utilities

| Binary | Purpose |
|--------|---------|
| `ud_run` | Run a 1v1 match: `ud_run <a.wasm> <b.wasm> [--map f] [--out f] [--seed N] [--ticks N]` |
| `ud_mapgen` | Generate a map JSON |
| `ud_mapcheck` | Validate a map JSON |
| `gen_example_replay` | Generate a fixture replay for tests |

### viz/ — replay renderers

| Binary / file | Description |
|---------------|-------------|
| `ud_viz` | Terminal renderer (raw ANSI) |
| `ud_viz_nc` | ncurses renderer (optional, requires libncurses) |
| `ud_viz_imgui` | Dear ImGui, isometric 2:1 projection (optional, requires libglfw3-dev) |
| `Makefile.em` | Emscripten browser build → `viz/web/index.html`; drag-and-drop `.ud` files |

ImGui build fetches Dear ImGui v1.91.6 via CMake FetchContent. The browser build uses the same
source; run `cmake -B build && cmake --build build` first to populate `build/_deps/`.

### sdk/cpp/ — C++ bot template

ABI headers in `sdk/cpp/include/`. Bot template in `sdk/cpp/bot_template.cpp`. Build with the
`sdk/cpp/Makefile` (requires `wasm32` clang toolchain). Exports `abi_version` symbol for runner
version check.

---

## Determinism Invariants — Never Violate

These silently break replay if violated. They are non-negotiable.

1. **Integer math only** in game state. No `float`/`double` touches game logic. `fixed.hpp`
   (Q32.32) is the only sanctioned exception.
2. **Single seeded PRNG**, drawn in a fixed order. RNG draws happen in ascending unit-id order
   within the combat phase. No other entropy source.
3. **Ordered containers only.** `std::map`, never `std::unordered_map`, in any logic path.
4. **All tie-breaks resolve to unit-id or structure-id**, assigned in deterministic creation order.
5. **Fixed phase order every tick**: validate → move → combat → economy → spawn → territory → wincheck.
6. **Bots decide against the frozen start-of-tick snapshot.** No bot sees mid-tick state.
7. **`World::rng_seed` must be set alongside `World::rng`.** Forgetting it breaks `Determinism.ReplayReproducesHash`.

Compile flags enforcing these: `-ffp-contract=off -fno-fast-math` (in `cmake/CompilerFlags.cmake`).

---

## ABI Rules

`abi.hpp` is the only place ABI structs live:
- Fixed-width integers only. No `std::` types.
- All fields naturally aligned — no `__attribute__((packed))`.
- Every struct has `sizeof`/`offsetof` static_asserts. Adding/changing a field → update asserts + bump `cfg::ABI_VERSION`.

---

## Coding Conventions

- **No comments explaining what code does.** Only comment the non-obvious *why*.
- **No magic numbers.** Every tunable goes in `config.hpp`.
- **No bare `int` or `size_t` in state structs.** Use fixed-width types.
- **`std::map` for anything iterated in game logic.**
- C++20 (`set(CMAKE_CXX_STANDARD 20)`). Headers using `operator<=>` must `#include <compare>` — libc++ (Emscripten) does not pull it in transitively.
- clang-format configured at root: 2-space indent, 100 column limit.

---

## Decided Design Questions

- **Line-of-sight:** Manhattan distance ≤ sight_radius only. No terrain blocking.
- **Blocked movement:** unit stays put, no retry. Deadlocks are accepted.

---

## What Is NOT Built Yet

- **Phase 3:** Voronoi territory (`territory.cpp` stubbed), descending win threshold, Claim Node fragility
- **Phase 5 remainder:** deterministic profiler, local tester CLI
- **Phase 6:** Tournament/ladder harness, stats aggregation
- **Phase 7:** Balance calibration pass

Out of v1 scope entirely (stop and ask if it seems necessary):
cross-architecture replay, terrain-respecting territory/vision, procedural maps, 8-connectivity,
zero-copy ABI, OS-level match sandboxing, 3rd resource type.

---

## Running Specific Tests

```bash
cd build && ctest --output-on-failure
./build/tests/engine_tests --gtest_filter="Determinism.*"
./build/tests/engine_tests --gtest_filter="Combat.*"
```

`Determinism.*` must always pass. If it flakes, stop everything and fix it first.
