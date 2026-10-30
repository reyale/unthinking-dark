# DESIGN.md — Untitled Unthinking Depths

A deterministic, server-executed strategy game where programmers submit bots that
command autonomous space fleets in 1v1 head-to-head matches. Matches are replayable
and built to be watchable.

This document is the implementation contract. Where it says **MUST**, the behavior is
determinism-critical and any deviation breaks replay reproducibility. Where it says
**(v1 placeholder)** or **(tune in sim)**, the value is expected to change during
calibration and should be a named constant, not a magic number.

---

## 1. Core Premise

- Two bots, each controlling a fleet, compete on a shared grid.
- Each bot is a program (compiled to WASM) that receives a fog-masked snapshot each
  tick and returns commands for its units. This is **army-brain** control: one call
  per tick returns commands for the whole fleet, not per-unit callbacks.
- The simulation is **deterministic given starting input** (map + seed + the two bot
  modules + pinned runtime config). The same inputs MUST always produce the same match.
- Matches run centrally on a server. The output is a replay log that a frontend plays
  back. Not intended to be human-playable in real time (many units, programmatic).

---

## 2. Runtime & Sandbox

### 2.1 Execution target
- Bots compile to `wasm32` via any LLVM-based toolchain (Rust, C/C++, Zig,
  AssemblyScript, TinyGo, ...).
- Bots run in **Wasmtime** with **fuel metering** enabled. Fuel is the gas limit.
- One **persistent Wasmtime instance per bot per match**. The instance keeps its linear
  memory across ticks (bots retain their own state/maps/plans). The engine never mutates
  bot memory out-of-band.

### 2.2 Determinism invariants (MUST)
These are non-negotiable; each one silently breaks replay if violated.
1. **Single seeded PRNG**, consumed in a fixed order (see combat phase). Seed stored in
   the match record. No wall-clock, no unseeded entropy anywhere in game logic.
2. **Integer / fixed-point math only** for anything affecting game state. No floats in
   game logic.
3. **Fixed phase order every tick**, no exceptions.
4. **All tie-breaks resolve to unit-id / structure-id**, which are themselves assigned
   deterministically (creation order). Never iterate hash-map order or pointer addresses.
5. **Bots decide against the frozen start-of-tick snapshot only.** No bot sees the other
   bot's commands or any mid-tick state.
6. **Pinned runtime config** stored in the match record: Wasmtime version, fuel costs,
   determinism flags (threads OFF, SIMD OFF, NaN canonicalization ON). A fuel-cost change
   across versions desyncs old replays — config is part of the match identity.

### 2.3 Budgets
- **Per-tick fuel budget** (lean): bounds per-tick reasoning. (v1 placeholder — tune in sim.)
- **One-time startup budget** (fat): a separate, generous budget for a setup/init call
  before tick 0, for precomputation and allocation. Overrunning startup = bot fails to
  initialize = upload/validation failure (it cannot play). Distinct from a per-tick forfeit.
- **Linear memory cap** per instance, fixed and identical for both bots, pinned in match
  config. (v1 placeholder: ~64 MB — tune.) Remember total server budget ≈ N_instances × cap.

### 2.4 Resource-violation handling (MUST be unified)
- Any in-match fuel exhaustion, OOM, panic, or trap → **the bot forfeits that tick**:
  its commands for the tick are treated as empty. Units fall back to **repeat-last-order
  / idle**. The match continues. No hard loss.
- Loss is always **emergent** from board state (a perpetually idle fleet gets
  out-maneuvered and loses), never a direct rules penalty for resource use.
- **Forfeited ticks are first-class statistics.** Per forfeited tick, record:
  `tick`, `faction_id`, `cause ∈ {fuel, memory, trap}`, `units_affected`. Aggregate per
  bot over time. Used for author feedback, spectator drama, and balance tuning.
- **Inspection of game state is free** (no fuel surcharge): the snapshot is bulk-copied
  into linear memory, so reading it is ordinary memory access. Fuel meters *reasoning*,
  not perception. Do NOT add allocation fuel surcharges either; loads/stores cost their
  normal tiny per-instruction fuel and nothing more.

---

## 3. World

- **Grid:** square, **4-connected (von Neumann)** movement and adjacency. Manhattan
  distance. (v1 size placeholder: ~80×80 — tune with win-condition pacing.)
- **Coordinates / ids:** integer. Unit and structure ids assigned in deterministic
  creation order.
