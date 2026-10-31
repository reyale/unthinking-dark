# IMPLEMENTATION_PLAN.md — Unthinking Depths

Companion to `DESIGN.md`. This is the build plan for Claude Code. Read `DESIGN.md` first;
this document specifies **what to build, in what order, with what technical contracts, and
how to verify each piece**. Section refs like §5 point into `DESIGN.md`.

---

## 0. Confirmed Technical Decisions (the spec this plan assumes)

**Engine**
- Language: **C++23**, compiler **GCC**. Target **x86-64 / Linux only** for v1
  (cross-architecture reproducibility is explicitly deferred — note it as an assumption so
  adding ARM later is a scoped effort, not a surprise).
- **Game-state math is pure integer first.** Fixed-point (**Q32.32 in `int64_t`**) is a
  *sanctioned fallback* only where a ratio is genuinely unavoidable (e.g. a Euclidean
  tie-break). **No `float`/`double` ever touches game state.**
- Build system: **CMake**.

**Determinism (MUST — each silently breaks replay if violated)**
- Single seeded PRNG, fixed draw order (combat draws in ascending unit-id order).
- No floats in game state; ordered containers only (`std::map`/`std::vector`+sort, never
  `std::unordered_map` iteration in logic paths).
- Compile flags: `-ffp-contract=off`, **no** `-ffast-math`, `-fno-fast-math`. Treat signed
  overflow/UB as determinism risks: run **UBSan + ASan** builds in CI.
- Fixed-width integer types (`<cstdint>`) everywhere in serialized/game state; no bare
  `int`/`size_t` in state structs.
- Fixed phase order every tick; all tie-breaks resolve to unit-id / structure-id (assigned
  in deterministic creation order).
- Bots decide against the frozen start-of-tick snapshot only.
- Pinned runtime config (Wasmtime version, fuel costs, determinism flags) is part of match
  identity and lives in the replay header.

**WASM host**
- **Wasmtime C API**, embedded in the engine process.
- Fuel metering = the gas limit. Per-instance memory cap. Determinism flags: threads OFF,
  SIMD OFF, NaN canonicalization ON.
- **One OS process per match, two bot instances in it.** Isolation is the **WASM sandbox**
  + a **minimal host-function surface** (ideally just snapshot-in / commands-out). OS-level
  isolation (seccomp/namespaces/container around the match process) is a **deployment**
  concern, deferred — does not affect engine code.

**Bot ABI (the cross-language contract — see §3 for layouts)**
- Flat C structs (`#[repr(C)]`-compatible, fixed layout), little-endian x86-64.
- **Engine → bot:** full fog-masked snapshot written into bot linear memory each tick
  (inspection is free; no fuel surcharge). **Bot → engine:** flat command buffer.
- **Copy** at the boundary for v1 (no zero-copy tricks yet).

**Replay**
- **Input-log model:** replay = `(map_id, seed, config_hash, per-tick command buffers for
  both bots)`. Playback **re-simulates** deterministically. Replays are **tied to engine
  version** (header pins engine + config hash; mismatch → cannot play, must re-sim with the
  right build). The viewer runs the simulation.

**Maps**
- Human-editable text/JSON, **point-symmetric** for 1v1 fairness, compiled to an internal
  binary at load. No procedural generation in v1.

**Hard caps (v1, engine-enforced — bound worst-case sizing)**
- Map ≤ **128×128**. Units ≤ **256 per side**. Ticks ≤ **4096 per match**.

**Determinism hash**
- Per-tick **rolling xxh3** digest over canonical serialized state → one final match
  fingerprint. Per-tick digests retained so a divergence is **binary-searchable to the exact
  tick**. (BLAKE3 is a drop-in swap only if tamper-evidence is later wanted.)

**Tests**
- **Unit tests** + **golden-replay tests** (checked-in input-log + expected final hash;
  CI asserts byte-identical reproduction).

**SDK**
- **C++ first** (bot-side compiles to `wasm32` via **clang/LLVM** — note engine is GCC,
  bot toolchain is clang; same language, different target). Other LLVM languages later.

**Open / stop-and-ask (do NOT improvise — `DESIGN.md` §13)**
- Attack-move halt precision (step-into-range-then-hold vs stop-one-short).

---

## 1. Repository Layout

