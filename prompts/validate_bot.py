#!/usr/bin/env python3
"""Validates a one-shot bot the way the ladder will meet it: build, profile, play.

    python3 prompts/validate_bot.py path/to/bot.cpp [--prefix DIR] [--seeds N] [--quick]

1. Build: bot.cpp alone, with the ladder's own bot build (prompts/build, ai_game_add_bot).
2. Profile: the engine's ud_profile suite (big armies, wide views, the largest map, long
   games). Any forfeit fails.
3. Play: every fixture bot, on every installed map and on generated maps up to the largest
   legal size, from both seats. Any fault (fuel, memory, trap) fails.
4. Report: results by opponent and by map, and the bot's worst fuel per tick and peak memory
   against the budgets, written to stdout and to report.md beside bot.cpp; the module it
   validated is saved beside it as bot.wasm.

Exit status: 0 passed, 1 didn't build, 2 faulted somewhere (the report says where).

The engine comes from an install prefix (bin/ud_run, share/ai_game/...): --prefix, else
$AI_GAME_PREFIX, else ../ai-game-install if it is current, else one built here once, in
build-oneshot/ (a Release build and install: a few minutes the first time). Standard library
only, so it runs anywhere the engine builds.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake", path=f"{Path.home()}/.local/bin:{os.environ.get('PATH', '')}")
# Generated maps besides the installed ones: a mid size, a big one, and the largest legal
# board, where fuel and memory run highest (config.hpp MAP_MAX_W/H).
GENERATED = [(48, 48), (96, 96), (128, 128)]
OPPONENTS_SKIP = {"idle"}  # it never plays; beating it proves nothing (it is still checked once)


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def own_prefix() -> Path:
    """An install of this checkout, built once in build-oneshot/."""
    build, prefix = REPO / "build-oneshot", REPO / "build-oneshot" / "install"
    if not (prefix / "bin" / "ud_run").exists():
        print("building the engine once into build-oneshot/ (a few minutes)...", flush=True)
        for cmd in ([CMAKE, "-S", REPO, "-B", build, "-DCMAKE_BUILD_TYPE=Release"],
                    [CMAKE, "--build", build, "-j", os.cpu_count() or 2],
                    [CMAKE, "--install", build, "--prefix", prefix]):
            p = run(cmd)
            if p.returncode:
                sys.exit(f"engine build failed: {' '.join(map(str, cmd))}\n{p.stdout[-3000:]}"
                         f"{p.stderr[-3000:]}")
    return prefix


def find_prefix(arg: str | None) -> Path:
    for cand in (arg, os.environ.get("AI_GAME_PREFIX")):
        if cand:
            return Path(cand).resolve()
    sibling = (REPO.parent / "ai-game-install").resolve()
    if (sibling / "bin" / "ud_run").exists():
        mine = run(["git", "-C", REPO, "describe", "--tags", "--always", "--dirty"]).stdout.strip()
        theirs = run([sibling / "bin" / "ud_run", "--version"]).stdout.split()
        if len(theirs) > 1 and theirs[1] == mine:
            return sibling
    return own_prefix()


def build(bot: Path, prefix: Path, out_dir: Path) -> Path | None:
    b = out_dir / "build"
    for cmd in ([CMAKE, "-S", REPO / "prompts" / "build", "-B", b, f"-DCMAKE_PREFIX_PATH={prefix}",
                 f"-DBOT={bot}"], [CMAKE, "--build", b]):
        p = run(cmd)
        if p.returncode:
            print(p.stdout[-6000:], p.stderr[-6000:], sep="\n")
            return None
    return b / "bot.wasm"


def profile(prefix: Path, wasm: Path) -> dict:
    p = run([prefix / "bin" / "ud_profile", wasm, "--json"])
    try:
        return json.loads(p.stdout)
    except json.JSONDecodeError:
        return {"error": (p.stderr or p.stdout)[-2000:], "forfeits": 1}


def maps(prefix: Path, tmp: Path) -> list[Path]:
    out = sorted((prefix / "share" / "ai_game" / "maps").glob("*.json"))
    for w, h in GENERATED:
        path = tmp / f"generated_{w}x{h}.json"
        run([prefix / "bin" / "ud_mapgen", "--width", w, "--height", h, "--seed", 7,
             "--name", path.stem, "--out", path])
        if path.exists():
            out.append(path)
    return out


def play(prefix, wasm, opp, map_path, seed, seat, tmp) -> dict:
    a, b = (wasm, opp) if seat == 0 else (opp, wasm)
    p = run([prefix / "bin" / "ud_run", a, b, "--map", map_path, "--seed", seed, "--json",
             "--out", tmp / f"{opp.stem}-{map_path.stem}-{seed}-{seat}.ud"])
    try:
        r = json.loads(p.stdout)
    except json.JSONDecodeError:
        return {"opp": opp.stem, "map": map_path.stem, "seat": seat, "seed": seed,
                "error": (p.stderr or p.stdout)[-500:]}
    me = r["sides"][seat]
    w = r["result"]["winner"]
    return {"opp": opp.stem, "map": map_path.stem, "seat": seat, "seed": seed,
            "score": 0.5 if w is None else float(w == seat), "how": r["result"]["text"],
            "ticks": r["ticks"], "fault": me["fault"],
            "error": me.get("error", "").replace("\x00", "").strip(),
            "fuel_peak": me["fuel_peak"], "mem_peak": me["mem_peak"]}


def table(rows, key) -> list[str]:
    acc = defaultdict(lambda: [0, 0, 0])
    for r in rows:
        if "score" in r:
            acc[r[key]][{1.0: 0, 0.5: 1, 0.0: 2}[r["score"]]] += 1
    lines = [f"| {key} | won | drawn | lost | score |", "|---|---|---|---|---|"]
    for k, (w, d, l) in sorted(acc.items()):
        lines.append(f"| {k} | {w} | {d} | {l} | {(w + d / 2) / max(w + d + l, 1):.0%} |")
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("bot", type=Path, help="the bot's bot.cpp")
    ap.add_argument("--prefix", help="an installed engine (default: see above)")
    ap.add_argument("--seeds", type=int, default=1, help="seeds per opponent, map and seat")
    ap.add_argument("--quick", action="store_true",
                    help="skip the generated maps and the profile: a fast check while iterating")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = ap.parse_args(argv)
    bot = args.bot.resolve()
    if not bot.exists():
        sys.exit(f"no such file: {bot}")

    prefix = find_prefix(args.prefix)
    report = [f"# Validation of {bot}", "", f"Engine: {run([prefix / 'bin' / 'ud_run', '--version']).stdout.strip()} "
              f"({prefix})", ""]
    with tempfile.TemporaryDirectory(prefix="oneshot-") as t:
        tmp = Path(t)
        wasm = build(bot, prefix, tmp)
        if wasm is None or not wasm.exists():
            print("\nBUILD FAILED: the output above is the compiler's.")
            return 1
        shutil.copyfile(wasm, bot.parent / "bot.wasm")  # the module validated: the one to enter
        print(f"built {wasm.stat().st_size:,} bytes -> {bot.parent / 'bot.wasm'}", flush=True)
        failed = False

        prof = None
        if not args.quick:
            print("profiling (the full ud_profile suite)...", flush=True)
            prof = profile(prefix, wasm)
            budgets = prof.get("budgets", {})
            report += ["## Profile", ""]
            if "error" in prof:
                report.append(f"ud_profile failed: {prof['error']}")
            else:
                report += [f"- init fuel {prof['init']['fuel']:,} of {budgets['fuel_init']:,}",
                           f"- worst tick fuel {prof['worst_fuel']:,} of {budgets['fuel_per_tick']:,} "
                           f"({prof['worst_fuel'] / budgets['fuel_per_tick']:.0%})",
                           f"- peak memory {prof['peak_memory'] / 2**20:.1f} MiB of "
                           f"{budgets['memory_bytes'] / 2**20:.0f} MiB", ""]
                report += ["| scenario | worst fuel | peak memory | forfeits |", "|---|---|---|---|"]
                for s in prof["scenarios"]:
                    report.append(f"| {s['name']} | {s['worst_fuel']:,} | "
                                  f"{s['peak_memory'] / 2**20:.1f} MiB | {sum(s['forfeits'].values())} "
                                  f"{s['first_fault'] if s['first_fault'] != 'none' else ''} |")
            report.append("")
            failed |= bool(prof.get("forfeits"))

        bots = prefix / "share" / "ai_game" / "bots"
        opps = [p for p in sorted(bots.glob("*.wasm")) if p.stem not in OPPONENTS_SKIP]
        pool_maps = (sorted((prefix / "share" / "ai_game" / "maps").glob("*.json")) if args.quick
                     else maps(prefix, tmp))
        jobs = [(o, m, 1000 + s, seat) for o in opps for m in pool_maps
                for s in range(args.seeds) for seat in (0, 1)]
        jobs.append((bots / "idle.wasm", pool_maps[0], 1000, 0))
        print(f"playing {len(jobs)} matches on {args.jobs} cores...", flush=True)
        with ThreadPoolExecutor(args.jobs) as ex:
            rows = list(ex.map(lambda j: play(prefix, wasm, *j, tmp), jobs))

    faults = [r for r in rows if r.get("fault", "none") != "none" or "score" not in r]
    failed |= bool(faults)
    played = [r for r in rows if "score" in r]
    report += ["## Matches", "",
               f"{len(played)} played, overall score "
               f"{sum(r['score'] for r in played) / max(len(played), 1):.0%} "
               f"(a win 1, a draw ½). Worst fuel in a tick "
               f"{max((r['fuel_peak'] for r in played), default=0):,}; peak memory "
               f"{max((r['mem_peak'] for r in played), default=0) / 2**20:.1f} MiB.", ""]
    report += table(played, "opp") + [""] + table(played, "map") + [""]
    if faults:
        # Grouped by cause: one fault usually repeats in every game, and the first is enough.
        causes = defaultdict(list)
        for r in faults:
            causes[(r.get("fault", "error"), r.get("error", ""))].append(r)
        report += ["## Faults: these lose matches on the ladder", ""]
        for (fault, error), rs in causes.items():
            where = sorted({f"{r['opp']} on {r['map']}" for r in rs})
            ticks = sorted(r["ticks"] for r in rs if "ticks" in r)
            report += [f"### {fault}, in {len(rs)} game(s)"
                       + (f", from tick {ticks[0]}" if ticks else ""), "",
                       "Against: " + ", ".join(where[:12]) + (" ..." if len(where) > 12 else ""),
                       "", "```", error or "(no message)", "```", ""]
    losses = [r for r in played if r["score"] < 1]
    if losses:
        report += ["## Games not won", ""]
        for r in losses[:40]:
            report.append(f"- vs {r['opp']} on {r['map']} (seat {r['seat']}): "
                          f"{'draw' if r['score'] == 0.5 else 'lost'}, {r['how']}, tick {r['ticks']}")
        report.append("")
    report.append("**FAILED**: see above." if failed else "**PASSED**: builds, never faulted.")

    text = "\n".join(report)
    print("\n" + text)
    (bot.parent / "report.md").write_text(text + "\n")
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
