# Unthinking Depths

A deterministic, server-executed strategy game where programmers submit bots that command autonomous space fleets in 1v1 head-to-head matches.

Each bot compiles to WASM and receives a fog-masked snapshot of the battlefield each tick, returning commands for its entire fleet. Matches run centrally, are fully replayable, and are designed to be watchable.

## How it works

- Two bots control fleets on a shared grid
- Each tick, bots get a fog-of-war snapshot and return commands — one call for the whole fleet ("army-brain" control)
- The simulation is fully deterministic: the same map, seed, and bot binaries always produce the same match
- Matches produce a replay log; a renderer plays it back with scrub, pause, and perspective toggle

## Unit roster

| Unit | Role | Special |
|------|------|---------|
| Drone | Economy | Harvests resources, builds structures |
| Interceptor | Brawler | Fast, high collision priority |
| Frigate | Ranged | First-strike; fires before the simultaneous phase |
| Artillery | Siege | Radius splash with friendly fire; cracks Claim Nodes |

Counter-triangle: Interceptor > Frigate > Artillery > Interceptor.

## Win conditions

1. **Base destruction** — destroy the enemy Command Core
2. **Territory control** — hold ≥ a descending threshold of Voronoi territory (starts ~78%, falls to ~51% at the tick cap), anchored by Claim Nodes
3. **Tick cap** — if neither side wins by the cap, the side with more territory wins; if the difference is within 5 percentage points, the match is a **draw**

The threshold falls over time so turtling loses. **Draws are allowed**: both Command Cores destroyed on the same tick, or two sides reaching the tick cap at near-equal territory.

## Bot API

```cpp
void on_init(MapInfo);                        // one-time setup, generous fuel budget
std::vector<Command> on_tick(WorldView);      // per tick, metered fuel budget
```

Bots compile to `wasm32` via any LLVM toolchain (Rust, C++, Zig, AssemblyScript, ...). Fuel is deterministic — authors get exact, reproducible gas/memory reports before submitting.

Bots must be built **without the WebAssembly bulk-memory feature** (`memory.copy`, `memory.fill`, ...);
the runner refuses modules that use it. The C++ SDK handles this (`-mno-bulk-memory` plus its own
`memcpy`/`memset`/`memmove`). Other toolchains need the equivalent flag, and recent ones enable
bulk memory by default.

## Open decisions to review

- **Bulk memory is disabled for bots.** Wasmtime charges `memory.fill`/`memory.copy` one unit of
  fuel whatever their length, so a bot could burn ~0.6 s of CPU per tick for a few thousand fuel
  (measured: 200 × 32 MiB fills = 2,601 fuel). Rejecting the instructions closes that hole but
  costs authors convenience: every toolchain must be told not to emit them, and bulk memory is on
  by default in clang 19+, recent Rust, and Zig. Revisit if that friction proves real, e.g. by
  rewriting bot modules at load to charge bulk instructions per byte, or if Wasmtime gains
  size-proportional fuel for them. A wall-clock limit is not an alternative: it would make
  forfeits machine-dependent and break determinism. See `runner/README.md`.

## Repository layout

```
engine/       Pure deterministic simulation — no I/O, no WASM in the hot path
runner/       Wasmtime host; wraps the engine for untrusted .wasm bots
sdk/cpp/      Bot template, ABI headers, local tester
viz/          Replay renderers (terminal, ncurses, Dear ImGui, browser/WASM)
maps/         Hand-authored point-symmetric maps
docs/         Design doc and implementation plan
tests/        Unit and golden-replay tests
tools/        CLI utilities: ud_run (match runner), ud_mapgen, ud_mapcheck
```

## Building

Requires GCC 14+, CMake 3.16+, libglfw3-dev (optional, for the ImGui visualizer).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

## Running a match

```bash
# Run two WASM bots and write a replay
./build/tools/ud_run bots/my_bot.wasm bots/opponent.wasm --out match.ud

# Watch the replay (terminal)
./build/viz/ud_viz match.ud

# Watch the replay (Dear ImGui — isometric, requires GLFW)
./build/viz/ud_viz_imgui match.ud
```

## Browser replay viewer

Build a self-contained `web/index.html` that runs in any browser — no install required for the viewer:

```bash
# Prerequisites: emsdk activated, CMake build done once (fetches imgui/xxhash)
cd viz && make -f Makefile.em

# Serve locally and open http://localhost:8080
make -f Makefile.em serve
```

Drop any `.ud` replay file onto the page to load it. Same controls as the desktop viewer.

## Coverage

Requires `gcovr` (`pip install gcovr`).

```bash
cmake -B build-cov -DCMAKE_BUILD_TYPE=Coverage
cmake --build build-cov --target coverage
xdg-open build-cov/coverage/index.html
```

For editor LSP support:

```bash
ln -s build/compile_commands.json compile_commands.json
```

## Design and implementation docs

- [docs/DESIGN.md](docs/DESIGN.md) — full game rules, tick resolution, win conditions, unit roster, ABI contract
- [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md) — build phases, technical decisions, acceptance criteria
