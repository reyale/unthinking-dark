#!/usr/bin/env python3
"""Writes the designed maps in maps/data/ (python3 maps/make_maps.py; check with ud_mapcheck).

`ud_mapgen` makes small random maps; these are laid out by hand, each to test something:
chokes, nebula ambushes, a long front, many lanes, the largest board. Every feature is drawn
point-symmetrically ((x, y) and (w-1-x, h-1-y) always match), and each map's randomness is
seeded, so re-running the script rewrites the same files.
"""

import json
import random
from collections import deque
from pathlib import Path

OPEN, ROCK, NEBULA, NODE = ".", "#", "~", "R"
RESOURCE_AMOUNT = 500
SPAWN_CLEAR = 4  # open ground around each spawn, for the Core and the starting units
HOME_NODES = ((3, 0), (0, 3), (4, 2), (2, 4))  # as ud_mapgen places them


class Board:
    def __init__(self, name: str, w: int, h: int, seed: int, spawn: int = 2):
        self.name, self.w, self.h = name, w, h
        self.rng = random.Random(seed)
        self.g = [[OPEN] * w for _ in range(h)]
        self.spawn = (spawn, spawn)
        for x in range(w):
            self.set(x, 0, ROCK)
        for y in range(h):
            self.set(0, y, ROCK)

    def mirror(self, x, y):
        return self.w - 1 - x, self.h - 1 - y

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            mx, my = self.mirror(x, y)
            self.g[y][x] = self.g[my][mx] = c

    def get(self, x, y):
        return self.g[y][x]

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, c)

    def blob(self, x, y, size, c, over=(OPEN,)):
        """A random-walk patch of `size` steps, drawn only over `over`."""
        for _ in range(size):
            if 0 < x < self.w - 1 and 0 < y < self.h - 1 and self.get(x, y) in over:
                self.set(x, y, c)
            dx, dy = self.rng.choice(((1, 0), (-1, 0), (0, 1), (0, -1)))
            x, y = x + dx, y + dy

    def wall(self, x0, y0, x1, y1, gaps=(), thick=1):
        """A straight wall of rock, open where t (0..1 along it) is within a gap's half-width."""
        n = max(abs(x1 - x0), abs(y1 - y0))
        for i in range(n + 1):
            t = i / n if n else 0
            if any(abs(t - g) <= half for g, half in gaps):
                continue
            x, y = round(x0 + (x1 - x0) * t), round(y0 + (y1 - y0) * t)
            self.rect(x, y, x + thick - 1, y + thick - 1, ROCK)

    def node_field(self, x, y, n, spread=2):
        """`n` resource nodes scattered around (x, y)."""
        for _ in range(n * 4):
            if n == 0:
                break
            px = x + self.rng.randint(-spread, spread)
            py = y + self.rng.randint(-spread, spread)
            if 0 < px < self.w - 1 and 0 < py < self.h - 1 and self.get(px, py) == OPEN:
                self.set(px, py, NODE)
                n -= 1

    def scatter_nodes(self, per_tiles):
        """Enough single nodes to bring the map to about one per `per_tiles` tiles."""
        want = self.w * self.h // per_tiles - self.count(NODE)
        for _ in range(want * 20):
            if want <= 0:
                break
            x, y = self.rng.randrange(1, self.w - 1), self.rng.randrange(1, self.h - 1)
            if self.get(x, y) == OPEN and self.far_from_spawns(x, y, SPAWN_CLEAR + 2):
                self.set(x, y, NODE)
                want -= 2

    def far_from_spawns(self, x, y, d):
        sx, sy = self.spawn
        mx, my = self.mirror(sx, sy)
        return abs(x - sx) + abs(y - sy) >= d and abs(x - mx) + abs(y - my) >= d

    def count(self, c):
        return sum(row.count(c) for row in self.g)

    def finish(self) -> dict:
        sx, sy = self.spawn
        for y in range(1, self.h - 1):
            for x in range(1, self.w - 1):
                if abs(x - sx) + abs(y - sy) <= SPAWN_CLEAR:
                    self.set(x, y, OPEN)
        for dx, dy in HOME_NODES:
            self.set(sx + dx, sy + dy, NODE)
        self.seal_pockets()
        rows = ["".join(r) for r in self.g]
        mx, my = self.mirror(sx, sy)
        rows[sy] = rows[sy][:sx] + "1" + rows[sy][sx + 1:]
        rows[my] = rows[my][:mx] + "2" + rows[my][mx + 1:]
        return {"name": self.name, "resource_amount": RESOURCE_AMOUNT, "grid": rows}

    def seal_pockets(self):
        """Rock over any open ground the spawns can't reach (the validator refuses it)."""
        seen = [[False] * self.w for _ in range(self.h)]
        q = deque([self.spawn])
        seen[self.spawn[1]][self.spawn[0]] = True
        while q:
            x, y = q.popleft()
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if (0 <= nx < self.w and 0 <= ny < self.h and not seen[ny][nx]
                        and self.get(nx, ny) != ROCK):
                    seen[ny][nx] = True
                    q.append((nx, ny))
        for y in range(self.h):
            for x in range(self.w):
                if not seen[y][x] and self.get(x, y) != ROCK:
                    self.set(x, y, ROCK)


