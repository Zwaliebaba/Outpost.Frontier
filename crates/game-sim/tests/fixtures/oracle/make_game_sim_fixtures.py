#!/usr/bin/env python3
"""Writes the synthetic inputs of the game-sim oracle suites (work package
P2.1) into this directory. Run it from anywhere; the output is committed.

    asteroid.list   x y z as f32 bit patterns (hex), one position per line
    orient.list     seed (decimal), freq (f32 hex), then 12 angles (f32 hex)
    strings.list    one string-cache request per line: "i <hex>" interned,
                    "t <hex>" temporary, "bay" the cargo-bay name,
                    "shutdown" StringShutdown
    groups.ini      [Group] blocks for Reputation::Startup
    feelings.ini    [Group] blocks with rep lines for Reputation::LoadFeelings
    reputation.list one query per line (see oracle_game_suites.cpp)
    zones/universe.ini, zones/systems/zt.ini
                    one system with a zone of every shape, for the zone suite
    zone_points.list
                    x y z as f32 bit patterns: the points and directions the
                    zone suite queries every zone with
    zone_queries/universe.ini, zone_queries/systems/zq.ini
                    one system with a zone of every shape (edge fractions
                    set) and the exclusion zones, for the zone_queries and
                    exclusion_zones suites (which also read zone_points.list)
    zone_segments.list
                    start and end, six f32 bit patterns per line
    zone_frustums.list
                    a frustum per line: rotation rows, apex, angle x,
                    angle y, distance (15 f32 bit patterns)
    zone_spheres.list
                    centre and radius, four f32 bit patterns per line
    exclusion.list  the exclusion suite's observers ("o x y z") and query
                    points ("q x y z"), f32 bit patterns
    spatial.list    the spatial suite's script: new, add, array, remove,
                    clear and the sphere, ray, frustum and zone-tree queries
                    (see oracle_vectors.rs spatial), floats as bit patterns
"""

import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def f32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def write(name, lines):
    with open(os.path.join(HERE, name), "w", newline="\n") as out:
        for line in lines:
            out.write(line + "\n")


def asteroid():
    values = [0.0, -0.0, 0.05, -0.05, 0.1, 1.0, -1.0, 1.9, 2.0, -2.0, 2.9, 127.9, 127.95,
              128.0, -128.0, -127.95, 255.9, 256.0, 1023.0, 1024.0, -1023.9, 65535.0,
              130943.9, 131071.9, 131072.0, -131072.0, 65408.0, 65535.9, 65536.0,
              1.0e9, -1.0e9, 3.0e9]
    lines = ["# x y z as f32 bits"]
    specials = [0x7FC00000, 0x7F800000, 0xFF800000, 0x00000001]
    for v in values:
        lines.append("%08x %08x %08x" % (f32(v), f32(-v * 0.5), f32(v * 0.25)))
        lines.append("%08x %08x %08x" % (f32(-v), f32(v), f32(-v)))
    for s in specials:
        lines.append("%08x %08x %08x" % (s, 0, 0))
        lines.append("%08x %08x %08x" % (0, s, 0))
    return lines


def orient():
    snap = [0.0, math.pi / 2, math.pi, 3 * math.pi / 2]
    odd = [0.1, -0.7, 2.5, 4.0]
    lines = ["# seed freq angles[3][4]"]
    seeds = [0, 1, 2, 7, 12345, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x00200401, 0x12345678]
    for seed in seeds:
        for freq in [-1.0, 0.0, 0.25, 0.5, 0.999, 1.0]:
            angles = snap * 3 if seed % 2 == 0 else snap + odd + snap
            lines.append("%d %08x %s" % (seed, f32(freq), " ".join("%08x" % f32(a) for a in angles)))
    return lines


def strings():
    def hexs(text):
        return text.encode("latin-1").hex()

    lines = ["# i <hex> interned, t <hex> temporary, bay, shutdown"]
    names = ["oracle_sc_HpMount", "oracle_sc_hpmount", "ORACLE_SC_HPMOUNT", "oracle_sc_other",
             "oracle_sc_HpMount", "oracle_sc_\xe9t\xe9", "oracle_sc_\xc9T\xc9", "bay", "BAY", "Bay"]
    for name in names:
        lines.append("i " + hexs(name))
        lines.append("t " + hexs(name))
    lines.append("i ")
    lines.append("t ")
    lines.append("bay")
    lines.append("i " + hexs("oracle_sc_\x00hidden"))
    lines.append("shutdown")
    lines.append("i " + hexs("oracle_sc_hpmount"))
    lines.append("i " + hexs("ORACLE_SC_HPMOUNT"))
    lines.append("i " + hexs("bay"))
    lines.append("bay")
    return lines


