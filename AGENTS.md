# Dev Agent Guide

How to work in this repo: what's where, how to build and test it, and the rules no change may
break. Read it before touching any code.

- Game rules, ABI contract and the reasons behind each decision: `docs/DESIGN.md`.
- What to work on next: `docs/IMPLEMENTATION_PLAN.md` (the roadmap).

---

## Build & Test

```bash
~/.local/bin/cmake -B build -DCMAKE_BUILD_TYPE=Debug   # once, or when CMakeLists.txt changes
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

```bash
./build/tests/engine_tests --gtest_filter="Determinism.*"
./build/tests/engine_tests --gtest_filter="Combat.*"
```

`Determinism.*` must always pass. If it flakes, stop everything and fix it first.

- Requires CMake ≥ 3.20 (system cmake is 3.16; use `~/.local/bin/cmake`, which is 4.x).
- Configuring downloads a pinned wasi-sdk (~120 MB, once) to build the fixture bots as `.wasm`;
  `-DUD_WASM_FIXTURES=OFF` skips it and the `fixture_wasm_tests`.
- **Linux only** for the native build: the runner's capped bot memory uses `mmap`/`mprotect`, and
  `runner/CMakeLists.txt` fails configuration elsewhere. (The Emscripten viewer build doesn't
  include the runner and is unaffected.)
- ASan + UBSan are on in Debug builds. Never suppress them: a sanitizer failure is a
  determinism bug, not a false positive.
- Coverage (`pip install gcovr`; separate build dir, no sanitizers):
  `cmake -B build-cov -DCMAKE_BUILD_TYPE=Coverage && cmake --build build-cov --target coverage`.
- LSP: `ln -s build/compile_commands.json compile_commands.json`.

**Version:** every tool takes `--version` and prints e.g. `ud_run v0.2.0-3-g5183546-dirty (ABI 17)`:
`git describe --tags --always --dirty`, re-read on every build (`cmake/Version.cmake`), or
`-DUD_VERSION=<string>` to set it. It is `game::build_version()` (`version.hpp`, in the engine
library, so installed users get it too); a new tool calls `game::print_version_if_asked()` first.

**Install** (for projects outside this repo, e.g. the tournament harness):
```bash
~/.local/bin/cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j$(nproc)
~/.local/bin/cmake --install build-rel --prefix <dir>       # the harness uses ../ai-game-install
```
`find_package(ai_game 0.1.0 REQUIRED)` (with `-DCMAKE_PREFIX_PATH=<dir>`) gives `ai_game::engine`,
`ai_game::maps`, `ai_game::runner`, and `AI_GAME_BOTS_DIR` / `AI_GAME_MAPS_DIR` (the fixture bots
as `.wasm`, the maps). Headers install flat to `include/ai_game/`, included by bare name as here.
Also installed: `ud_run`, `ud_mapgen`, `ud_mapcheck`, `ud_profile` and the viewers, and the
bot SDK: `ai_game_add_bot(<name> SOURCES ...)` compiles a `game::Bot` to `.wasm` against the
installed headers (`ud/bot_host.hpp` included), with the same command line as the fixtures
(`cmake/WasmBot.cmake`, shared); `AI_GAME_WASI_SDK` points it at a wasi-sdk 24, else it downloads
one. Rules in `cmake/Install.cmake`. The libraries carry what users must build and link with
(determinism flags, a static libstdc++, the sanitizers for a Debug install); the version must
match exactly, since ABI and replay format change between versions. Keep xxhash, zstd, json and
Wasmtime out of public headers: the install doesn't ship their headers.

---

## Where Things Are

### engine/src/ — deterministic simulation

| File | Role |
|------|------|
| `abi.hpp` | **Single source of truth for all bot/engine wire structs.** Locked with `offsetof`/`sizeof` static_asserts. Do not define ABI structs anywhere else. |
| `config.hpp` | Every balance constant and hard cap (unit stats, costs, yields, territory, budgets). No magic numbers anywhere else. |
| `ids.hpp` | Strong typed `UnitId`, `StructureId`, `FactionId`, and `EntityId` (a command's issuer). Units and structures share one id sequence (`World::next_entity_id`): ids never collide. Never raw `uint32_t` for ids. |
| `rng.hpp/.cpp` | xoshiro256** PRNG, hand-rolled (`std::uniform_int_distribution` is not portable). `bot_seed(match seed, faction)`: each bot's init seed; the engine never draws from it. |
| `fixed.hpp/.cpp` | Q32.32 fixed-point. `isqrt()` seeds from `std::sqrt` (unused by game logic: make it integer-only before relying on it). |
| `grid.hpp/.cpp` | `Vec2`, `Terrain`, 4-connected grid helpers (`manhattan()`, `is_passable()`). |
| `stats.hpp/.cpp` | `UnitStats` and `stats_for(UnitType)`, built from `config.hpp`. |
| `entity.hpp` | `UnitType`, `StructureType`, `OrderType` enums; core structs. |
| `world.hpp/.cpp` | Authoritative game state. `std::map` for units/structures (never unordered). |
| `vision.hpp` | `sees_tile()` (the one sight rule) and `FactionSight` (what a faction sees now, for targeting; memoised per tile). Use `structure_sight()` / `sight_radius()`, never the constants directly. |
| `occupancy.hpp/.cpp` | Flat per-tile grids of who stands where, rebuilt each movement step and in combat (again after the first-strike purge: it holds pointers). Range queries visit only tiles in range, nearest ring first, ties to the lower id: exactly what an id-ordered scan gives, which they fall back to if a hand-built world stacks units. |
| `snapshot.hpp/.cpp` | Fog-masked snapshot builder, via `sees_tile()`. |
| `command.hpp/.cpp` | `ValidatedCommands` and `validate_commands()`. |
| `movement.hpp/.cpp` | Stepped simultaneous movement: chains advance, swaps and closed loops are blocked whole. |
| `combat.hpp/.cpp` | First strike (Frigates, simultaneous among themselves), then the simultaneous sub-phase (splash with friendly fire, finished claims fire), shields, regen. Only seen targets. |
| `economy.hpp/.cpp` | Harvest into Drone cargo, deposit, spend, production, construction; Drone, Factory and unit caps (`cfg::unit_cap(w, h)`). |
| `territory.hpp/.cpp` | `territory_owners()` (the one definition of territory), `territory_pct()`, Claim Node fragility, `win_threshold()`. |
| `wincheck.hpp/.cpp` | Fixed-order checks: base destruction → territory threshold → tick cap; `fault_result()`, `result_reason()`. |
| `statehash.hpp/.cpp` | Rolling xxh3 via xxhash (`XXH_INLINE_ALL`). |
| `tick.hpp/.cpp` | Phase pipeline. `run_tick_phases()` (raw commands) and `run_tick()` (calls bots). |
| `replay.hpp/.cpp` | Input-log model. `replay()` re-simulates; `replay_frames()` builds `FrameState`s. |
| `replay_io.hpp/.cpp` | `write_replay`/`read_replay` (+ `_file` variants). |
| `bot_iface.hpp` | `game::Bot`, implemented by native fixtures and `WasmBot`. |
| `frame.hpp/.cpp` | `FrameState` (renderable snapshot). |
| `match.hpp/.cpp` | `run_match()`: drives the tick loop, records the replay log. |
| `file_io.cpp` | Raw + Zstd file I/O. Zstd is `#ifdef`'d out under `__EMSCRIPTEN__`. |

