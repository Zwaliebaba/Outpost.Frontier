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


def main():
    write("asteroid.list", asteroid())
    write("orient.list", orient())
    write("strings.list", strings())
    write("groups.ini", groups())
    write("feelings.ini", feelings())
    write("reputation.list", reputation())
    zones()


if __name__ == "__main__":
    main()
