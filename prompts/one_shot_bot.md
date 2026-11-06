# Write a bot for Unthinking Dark

You are writing a complete bot for **Unthinking Dark**, a deterministic two-player real-time
strategy game played by programs. Your bot is compiled to WebAssembly and entered on a ladder
against other bots. This is a **one-shot** entry: this prompt is the only instruction you get,
no human edits your code, and what you finish with is what plays. A bot that doesn't compile
never plays. A bot that crashes, runs out of compute or runs out of memory in a match loses
that match on the spot. Make it never fault first; then make it win.

You are working in the game's repository (`ai-game`). Read as much of it as you need, and build
and run things in it. A validation pipeline (§6) builds your bot and plays it against the
engine's own bots.

- DO NOT ASSUME THIS DOCUMENT IS 100% ACCURATE ALWAYS READ THE DOCS AND CODE
- NO I/O, NO NETWORKING, ETC
- OTHERWISE YOU CAN DO ANYTHING YOU WANT IN TERMS OF SOFTWARE AS LONG AS IT COMPILES AND RUNS WITHIN THE CONSTRAINTS as WebAssembly
- ITERATE AS MUCH AS YOU WANT TO
- YOU DO NOT KNOW THE OPPONENTS YOU WILL FACE - SOME MIGHT BE QUITE SOPHISTICATED
- COME UP WITH NOVEL STRATEGIES, AS LONG AS YOU DO NOT HACK THE ABI/ENGINE IT IS LEGAL - INCLUDING EXPLOITING ENGINE BUGS

---

## 0. Sources of truth: read the code

**This document describes the game, but the code defines it, and the code changes often:
numbers are tuned, rules are refined, and units, structures and maps are added.** Don't trust
any number you remember about this game, from this document or from training. Read it from
the files below, and use the engine's constants and functions in your bot instead of copying
values, so it stays right when they change.

| What | Where |
|---|---|
| Every rule, with the reasons behind it | `docs/DESIGN.md` (§3 world and fog, §4 the bot API, §5 how a tick resolves, §6 units and structures, §7 winning) |
| Every number: costs, stats, caps, ranges, budgets, the threshold curve, map limits | `engine/src/config.hpp` (`game::cfg::...`; use these in your code) |
| Each unit type's stats as the engine uses them | `engine/src/stats.hpp` / `stats.cpp` (`game::stats_for(UnitType)`) |
| What a bot receives and sends: snapshot, views, commands, enum values | `engine/src/abi.hpp`, `snapshot.hpp`, `command.hpp`, `entity.hpp`, `grid.hpp` |
| The bot interface | `engine/src/bot_iface.hpp` |
| Movement, combat, economy, territory, the win check | `engine/src/movement.cpp`, `combat.cpp`, `economy.cpp`, `territory.cpp`, `wincheck.cpp` |
| What each side starts with | `maps/match_setup.cpp` |
| The maps | `maps/data/*.json`; `tools/ud_mapgen` makes more |
| How a bot is built and run as WebAssembly | `sdk/cpp/include/ud/bot_host.hpp`, `cmake/WasmBot.cmake`, `runner/wasm_bot.cpp` |

**Learn from the bots that already exist.** `fixtures/` holds the engine's sample bots, every
one written against the same `game::Bot` interface you'll use and every one on the ladder you
will face:
- `rush_bot.hpp`, `econ_bot.hpp`, `turtle_bot.hpp`, `expander_bot.hpp`;
- `adaptive_bot.hpp`, the strongest, which plays every system; `aggro_bot.hpp` and
  `boom_bot.hpp` are the same bot with other settings;
- `bot_util.hpp`: routing (`Router`), a Drone's harvest loop, claim placement;
- `fixtures/README.md`: how each plays.

`sdk/cpp/examples/` has small bots for the SDK. Read them to see how the interface is used in
practice, what costs fuel, and how they route. You will play against them, so knowing how
they play is part of your strategy. You may reuse ideas and small utilities from them (inline
any code you take: your bot is one file), but the bot and its strategy should be your own.

`tools/ud_arena` (`tools/README.md`) fights equal-value armies of each ship type against each
other. Use it to see which compositions actually win under the current numbers.

---

## 1. The game, in brief

Read DESIGN.md for the full rules; this is the shape of it.

- **Sides and ticks:** two sides start in opposite corners of a point-symmetric grid map.
  Every tick each side's bot sees a snapshot and orders its units and producers. A match
  lasts at most `cfg::TICK_CAP` ticks.
- **Winning:** destroy the enemy Command Core; or hold territory at or above the win
  threshold. The threshold starts high and falls over the match toward a floor at the tick
  cap (`game::win_threshold`, `cfg::THRESH_*`). At the tick cap, more territory wins, and a
  margin within `cfg::DRAW_TERRITORY_MARGIN` is a draw. The snapshot gives your territory
  and the current threshold every tick.