def crossroads() -> dict:
    """48×48: four walls box in a central plaza, entered only through narrow gaps."""
    b = Board("crossroads", 48, 48, seed=11)
    c = 24
    for x0, y0, x1, y1 in ((14, 14, 33, 14), (14, 14, 14, 33)):
        b.wall(x0, y0, x1, y1, gaps=((0.5, 0.09),), thick=2)
    b.node_field(c - 2, c - 2, 3, spread=3)
    b.node_field(c - 7, c + 4, 2)
    for _ in range(8):
        b.blob(b.rng.randrange(4, 44), b.rng.randrange(4, 22), 6, ROCK)
    b.blob(9, 30, 30, NEBULA)
    b.blob(30, 8, 30, NEBULA)
    b.scatter_nodes(45)
    return b.finish()


def nebula_reach() -> dict:
    """64×48: wide bands of nebula across the middle; fights happen half-blind."""
    b = Board("nebula_reach", 64, 48, seed=23)
    for i in range(6):
        b.blob(10 + i * 9, 20 + b.rng.randrange(-4, 5), 40, NEBULA)
    for _ in range(10):
        b.blob(b.rng.randrange(6, 58), b.rng.randrange(6, 24), 5, ROCK)
    b.node_field(32, 24, 3, spread=3)
    b.node_field(16, 30, 2)
    b.node_field(48, 12, 2)
    b.scatter_nodes(50)
    return b.finish()


def the_spine() -> dict:
    """80×40: a long board split by a rock spine with three passes; flanking takes time."""
    b = Board("the_spine", 80, 40, seed=37)
    b.wall(1, 19, 39, 19, gaps=((0.35, 0.04), (1.0, 0.04)), thick=2)
    for x in range(10, 75, 12):
        b.blob(x, b.rng.randrange(5, 15), 7, ROCK)
    b.blob(40, 13, 30, NEBULA)
    b.blob(22, 29, 30, NEBULA)
    b.node_field(40, 26, 2, spread=2)
    b.node_field(25, 10, 3)
    b.node_field(60, 31, 2)
    b.scatter_nodes(55)
    return b.finish()


def archipelago() -> dict:
    """96×96: islands of rock, each ringed by nodes; many lanes, no one front."""
    b = Board("archipelago", 96, 96, seed=41)
    for _ in range(22):
        x, y = b.rng.randrange(10, 86), b.rng.randrange(10, 48)
        if not b.far_from_spawns(x, y, 14):
            continue
        b.blob(x, y, b.rng.randrange(10, 25), ROCK)
        b.node_field(x, y, 2, spread=4)
    for _ in range(6):
        b.blob(b.rng.randrange(10, 86), b.rng.randrange(10, 48), 18, NEBULA)
    b.node_field(48, 48, 3, spread=3)
    b.scatter_nodes(65)
    return b.finish()


def deep_field() -> dict:
    """128×128, the largest legal board: wide open space, resource fields far from home,
    nebula pockets. The budgets are sized for this (DESIGN §2.3)."""
    b = Board("deep_field", 128, 128, seed=53, spawn=3)
    for _ in range(30):
        b.blob(b.rng.randrange(8, 120), b.rng.randrange(8, 64), b.rng.randrange(6, 20), ROCK)
    for _ in range(10):
        b.blob(b.rng.randrange(8, 120), b.rng.randrange(8, 64), 25, NEBULA)
    for x, y in ((30, 30), (64, 64), (90, 40), (40, 95), (20, 70)):
        b.node_field(x, y, 4, spread=4)
    b.scatter_nodes(80)
    return b.finish()


MAPS = (crossroads, nebula_reach, the_spine, archipelago, deep_field)

if __name__ == "__main__":
    out = Path(__file__).resolve().parent / "data"
    for make in MAPS:
        m = make()
        path = out / f"{m['name']}.json"
        path.write_text(json.dumps(m, indent=2) + "\n")
        g = m["grid"]
        print(f"{path.name}: {len(g[0])}x{len(g)}, "
              f"{sum(r.count(NODE) for r in g)} nodes, {sum(r.count(NEBULA) for r in g)} nebula, "
              f"{sum(r.count(ROCK) for r in g)} rock")