```
/engine                      # pure deterministic sim — no Wasmtime, no I/O in hot path
  /include/engine/*.hpp
  /src
    config.hpp/.cpp          # ALL §12 balance constants + the v1 caps; one place
    fixed.hpp                # Q32.32 helpers (used only where integer won't do)
    rng.hpp/.cpp             # seeded PRNG; explicit ordered draw API
    grid.hpp/.cpp            # map, tiles, terrain, coords, Manhattan distance
    ids.hpp                  # strong id types (UnitId, StructureId) — creation order
    entity.hpp/.cpp          # units, structures, stat tables
    world.hpp/.cpp           # authoritative game state
    snapshot.hpp/.cpp        # fog-mask → flat WorldView structs (ABI types live here)
    command.hpp/.cpp         # flat Command structs + validation
    bot_iface.hpp            # abstract Bot interface (scripted + WASM both implement)
    movement.hpp/.cpp        # §5 Phase 2 (stepped, simultaneous)
    combat.hpp/.cpp          # §5 Phase 3 (first-strike then simultaneous)
    economy.hpp/.cpp         # §5 Phase 4
    territory.hpp/.cpp       # §7 Voronoi + threshold + claim fragility
    wincheck.hpp/.cpp        # §5 Phase 6 + tie-break ladder
    tick.hpp/.cpp            # the phase pipeline orchestrator
    statehash.hpp/.cpp       # canonical serialize + rolling xxh3
    replay.hpp/.cpp          # input-log write/read
    match.hpp/.cpp           # (map,seed,botA,botB,config) -> ReplayLog + Outcome
/runner                      # Wasmtime C API host; implements bot_iface for .wasm bots
/sdk/cpp                     # bot template, ABI headers (shared with engine), local tester
/profiler                    # scenario suite + fuel/memory reports (shares /runner)
/harness                     # tournament/ladder, stats aggregation, batch sim
/viz                         # replay renderer (re-sims via engine; minimal first)
/maps                        # hand-authored symmetric maps (text/JSON) + loader
/fixtures                    # scripted bots, golden input-logs + expected hashes
/third_party                 # wasmtime (C API), xxhash, json lib, etc.
CMakeLists.txt
```

Invariant: `/engine` depends on nothing downstream. `/runner`, `/viz`, `/harness`,
`/profiler` all depend on `/engine`. The **ABI struct headers in `snapshot.hpp` /
`command.hpp` are shared verbatim with `/sdk/cpp`** so engine and bots agree byte-for-byte.

---

## 2. The ABI (most detail-dense part — review closely)

The byte contract between engine and arbitrary-LLVM-language bots. All structs are
fixed-layout, little-endian, naturally aligned, fixed-width integers. Q32.32 only if needed.

### 2.1 Memory model & handshake
- Each bot exports (WASM exports): linear `memory`, an `init(seed: u32) -> ()`, an
  `on_tick() -> u32` (returns command count written), and two fixed buffer addresses it
  exports as globals: `SNAPSHOT_ADDR` (where engine writes the snapshot) and
  `COMMAND_ADDR` (where bot writes commands). Bot reserves these buffers statically.
- Buffer sizes are **capped and known** from §0 caps, so the engine can validate the bot's
  reserved region is large enough at init and reject otherwise (init failure, not a forfeit).
- Per tick: engine `memcpy`s the serialized snapshot into `SNAPSHOT_ADDR`, refuels, calls
  `on_tick`, reads the returned count, `memcpy`s that many `Command` structs out of
  `COMMAND_ADDR`, then validates (§5 Phase 1).
- **Copy in / copy out** for v1. (Zero-copy linear-memory aliasing is a later optimization.)