def groups():
    return [
        "[Group]",
        "nickname = li_n_grp",
        "ids_name = 196846",
        "ids_info = 66146",
        "ids_short_name = 196946",
        "",
        "[Group]",
        "nickname = li_p_grp",
        "ids_name = 196847.9",
        "ids_info = 66147",
        "rep = 0.5, li_n_grp",
        "",
        "[Group]",
        "nickname = fc_q_grp",
        "ids_name = -5",
        "",
        "[Group]",
        "nickname = a_very_long_nickname_grp",
        "ids_name = 7",
        "ids_info = 8",
        "ids_short_name = 9",
        "",
        "[Group]",
        "nickname = LI_N_GRP",
        "ids_name = 11",
        "ids_info = 12",
        "ids_short_name = 13",
        "",
        "[Other]",
        "nickname = ignored",
    ]


def feelings():
    return [
        "[Group]",
        "nickname = li_n_grp",
        "rep = 0.25, li_p_grp",
        "rep = -0.75, fc_q_grp",
        "rep = 0.9, li_p_grp",
        "rep = 0.1, nobody_grp",
        "",
        "[Group]",
        "nickname = li_p_grp",
        "rep = -1, li_n_grp",
        "",
        "[Group]",
        "nickname = fc_q_grp",
        "ids_name = 1",
        "",
        "[Group]",
        "nickname = nobody_grp",
        "rep = 1, li_n_grp",
    ]


def reputation():
    def h(name):
        return name.encode().hex() if name else "-"

    names = ["li_n_grp", "LI_P_GRP", "fc_q_grp", "a_very_long_nickname_grp", "a_very_long_nic",
             "nobody_grp", "", "fc_ln_grp"]
    lines = ["# id <name>; feel <from> <to>; affect <from> <to> <delta f32>; story <name>;",
             "# set <name> <other>:<f32> ... -- names as hex, '-' for the empty name"]
    for name in names:
        lines.append("id " + h(name))
    pairs = ["li_n_grp", "li_p_grp", "fc_q_grp", "nobody_grp", ""]
    for a in pairs:
        for b in pairs:
            lines.append("feel %s %s" % (h(a), h(b)))
    deltas = [0.25, 0.5, 0.5, -3.0, float("nan"), 0.0, -0.0, 1e-8]
    for delta in deltas:
        lines.append("affect %s %s %08x" % (h("li_n_grp"), h("li_p_grp"), f32(delta)))
        lines.append("affect %s %s %08x" % (h("fc_q_grp"), h("li_n_grp"), f32(delta)))
    lines.append("affect %s %s %08x" % (h("nobody_grp"), h("li_n_grp"), f32(0.5)))
    lines.append("affect %s %s %08x" % (h("li_n_grp"), h("nobody_grp"), f32(0.5)))
    for name in ["fc_ln_grp", "fc_uk_grp", "fc_q_grp", "li_n_grp", ""]:
        lines.append("story " + h(name))
    lines.append("set %s %s:%08x %s:%08x %s:%08x" % (h("li_p_grp"), h("li_n_grp"), f32(0.75),
                                                   h("fc_q_grp"), f32(-0.5), h("li_n_grp"), f32(0.1)))
    lines.append("set %s %s:%08x" % (h("new_grp"), h("li_n_grp"), f32(0.3)))
    lines.append("set %s" % h("li_n_grp"))
    return lines


