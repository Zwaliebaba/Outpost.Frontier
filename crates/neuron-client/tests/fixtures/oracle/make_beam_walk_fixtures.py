"""Writes beam_walk.list, the P5.6 beam lifecycle oracle inputs (CppSource/Oracle/oracle_beam.cpp).

One case per line, operations separated by " ; ", the floats as the hex of their f32.
Run from this directory: python make_beam_walk_fixtures.py
"""

import struct


def h(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def floats(values):
    return ",".join(h(v) for v in values)


ARCH = (
    [1.0, 0.2, 0.1, 0.9, 0.5, 0.3, 1.0, 1.0, 0.8, 0.6, 0.6, 0.6, 1.0, 0.4, 0.2, 0.3, 0.3, 0.9]
    + [1.0, 0.7]
    + [12.0, 60.0, 1.0, 0.6, 1.0, 2.5, 4.0, 1.0]
)
FRESH = (-1.0, -1.0, 0.1)
CAM = "cam %s %s %s %s" % (
    floats([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]),
    h(1.0),
    floats([0.70710677, 0.0, 0.70710677]),
    floats([0.0, 0.8, 0.6]),
)
# A turned camera, offset.
CAM2 = "cam %s %s %s %s" % (
    floats([0.8, 0.0, -0.6, 0.0, 1.0, 0.0, 0.6, 0.0, 0.8, 5.0, -2.0, 30.0]),
    h(2.5),
    floats([0.8, 0.0, 0.6]),
    floats([0.0, 0.70710677, 0.70710677]),
)


def at(x, y, z):
    return [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, x, y, z]


def beam(i, pos, mode=0, flags=0, state=FRESH, vel=(0.0, 0.0, 0.0), prio=1.0):
    return "beam %d %d %02x %s %s %s %s %s" % (
        i, mode, flags, floats(ARCH), floats(state), floats(at(*pos)), floats(vel), h(prio)
    )


def typ(off=0, near=1000.0, far=2000.0, prio=1.0):
    return "type %d %s %s %s" % (off, h(near), h(far), h(prio))


def own(*ids):
    return [op for i in ids for op in ("link %d" % i, "release %d" % i)]


def walks(n, dt=0.05):
    return ["walk %s" % h(dt)] * n


CASES = [
    # Two visible beams and one behind the camera, which the off-screen rule kills.
    [CAM, typ(), beam(0, (0.0, 0.0, -10.0)), beam(1, (0.0, 2.0, -40.0), mode=1),
     beam(2, (0.0, 0.0, 100.0))] + own(0, 1, 2) + walks(4),
    # The same with EXIST_OFFSCREEN: the hidden beam keeps ticking without geometry.
    [CAM, typ(off=1), beam(0, (0.0, 0.0, 100.0)), beam(1, (300.0, 0.0, -10.0))] + own(0, 1) + walks(3),
    # Killed beams: one fades over 0.3 s, the single-segment one dies at once, one is killed
    # twice. The case keeps its references to two of them.
    [CAM, typ(), beam(0, (0.0, 0.0, -20.0), state=(0.2, -1.0, 0.1)),
     beam(1, (0.0, 0.0, -20.0), flags=2), beam(2, (1.0, 0.0, -20.0), state=(0.2, 0.15, 0.1))]
    + ["link 0", "link 1", "link 2", "release 2"] + walks(1) + ["kill 0", "kill 1", "kill 2", "kill 2"]
    + walks(8, 0.1),
    # Below the performance level: killed by its visibility test.
    [CAM, typ(), beam(0, (0.0, 0.0, -10.0), prio=0.5), beam(1, (0.0, 0.0, -12.0))] + own(0, 1) + walks(2),
    # Moving beams: one through the camera's near plane, one out past pbubble.
    [CAM2, typ(near=40.0, far=80.0), beam(0, (5.0, -2.0, 0.0), vel=(0.0, 0.0, 30.0)),
     beam(1, (5.0, -2.0, 0.0), vel=(0.0, 0.0, -400.0)), beam(2, (6.0, -1.0, -20.0), vel=(0.5, 0.25, 1.0))]
    + own(0, 1, 2) + walks(6, 0.1),
    # Unlinked beams are not walked; a released, linked one lives on in its bucket.
    [CAM, typ(), beam(0, (0.0, 0.0, -10.0)), beam(1, (0.0, 0.0, -10.0)), beam(2, (0.0, 0.0, -10.0))]
    + ["link 2", "link 0", "release 0"] + walks(2),
]


def main():
    lines = ["# P5.6 beam lifecycle oracle inputs, written by make_beam_walk_fixtures.py."]
    lines += [" ; ".join(case) for case in CASES]
    with open("beam_walk.list", "w", newline="\n") as out:
        out.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