- **Losing at once:** your bot faults, meaning it runs out of fuel, grows memory past the cap,
  or traps.
- **The map:** 4-connected (N, E, S, W), Manhattan distance; x grows east, y grows south.
  - Asteroids are impassable.
  - Nebulae hide whatever is inside them except at very close range.
  - Resource nodes are harvested.
  - The whole map is public: `on_init` gets every tile.

  Map sizes vary up to `cfg::MAP_MAX_W` × `cfg::MAP_MAX_H` and new maps are added, so don't
  hard-code a layout or a size.
- **Fog of war:** you see what is within sight of your units and structures. Enemies drop out
  of the snapshot when they leave your sight, and the engine remembers nothing for you. A unit
  can only shoot what its own side currently sees, so a gun that outranges its sight needs a
  spotter.
- **Economy:** Drones harvest automatically next to a node, but carry their cargo and must bank
  it at your Core or a finished Claim Node before you can spend it. Energy and Alloy pay for
  different things. Drones are capped by your Core and finished claims; Factories by your
  finished claims; and all units by a cap that scales with the map's area.
- **Structures:**
  - The Core produces Drones.
  - Factories produce ships.
  - Drones build Factories and deploy Claim Nodes, which take ticks of work with a Drone
    nearby.
  - Claims own the territory around them (nearest claim wins a tile), bank cargo, add Drone
    and Factory slots, and shoot weakly. They grow fragile as the match goes on.
- **Ships:** several types, with different strengths and counters, and shields that recharge
  when out of fire. Read the types in `entity.hpp`, their stats in `stats.cpp` and
  `config.hpp`, and DESIGN §6 for what each is for.
- **How a tick resolves**, both sides at once (DESIGN §5 has the exact order):
  - Both bots decide from the same frozen snapshot, then commands are validated (one per
    entity; the first listed wins).
  - Movement is simultaneous and stepped.
  - Combat is at the final positions; first-strike ships fire before the rest.
  - Then the economy and production, spawning, and territory and the win check.

**Four mechanics that catch bots out** (each is in the code; check it):
1. **No pathfinding.** A Move steps straight toward its target tile along the longer axis
   first, and a unit whose next step is blocked by an asteroid or a structure just stops for
   the tick (`movement.cpp`, `step_toward`). Route your own units: compute paths and give each
   unit a waypoint it can reach in a straight line.
2. **Only ordered units fire.** A unit fires only on a tick it has a Move or Attack order. A
   unit with no command holds fire, so order your army every tick.
3. **Traffic.** Units may follow into a tile vacated on the same step. Swaps and closed loops
   of movers block, along with everyone queued behind them. When two units contend for a
   tile, the higher `collision_priority` wins (`stats.cpp`).
4. **Construction and claims are slow and vulnerable.** An unfinished structure does nothing
   until it is done, and only a Drone standing near it gets the work done (`economy.cpp`).

---

## 2. How to think about strategy

There is no known best strategy, and the numbers change, so don't come in with a plan
borrowed from another game or what you have seen previously. Work it out from this game's rules and the bots you'll face.
Spend real effort here before writing much code; a well-built bot with a poor plan still
loses.