def zones():
    os.makedirs(os.path.join(HERE, "zones", "systems"), exist_ok=True)
    universe = [
        "[System]",
        "nickname = zt",
        "msg_id_prefix = gcs_refer_system_zt",
        "file = systems\\zt.ini",
        "pos = 1, 2",
    ]
    shapes = [
        ("z_sphere", "SPHERE", "1000", "0, 0, 0", None),
        ("z_ellipsoid", "ELLIPSOID", "3000, 1000, 2000", "5000, 0, 0", None),
        ("z_ellipsoid_rot", "ELLIPSOID", "3000, 1000, 2000", "-5000, 100, 0", "0, 45, 0"),
        ("z_box", "BOX", "2000, 1000, 4000", "0, 0, 8000", None),
        ("z_box_rot", "BOX", "2000, 1000, 4000", "0, 3000, -8000", "30, 0, 60"),
        ("z_cylinder", "CYLINDER", "1500, 3000", "10000, 0, 10000", None),
        ("z_cylinder_rot", "CYLINDER", "1500, 3000", "-10000, 0, 10000", "90, 0, 0"),
        ("z_ring", "RING", "4000, 2000, 1000", "10000, 0, -10000", None),
        ("z_ring_rot", "RING", "4000, 2000, 1000", "-10000, 0, -10000", "0, 0, 33"),
        ("z_none", "WEDGE", "100", "20000, 0, 0", None),
    ]
    system = []
    for name, shape, size, pos, rotate in shapes:
        system += ["[Zone]", "nickname = " + name, "pos = " + pos, "shape = " + shape, "size = " + size]
        if rotate:
            system.append("rotate = " + rotate)
        system.append("")
    write(os.path.join("zones", "universe.ini"), universe)
    write(os.path.join("zones", "systems", "zt.ini"), system)

    centres = [(0, 0, 0), (5000, 0, 0), (-5000, 100, 0), (0, 0, 8000), (0, 3000, -8000),
               (10000, 0, 10000), (-10000, 0, 10000), (10000, 0, -10000), (-10000, 0, -10000)]
    offsets = [(0, 0, 0), (0.005, 0, 0), (300, 200, -100), (900, 0, 0), (0, 1400, 0), (2500, 0, 0),
               (-1700, 600, 1200), (0, 0, 3100), (3000, 0, 0), (12000, -400, 0), (2950, 0, 0),
               (1000, 450, 900)]
    lines = ["# x y z as f32 bits; lines starting with d are directions"]
    for c in centres:
        for o in offsets:
            lines.append("%08x %08x %08x" % (f32(c[0] + o[0]), f32(c[1] + o[1]), f32(c[2] + o[2])))
    for d in [(1, 0, 0), (0, 1, 0), (0, 0, 1), (0.6, 0.8, 0), (0.48, 0.6, 0.64), (0, 0.00001, 1),
              (-0.577350269, 0.577350269, -0.577350269)]:
        lines.append("d %08x %08x %08x" % (f32(d[0]), f32(d[1]), f32(d[2])))
    write("zone_points.list", lines)


CENTRES = [(0, 0, 0), (5000, 0, 0), (-5000, 100, 0), (0, 0, 8000), (0, 3000, -8000),
           (10000, 0, 10000), (-10000, 0, 10000), (10000, 0, -10000), (-10000, 0, -10000),
           (20000, 0, 0)]


def hexes(values):
    return " ".join("%08x" % f32(v) for v in values)


