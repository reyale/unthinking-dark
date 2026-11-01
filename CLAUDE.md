# Dev Agent Guide

Read this before touching any code. It covers what exists, what invariants must never break,
and what is explicitly out of scope.

Full game rules and ABI spec: `docs/DESIGN.md`.
Build order and acceptance criteria: `docs/IMPLEMENTATION_PLAN.md`.

---

## Current State

**Phases 1–5 complete. 300/300 tests passing.**

| Phase | Status | Notes |
|-------|--------|-------|
| 1 — Engine | ✅ Done | Deterministic core, full test suite |
| 2 — Visualizer | ✅ Done | Terminal, ncurses, Dear ImGui (isometric), browser WASM |
| 3 — Territory | ✅ Done | Voronoi partition, descending win threshold, Claim Node fragility |
| 4 — WASM runner | ✅ Done | Wasmtime C API in `runner/`; `ud_run` CLI in `tools/` |
| 5 — Bot SDK | ✅ Done | C++ template + ABI headers in `sdk/cpp/`; `ud_profile` deterministic profiler in `profiler/` |
| 6 — Tournament | 🔲 Not started | |
| 7 — Calibration | 🔲 Not started | |

Additional work:
- **Draws are allowed** (`WinReason::Draw`): both Command Cores die the same tick, or the tick cap is
  reached with territory difference ≤ `cfg::DRAW_TERRITORY_MARGIN` (5%). Don't "fix" them away.
- `ABI_VERSION` is 13 (8 added `StructureView`: all own structures; 9 added `EnemyStructureView`:
  enemy structures in sight; 10 renamed `Command::unit_id` → `entity_id`: Produce is issued by the
  Core (Drones) or a Factory (ships) itself; 11 added `UnitView::cargo_energy/cargo_alloy`; 12 added `StructureView::build_ticks_left` and
  `EnemyStructureView::building` for claim construction; 13: Factories are built the same way,
  so those fields now apply to them too). Bumping it invalidates all existing replay files and every compiled bot.
- Hard caps `UNIT_HARD_CAP` (256, queued ships count) and `STRUCTURE_HARD_CAP` (128) are enforced in
  `economy.cpp`: they bound the snapshot buffer, and the runner clamps to them when copying in.

---

## Build & Test

```bash
~/.local/bin/cmake -B build -DCMAKE_BUILD_TYPE=Debug   # once, or when CMakeLists.txt changes
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

Requires CMake ≥ 3.20 (system cmake is 3.16; use `~/.local/bin/cmake` which is 4.x).

**Linux only** for the native build: the WASM runner's capped bot memory uses `mmap`/`mprotect`, and
`runner/CMakeLists.txt` fails configuration elsewhere. (The Emscripten viewer build doesn't include
the runner and is unaffected.)

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
| `config.hpp` | Every balance constant and hard cap (unit stats, production costs, harvest yields, territory). No magic numbers anywhere else. |
| `ids.hpp` | Strong typed `UnitId`, `StructureId`, `FactionId`, and `EntityId` (a command's issuer). Units and structures share one id sequence (`World::next_entity_id`): ids never collide. Never raw `uint32_t` for ids. |
| `rng.hpp/.cpp` | xoshiro256** PRNG. Hand-rolled — `std::uniform_int_distribution` is not portable. |
| `fixed.hpp/.cpp` | Q32.32 fixed-point. `isqrt()` seeds from `std::sqrt` (currently unused by game logic — make it integer-only before relying on it). |
| `grid.hpp/.cpp` | `Vec2`, `Terrain`, 4-connected grid helpers (`manhattan()`, `is_passable()`). |
| `stats.hpp/.cpp` | `UnitStats` and `stats_for(UnitType)`, built from `config.hpp`. |
| `entity.hpp` | `UnitType`, `StructureType`, `OrderType` enums; core structs. |
| `world.hpp/.cpp` | Authoritative game state. `std::map` for units/structures (never unordered). |
| `vision.hpp/.cpp` | `sees_tile()` (the one sight rule: Manhattan, nebula tiles only within `NEBULA_SIGHT`) and `NebulaSight` (who can target into which nebula; combat and MoveAttack). |
| `snapshot.hpp/.cpp` | Fog-masked snapshot builder, via `sees_tile()`. Own units and structures always included; enemy units and structures only on visible tiles. |
| `command.hpp/.cpp` | `ValidatedCommands` and `validate_commands()`. |
| `movement.hpp/.cpp` | Stepped simultaneous movement. Chains advance (a unit may enter a tile vacated on the same step); swaps and other closed loops are blocked whole. |
| `combat.hpp/.cpp` | Sub-phase A: first-strike (Frigates). Sub-phase B: simultaneous; splash has friendly fire; finished Claim Nodes fire here too. |
| `economy.hpp/.cpp` | Harvest into Drone cargo, deposit at own Core/Claim Node, spend, production. Drone cap = 1 per claim + 1 per Core; Factory cap = max(claims / 2, 1). |
| `territory.hpp/.cpp` | `territory_owners()` (per-tile owner grid, the one definition of territory) and `territory_pct()`; Voronoi territory as a share of **passable** tiles (squared-Euclidean nearest Claim Node, cross-faction ties → neutral, `CLAIM_INFLUENCE` radius; each claim stamps only its radius), Claim Node hp fragility, `win_threshold()`. |
| `wincheck.hpp/.cpp` | Fixed-order checks: base destruction → territory threshold → tick cap. |
| `statehash.hpp/.cpp` | Rolling xxh3 via xxhash (`XXH_INLINE_ALL`). |
| `tick.hpp/.cpp` | Phase pipeline. `run_tick_phases()` (raw commands) and `run_tick()` (calls bots). |
| `replay.hpp/.cpp` | Input-log model. `replay()` re-simulates; `replay_frames()` builds `FrameState` vector. |
| `replay_io.hpp/.cpp` | `write_replay`/`read_replay` (+ `_file` variants) for `ReplayLog`. |
| `bot_iface.hpp` | `game::Bot` interface implemented by native fixtures and `WasmBot`. |
| `frame.hpp/.cpp` | `FrameState` (renderable snapshot) and `replay_frames()`. |
| `match.hpp/.cpp` | `run_match()`: drives tick loop, records replay log. |
| `file_io.cpp` | Raw + Zstd file I/O. Zstd sections are `#ifdef`'d out under `__EMSCRIPTEN__`. |