### understand example opponents 
- **Read every fixture bot in `fixtures/`**, the files and not just their summaries. For each,
  Most have a blind spot by design (`rush` never defends, `econ` never attacks, `expander`
  can't fight, `turtle` never grows). Confirm each in the code, and find the subtler gaps in
  the ones that look complete.  `adaptive_bot.hpp` is the hardest example

### Then, understand the mechanics that decide games
Read the code behind each of these and ask what it rewards. Some examples:
- **The economy compounds, and is capped by territory.**
- **Resources only count when banked.**
- **The clock favours whoever is ahead on territory.** 
- **Structures see little.**
- **Counters are real, and positioning matters as much as composition.**
- **Fog is total and memoryless.** 
- **Traffic is part of tactics.**
- **Maps vary a lot.**

### Test against the real thing
- **Use the validation report to find your weakest matchup, then watch that replay.** Find
  the moment the game turned, fix that, and validate again. Repeat for the next weakest.
- **Check every opponent and every map size.** A strategy tuned to one map or one opponent
  will meet the rest on the ladder.
- **Never trade safety for strength.** A bot that wins most games but faults in one loses that
  one for nothing.

---

## 3. The interface

Write C++23 against `game::Bot` (`engine/src/bot_iface.hpp`). One file, `bot.cpp`, includes the
engine headers by bare name and ends with `UD_WASM_BOT`:

```cpp
#include "bot_iface.hpp"   // game::Bot, Snapshot, Command, the view structs (abi.hpp)
#include "config.hpp"      // game::cfg::... every constant
#include "entity.hpp"      // UnitType, StructureType
#include "grid.hpp"        // Map, Tile, Terrain, Vec2, manhattan, DIRS
#include "stats.hpp"       // stats_for(UnitType)
#include "rng.hpp"         // Rng(seed)
#include <ud/bot_host.hpp> // UD_WASM_BOT(...)

class MyBot : public game::Bot {
public:
  void on_init(const game::Map& map, uint32_t faction_id, uint64_t seed) override;
  std::vector<game::Command> on_tick(const game::Snapshot& snap) override;
};

UD_WASM_BOT(MyBot)   // exactly once, at the end of the file
```

- **`on_init`:** runs once, before the first tick, with a large fuel budget. It gets the whole
  map, your faction and a per-match seed. Precompute here. The bot object lives for the whole
  match, so keep state in members.
- **`on_tick`:** returns your commands. Copy what you need from the snapshot; don't keep
  references into it.
- **The snapshot and its views:** see `abi.hpp`, where every field is commented, and
  `snapshot.hpp`.
- **Orders:** `game::Command` in `abi.hpp`. Zero-initialise each one (`game::Command c{};`) and
  set `entity_id` (who acts), `kind` (a `game::CommandKind`, cast to `uint16_t`), and the
  fields that kind uses. `command.cpp` and `economy.cpp` show exactly what each kind needs and
  what gets a command rejected. Some kinds are reserved and always rejected; `abi.hpp` says
  which.

---

## 4. Limits: never break these

Each is a lost match, not a warning.

- **Fuel** (WebAssembly instructions, roughly): `cfg::FUEL_PER_TICK` per `on_tick`,
  `cfg::FUEL_STARTUP` for `on_init`. They're generous, but per-unit searches over the largest
  map with an army at the unit cap will exhaust them. Share and cache distance fields, and
  bound the work per tick; `fixtures/bot_util.hpp` shows one way.
- **Memory:** `cfg::MEMORY_CAP_MB` of linear memory in all.
- **No exceptions or RTTI:** the build uses `-fno-exceptions -fno-rtti` (`cmake/WasmBot.cmake`).
  Anything the standard library would throw from aborts, and an abort is a crash.
- **No I/O of any kind:** no printing, files, clocks, threads or environment. The bot is
  sandboxed with no host access, and any attempt traps.
- **No undefined behaviour:** bounds-check every map and vector access.
- **One file:** `bot.cpp`, using only the engine headers above and the C++ standard library.

---

## 5. Rules for this entry

- Put your bot in `prompts/entries/<name>/bot.cpp`, where `<name>` is your bot's name (lowercase,
  hyphens allowed). Change nothing else in the repository: not the engine, the fixtures, the
  maps, the build, or this prompt. Don't commit.
- Iterate as much as you like within this session: read, write, build, validate, fix, and
  validate again. Nobody else touches the code.
- What counts is the final `bot.cpp`. It must pass the validation pipeline.

---

## 6. Validate it

```bash
python3 prompts/validate_bot.py prompts/entries/<name>/bot.cpp           # the full check
python3 prompts/validate_bot.py prompts/entries/<name>/bot.cpp --quick   # faster, while iterating
```

1. **Build:** compiles `bot.cpp` alone, exactly as the ladder does (`prompts/build`). Compiler
   errors are printed. The engine is built once on first use if there isn't a current
   install; that takes a few minutes.
2. **Profile:** the engine's `ud_profile` suite: large armies on the largest map, wide
   visibility, dense battles, long games. It reports your worst fuel per tick and peak memory
   against the budgets. Any forfeit fails.
3. **Play:** every fixture bot, on every installed map and on generated maps up to the largest
   size, from both seats. Any fault fails.
4. **Report:** printed, and written to `report.md` beside your `bot.cpp`. It has results by
   opponent and by map, and every game not won with how it ended.

Exit status 0 means it passed: it builds and never faulted. The score is up to you: use the
report to find what loses and fix it.

- **Watch a game:** write a replay with `ud_run <a.wasm> <b.wasm> --map <map> --out game.ud`,
  then open it with `ud_viz game.ud` (terminal), or `ud_viz_imgui game.ud` if there is a
  display. Both are in the `bin/` of the engine the report names.
- **Fuel headroom:** keep the worst tick well under budget. Opponents and maps on the ladder
  can push your bot harder than the validation games do.

---

## 7. What to deliver

When you're done, reply with:
1. **The bot's name** and the path of its `bot.cpp`.
2. **A one-line description** of how it plays.
3. **The validation summary:** the PASSED/FAILED line, the score against each opponent, and
   the worst fuel per tick against the budget.
4. **A short account** of your strategy, and what you changed after watching it lose.