### fixtures/ — sample bots (native and `.wasm` from one source)

`IdleBot` (empty commands; determinism tests), `RushBot` (Interceptor waves; never defends),
`EconBot` (fast claims, capped defensive army; never attacks), `TurtleBot` (one home claim,
guards its base), `ExpanderBot` (claim lattice; never attacks), and `AdaptiveBot`, the combat
fixture that plays every system (counter-picks, scouting, nebula ambush, waves when ahead).
Its knobs are an `AdaptiveStyle`, jittered per match from the init seed (`jitter_pct`, 20%);
`AggroBot` and `BoomBot` are the same bot with other styles. Ships fire only on a tick they are
ordered to Move or Attack, so it orders every ship every tick.

Every fixture also builds as `.wasm` (`fixtures/CMakeLists.txt`, `sdk/cpp/include/ud/bot_host.hpp`)
into `build/fixtures_wasm/`; `FixtureWasm.*` requires each to play exactly as natively, with no
forfeits. Fixtures must therefore use only what `game::Bot` receives, and fit the fuel budget.
More on each in `fixtures/README.md`.

The matchup tests in `map_tests` are the balance guards: `Turtle.*` (the turtle loses to the
descending threshold on every map), `Rush.*` (the rush kills both bases), `Econ.*` (the boom
out-expands and holds off the rush), `Adaptive.*` (it beats both, and two of them fight). With
`Arena.*` and `ud_arena`, re-run them after any balance change.

`bot_util.hpp`: the engine never re-routes a blocked unit, so bots route themselves. `Router`
(own units block only the first step, lower id has right of way, enemy structures remembered
once seen; goal-sharing distance fields kept across ticks, flooding capped per tick by
`kFloodBudget`, which keeps AdaptiveBot near 2–3M fuel per tick on 80×80), `waypoint()`,
`harvest_step()`, `builders()`, `ClaimLattice`.