`fixtures/` — native test bots: `IdleBot` (empty commands; determinism tests), `RushBot`, `EconBot`,
`TurtleBot` (one home claim, guards its base) and `ExpanderBot` (its Core produces Drones,
walks claimers out to a claim lattice, never attacks), and `AdaptiveBot`, the combat fixture
that plays every system: Factories, counter-picked ships (Battleships when rich), an
Interceptor scout hidden in a nebula by the enemy Core, a home ambush in a nebula, and pushes
in waves (gather out of sight, then enemy claims, unfinished first, then the Core) when its
army outweighs what it has seen; it fights enemy ships near its structures first. Ships fire
only on a tick they are ordered to Attack/MoveAttack, so it orders every ship every tick, with
focus fire via `target_id`.
`Turtle.*` in `map_tests` proves the turtle loses to the descending threshold on every map;
`Adaptive.*` proves the adaptive bot destroys both and that two of them fight.
`bot_util.hpp` has a BFS `Router`: the engine never re-routes a blocked unit, so bots route
themselves (own units block only the first step, lower id has right of way; enemy structures
are remembered once seen). `waypoint()` lets a fast unit cover its full speed along a route;
`harvest_step()` runs a Drone's carry loop: node, then drop-off; `builders()` keeps a Drone
by each unfinished structure; `ClaimLattice` hands out claim sites.

### runner/ — Wasmtime WASM bot host

`WasmBot` implements `game::Bot`. Loads `.wasm` files via Wasmtime C API, enforces fuel limits,
validates ABI version before first tick. `load_wasm_file()` reads bytes; `WasmBot::healthy()`
must be true before use.

**Forfeits:** a tick where a bot runs out of fuel, traps, or tries to grow memory past
`cfg::MEMORY_CAP_MB` is forfeited: its commands are discarded and that side stalls for the tick.
The cap is enforced by a host memory creator in `wasm_bot.cpp` (the only C API hook that sees grow
requests); the refused grow returns -1 and the memory never exceeds the cap. The cause is
`BotMetrics::fault` (`TickFault::{None,OutOfFuel,OutOfMemory,Trap}`), stored per tick per bot in
`TickEntry::fault_a/b`, carried into `FrameState::forfeits`, shown in the viewer as "STALLED", and
summarised by `ud_run`. `TickFault` values are written to replay files: append only.

**Bulk memory is disabled** (`memory.copy`/`memory.fill` cost 1 fuel regardless of length, so
they would unbound CPU per tick). Such modules fail to load. The SDK builds with
`-mno-bulk-memory` and links `sdk/cpp/runtime.cpp` (loop-based `memcpy`/`memset`/`memmove`).
Flagged for review in README.md "Open decisions to review".

**Replay file format** is versioned by its magic (`UDREPLY2`), separately from `ABI_VERSION`.
Change the layout → bump the magic and refuse the old one with a clear message (see `replay_io.cpp`).

### tools/ — CLI utilities

| Binary | Purpose |
|--------|---------|
| `ud_run` | Run a 1v1 match: `ud_run <a.wasm> <b.wasm> [--map f] [--out f] [--seed N] [--ticks N]` |
| `ud_mapgen` | Generate a map JSON |
| `ud_mapcheck` | Validate a map JSON |

