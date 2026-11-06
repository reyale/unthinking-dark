# IMPLEMENTATION_PLAN.md — Unthinking Dark: engine roadmap

What to build next in this repo (the engine, runner, SDKs, maps and tools), in rough priority
order, each with its goal, what's done when it's done, and where to stop and ask. `DESIGN.md`
holds the rules (section refs like §5 point there); `AGENTS.md` holds how to work in the code.
The ladder has its own plan (`../ai-game-tournament/IMPLEMENTATION_PLAN.md`). The v1 build plan
is in [archive/IMPLEMENTATION_PLAN_v1.md](archive/IMPLEMENTATION_PLAN_v1.md).

*Draft: the items and their order are a starting point, to be edited.*

---

## Where we are

v1 is built:
- **Engine:** the deterministic simulation, replays that re-simulate exactly, territory with the
  descending threshold.
- **Viewers:** terminal, ncurses, Dear ImGui and the browser.
- **Runner:** Wasmtime with fuel and memory caps, no host imports (§2.1a).
- **Bot tooling:** the C++ SDK and the deterministic profiler.
- **Calibration:** a first pass with `ud_arena` and the fixture matchups.

---

## 1. Tune the balance against measured targets

**Goal:** constants that meet the §12 targets, checked against numbers rather than impressions.
The numbers come from the ladder's balance report (its plan, item 2): win types, the draw rate,
match lengths, forfeits by army size, and fixture-vs-fixture results per map.

- **Record the numbers:** save the round of record's numbers in DESIGN §12, next to the targets.
- **Tune only what misses a target,** with named constants in `config.hpp` and the reason written
  down. Re-run `ud_arena`, `Arena.*` and the matchup tests in `map_tests` after each change.
- **Budgets:** re-measure them with `ud_profile` if the caps or fixtures change (§2.3).

**Done when:** the round of record meets the §12 targets (a low draw rate, turtles lose, the
counter-triangle holds, durations below the cap), or the misses are written up.

**Stop and ask:** if no set of numbers hits the targets, the problem may be a mechanic rather than
a number. Bring the data.

## 2. More and bigger maps

**Goal:** a map pool that tests more than the opening. Only `small_open` and `twin_peaks` ship
today, both small; budgets are sized for 128×128.

- **Hand-made maps** in `maps/data/`, point-symmetric (`ud_mapcheck`): mid-size and large ones,
  maps with chokes, and maps rich in nebulae.
- **Map tests:** the matchup tests in `map_tests` run on every map, so each new map gets them for
  free; a map they fail is a finding.
- **Profiler:** re-check `max_legal` on the largest map.

**Done when:** at least 4 maps across a range of sizes are installed, and they all pass the
matchup tests.

**Stop and ask:** procedural maps are out of v1 scope.

## 3. SDKs beyond C++

**Goal:** bots in other languages without hand-written ABI code.

- **Rust first:** a crate with the `abi.hpp` structs (`#[repr(C)]`, layout tests against the C++
  asserts), a `Bot` trait, and a template that builds with bulk memory off.
- **Then another language,** perhaps AssemblyScript or Zig.
- **Each SDK's example bot** loads in the runner and passes `ud_profile`.

**Done when:** the Rust example bot plays a full `ud_run` match with no forfeit, and an ABI bump
fails its layout tests until it is updated.

**Stop and ask:** the bulk-memory decision (README "Open decisions") gets harder with every new
toolchain: revisit it here if it's the main friction.

## 4. Keep the runner's wall up

**Goal:** the bot sandbox stays as tight as it is today (§2.1a) as Wasmtime moves on.

- **Wasmtime updates:** keep it current. On every update, re-run `Sandbox.*` and check the new
  version's defaults: any feature it newly turns on is off here unless a bot needs it.
- **New probes:** add a `Sandbox.*` test for any new way out that turns up.

**Done when:** ongoing.

---

## Open questions (not yet planned)

- **Kiting vs running down** (§13): validate with chasing and retreating fights; `ud_arena` only
  fights head-on.
- **Upkeep:** revisit only if play shows hoarding or spam.
- **Deferred mechanics** (§13): terrain-respecting territory and vision, extra terrain, more
  shield and weapon interactions.