- **Terrain (v1, minimal):**
  - **Asteroids** — impassable. Serve as both obstacles and (when packed) walls/corridors.
    Resource nodes are embedded in / adjacent to asteroid fields.
  - **Nebulae** — passable, **vision-blocking**. Amplify active-only fog; create ambush
    zones. (LoS interaction precise rules: see Deferred.)
- **Fog of war:** **active-only.** A faction sees a tile only if currently within a
  friendly unit's sight radius and not blocked by nebula. No terrain memory in v1 (going
  blind when you leave is intended). Engine computes visibility and masks the snapshot —
  cheating is structurally impossible because hidden state is never serialized to the bot.
- **Maps (v1):** assume a small set of hand-authored, **point-symmetric** maps for
  fairness (1v1, deterministic — an asymmetric map advantages one side). Map generation
  is out of scope for v1; load maps from a file format (see §9).

---

## 4. Bot API (the author contract)

### 4.1 Shape
```
// Author implements:
fn on_init(map_info) -> ()          // one-time, fat startup budget, allocate/precompute
fn on_tick(world: WorldView) -> Vec<Command>   // per tick, lean fuel budget
```

### 4.2 WorldView (fog-masked, free to read)
Contains only what fog permits:
- `tick`: current tick number
- `my_faction_id`
- `my_units`: full info — `{id, type, pos, hp, cooldowns, current_order, ...}`
- `visible_enemies`: `{id, type, pos, hp, ...}` for enemies currently in vision
- `visible_tiles`: terrain / resources / occupancy within vision
- `my_resources`: `{energy, alloy}`
- `territory_pct`: current Voronoi territory share for this faction
- `win_threshold`: current threshold value this tick (see §7)
- map bounds / static map info

Bot must track its own memory of things that leave vision (active-only fog: no
engine-provided last-seen positions in v1).

### 4.3 Commands
One validated command max per unit per tick. Command modes:
- `Move(unit_id, path/target)` — move only; ignore enemies.
- `MoveAttack(unit_id, path/target)` — advance, but **auto-halt** as soon as a valid
  enemy target is in range (then fire in combat phase).
- `Attack(unit_id, target?)` — attack in place / hold and fire; does not move.
- Economy/structure commands: `Gather(unit_id, node)`, `Build(unit_id, what, where)`,
  `DeployClaim(unit_id, where)`, `Produce(factory_id, unit_type)`.

Commands are **proposals**. The engine validates and resolves; invalid/contradictory
commands are rejected deterministically. A bot proposing two commands for one unit:
**first listed wins, rest ignored** (v1 rule).

---

## 5. Tick Resolution (the per-tick law — MUST follow order exactly)

### Phase 0 — Snapshot & Decide
- Engine builds each faction's fog-masked `WorldView` from frozen start-of-tick state.
- Both bots run `on_tick` **in parallel**, each metered by its own per-tick fuel budget.
- Fuel exhaustion / OOM / trap → that bot's command list = empty (forfeit, logged §2.4).
- Commands collected, **not yet applied**.

### Phase 1 — Validate
- Filter every command deterministically. Reject: command for dead/nonexistent/enemy
  unit; second command for a unit already commanded (first listed wins); illegal
  target/range/cost; illegal build placement.
- Output: at most one validated command per surviving unit. This is authoritative.

### Phase 2 — Movement (stepped, simultaneous, then combat)
Movement resolves **before** combat. Combat resolves against final positions, so a unit
may close distance and fire in the same tick.

Stepped movement, up to `N = max unit speed` micro-steps, all units in lockstep:
```
for step in 1..=N:
    for each unit still moving:
        if MoveAttack and a valid enemy target is now in range (+LoS):
            mark unit "halted" (will fire in combat phase); it takes no further steps
        else:
            propose next tile along the unit's path
    resolve all proposed tiles simultaneously:
        - tile contested by 2+ units → highest collision-priority (tonnage) wins;
          ties → lowest unit-id
        - swaps (A↔B exchange tiles) are blocked; both stay
        - blocked unit → stays put / retries (v1 placeholder; see Deferred)
    apply all valid steps
```
- The MoveAttack halt check MUST run at the same per-step granularity as movement, in
  lockstep, or first-strike/kiting interactions break.
- `Move` steps full N unless blocked. `Attack`-in-place units don't move.