def zone_queries():
    os.makedirs(os.path.join(HERE, "zone_queries", "systems"), exist_ok=True)
    universe = [
        "[System]",
        "nickname = zq",
        "msg_id_prefix = gcs_refer_system_zq",
        "file = systems\\zq.ini",
    ]
    zones = [
        ("q_sphere", "SPHERE", "1000", "0, 0, 0", None, "0.25"),
        ("q_ellipsoid", "ELLIPSOID", "3000, 1000, 2000", "5000, 0, 0", None, "0.5"),
        ("q_ellipsoid_rot", "ELLIPSOID", "3000, 1000, 2000", "-5000, 100, 0", "0, 45, 0", "0.1"),
        ("q_box", "BOX", "2000, 1000, 4000", "0, 0, 8000", None, "0.3"),
        ("q_box_rot", "BOX", "2000, 1000, 4000", "0, 3000, -8000", "30, 0, 60", None),
        ("q_cylinder", "CYLINDER", "1500, 3000", "10000, 0, 10000", None, "0.2"),
        ("q_cylinder_rot", "CYLINDER", "1500, 3000", "-10000, 0, 10000", "90, 0, 0", "0.6"),
        ("q_ring", "RING", "4000, 2000, 1000", "10000, 0, -10000", None, "0.4"),
        ("q_ring_rot", "RING", "4000, 2000, 1000", "-10000, 0, -10000", "0, 0, 33", "0.05"),
        ("q_none", "WEDGE", "100", "20000, 0, 0", None, None),
        ("x_a", "SPHERE", "200", "300, 0, 0", None, "0.5"),
        ("x_b", "ELLIPSOID", "200, 100, 150", "-400, 100, 0", None, "0.2"),
        ("x_c", "BOX", "100, 100, 100", "0, -500, 0", None, None),
        ("x_d", "SPHERE", "300", "5500, 0, 0", None, "0.3"),
        ("x_far", "SPHERE", "100", "30000, 30000, 0", None, "0.1"),
        ("x_edge", "SPHERE", "400", "0, 800, 0", None, "0.3"),
    ]
    extras = {
        "q_sphere": ["property_flags = 128"],
        "q_ellipsoid": ["property_flags = 1"],
        "q_box": ["property_flags = 32768"],
        "q_cylinder": ["damage = 3"],
        "x_a": ["property_flags = 64"],
        "x_b": ["property_flags = 2"],
        "x_edge": ["property_flags = 4", "damage = 5"],
    }
    system = []
    for name, shape, size, pos, rotate, edge in zones:
        system += ["[Zone]", "nickname = " + name, "pos = " + pos, "shape = " + shape, "size = " + size]
        if rotate:
            system.append("rotate = " + rotate)
        if edge:
            system.append("edge_fraction = " + edge)
        system += extras.get(name, [])
        system.append("")
    write(os.path.join("zone_queries", "universe.ini"), universe)
    write(os.path.join("zone_queries", "systems", "zq.ini"), system)

    rel = [((-20000, 0, 0), (20000, 0, 0)), ((0, -20000, 10), (0, 20000, 10)),
           ((-5000, -3000, -4000), (5000, 3000, 4000)), ((-20000, 9000, 0), (20000, 9000, 0)),
           ((-20000, 0, 0), (-15000, 0, 0)), ((0, 0, 0), (20000, 0, 0)),
           ((300, -6000, 200), (300, 6000, 200)), ((3000, 0, -20000), (3000, 0, 20000))]
    lines = ["# start x y z, end x y z as f32 bits"]
    for c in CENTRES:
        for a, b in rel:
            lines.append(hexes([c[0] + a[0], c[1] + a[1], c[2] + a[2], c[0] + b[0], c[1] + b[1], c[2] + b[2]]))
    write("zone_segments.list", lines)

    r = 0.70710677
    rotations = [
        [1, 0, 0, 0, 1, 0, 0, 0, 1],
        [0, 0, 1, 0, 1, 0, -1, 0, 0],
        [-1, 0, 0, 0, 1, 0, 0, 0, -1],
        [0, 0, -1, 0, 1, 0, 1, 0, 0],
        [1, 0, 0, 0, 0, -1, 0, 1, 0],
        [r, 0, r, 0, 1, 0, -r, 0, r],
    ]
    apexes = [(0, 0, 30000), (30000, 0, 0), (0, 30000, 0), (0, 0, 0), (5000, 0, -3000), (-10000, 0, 20000)]
    angles = [(1.0, 0.75), (0.3, 0.2), (2.0, 1.5)]
    lines = ["# rotation rows (9), apex (3), angle x, angle y, distance as f32 bits"]
    for rot in rotations:
        for apex in apexes:
            for ax, ay in angles:
                for distance in [40000, 9000]:
                    lines.append(hexes(rot + list(apex) + [ax, ay, distance]))
    write("zone_frustums.list", lines)

    rel = [((0, 0, 0), 10), ((2500, 0, 0), 100), ((2500, 0, 0), 1000), ((0, 5000, 0), 3000),
           ((9000, 9000, 9000), 50), ((1600, 0, 0), 200), ((0, 0, 2600), 700), ((3500, 0, 0), 600)]
    lines = ["# centre x y z, radius as f32 bits"]
    for c in CENTRES:
        for o, radius in rel:
            lines.append(hexes([c[0] + o[0], c[1] + o[1], c[2] + o[2], radius]))
    write("zone_spheres.list", lines)

    lines = ["# o x y z: an observer the managers sort for; q x y z: a query point"]
    observers = [(0, 0, 0), (2000, 0, 0), (-3000, 500, 0), (0, 0, 40000), (5600, 0, 0), (30000, 29000, 0)]
    queries = [(0, 0, 0), (300, 0, 0), (350, 50, 0), (-400, 100, 0), (-420, 120, 10), (0, -500, 0),
               (0, -480, 30), (0, 800, 0), (0, 700, 100), (0, 1150, 0), (5500, 0, 0), (5650, 0, 0),
               (30000, 30000, 0), (900, 0, 0), (2000, 0, 0)]
    for o in observers:
        lines.append("o " + hexes(o))
        for q in queries:
            lines.append("q " + hexes(q))
    write("exclusion.list", lines)


class Lcg:
    def __init__(self, seed):
        self.state = seed

    def unit(self):
        self.state = (self.state * 1103515245 + 12345) & 0xFFFFFFFF
        return ((self.state >> 8) & 0xFFFF) / 65535.0

    def range(self, low, high):
        return low + (high - low) * self.unit()


