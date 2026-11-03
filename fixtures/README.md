# fixtures

Shared test helpers included by unit tests across the `tests/` directory.

Not a test suite itself — no `TEST()` macros here. Just concrete implementations
of engine interfaces that tests need to instantiate without caring about bot logic.

## Contents

- `idle_bot.hpp` — `IdleBot`: a `Bot` that always returns empty commands. Used by
  determinism and replay tests that need a valid bot but want no game activity.
- `rush_bot.hpp` — `RushBot`: two claims by home, then a Factory and nonstop Interceptors,
  sent in waves of five at enemy claims and then the Core; never defends. Breaks the turtle
  and the expander by base kill (`tests/test_rush_econ.cpp`).
- `econ_bot.hpp` — `EconBot`: the boom. Expands over the claim lattice with more claimers as
  it grows, up to three Factories, and a defensive army (Frigates, Artillery when alloy is
  spare) capped at six plus one per claim, spread out guarding claims; never attacks. Beats
  the turtle and the expander on territory and holds off the rush.
- `turtle_bot.hpp` — `TurtleBot`: one Claim Node beside its base, combat units guard home.
- `expander_bot.hpp` — `ExpanderBot`: grows to a few Drones and walks the
  spare ones out to claim sites spreading from home; never attacks, so it can only win on
  territory. Its opponent in `tests/test_turtle.cpp`.
- `adaptive_bot.hpp` — `AdaptiveBot`: the combat fixture. Expands, builds Factories,
  counter-picks ships, scouts, ambushes from nebulae and pushes in waves at enemy claims and
  then the Core. Its tests are `tests/test_adaptive.cpp`.
- `bot_util.hpp` — `Router`, `waypoint()`, `builders()`, `ClaimLattice`, `focus_fire()`,
  `factory_site()`, `rally_point()`, and small helpers shared by the bots. `Router` routes by distance fields shared by every unit given the same
  goal (`GoalKey`); `route()`'s are kept across ticks (terrain and structures; units are
  handled at the first step, by a lookahead and by short detours), grown lazily, and capped
  per tick (`kFloodBudget`), so a bot's routing fits the fuel budget a WASM bot gets.

## Every fixture is also a WASM bot

The fixtures are written against `game::Bot` (`on_init(Map)`, `on_tick(Snapshot)`), exactly
what the wire carries, so each one also builds, unchanged, as a `.wasm` module:
`fixtures/CMakeLists.txt` compiles `wasm_main.cpp` once per fixture with wasi-sdk (fetched
and pinned by the top-level CMakeLists; `-DUD_WASM_FIXTURES=OFF` skips it), and
`sdk/cpp/include/ud/bot_host.hpp` rebuilds the `Map` and `Snapshot` from the runner's buffers.
The modules land in `build/fixtures_wasm/` and run with `ud_run` like any bot.

`tests/test_fixture_wasm.cpp` (`FixtureWasm.*`) plays each module against the WASM
AdaptiveBot and the same match natively, and requires the same commands every tick and no
forfeits: a fixture change that breaks the port, or outgrows the fuel budget, fails there.
To add a fixture: `<name>_bot.hpp` defining `game::<Class>`, an entry in
`fixtures/CMakeLists.txt`, and one in that test (`FixtureWasmBuild.EveryBuiltModuleIsTested`
catches a missing one).