### 2.2 Snapshot structs (engine → bot) — illustrative; finalize in code
```c
// All multi-byte fields little-endian. Sizes fixed. Counts precede arrays.
struct SnapshotHeader {
  uint32_t tick;
  uint32_t my_faction_id;
  int32_t  energy;
  int32_t  alloy;
  uint32_t territory_pct;      // 0..100 integer
  uint32_t win_threshold;      // 0..100 integer, current tick's threshold
  uint32_t map_w, map_h;
  uint32_t my_unit_count;
  uint32_t visible_enemy_count;
  uint32_t visible_tile_count;
  // offsets (in bytes from SNAPSHOT_ADDR) to each array, for forward-compat
  uint32_t my_units_off, enemies_off, tiles_off;
  uint32_t my_structure_count, my_structures_off; // appended in ABI 8
  uint32_t visible_enemy_structure_count, enemy_structures_off; // appended in ABI 9
};
struct StructureView {       // my structures: all of them, regardless of fog (ABI 8)
  uint32_t id; uint16_t type;
  uint16_t producing;        // UnitType in the Factory queue, or PRODUCING_NONE (0xFFFF)
  int32_t x, y; int32_t hp;
  uint32_t production_ticks_left;
  uint32_t build_ticks_left; // Claim Node under construction; 0 = finished (ABI 12)
};
struct UnitView {            // my units: full info
  uint32_t id; uint16_t type; uint16_t _pad;
  int32_t x, y; int32_t hp;
  uint16_t current_order; uint16_t _pad2; // order enum (Move/MoveAttack/Attack/Idle/...)
  int32_t cargo_energy, cargo_alloy;      // Drones: carried, not yet deposited
};
struct EnemyView {           // visible enemies: observable info only
  uint32_t id; uint16_t type; uint16_t _pad;
  int32_t x, y; int32_t hp;
};
struct EnemyStructureView {  // enemy structures on fog-visible tiles only (ABI 9)
  uint32_t id; uint16_t type; uint16_t building; // 1 = Claim Node under construction (ABI 12)
  int32_t x, y; int32_t hp;
};
struct TileView {            // only fog-visible tiles
  int32_t x, y;
  uint16_t terrain;          // enum: open/asteroid/nebula/resource_node
  uint16_t occupant_faction; // 0=none,1=me,2=enemy (occupancy only if visible)
  int32_t  resource_amount;  // if resource node
};
```

### 2.3 Command structs (bot → engine)
```c
struct Command {
  uint32_t entity_id; // issuer: a unit, or the Core/Factory for Produce (ABI 10)
  uint16_t kind;   // enum: Move, MoveAttack, Attack, Gather (reserved, rejected), Build, DeployClaim, Produce
  uint16_t arg_type;
  int32_t  ax, ay; // target tile (Move/MoveAttack/Attack-at / Build / DeployClaim)
  uint32_t target_id; // Attack target (unit or structure id)
  uint16_t aux;    // unit_type for Produce/Build; reserved otherwise
  uint16_t _pad;
};
```
- Bot writes an array of `Command` at `COMMAND_ADDR`, returns the count from `on_tick`.
- Count is clamped to a max (≤ units + structures); over-cap → truncate + log.
- Validation rules: §5 Phase 1. Duplicate command for one unit → **first listed wins**.

### 2.4 ABI versioning
- A `ABI_VERSION` constant in the shared header, embedded in the match/config hash. Bots
  compiled against a different ABI version are rejected at load (consistent with
  replay-tied-to-engine-version).

**Acceptance for the ABI:** a C++ bot and a second bot in a *different* LLVM language
(e.g. Rust or AssemblyScript, even a stub) both read the same snapshot bytes and produce
valid commands — proving the contract is language-neutral.

---

## 3. Phase Plan

Each phase: **Goal · Deliverables · Acceptance · Stop-and-ask.** Do not advance until
acceptance passes. The determinism-hash test, once it exists, is part of every later
phase's acceptance implicitly.

### Phase 1 — Deterministic Core Engine (no WASM)
**Goal:** complete deterministic match via scripted in-process bots. Foundation; spend care.

**Deliverables**
- `config`, `grid`, `ids`, `entity`, `world`, `rng`, `fixed`.
- `bot_iface` abstract interface; scripted test bots implement it directly.
- `snapshot` (fog-mask → ABI structs §2.2) and `command` (§2.3) + validation.
- Full §5 pipeline with real phase boundaries: Phase 0 snapshot+decide → 1 validate →
  2 stepped movement → 3 combat (first-strike then simultaneous, RNG ascending unit-id) →
  4 economy → 5 spawn/upkeep → 6 territory+wincheck (territory may be stubbed until Phase 3,
  but the phase must exist and be ordered).
- `statehash`: canonical serialize (fixed field order: tick, then units by id, structures
  by id, resources, rng counter) + **rolling xxh3**; retain per-tick digests.
- `match` runner + `replay` write (input-log).

**Acceptance**
- **Determinism keystone:** same `(map,seed,botA,botB,config)` × N runs → identical final
  xxh3. Wired into CI now; never flakes.