def spatial():
    rng = Lcg(2026)
    lines = ["# new <type>; add <obj> x y z r; array obj:x:y:z:r ...; remove <obj>; clear;",
             "# sphere x y z r; ray ox oy oz dx dy dz length; frustum <15 words>; tree x y z r"]

    def entry(spread=20000.0):
        return (rng.range(-spread, spread), rng.range(-spread * 0.2, spread * 0.2),
                rng.range(-spread, spread), rng.range(10.0, 3000.0))

    def sphere():
        x, y, z, _ = entry(22000.0)
        lines.append("sphere " + hexes([x, y, z, rng.range(0.0, 6000.0)]))

    def ray():
        x, y, z, _ = entry(25000.0)
        d = [rng.range(-1.0, 1.0), rng.range(-0.3, 0.3), rng.range(-1.0, 1.0)]
        n = math.sqrt(sum(c * c for c in d)) or 1.0
        lines.append("ray " + hexes([x, y, z] + [c / n for c in d] + [rng.range(1000.0, 60000.0)]))

    def frustum():
        yaw = rng.range(0.0, 2 * math.pi)
        c, s_ = math.cos(yaw), math.sin(yaw)
        rot = [c, 0, s_, 0, 1, 0, -s_, 0, c]
        x, y, z, _ = entry(20000.0)
        lines.append("frustum " + hexes(rot + [x, y, z, rng.range(0.3, 2.0), rng.range(0.2, 1.5),
                                               rng.range(2000.0, 40000.0)]))

    def queries(spheres, rays, frustums):
        for _ in range(spheres):
            sphere()
        for _ in range(rays):
            ray()
        for _ in range(frustums):
            frustum()

    def add(obj, e):
        lines.append("add %d %s" % (obj, hexes(e)))

    def array(first, entries):
        lines.append("array " + " ".join("%d:%s" % (first + i, hexes(e).replace(" ", ":"))
                                         for i, e in enumerate(entries)))

    # A: one by one, with removals.
    lines.append("new BSP")
    for obj in range(1, 121):
        add(obj, entry())
        if obj % 20 == 0:
            queries(6, 4, 1)
    for obj in range(3, 120, 8):
        lines.append("remove %d" % obj)
    queries(10, 8, 6)
    lines.append("remove 9999")
    lines.append("clear")
    queries(2, 2, 1)
    for obj in range(200, 205):
        add(obj, entry())
    queries(4, 4, 1)

    # B: built from an array, then added to; a second array is dropped.
    lines.append("new bsp")
    array(300, [entry() for _ in range(150)])
    queries(12, 10, 6)
    for obj in range(500, 530):
        add(obj, entry())
    queries(8, 8, 2)
    array(600, [entry() for _ in range(5)])
    queries(3, 0, 0)

    # C: the linear partition.
    lines.append("new Linear")
    array(700, [entry() for _ in range(40)])
    for obj in range(800, 810):
        add(obj, entry())
    lines.append("remove 805")
    queries(8, 2, 2)
    lines.append("clear")
    queries(1, 0, 0)

    # D: coincident and collinear positions. (Nine coincident entries would
    # make the reference split the same leaf for ever.)
    lines.append("new BSP")
    for obj in range(900, 906):
        add(obj, (100.0, 50.0, -30.0, 5.0))
    for obj in range(920, 930):
        add(obj, (float(obj - 920) * 10.0, 0.0, 0.0, 2.0))
    lines.append("sphere " + hexes([100.0, 50.0, -30.0, 1.0]))
    lines.append("sphere " + hexes([0.0, 0.0, 0.0, 500.0]))
    lines.append("ray " + hexes([-100.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1000.0]))
    lines.append("ray " + hexes([100.0, 50.0, -100.0, 0.0, 0.0, 1.0, 1000.0]))

    # E: a root leaf from a small array, then overflowed by adds.
    lines.append("new BSP")
    array(1000, [entry(3000.0) for _ in range(5)])
    queries(3, 3, 1)
    for obj in range(1010, 1030):
        add(obj, entry(3000.0))
        if obj % 5 == 0:
            queries(2, 2, 0)

    # F: names without a partition.
    lines.append("new QuadTree")
    lines.append("new Octree")

    # The zone tree of system zq.
    for c in CENTRES + [(300, 0, 0), (0, 800, 0), (-400, 100, 0), (5500, 0, 0), (0, -500, 0)]:
        for o, r in [((0, 0, 0), 0.0), ((150, 0, 0), 10.0), ((0, 700, 0), 300.0), ((2000, 0, 0), 2500.0)]:
            lines.append("tree " + hexes([c[0] + o[0], c[1] + o[1], c[2] + o[2], r]))
    write("spatial.list", lines)


def main():
    write("asteroid.list", asteroid())
    write("orient.list", orient())
    write("strings.list", strings())
    write("groups.ini", groups())
    write("feelings.ini", feelings())
    write("reputation.list", reputation())
    zones()
    zone_queries()
    spatial()


if __name__ == "__main__":
    main()
