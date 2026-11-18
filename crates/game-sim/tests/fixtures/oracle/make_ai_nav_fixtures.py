#!/usr/bin/env python3
"""Writes the synthetic inputs of the AI navigation oracle suites (work
package P2.5: intercepts, obstacle avoidance, the movement ideas' helpers)
into this directory. Run it from anywhere; the output is committed. The C++
side is CppSource/Oracle/oracle_ai_nav.cpp, the Rust side
crates/game-sim/tests/oracle_ai_nav.rs. A sibling of make_ai_fixtures.py.

    ai_nav.list   97 32-bit words (hex) per line:
                  object A (34), object B (34), point (3), threshold,
                  cosine threshold, angular speed, linear speed, three
                  slots (timer, point, direction; 7 each), extra check
                  (0 or 1).
                  An object is position (3), rotation (9, row order),
                  velocity (3), angular velocity (3), radius, centre (3),
                  extents min (3), max (3), then the words type flags,
                  extents fail (0 or 1), and the floats throttle, angular
                  speed XY, speed, atmosphere range. Every float is its
                  f32 bit pattern; the type and the two flags are plain
                  integers in hex.
"""

import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def f32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def rf(value):
    """The f32 nearest to value, as a Python float."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def write(name, lines):
    path = os.path.join(HERE, name)
    with open(path, "w", newline="\n") as out:
        for line in lines:
            out.write(line + "\n")


class Gen:
    """Marsaglia's xorshift (13, 17, 5), as the other fixture scripts use."""

    def __init__(self, seed):
        self.state = seed or 1

    def u32(self):
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x
        return x

    def uniform(self, lo, hi):
        return lo + (hi - lo) * (self.u32() / 4294967296.0)

    def pick(self, items):
        return items[self.u32() % len(items)]

    def vec(self, scale):
        return [self.uniform(-scale, scale) for _ in range(3)]

    def unit(self):
        v = self.vec(1.0)
        n = math.sqrt(sum(c * c for c in v)) or 1.0
        return [c / n for c in v]

    def rotation(self):
        q = [self.uniform(-1.0, 1.0) for _ in range(4)]
        n = math.sqrt(sum(c * c for c in q)) or 1.0
        w, x, y, z = (c / n for c in q)
        return [
            1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
            2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
            2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y),
        ]


TYPES = [0x0, 0x1, 0x4, 0x20, 0x200, 0x10000, 0x20000, 0x30000, 0x20C150,
         0x10200, 0x2000_0000]


def obj(g, near=None):
    """34 words: one object record."""
    pos = g.vec(300.0) if near is None else [c + d for c, d in zip(near, g.vec(150.0))]
    rot = g.rotation() if g.u32() % 6 else [1, 0, 0, 0, 1, 0, 0, 0, 1]
    vel = g.vec(g.pick([0.0, 0.5, 20.0, 200.0]))
    ang = g.vec(g.pick([0.0, 0.05, 1.0]))
    radius = g.pick([0.0, 1.0, g.uniform(1.0, 60.0), g.uniform(1.0, 200.0)])
    centre = [c + d for c, d in zip(pos, g.vec(g.pick([0.0, 2.0])))]
    shape = g.u32() % 4
    half = [g.uniform(1.0, 40.0) for _ in range(3)]
    if shape == 1:      # flat
        half[1] *= 0.1
    elif shape == 2:    # tall
        half[0] *= 0.1
        half[2] *= 0.1
    lo = [-h * g.uniform(0.5, 1.5) for h in half]
    hi = [h * g.uniform(0.5, 1.5) for h in half]
    floats = [rf(v) for v in pos + rot + vel + ang + [radius] + centre + lo + hi]
    words = [f32(v) for v in floats]
    words.append(g.pick(TYPES))
    words.append(1 if g.u32() % 7 == 0 else 0)
    words += [f32(rf(v)) for v in (
        g.uniform(0.0, 1.0),
        g.pick([0.0, 1.0, g.uniform(0.2, 3.0)]),
        g.pick([0.0, 80.0, g.uniform(10.0, 400.0)]),
        g.pick([0.0, g.uniform(0.0, 300.0)]),
    )]
    assert len(words) == 34
    return words, floats


def local_to_world(rot, pos, v):
    return [pos[r] + sum(rot[r * 3 + c] * v[c] for c in range(3)) for r in range(3)]


def nav():
    g = Gen(0x5EED2510)
    lines = ["# 97 words per line; see make_ai_nav_fixtures.py"]
    for case in range(240):
        a, af = obj(g)
        b, bf = obj(g, near=af[0:3] if case % 2 else None)
        angular = g.pick([0.0, 1.0, g.uniform(0.1, 2.0)])
        linear = g.pick([0.0, 50.0, g.uniform(5.0, 300.0)])
        if case % 3 == 0 and angular != 0.0 and linear != 0.0:
            # Inside one of A's turn circles.
            radius = linear / angular
            side = g.pick([1.0, -1.0])
            local = [side * radius * g.uniform(0.2, 1.8), g.uniform(-5.0, 5.0),
                     g.uniform(-radius, radius)]
            point = local_to_world(af[3:12], af[19:22], local)
        else:
            point = [c + d for c, d in zip(af[0:3], g.vec(g.pick([5.0, 100.0, 400.0])))]
        params = point + [
            g.pick([0.0, 10.0, g.uniform(0.0, 400.0)]),
            g.uniform(-1.0, 1.0),
            angular,
            linear,
        ]
        slots = []
        for index in range(3):
            if index < 2 and case % 4 == 1:
                # A capsule whose middle is near B, wide enough that the ray toward B
                # leaves it about where B is: NudgeObstaclePathPoint's point arms.
                offset = [c * g.uniform(5.0, 50.0) for c in g.unit()]
                length = math.sqrt(sum(c * c for c in offset))
                seg = g.vec(60.0)
                mid = [p + o for p, o in zip(bf[0:3], offset)]
                start = [m - 0.5 * d for m, d in zip(mid, seg)]
                slots += [length * g.uniform(0.9, 1.6)] + start + seg
                continue
            timer = g.pick([0.0, -0.25, g.uniform(0.0, 1.0), g.uniform(1.0, 30.0)])
            slots += [timer] + [c + d for c, d in zip(af[0:3], g.vec(200.0))] + g.vec(150.0)
        words = a + b + [f32(rf(v)) for v in params + slots] + [g.u32() & 1]
        assert len(words) == 97
        lines.append(" ".join("%08x" % w for w in words))
    return lines


def main():
    write("ai_nav.list", nav())


if __name__ == "__main__":
    main()