- **Cross-process determinism:** same inputs in separate processes → same hash.
- **Divergence localization works:** a deliberately-nondeterministic test bot makes the
  per-tick digests diverge at a detectable tick (prove the binary-search tooling).
- UBSan + ASan builds pass on the test matches.
- Fog test: bot's snapshot never contains an entity outside its vision.
- Combat: first-strike kills before simultaneous; mutual kills both die; splash hits
  friendlies. Movement: tile contest → tonnage then id; chains advance, swaps and loops blocked; MoveAttack halts on
  the step a target enters range.

**Stop-and-ask**
- **Line-of-sight** (needed in `snapshot` fog-mask and MoveAttack halt) — surface, don't guess.

### Phase 2 — Replay Playback + Minimal Visualizer
**Goal:** watch matches (you can't tune what you can't see). Input-log model.

**Deliverables**
- Replay header (map_id, seed, engine+config hash, ABI version) + per-tick command buffers.
- **Re-simulating player:** feed the input-log back through `/engine`, emit per-tick render
  state. Prove re-sim reproduces the original final hash.
- Minimal renderer (ASCII/2D ok): scrub, pause, perspective toggle (faction-fog vs god-view),
  visible **stall/forfeit** state.

**Acceptance**
- Scripted match plays back start to finish from the input-log alone.
- Re-sim hash == original match hash (this is what makes input-log replay safe).
- Header mismatch (wrong engine/config) → refuses to play, with a clear message.

**Stop-and-ask** — none expected.

### Phase 3 — Win Condition & Territory (§7)
**Goal:** decisive endings: base-destruction OR Voronoi-territory ≥ descending threshold.
Draws are allowed (double base death; near-equal territory at the tick cap, DESIGN §7).

**Deliverables**
- `territory`: straight-line Voronoi by nearest living Claim Node, max influence distance,
  ties→neutral, integer tile count → `territory_pct`. (If a Euclidean tie-break is needed,
  this is the sanctioned Q32.32 spot — otherwise stay integer/Manhattan.)
- Game clock `g = tick / tick_cap` driving **both** threshold curve (`78 − 27·g²` placeholder)
  and claim fragility (`eff_hp = base·(1−g) + floor`).
- `wincheck`: §5 Phase 6 fixed-order checks + full tie-break ladder (§7.4).

**Acceptance**
- Killing a claim re-partitions its region deterministically (assert tile-ownership flips).
- Turtle bot reliably loses to the descending threshold (anti-turtle works in sim).
- Double-base-death same tick → draw. The ladder (only reachable if both sides meet the
  threshold together) always names a winner.
- Determinism hash still green with territory active.

**Stop-and-ask**
- If straight-line Voronoi through packed asteroids renders visibly wrong, surface the
  terrain-respecting-territory question (§13) rather than switching models silently.

### Phase 4 — WASM Bot Runner (Wasmtime C API)
**Goal:** untrusted `wasm32` bots, fuel-metered, sandboxed, deterministic. Thin adapter
implementing `bot_iface`.

**Deliverables**
- Wasmtime C API embed: instance per bot, persistent across ticks, refuel each tick.
- Determinism flags (threads off, SIMD off, NaN canon on); pinned config recorded.
- ABI handshake (§2.1): validate exports + buffer sizes at init; snapshot memcpy-in,
  command memcpy-out.
- Budgets: fat one-time `init` budget vs lean `on_tick` budget; init overrun → load failure
  (cannot play), distinct from a tick forfeit.
- Unified failure (§2.4): fuel exhaustion / OOM / trap → empty commands this tick →
  repeat-last/idle, logged with cause `{fuel,memory,trap}`.

**Acceptance**
- A trivial real `wasm32` bot plays a full match through the runner.
- **Determinism across runs (and across x86-64 machines)** with pinned config: same fuel
  counts, same final hash. (Determinism keystone extended to the WASM path.)
- Fuel-exhaustion, OOM, trap each → logged tick-forfeit, match continues (no hard loss).
- Init-budget overrun rejected at load, distinct from tick forfeit.
- Sandbox: a hostile bot cannot reach host state or exceed the memory cap.

**Stop-and-ask**
- Any determinism-relevant Wasmtime config option ambiguous/unavailable in the pinned
  version — config is match identity; do not paper over.

### Phase 5 — C++ Bot SDK + Deterministic Profiler
**Goal:** authors write a bot in C++, compile to `wasm32` (clang), test locally, get exact
pre-match fuel/memory reports.