### Phase 3 — Combat (against final positions)
- **Targeting rule (deterministic, part of contract):** command's target if valid and in
  range; else nearest enemy in range; else no attack.
- **First-strike sub-phase:** first-strike units (Frigate) compute & apply damage first.
  Iterate attackers in **ascending unit-id order**, drawing from the single seeded PRNG
  in that order. Targets reduced to ≤0 hp are destroyed **before** the simultaneous
  sub-phase (this is the whole point of first-strike).
- **Simultaneous sub-phase:** all non-first-strike damage (Interceptor, Artillery)
  computed against survivors, then applied together. Mutual kills both die. Splash
  (Artillery) resolves here: damage to every unit in radius, **friendly fire included**,
  all simultaneous.
- Apply deaths at end of phase.

### Phase 4 — Economy
- Harvest (Drones on/adjacent to resource nodes) credits `energy` / `alloy`.
- Spend resolves in deterministic id order: builds, production, claim deployment; costs
  deducted. Insufficient resources → action fails (logged).
- Build and DeployClaim need a free target tile at this point (after movement): passable,
  no structure, no living unit. Otherwise the action fails and nothing is deducted.

### Phase 5 — Spawn / Upkeep
- Newly produced ships appear on the first free tile adjacent to their Factory, checked
  in N, E, S, W order (free = passable, no structure, no living unit). If all four are
  taken, the finished ship waits in the Factory (its queue stays blocked) and placement
  is retried every tick; if nothing moves out of the way it is never produced. Factories
  resolve in ascending structure-id order.
- **Claim Node hp recomputed against the game-progress clock** (claims grow fragile over
  the match; see §7).
- Per-tick upkeep/decay if any.

### Phase 6 — Territory & Win Check
- Recompute Voronoi territory (§7), compute current threshold.
- **Win checks, in fixed order:**
  1. **Base destroyed?** A faction whose Command Core died this tick loses. Both died same
     tick → tie-break ladder.
  2. **Territory ≥ threshold?** Faction at/over current threshold wins. Both qualify
     (guard; impossible while threshold > 50%) → tie-break ladder.
  3. **Tick cap reached?** → tie-break ladder.
- No win → advance tick, loop to Phase 0.

---

## 6. Unit & Structure Roster (v1 placeholders — iterate fast)

All stats integer. Numbers are starting guesses for the simulator to tune.

| Unit | hp | dmg | range | sight | speed | special | counters |
|------|----|----|------|------|------|---------|----------|
| **Drone** (fabricator) | 20 | — | — | 4 | 1 | harvests, builds structures, deploys claims | n/a (soft economy unit) |
| **Interceptor** (brawler) | 60 | 12 | 1 | 5 | 2 | simultaneous dmg; **high collision priority**; cheap | strong vs Frigate, weak vs Artillery |
| **Frigate** (ranged) | 35 | 14 | 4 | 6 | 2 | **first-strike**; low collision priority | strong vs Artillery, weak vs Interceptor |
| **Artillery** (siege) | 30 | 10 | 3 | 5 | 1 | **radius-1 splash, friendly fire ON**; simultaneous dmg; slow; expensive; cracks claims | strong vs Interceptor (clumps), weak vs Frigate |

Counter-triangle: Interceptor > Frigate > Artillery > Interceptor. Composition and
positioning beat raw numbers.

| Structure | hp | role |
|-----------|----|------|
| **Command Core** (base) | 600 | destroyed = instant loss; produces Drones; stationary |
| **Factory** | 150 | produces Interceptor/Frigate/Artillery; stationary; built by Drone |
| **Claim Node** | scales high→~1 over match | Voronoi territory anchor; weak aura (deters lone scouts, ignorable by real forces); deployed by Drone via teleport (v1); cracked by firepower (esp. Artillery splash) |

**Resources (2 types, v1):**
- **Energy** — gates combat ships (spent at Factory).
- **Alloy** — gates structures (Factories, Claim Nodes).
- Drones harvest both from asteroid-embedded nodes. (A 3rd resource is a possible add if
  build orders prove too flat — defer.)

**Unit cap:** economy-gated soft cap + a hard ceiling (v1 placeholder ~200–300) purely to
bound fuel/memory worst-case and keep rendering legible.

---

## 7. Win Condition & Territory (the anti-draw spine)

Two win paths, both decisive; designed so draws are essentially impossible.