Standard match setup (Command Core + starting units on free tiles near each spawn) lives in
`maps/match_setup.hpp` as `maps::world_from_map()`, shared by `ud_run` and the map tests.
| `gen_example_replay` | Generate a fixture replay for tests |

### viz/ — replay renderers

| Binary / file | Description |
|---------------|-------------|
| `ud_viz` | Terminal renderer (raw ANSI) |
| `ud_viz_nc` | ncurses renderer (optional, requires libncurses) |
| `ud_viz_imgui` | Dear ImGui, isometric 2:1 projection (optional, requires libglfw3-dev). Z toggles claim zones, drawn with the engine's `territory_owners()`. Resource nodes are amber crystal clusters, nebulae drifting violet gas (thin veil drawn over units inside); a Drone's cargo is a matching amber bar on its lower-left edge (HP is lower-right) |
| `ud_shape_editor` | ImGui editor for unit/structure render shapes; reads/writes `.udshapes` files (`shape_model.hpp`) |
| `Makefile.em` | Emscripten browser build → `viz/web/index.html`; drag-and-drop `.ud` files |

Both ImGui apps default to the repo's `shapes.udshapes` (path baked in via the `UD_SHAPES_FILE`
CMake cache variable); override per run with `--shapes <file>`. `ud_viz_imgui` hot-reloads it on save.

ImGui build fetches Dear ImGui v1.91.6 via CMake FetchContent. The browser build uses the same
source; run `cmake -B build && cmake --build build` first to populate `build/_deps/`.

### profiler/ — deterministic fuel/memory profiler (DESIGN §10)

`profiler_lib` + `ud_profile <bot.wasm> [--scenario NAME]... [--ticks] [--list]`. Each scenario
is a real match through `game::run_match` with a `runner::WasmBot`, so reported fuel equals the
match meter exactly (`Profiler.FuelAndMemoryMatchARealMatchExactly` guards this — never fork the
metering path). The opponent is passive and its Command Core sits in an asteroid ring beyond
every weapon's range, so scenarios always run their full length. Suite: `early_game`,
`army_scaling/{10,50,200,256}`, `dense_battle`, `wide_visibility`, `max_legal`, `long_running`
(memory curve for leaks). Exit status 2 on any forfeit.

### sdk/cpp/ — C++ bot template

ABI headers in `sdk/cpp/include/`. Bot template in `sdk/cpp/bot_template.cpp`. Examples:
`idle_bot`, `rush_bot`, plus profiler demonstrations `quadratic_bot` (deliberately O(n²)) and
`leaky_bot` (deliberately leaks memory), and `copy_bot` (exercises `runtime.cpp`). Build with the
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
- C++23 (`set(CMAKE_CXX_STANDARD 23)`; googletest is built at C++20). Headers using `operator<=>` must `#include <compare>` — libc++ (Emscripten) does not pull it in transitively.
- clang-format configured at root: 2-space indent, 100 column limit.

---

## Decided Design Questions

- **Line-of-sight:** Manhattan distance ≤ sight_radius, no rays. Nebulae hide what is inside:
  a nebula tile (and its occupant) is seen only from within `NEBULA_SIGHT` (1), and can't be
  targeted by a side that doesn't see it. Asteroids don't block sight.
- **Construction:** every Drone-placed structure (Factory, Claim Node) needs `BUILD_TICKS`
  ticks with an own Drone within its placement range (`placement_range()`: 1 for a Factory,
  2 for a claim); it waits, attackable, otherwise. Unfinished claim: no territory, no
  drop-off, no Drone/Factory slot, doesn't shoot. Unfinished Factory: can't produce, but
  counts toward the Factory cap.
- **Claims shoot, very weakly:** a finished claim deals `CLAIM_ATTACK_DMG` (1) per tick to the
  nearest visible enemy unit within `CLAIM_ATTACK_RANGE` (2). Sized to beat one Interceptor at
  full claim hp but lose to two (`Combat.FullStrengthClaimSeesOffALoneScoutButNotTwo`).
- **No Gather command:** harvesting is automatic; the kind value is reserved and rejected.
- **No upkeep** for now; revisit only if calibration shows hoarding or spam.
- **Terrain memory:** terrain once seen stays known (it is static). Units and structures need
  current sight. The snapshot still sends only in-sight tiles; bots remember the rest themselves.
- **Economy:** Drones carry what they harvest and bank it at their own Core or a Claim Node.
  Drone cap is 1 per Claim Node + 1 per Core. Factory cap is max(finished claims / 2, 1).
  Factories cost 50 Energy + 100 Alloy. Nodes never deplete (decided, not an omission).
- **Blocked movement:** a unit may follow into a tile vacated on the same step, so columns move
  together. Swaps and any closed loop of movers are blocked, with everyone queued behind them:
  that is bad bot behavior and is meant to cost the bot. A blocked unit stays put for the step.
  The engine does no path-finding; routing, choke points and group ordering are the bot's job.

---

## What Is NOT Built Yet

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
