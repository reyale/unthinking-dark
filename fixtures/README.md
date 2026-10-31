# fixtures

Shared test helpers included by unit tests across the `tests/` directory.

Not a test suite itself — no `TEST()` macros here. Just concrete implementations
of engine interfaces that tests need to instantiate without caring about bot logic.

## Contents

- `idle_bot.hpp` — `IdleBot`: a `Bot` that always returns empty commands. Used by
  determinism and replay tests that need a valid bot but want no game activity.
- `rush_bot.hpp`, `econ_bot.hpp` — MoveAttack toward the enemy corner / harvest only.
- `turtle_bot.hpp` — `TurtleBot`: one Claim Node beside its base, combat units guard home.
- `expander_bot.hpp` — `ExpanderBot`: builds a Factory, grows to a few Drones and walks the
  spare ones out to claim sites spreading from home; never attacks, so it can only win on
  territory. Its opponent in `tests/test_turtle.cpp`.
- `bot_util.hpp` — `Router` (BFS pathing around structures; own units block only the first
  step), plus small helpers shared by the turtle and expander.
