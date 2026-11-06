# Prompts

`one_shot_bot.md` is the prompt for **one-shot** ladder entries: an AI writes a whole bot from
this prompt alone, with no human edits, and the bot goes on the ladder as the AI left it
(author type `one_shot`, ai-game-tournament's DESIGN.md "Authorship"). Comparing one-shot bots
is only fair if they all start from the same prompt and the same repository, so both are
recorded with every entry.

The model works inside this repository: it reads the engine, the fixture bots and the docs
(the prompt sends it to the code for every rule and number, which change), writes its bot to
`prompts/entries/<name>/bot.cpp`, and checks it with `validate_bot.py` as often as it likes
before it finishes. "One-shot" means one prompt and no human help, not one try at compiling.

## Running a one-shot

1. Start from a clean checkout at a known commit (`git rev-parse --short HEAD`). Give an agent
   with shell access, in the repository's root, the contents of `one_shot_bot.md` as its whole
   instruction. Record the model id, the date and the commit: together they identify the
   entry.
2. Let it run to the end without help. Keep its final reply (name, description, validation
   summary, strategy notes) beside the bot as `prompts/entries/<name>/reply.md`.
3. Check that it changed nothing outside `prompts/entries/<name>/` (`git status`). An entry
   that edited the engine, fixtures or maps is void.
4. Run the validation yourself on the final file:

   ```bash
   python3 prompts/validate_bot.py prompts/entries/<name>/bot.cpp
   ```

   It must pass (exit 0). `report.md` lands beside the bot.

## The pipeline: `validate_bot.py`

`python3 prompts/validate_bot.py <bot.cpp> [--quick] [--seeds N] [--prefix DIR]`

1. **Build:** `bot.cpp` alone, with `prompts/build/CMakeLists.txt`. That is
   `ai_game_add_bot`, the same wasi-sdk and flags as every ladder bot.
2. **Profile:** `ud_profile`'s full suite; any forfeit fails.
3. **Play:** every fixture bot except idle, on every installed map plus generated maps up to
   the largest legal size, both seats; any fault fails.
4. **Report:** to stdout and `report.md`, and the tested module as `bot.wasm`, all beside the
   bot:
   - the profile's worst fuel and peak memory against the budgets;
   - scores by opponent and by map;
   - every fault;
   - every game not won.

Exit 0 passed, 1 didn't build, 2 faulted. The engine comes from `--prefix`, else
`$AI_GAME_PREFIX`, else `../ai-game-install` if it matches this checkout's `git describe`, else
an install it builds once in `build-oneshot/` (ignored by git). `--quick` skips the profile and
the generated maps while iterating. It needs only Python 3.8+ and CMake 3.20+ (it looks in
`~/.local/bin` too).

## Keeping the prompt true

The prompt describes the game qualitatively and sends the model to the code for every number
(`config.hpp`, `stats.cpp`, `abi.hpp`, DESIGN.md), so tuning and new content don't make it
wrong. When the rules themselves change (a new mechanic, a changed interface, a new header a
bot needs, a change to how bots are built), update it in the same change, and check that its
pointers still lead somewhere. `AGENTS.md` says so too.