### 7.1 Base destruction
Destroy the enemy **Command Core** → instant win. The clean kill.

### 7.2 Territory ≥ descending threshold
- **Territory model: Voronoi by Claim Node.** Every passable tile is owned by the faction
  whose **nearest living Claim Node** is closest (straight-line distance, v1 — ignores
  terrain). Ties → neutral. A claim only owns tiles within a **max influence distance**
  (v1 placeholder: radius covering ~2–3% of map); tiles beyond any claim are neutral.
  Killing a claim re-partitions its region to the next-nearest claims (visible, dramatic
  territory swing).
- `territory_pct` = owned passable tiles / total passable tiles (integer count).
- **Descending threshold:** a single **game-progress clock** `g ∈ [0,1]` (`g = tick / tick_cap`)
  drives the threshold. (v1 placeholder curve: `threshold = 78 − 27·g²`, i.e. ~78% at
  g=0, convex descent to ~51% at g=1. Tune in sim.) Never exactly 100% (always
  theoretically winnable) and never exactly 50% (the endgame is always decisive).
- The **same `g` clock** drives **Claim Node fragility**: claims are durable early,
  ~instantly killable late (e.g. `effective_hp = base·(1−g) + floor`, floor ≈ "one unit
  cracks it"). This makes the endgame deliberately volatile and decisive.

### 7.3 Tick cap
(v1 placeholder ~3000 — the point where threshold reaches ~51%.) Reaching it → tie-break.

### 7.4 Tie-break ladder (draw-killer floor; near-never reached)
Evaluated in order until one side wins:
1. Higher territory %
2. Higher total Command Core hp remaining
3. More enemy units destroyed
4. More resources gathered (lifetime)
5. Fewer forfeited ticks
6. Lowest deterministic hash of `faction_id + seed` (theoretical coin-flip floor)

---

## 8. Statistics & Telemetry

Tracked per match, aggregated per bot over time. First-class (not afterthoughts):
- **Forfeited ticks** with cause breakdown (§2.4) — author feedback, spectator drama,
  balance signal (if strong bots forfeit at scale, fuel budget is too tight for the army
  sizes the game permits).
- Match outcome, win path (base vs territory vs tie-break), duration in ticks.
- Per-faction: peak units, units killed/lost, resources gathered, peak territory %,
  peak memory, peak per-tick fuel.

---

## 9. Replay & Rendering (watchability is a goal)

- Because all game state is integer grid state per tick, a match is a sequence of cheap
  frames. The replay log is the source of truth; the renderer just plays it back
  (scrub, pause, switch perspective: per-faction fog view or god-view).
- Each frame: tiles, unit positions/types/hp, structures/hp, fog overlay per perspective,
  Voronoi territory fill (two-color), and **events** (attacks, deaths, builds, claim
  deploys/cracks, **stalls/forfeits**).
- **Stalls/forfeits get a visible state** (greyed unit / stutter / "!"), turning a failure
  mode into spectator information and an author debugging signal.
- **Spectator HUD:** two-color territory gauge + a **descending threshold marker** sweeping
  toward 50%, plus Command Core hp bars. The legible stakes model: "blue tide must reach
  the falling line before time runs out, or break the red core."
- Match record MUST embed: map, seed, both bot module hashes, pinned runtime config (§2.2).

---

## 10. Author Tooling (deterministic gas profiler)

Because fuel is deterministic, authors get exact, reproducible gas/memory reports
pre-match (a measurement, not an estimate).
- **Tier 1:** total fuel per tick (read Wasmtime fuel counter before/after).
- **Tier 2 (high value):** worst-case **scenario suite** (below) — report per-scenario
  fuel + peak memory, worst/peak bolded, vs budget.
- **Tier 3 (later):** hotspot attribution (function-level, or source-level via DWARF).
- **Trust requirement (MUST):** the profiler runs the **identical pinned runtime config**
  as ranked matches. The number must match the match meter exactly.
- Runtime + tester + profiler are distributed via git so authors test locally.

### Scenario suite (fuel scales with army size under army-brain)
- `army_scaling` — same scenario at 10 / 50 / 200 / max units (the headline fuel-vs-scale
  curve; catches the bot that's brilliant early and forfeits late).
- `dense_battle` — many units + many visible enemies (stresses O(n²) target selection).
- `wide_visibility` — maximum map area in view.
- `max_legal` — literal worst case the rules permit (unit cap, view cap).
- `early_game` — tiny/cheap baseline; catches fixed per-tick overhead.
- A **long-running** scenario reporting **peak memory over time** (catches leaks — a real
  risk with persistent instances; a leaking bot degrades after thousands of ticks).

---

## 11. Build Order (suggested implementation sequence)

1. **Deterministic core engine** (no WASM yet): grid, units, the §5 tick loop with a
   hardcoded scripted "bot" interface, seeded PRNG, integer math. Prove determinism:
   same input → identical match hash across runs.
2. **Replay log format** + a minimal renderer/visualizer (even ASCII/2D) — you need to
   *see* matches to tune anything.
3. **Win condition + territory** (Voronoi, descending threshold, claims).
4. **WASM bot runner** (Wasmtime, fuel metering, memory cap, snapshot
   serialization across the boundary, forfeit handling).
5. **Bot SDK** (project template, `on_init`/`on_tick`, WorldView/Command types) + local
   tester + Tier 1/2 profiler.
6. **Tournament/ladder** harness + stats aggregation.
7. Calibration pass in sim: tune the placeholder numbers (§12).

Keep the engine pure/deterministic and the WASM layer a thin shell around it, so the
profiler, replays, and ranked matches all share one simulation path.

---

## 12. Calibration Targets (tune in sim — name these as constants)

The handful of numbers that drive the whole game's pace and balance:
- Threshold **start %** (~78) and **curve shape** (convex to ~51 at cap).
- **Tick cap** (~3000).
- Claim **max influence distance** (~2–3% of map).
- Claim **hp curve** (durable→~instant via `g`), **cost**, **build time**.
- Claim **aura** (deters lone scout, ignorable by real force).
- Unit **stat blocks** (§6) — toward "counters feel real, no dominant unit."
- **Unit cap** (soft economy gate + hard ceiling ~200–300).
- **Per-tick fuel budget**, **startup budget**, **memory cap**.
- Map **size** (~80×80) and asteroid/nebula/resource layout.

Tuning goal: most games end decisively (base kill or threshold) with a mid-game contest
and a late climax; near-zero draws; near-zero turtle-offs.

---

## 13. Deferred / Open Items (explicitly out of v1 scope)

**Pathing bucket (resolve later):**
- Blocked-unit behavior final rule (currently stay-put/retry placeholder; watch for
  two-units-retry-forever deadlock within a movement phase — wastes the phase, a
  correctness-tuning issue not a spine issue).
- Attack-move halt precision (step-into-range-then-hold vs. stop-one-tile-short).
- Per-step re-pathing ownership (bot-owned vs engine-owned).
- Stepped-move × first-strike × ranged-halt-distance interaction (the kiting-vs-run-down
  balance — validate in sim; it's the heart of the counter-triangle).
- **Line-of-sight definition** for the halt check and ranged combat through
  nebula/asteroids (currently hand-waved).

**Other deferred:**
- Terrain-respecting territory (claim signal flowing around walls, not through) and
  vision blocked by solid walls — v1 uses straight-line Voronoi and may look odd through
  packed asteroids; revisit if so.
- Extra terrain: structural wall skin (megastructure debris), gravity wells, hazard
  tiles, jump lanes, sensor/interference zones.
- Claim placement gating (currently teleport-anywhere; fallback is
  "deploy where a builder can walk, with a vulnerable build window").
- Scratch memory region zeroed each tick.
- 3rd resource type, additional unit types (Scout, etc.), claim tiers.
- Procedural map generation.
- 8-connectivity / diagonals.

---

## 14. Glossary

- **Army-brain:** one `on_tick` call per tick returns commands for the whole fleet
  (vs. per-unit callbacks). Chosen because coordination (focus-fire, formations) becomes
  ordinary in-function code instead of cross-call message-passing.
- **Claim Node:** structure that anchors Voronoi territory; must be destroyed by firepower
  to flip the region it controls.
- **Fuel:** Wasmtime's deterministic instruction meter = the gas limit. Deterministic, so
  it's both the in-match rule and the basis of the pre-match profiler.
- **Forfeit (tick):** a bot failed to produce valid commands this tick (fuel/OOM/trap);
  its units repeat-last/idle. Tracked as a stat; never a direct loss.
- **g (game-progress clock):** `tick / tick_cap`, drives both the descending win threshold
  and Claim Node fragility.