### runner/ — Wasmtime bot host

`WasmBot` implements `game::Bot`: loads `.wasm` (`load_wasm_file()`), checks the ABI version and
the buffer addresses, meters fuel, caps memory. `healthy()` must be true before use.

- **Isolation (DESIGN §2.1a):** the linker is empty (no WASI or any host function), so any
  import fails the load. One 32-bit memory and one table (`cfg::TABLE_CAP_ELEMENTS`) per store
  (`wasmtime_store_limiter`); multi-memory, memory64, GC, function references, exceptions and
  stack switching are off. `Sandbox.*` tests try each way out: keep them passing, and keep new
  Wasm features off unless a bot needs them.
- **Forfeits (DESIGN §2.4):** out of fuel, a trap, or a grow past `cfg::MEMORY_CAP_MB` loses the
  match (both in one tick: a Draw). The faulting tick is recorded, not played: `fault_result()`
  decides it in `run_match`, `replay()` and `replay_frames()` alike. A faulted `WasmBot` is never
  called again. The cap is a host memory creator in `wasm_bot.cpp` (the only C API hook that
  sees grow requests). The cause is `BotMetrics::fault`, stored in the last `TickEntry` and the
  last frame's `FrameState::forfeits`; everything reports it through `result_reason()`.
- **Bulk memory is disabled:** `memory.copy`/`memory.fill` cost 1 fuel whatever their length.
  The SDK builds with `-mno-bulk-memory` and links `sdk/cpp/runtime.cpp`. Open decision: README.md.
- Copy-on-write memory init is off: with the capped creator, Wasmtime panics on modules with
  sizeable data segments (any bot linking libc).

### tools/ — CLI utilities

| Binary | Purpose |
|--------|---------|
| `ud_run` | One 1v1 match: `ud_run <a.wasm> <b.wasm> [--map f] [--out f] [--seed N] [--ticks N] [--json]`. `--json` is for the harness: result, per-side faults and stats, from replaying the frames (which also checks the replay reproduces the hash). |
| `ud_mapgen` / `ud_mapcheck` | Generate / validate a map JSON. |
| `gen_example_replay` | A fixture replay for tests. |
| `ud_arena` | Balance arena: equal-value armies (pure and every mix) fight on an open field; prints margins and rankings. Logic in `arena.hpp/.cpp`, tested by `Arena.*` (mirror fights must stay exactly even). See `tools/README.md`. |

Standard match setup (Core + starting units near each spawn) is `maps::world_from_map()`
(`maps/match_setup.hpp`), shared by `ud_run` and the map tests. Maps live in `maps/data/`.

### profiler/ — deterministic fuel/memory profiler (DESIGN §10)

`ud_profile <bot.wasm> [--scenario NAME]... [--ticks] [--list] [--json]`. Each scenario is a real
match through `game::run_match` with a `runner::WasmBot`, so reported fuel equals the match meter
exactly (`Profiler.FuelAndMemoryMatchARealMatchExactly`: never fork the metering path). The
opponent is passive and walled off, so scenarios run their full length. Exit status 2 on any
forfeit.

### viz/ — replay renderers

`ud_viz` (terminal), `ud_viz_nc` (ncurses, optional), `ud_viz_imgui` (Dear ImGui, isometric,
optional; Z toggles claim zones via `territory_owners()`), `ud_shape_editor` (edits `.udshapes`).
`Makefile.em` builds `ud_viz_imgui` for the browser → `viz/web/`; the tournament site's watch page
loads a replay by writing `/replay.ud` to the module's FS and calling `_load_replay_from_fs()`.
Both ImGui apps default to the repo's `shapes.udshapes` (`--shapes <file>` overrides;
`ud_viz_imgui` hot-reloads it); the browser build embeds it, so rebuild it after editing shapes.
Dear ImGui v1.91.6 comes from FetchContent: configure the CMake build once before `Makefile.em`.

### sdk/cpp/ — C++ bot SDK

ABI headers in `sdk/cpp/include/`. `bot_template.cpp` is the raw ABI (`-nostdlib`); a bot using
`game::Bot` and the standard library uses `ud/bot_host.hpp` (`UD_WASM_BOT(Class)`) and
`wasi_stubs.cpp` (traps for the WASI calls wasi-libc links), built with wasi-sdk. Examples:
`idle_bot`, `rush_bot`, and profiler demonstrations `quadratic_bot`, `leaky_bot`, `copy_bot`,
built by `sdk/cpp/Makefile` next to their sources (ignored by git).

---

## Determinism Invariants — Never Violate

These silently break replay if violated. They are non-negotiable.

