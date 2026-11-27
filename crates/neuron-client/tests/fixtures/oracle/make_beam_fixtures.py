"""Writes beam.list, the P5.6 beam geometry oracle inputs (CppSource/Oracle/oracle_beam.cpp).

Each line: beam MODE HEAD TRAIL FLAGS ARCH STATE XFORM, the floats as the hex of their f32.
Run from this directory: python make_beam_fixtures.py
"""

import struct


def h(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def floats(values):
    return ",".join(h(v) for v in values)


NAN = float("nan")

# tip, tail, core, sec_core, outter, sec_outter colours; head and trail brightness;
# tip_length, tail_length, core_length, core_width, sec_core_width, head_width, flash_size, +0x7c.
SPEAR = (
    [1.0, 0.2, 0.1, 0.9, 0.5, 0.3, 1.0, 1.0, 0.8, 0.6, 0.6, 0.6, 1.0, 0.4, 0.2, 0.3, 0.3, 0.9]
    + [1.0, 0.7]
    + [12.0, 60.0, 1.0, 0.6, 1.0, 2.5, 4.0, 1.0]
)
BOLT = (
    [0.2, 0.8, 1.0, 0.1, 0.4, 0.9, 1.0, 1.0, 1.0, 0.3, 0.6, 1.0, 0.5, 0.7, 1.0, 0.2, 0.2, 0.6]
    + [0.9, 0.6]
    + [8.0, 25.0, 15.0, 0.75, 0.5, 2.0, 3.5, 1.0]
)
# Lengths below core_width and a negative core_length (the bolt's clamps).
SHORT = BOLT[:20] + [0.25, 0.5, -3.0, 0.75, 0.5, 0.5, 0.3, 1.0]
# Brightness above one: the colour bytes wrap.
BRIGHT = SPEAR[:18] + [2.5, 3.0] + SPEAR[20:]

IDENTITY = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
YAW = [0.8, 0.0, -0.6, 0.0, 1.0, 0.0, 0.6, 0.0, 0.8, 1500.25, -320.5, 7781.0]
TILT = [0.70710677, -0.70710677, 0.0, 0.5, 0.5, -0.70710677, 0.5, 0.5, 0.70710677, -42.0, 9.5, 0.125]

# (mode, head, trail, flags, arch, (progress, lifetime, denom), xform, note)
CASES = [
    (0, 0, 0, 0, SPEAR, (0.25, -1.0, 0.1), IDENTITY, "spear at full length"),
    (0, 0, 0, 0, SPEAR, (0.05, -1.0, 0.1), YAW, "spear half grown"),
    (0, 1, 1, 0, SPEAR, (0.033, -1.0, 0.1), TILT, "spear, the other texture bands"),
    (0, 0, 0, 0, SPEAR, (-1.0, -1.0, 0.1), YAW, "before the first draw, t = -10"),
    (0, 0, 0, 0, SPEAR, (0.0, -1.0, 0.1), YAW, "t = 0, no flash"),
    (0, 0, 0, 8, SPEAR, (0.0, -1.0, 0.1), YAW, "t = 0, the flash"),
    (0, 0, 0, 2, SPEAR, (0.25, -1.0, 0.1), TILT, "single segment only"),
    (0, 0, 0, 1, SPEAR, (0.1, -1.0, 0.1), TILT, "progress at the denominator"),
    (0, 0, 0, 0, SPEAR, (0.25, 0.15, 0.1), YAW, "spear half faded"),
    (0, 0, 0, 0, SPEAR, (0.25, 0.0, 0.1), YAW, "lifetime exactly 0"),
    (0, 0, 0, 0, BRIGHT, (0.25, 0.6, 0.1), YAW, "bright and over-faded"),
    (0, 0, 0, 0, SPEAR, (NAN, NAN, 0.1), IDENTITY, "unordered progress and lifetime"),
    (1, 0, 0, 0, BOLT, (0.25, -1.0, 0.1), IDENTITY, "bolt at full length"),
    (1, 0, 0, 0, BOLT, (0.07, -1.0, 0.1), YAW, "bolt growing"),
    (1, 1, 1, 0, BOLT, (0.013, -1.0, 0.1), TILT, "bolt, the other texture bands"),
    (1, 0, 0, 0, BOLT, (0.0, -1.0, 0.1), YAW, "bolt at t = 0 draws one segment"),
    (1, 0, 0, 8, BOLT, (0.0, -1.0, 0.1), YAW, "bolt at t = 0 with the flash bit"),
    (1, 0, 0, 0, BOLT, (-1.0, -1.0, 0.1), TILT, "bolt before the first draw"),
    (1, 0, 0, 0, BOLT, (0.25, 0.2, 0.1), TILT, "bolt fading"),
    (1, 0, 0, 0, SHORT, (0.05, -1.0, 0.1), YAW, "bolt clamps"),
    (1, 0, 0, 0, SHORT, (0.25, -1.0, 0.1), IDENTITY, "bolt clamps at full length"),
    (2, 0, 0, 0, SPEAR, (0.25, -1.0, 0.1), IDENTITY, "an unknown mode builds nothing"),
]


def main():
    lines = ["# P5.6 beam geometry oracle inputs, written by make_beam_fixtures.py."]
    for mode, head, trail, flags, arch, state, xform, note in CASES:
        assert len(arch) == 28 and len(xform) == 12, note
        lines.append(
            "beam %d %d %d %02x %s %s %s" % (mode, head, trail, flags, floats(arch), floats(state), floats(xform))
        )
    with open("beam.list", "w", newline="\n") as out:
        out.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
