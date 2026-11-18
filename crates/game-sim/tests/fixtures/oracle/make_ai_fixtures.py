#!/usr/bin/env python3
"""Writes the synthetic inputs of the AI oracle suites (work package P2.5)
into this directory. Run it from anywhere; the output is committed. The C++
side is CppSource/Oracle/oracle_ai.cpp, the Rust side
crates/game-sim/tests/oracle_ai.rs.

    ai_geometry.list   66 f32 bit patterns (hex) per line: origin, dir,
                       center, radius, transform A (9 + 3), box A
                       (min x, max x, min y, max y, min z, max z),
                       transform B, box B, capsule (radius, start, dir),
                       point, a1, b0, b1, radius b
    ai_steer.list      a seed (decimal), then 28 f32 bit patterns:
                       reference (9), forward (3), hint (3), threshold,
                       angle, matrix B (9), spread, bank
    ai_state_graph.list
                       the state-graph files of ai_state_graph/, in load
                       order (a name with no file is a missing file)
    ai_state_graph/    the synthetic state-graph files
    ai_behavior_names.list
                       behaviour names to look up, one per line
    ai_combat.list     28 f32 bit patterns per line: seed, munition kind,
                       auto turret (the three integers as floats), dt,
                       damage level, the eleven gun-group interval
                       parameters (CppSource/include/oracle_ai.h), a
                       threshold timer (active, threshold, duration,
                       accumulator), a gun slot (burst, no burst,
                       interval, phase), the aim refresh timer and a
                       line-of-fire cache slot (age, refresh timer, needs
                       refresh); the C++ side is
                       CppSource/Oracle/oracle_ai_combat.cpp
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
    os.makedirs(os.path.dirname(path), exist_ok=True)
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

    def box(self, scale):
        out = []
        for _ in range(3):
            a = self.uniform(-scale, scale)
            b = a + self.uniform(0.1, 2 * scale)
            out += [a, b]
        return out


IDENTITY = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]


def hexes(values):
    return " ".join("%08x" % f32(v) for v in values)


def geometry():
    g = Gen(0x5EED2500)
    lines = ["# 66 f32 per line; see make_ai_fixtures.py"]
    for case in range(160):
        near = case % 4 != 3
        origin = g.vec(60.0)
        center = g.vec(20.0) if near else g.vec(200.0)
        # Aim most rays at the shapes so that both hit and miss paths run.
        if case % 3 != 2:
            target = [c + o for c, o in zip(center, g.vec(5.0))]
            d = [t - o for t, o in zip(target, origin)]
            n = math.sqrt(sum(c * c for c in d)) or 1.0
            direction = [c / n for c in d]
        else:
            direction = g.unit()
        if case == 0:
            direction = [0.0, 0.0, 0.0]
        if case == 1:
            direction = [0.0, 0.0, 1.0]
            origin = [0.0, 0.0, -50.0]
            center = [0.0, 0.0, 0.0]
        radius = g.uniform(0.5, 30.0)
        rot_a = IDENTITY if case % 7 == 0 else g.rotation()
        xform_a = rot_a + center
        box_a = g.box(15.0)
        rot_b = IDENTITY if case % 5 == 0 else g.rotation()
        xform_b = rot_b + [c + o for c, o in zip(center, g.vec(25.0))]
        box_b = g.box(15.0)
        cap_dir = g.vec(40.0)
        if case % 11 == 0:
            cap_dir = [d * 30.0 for d in direction]  # parallel to the ray
        if case == 2:
            cap_dir = [0.0, 0.0, 0.0]
        capsule = [g.uniform(0.5, 10.0)] + [c + o for c, o in zip(center, g.vec(5.0))] + cap_dir
        point = [c + o for c, o in zip(center, g.vec(20.0))]
        a1 = [o + d * g.uniform(0.0, 120.0) for o, d in zip(origin, direction)]
        b0 = g.vec(60.0)
        b1 = [b + o for b, o in zip(b0, g.vec(30.0))]
        rb = g.uniform(0.5, 20.0)
        values = (origin + direction + center + [radius] + xform_a + box_a + xform_b + box_b
                  + capsule + point + a1 + b0 + b1 + [rb])
        assert len(values) == 66
        lines.append(hexes(values))
    return lines


def steer():
    g = Gen(0x5EED2501)
    lines = ["# seed, then 28 f32; see make_ai_fixtures.py"]
    for case in range(120):
        ref = IDENTITY if case % 6 == 0 else g.rotation()
        forward = g.vec(10.0)
        hint = g.unit()
        if case % 9 == 0:
            hint = [-c for c in forward]  # parallel to the forward axis
        if case == 1:
            forward = [0.0, 0.0, 0.0]
        if case == 2:
            forward = [0.001, 0.0, 0.0]
        if case % 13 == 5:
            forward = [ref[1], ref[4], ref[7]]  # along the reference's column 1
        threshold = [0.999, 0.5, 0.99, 1.0][case % 4]
        angle = g.uniform(-3.5, 3.5)
        mat_b = g.rotation() if case % 8 else IDENTITY
        spread = g.uniform(0.0, 0.5)
        bank = g.uniform(-0.5, 1.0) if case % 3 else 0.0
        seed = g.u32() & 0x7FFFFFFF
        values = ref + forward + hint + [threshold, angle] + mat_b + [spread, bank]
        assert len(values) == 28
        lines.append("%u %s" % (seed, hexes(values)))
    return lines


def graph_text(graphs, behaviors=21, count=None):
    text = ["state_graph_count %d" % (len(graphs) if count is None else count),
            "behavior_count %d" % behaviors, ""]
    for name, kind, rows in graphs:
        text.append(name)
        text.append(kind)
        text.append("")
        for row in rows:
            text.append(" ".join(row) + " ")
        text.append("")
    return text


def rows_of(g, density):
    rows = []
    for _ in range(21):
        row = []
        for _ in range(21):
            if g.uniform(0.0, 1.0) < density:
                row.append("%.2f" % g.uniform(0.0, 1.0))
            else:
                row.append("0.00")
        rows.append(row)
    return rows


def state_graphs():
    g = Gen(0x5EED2502)
    files = {}
    files["wrong_count.db"] = graph_text([("FIGHTER", "LEADER", rows_of(g, 0.3))], behaviors=20)
    files["normal.db"] = graph_text([
        ("NOTHING", "LEADER", rows_of(g, 0.0)),
        ("FIGHTER", "LEADER", rows_of(g, 0.3)),
        ("FIGHTER", "ESCORT", rows_of(g, 0.3)),
        ("Gunboat", "LEADER", rows_of(g, 0.5)),
        ("gunboat", "ESCORT", rows_of(g, 0.5)),
        ("CRUISER", "XESCORTY", rows_of(g, 1.0)),
    ])
    odd = rows_of(g, 0.4)
    odd[0][0] = "1e-2"
    odd[0][1] = "-0.5"
    odd[0][2] = "+.25"
    odd[0][3] = "7."
    odd[0][4] = "0.5.5"
    odd[1][0] = "12345678901"
    long_name = "L" * 70
    very_long = "V" * 300
    files["tokens.db"] = graph_text([
        (long_name, "ESCORT", odd),
        (very_long, "LEADER", rows_of(g, 0.2)),
        ("AFTER", "LEADER", rows_of(g, 0.2)),
    ])
    junk = rows_of(g, 0.5)
    junk[2][5] = "x0.5"
    files["junk.db"] = graph_text([("JUNK", "LEADER", junk), ("NEXT", "ESCORT", rows_of(g, 0.5))])
    truncated = graph_text([("FIRST", "LEADER", rows_of(g, 0.5))], count=3)
    files["truncated.db"] = truncated + ["SECOND", "ESCORT", "0.25 0.5"]
    for name, text in files.items():
        write(os.path.join("ai_state_graph", name), text)
    # A missing file and the wrong behaviour count come first: the reference
    # keeps its old graph count after either, and with no graphs loaded yet
    # that count is still 0.
    order = ["missing.db", "wrong_count.db", "normal.db", "tokens.db", "junk.db", "truncated.db"]
    return ["# state-graph files in load order"] + order


def behavior_names():
    names = ["NULL", "null", "Buzz", "GOTO", "trail", "Flee", "Evade", "Idle", "Dock",
             "Launch", "InstantTradeLane", "instanttradelane", "Formation",
             "Large Ship (move)", "large ship (move)", "Cruise", "Strafe", "Guide",
             "Face", "Loot", "Follow", "DrasticEvade", "FreeFlight", "Delay",
             "", "Nothing", "Dock ", "BEHAVIOR_ID_NONE"]
    return ["# behaviour names, one per line"] + names


def combat():
    g = Gen(0x5EED2502)
    lines = ["# 28 f32 per line; see make_ai_fixtures.py"]
    # Kind 4 (the cruise disruptor) reads the owner's personality; 6 is
    # outside the switch.
    kinds = [0, 0, 1, 2, 3, 5, 6]
    for case in range(140):
        seed = g.u32() & 0xFFFFFF
        kind = kinds[case % len(kinds)]
        turret = 1 if case % 3 == 1 else 0
        dt = [0.0, 0.05, 0.1, 0.25, 1.0][case % 5] if case % 7 else g.uniform(0.0, 2.0)
        level = g.uniform(-10.0, 200.0) if case % 4 else 0.0
        params = [g.uniform(0.0, 6.0) for _ in range(11)]
        if case % 10 == 3:
            params = [0.5, 0.2, 0.5, 4.0, 0.3, 2.0, 4.0, 2.0, 0.3, 3.0, 0.5]
        active = 1.0 if case % 2 else 0.0
        threshold = g.uniform(0.0, 150.0)
        duration = g.uniform(0.0, 3.0)
        accumulator = g.uniform(0.0, 3.0)
        burst = g.uniform(0.0, 5.0)
        no_burst = g.uniform(0.0, 3.0)
        interval = g.uniform(0.0, 1.0) if case % 6 else [0.0, -0.5][case % 12 // 6]
        phase = g.uniform(-3.0, 5.0)
        aim = g.uniform(-1.0, 2.0)
        age = g.uniform(0.0, 11.0)
        refresh = g.uniform(0.0, 1.2)
        needs = float(case % 2 == 0)
        values = [float(seed), float(kind), float(turret), dt, level] + params + [
            active, threshold, duration, accumulator, burst, no_burst, interval, phase,
            aim, age, refresh, needs]
        assert len(values) == 28
        lines.append(hexes(values))
    return lines


def main():
    write("ai_geometry.list", geometry())
    write("ai_steer.list", steer())
    write("ai_state_graph.list", state_graphs())
    write("ai_behavior_names.list", behavior_names())
    write("ai_combat.list", combat())


if __name__ == "__main__":
    main()