1. **Integer math only** in game state. No `float`/`double` touches game logic. `fixed.hpp`
   (Q32.32) is the only sanctioned exception.
2. **Single seeded PRNG**, drawn in a fixed order: ascending unit-id order within combat. No
   other entropy source.
3. **Ordered containers only.** `std::map`, never `std::unordered_map`, in any logic path.
4. **All tie-breaks resolve to unit-id or structure-id**, assigned in deterministic creation order.
5. **Fixed phase order every tick**: validate → move → combat → economy → spawn → territory → wincheck.
6. **Bots decide against the frozen start-of-tick snapshot.** No bot sees mid-tick state.
7. **`World::rng_seed` must be set alongside `World::rng`.** Forgetting it breaks
   `Determinism.ReplayReproducesHash`.

Compile flags enforcing these: `-ffp-contract=off -fno-fast-math` (`cmake/CompilerFlags.cmake`).

---

## Contracts and Formats

- **ABI structs** live only in `abi.hpp`: fixed-width integers, no `std::` types, naturally
  aligned (no `packed`), `sizeof`/`offsetof` static_asserts on every struct. Adding or changing
  a field → update the asserts and bump `cfg::ABI_VERSION` (now 17), and add a line to DESIGN
  §4.4. A bump invalidates every compiled bot and every replay file.
- **Replay files** are versioned by their magic (`UDREPLY2`), separately from the ABI. Change the
  layout → bump the magic and refuse the old one with a clear message (`replay_io.cpp`).
- **Append only:** `TickFault` and `WinReason` values (written to replays), and the keys of
  `ud_run --json` and `ud_profile --json` (the tournament harness stores them).
- **Caps** bound the snapshot buffer: `UNIT_HARD_CAP` (the unit cap on the largest map) and
  `STRUCTURE_HARD_CAP`; the runner clamps to them when copying in. Budgets are sized once for
  the largest map (DESIGN §2.3): re-measure with `ud_profile` if caps or fixtures change.

---

## Coding Conventions

- **No comments explaining what code does.** Only comment the non-obvious *why*.
- **No magic numbers.** Every tunable goes in `config.hpp`.
- **No bare `int` or `size_t` in state structs.** Use fixed-width types.
- **`std::map` for anything iterated in game logic.**
- C++23 (`set(CMAKE_CXX_STANDARD 23)`; googletest is built at C++20). Headers using `operator<=>`
  must `#include <compare>`: libc++ (Emscripten) doesn't pull it in transitively.
- clang-format configured at root: 2-space indent, 100 column limit.

---

## Decided — Don't Undo

Each was argued out; the reasons are in DESIGN. Change one only when asked.

- **Draws are allowed** (§7): both Cores die the same tick, both bots fault the same tick, or the
  tick cap with territory within `DRAW_TERRITORY_MARGIN`. Don't "fix" them away.
- **A fault forfeits the match** (§2.4), not just the tick. Rolling the bot back was built and
  measured, then dropped: a full memory copy per tick, ~57 µs per MB.
- **No attack-move halt** (§4.3): every Move steps its full speed and then fires; where to stop is
  the bot's call. `MoveAttack` and `Gather` kinds are reserved and rejected.
- **Line of sight** (§3): Manhattan, no rays; nebulae hide what's inside beyond `NEBULA_SIGHT`;
  asteroids don't block sight. Claims and Factories see less than ships fire: deliberate, don't
  raise it. The Core sees `CORE_SIGHT`, so nothing shoots it unseen.
- **No blind fire** (§3): only what the side sees can be targeted; Artillery needs a spotter.
- **First strike is simultaneous among Frigates** (§5 Phase 3).
- **Shields** (§5 Phase 3): hardened on Interceptors, pierced by Artillery, none on Drones or
  structures.
- **Construction** (§5 Phase 4): Factories and claims need `BUILD_TICKS` of nearby Drone work;
  unfinished ones do nothing but count toward the Factory cap.
- **Claims shoot, very weakly** (§6): beat one Interceptor, lose to two.
- **Economy** (§5 Phase 4, §6): Drones carry and bank cargo; nodes never deplete; no upkeep
  (revisit only if play shows hoarding or spam).
- **Terrain is public** (§4.2): the whole map before `init`; fog hides units and structures only.
- **Blocked movement** (§5 Phase 2): swaps and closed loops block whole; the engine does no
  path-finding.
- **Bot seeds** (§4.2): a per-bot seed at init, new each match, the same on a re-run; not game
  state.

---

## Out of Scope — Stop and Ask

Cross-architecture replay, terrain-respecting territory/vision, procedural maps, 8-connectivity,
zero-copy ABI, a 3rd resource type. OS-level isolation of matches is the tournament harness's job
