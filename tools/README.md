# tools/

Developer utilities for Unthinking Depths.

## gen_example_replay

Generates a demo replay file for manual testing of the visualizer.

```bash
./build/tools/gen_example_replay [output_path]
```

Defaults to `demo.ud` if no path is given. Use a `.ud.zst` extension to
write a zstd-compressed replay instead:

```bash
./build/tools/gen_example_replay demo.ud.zst
```

The generated match is two idle bots on a 20×20 map with some terrain, running
for 100 ticks. Feed the output to `ud_viz` to inspect it:

```bash
./build/viz/ud_viz demo.ud
```

## ud_arena

The balance arena. Armies of equal value meet on an open field and fight to the last ship. Both
sides get the same commander: every ship fires with focus fire, and the rest move down one
distance field to the nearest enemy ship. Ships standing to fire count as walls, so the others
flow around the firing line and surround instead of queueing behind it. A moving ship stops on
the first tile from which it can fire at where an enemy its side will see is headed (the
enemy's last move, repeated): the engine never halts a Move. The commander is told where the
enemy is, because the arena tests fighting, not scouting. Nothing fires blind, so
Artillery needs other ships to spot for it. What survives, weighed by cost and hull left, shows
which ships and mixes are worth their price.

```bash
./build/tools/ud_arena                         # pure-ship matrices at 600/1200/2400, round robin
./build/tools/ud_arena --spacing 2             # blocks spread out: splash hits fewer ships
./build/tools/ud_arena --budget 900 --parts 6  # one size, mixes in sixths of the budget
./build/tools/ud_arena --replay "60I 0F 0A 0B" "0I 24F 0A 0B" fight.ud   # watch one fight
```

- **Value** is energy + alloy × `HARVEST_ENERGY / HARVEST_ALLOY` (alloy harvests at half the
  rate, so it counts double).
- **Margin** is the share of one army's value left minus the other's, from -1 to +1. Every
  pairing is played both ways round, because faction 0 spawns first and gets the lower ids,
  which decide movement's right of way.
- **Worth** is what one ship is worth against a pure army of another, in value: the army's value
  divided by how many of the ship it takes to break even. Above the ship's price means it is
  cheap for that fight; `-` means even 4× the value can't break even.
- **Field:** the field is mirrored left to right. Blocks are 16 ranks wide with the shortest
  range at the front. `--spacing` sets the gap between neighbours.
- **Mirror fights** come out exactly even (`Arena.MirrorFightsAreEven`). The "first-mover bias"
  line checks that on every pure army and should read +0.000.
- **Speed:** a debug build is slow at this. Building the arena with `-O2` takes about a second
  per full run.
