# viz/

Replay visualizer for Unthinking Depths.

## Executables

**`ud_viz <replay.ud|replay.ud.zst>`** — interactive ASCII terminal viewer.

Re-simulates the match from its input log (does not store per-frame state in the
file) and presents it frame by frame in the terminal.

## Controls

| Key       | Action                        |
|-----------|-------------------------------|
| `n` / ` ` | Next frame                   |
| `p`       | Previous frame                |
| `g`       | God view (no fog of war)      |
| `0`       | Faction 0 fog-of-war view     |
| `1`       | Faction 1 fog-of-war view     |
| `q`       | Quit                          |

## Glyphs

| Glyph | Meaning          |
|-------|------------------|
| `d`   | Drone            |
| `D`   | Drone carrying cargo |
| `i`   | Interceptor      |
| `f`   | Frigate          |
| `a`   | Artillery        |
| `C`   | Command Core     |
| `T`   | Factory          |
| `N`   | Claim Node       |
| `t` `n` | Factory / Claim Node under construction |
| `#`   | Asteroid         |
| `~`   | Nebula           |
| `$`   | Resource Node    |

Faction 0 units/structures are rendered in **cyan**, faction 1 in **red**.

## Library

`viz_lib` (static) exposes the rendering API for use in unit tests and other
tools without pulling in terminal I/O:

- **`viz::build_render_frame(FrameState, Map, view, frame_idx, total_frames)`**  
  Pure function — converts game state to a `RenderFrame` (grid of glyphs +
  colors). No I/O side effects; safe to call in tests.

- **`viz::print_ascii_frame(RenderFrame)`**  
  Writes ANSI escape codes to stdout. Only call from the interactive viewer.