**Deliverables**
- `/sdk/cpp`: project template, the **shared ABI headers** (same `snapshot.hpp`/`command.hpp`
  the engine uses), `init`/`on_tick` scaffolding, clang→wasm32 build instructions, a sample bot.
- Local tester: run bot vs a sample opponent on chosen map+seed → viewable replay.
- **Profiler (§10):** Tier 1 (fuel/tick via Wasmtime counter), Tier 2 scenario suite
  (`army_scaling` 10/50/200/max, `dense_battle`, `wide_visibility`, `max_legal`,
  `early_game`, + long-running memory-leak scenario) → per-scenario fuel + peak memory,
  worst/peak bolded, vs budget.
- **Trust requirement (MUST):** profiler runs the **identical pinned config** as ranked
  matches; reported fuel == match-meter fuel exactly. Shared via git for local use.

**Acceptance**
- Profiler fuel for a scenario == fuel the same input burns in a real match (exact).
- A deliberately O(n²) sample bot shows the army-scaling worst-case spike.
- A deliberately-leaking sample bot shows rising peak memory in the long scenario.
- Tier 3 hotspot attribution explicitly deferred.

**Stop-and-ask**
- Any profiler/match meter disagreement → core author promise broken; find the divergence.

### Phase 6 — Tournament Harness + Stats (§8)
**Goal:** run many matches; aggregate results + balance telemetry.

**Deliverables**
- Batch runner (round-robin and/or ladder/ELO), parallel **across** matches (each match
  stays single-threaded-deterministic).
- Stats: forfeited ticks w/ cause, win path (base/territory/tie-break), duration, per-faction
  peak units/kills/losses/resources/peak territory/peak memory/peak fuel.
- Persist replays + match records (each embeds map, seed, bot hashes, pinned config).

**Acceptance**
- Tournament of several sample bots → ranking + stats, end to end.
- Forfeit-rate-by-army-size queryable (the fuel-budget balance signal).
- Re-simulating any stored match record reproduces its outcome exactly.

**Stop-and-ask** — none expected (orchestration over a frozen engine).

### Phase 7 — Calibration Pass
**Goal:** turn placeholders into a game that plays well. Iterative, data-driven.

**Deliverables**
- Sim battery across sample bots/maps collecting §8 stats.
- Tune §12 constants toward: decisive endings (base or threshold), mid-game contest + late
  climax, **rare draws, turtles lose**, no dominant unit. Keep values as named
  constants; document reasoning.

**Acceptance (targets, expect iteration)**
- Draw rate low (draws are allowed, not a defect); turtles reliably lose; counter-triangle holds; durations cluster below cap.

**Stop-and-ask**
- If no parameter set hits targets, the issue may be structural (mechanic, not number) —
  surface with supporting stats.

---

## 4. Cross-Cutting (every phase)
- Determinism hash test stays green; extend it to each new path. A flake is a build-breaker.
- No floats in game state; ordered containers in logic; one seeded PRNG; `-ffp-contract=off`,
  no fast-math; UBSan/ASan in CI; fixed-width ints in state.
- All tunables in `config`. No inline magic numbers.
- `/engine` stays dependency-free of downstream modules. Shared ABI headers stay byte-identical
  between engine and SDK.
- Deferred items (§13) surfaced, never silently implemented.

## 5. Definition of Done (v1)
- Two real `wasm32` bots play a full fog-of-war deterministic match that reproduces
  byte-identically across runs and x86-64 machines under pinned config; plays back from its
  input-log via re-simulation; ends decisively or in a rule-defined draw; tracks
  forfeits + §8 stats; re-simulates from its stored record.
- An author clones `/sdk/cpp`, writes a bot, compiles to wasm32 (clang), runs it locally,
  and gets an exact fuel/memory profile matching ranked metering.
- A tournament over several bots yields a ranking + balance telemetry.

## 6. Explicit Non-Goals (v1 — do not build; stop-and-ask if seemingly needed)
Cross-architecture replay; terrain-respecting territory/vision; structural wall skins,
gravity wells, hazards, jump lanes; claim placement gating; scratch memory region; 3rd
resource; extra unit types; claim tiers; procedural maps; 8-connectivity; Tier-3 profiler;
zero-copy ABI; OS-level match sandboxing (deployment concern, not engine).
