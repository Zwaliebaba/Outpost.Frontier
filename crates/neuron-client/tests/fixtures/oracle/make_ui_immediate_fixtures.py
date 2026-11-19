"""Writes ui_immediate.list, the input of the P4.13 immediate-mode layer suite
(Oracle.exe neuron-client, oracle_ui_immediate.cpp, and
crates/neuron-client/tests/oracle_vectors.rs). Run from any folder:

    python make_ui_immediate_fixtures.py [--check]

With --check nothing is written; the script fails when the list differs
from what it would write.

ui_immediate.list, one case per line: layer operations separated by " ; "
(the format is in oracle_ui_immediate.cpp).
"""

import os
import struct
import sys


def fb(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def fl(*values):
    return " ".join(fb(v) for v in values)


def v(x, y, z=0.0):
    return "v " + fl(x, y, z)


def tc(u, vv):
    return "tc " + fl(u, vv)


IDENTITY12 = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)

# The Win3D camera of a 640 x 480 surface (Win3DStartupManager): its vertical
# field of view, aspect and planes, the view rectangle, an identity view.
WIN3D_CAM = "cam %s %s %s %s 0 0 639 479 %s" % (
    fb(26.989_8), fb(4.0 / 3.0), fb(0.1), fb(1000.0), fl(*IDENTITY12))

# A camera turned and moved, for the view words.
TURNED_CAM = "cam %s %s %s %s 10 20 409 319 %s" % (
    fb(50.0), fb(1.25), fb(0.25), fb(5000.0),
    fl(0.0, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 3.0, -4.0, 12.5))


def line_op(batch, a, b, color, edge, width):
    return "line %s %s %s %08x %08x %s" % (batch, fl(*a), fl(*b), color, edge, fb(width))


def quad(x0, y0, x1, y1):
    return [tc(0.0, 0.0), v(x0, y0), tc(1.0, 0.0), v(x1, y0), tc(1.0, 1.0), v(x1, y1),
            tc(0.0, 1.0), v(x0, y1)]


CASES = [
    # The colour setters: scale, truncation and the clamp after it; bytes.
    ["c4f %s ; state" % fl(1.0, 0.5, -2.0, 2.0),
     "c4f %s ; state" % fl(0.999, 0.0039, 0.00392157, 1.0e10),
     "c3f %s ; state" % fl(0.25, 0.75, 1.5),
     "c3ub 1 2 3 ; state", "c4ub 10 20 30 40 ; state"],
    # Every primitive untextured, with the texture stages disabled.
    ["c3ub 255 0 0", "begin 1", v(1.0, 2.0), v(3.0, 4.0), "end",
     "begin 2", v(0.0, 0.0), v(10.0, 0.0), v(10.0, 10.0), v(0.0, 10.0), "end",
     "begin 3", v(0.0, 0.0), v(5.0, 5.0), v(9.0, 1.0), "end",
     "begin 4", v(0.0, 0.0), v(5.0, 0.0), v(5.0, 5.0), "end",
     "begin 5", v(0.0, 0.0), v(5.0, 0.0), v(0.0, 5.0), v(5.0, 5.0), "end",
     "begin 6", v(0.0, 0.0), v(5.0, 0.0), v(5.0, 5.0), v(0.0, 5.0), "end",
     "begin 9", v(1.0, 1.0), v(6.0, 1.0), v(6.0, 6.0), "end", "state"],
    # A line loop's strip and closing line; a lone vertex; an empty end; an
    # invalid mode draws nothing.
    ["c4ub 0 255 0 128", "begin 7", v(0.0, 0.0), v(4.0, 0.0), v(4.0, 4.0), "end",
     "begin 7", v(2.0, 2.0), "end", "begin 2", "end", "begin 11", v(1.0, 1.0), "end",
     "state"],
    # Quads with a known texture: the textured stage tables, the triangulated
    # indices, then an unknown handle disables the stage.
    ["known 1a2b", "tex 1a2b", "c3ub 255 255 255", "begin 8"] + quad(10.0, 20.0, 110.0, 70.0)
    + quad(200.0, 20.0, 300.0, 70.0) + ["end", "tex 77", "begin 8"] + quad(0.0, 0.0, 1.0, 1.0)
    + ["end", "en de1", "begin 4", v(0.0, 0.0), v(1.0, 0.0), v(0.0, 1.0), "end"],
    # Quads with an incomplete quad and a stray index.
    ["begin 8"] + quad(0.0, 0.0, 2.0, 2.0) + [v(9.0, 9.0), v(8.0, 8.0), "end"],
    # The capability switches and their re-applied depth and blend states.
    ["en b71 ; dis b71 ; en b44 ; dis b44 ; en b50 ; dis b50 ; dis b60 ; en b60",
     "en be2 ; blend 5 6 ; dis be2 ; blend 2 1 ; zfunc 8 ; zwrite 0 ; en b71 ; zfunc 4",
     "zwrite 1 ; dis de1 ; en 1234 ; dis 1234 ; linear ; point ; state"],
    # Lighting goes on for the draw and the saved state comes back.
    ["get 137 1 ; en b50 ; begin 4", v(0.0, 0.0), v(1.0, 0.0), v(0.0, 1.0), "end",
     "dis b50 ; begin 4", v(0.0, 0.0), v(1.0, 0.0), v(0.0, 1.0), "end"],
    # The projection cache: an ortho repeated, a perspective, push, pop and
    # the counters; the modelview reset of every ortho call.
    ["ortho2d ; ortho2d ; state ; persp %s ; persp %s ; push ; ortho %s ; pop ; state"
     % (fl(50.0, 1.25, 0.1, 1000.0), fl(50.0, 1.25, 0.1, 1000.0),
        fl(-1.0, 1.0, -1.0, 1.0, 0.0, 10.0))],
    # A changed parameter after a kind matches; pop with nothing pushed.
    ["persp %s ; persp %s ; pop ; push ; pop ; state"
     % (fl(50.0, 1.25, 0.1, 1000.0), fl(50.0, 1.25, 0.2, 1000.0))],
    # The cache off: push and pop read and write the pipeline's matrix.
    ["cache 0 ; ortho2d ; ortho2d ; push ; persp %s ; pop ; state" % fl(40.0, 1.0, 1.0, 100.0)],
    # A pipeline with world and view split.
    ["ps 7 0 ; ortho2d ; begin 3", v(0.0, 0.0), v(1.0, 1.0), "end", WIN3D_CAM, "applycam"],
    # Viewports: the surface, a camera's rectangle, a raw rectangle, the query.
    ["surface 1024 768 ; vp ; getvp ba2 ; " + TURNED_CAM + " ; vpcam ; getvp ba2 ; getvp 0",
     "vprect 5 6 300 200 ; getvp ba2 ; orthocam ; ortho2d ; state"],
    # Win3DApplyCamera with the view as the modelview.
    [TURNED_CAM, "applycam ; applycam ; state"],
    # The clear colour.
    ["clear %s ; clear %s" % (fl(0.0, 0.0, 0.0, 1.0), fl(0.5, 0.25, 1.5, -1.0))],
    # Win3DResetState keeps the colour and texture coordinate.
    ["c3ub 9 8 7 ; tc %s ; en be2 ; persp %s ; reset ; state ; begin 1" % (
        fl(0.5, 0.25), fl(50.0, 1.25, 0.1, 1000.0)), v(1.0, 1.0), "end"],
    # The sphere grid of the target view.
    ["c3ub 0 255 255 ; sphere %s 8 5" % fb(2.0)],
    ["sphere %s 3 2" % fb(0.5)],
    # The line batchers: A through the camera's perspective, B through its 2D
    # box; horizontal, vertical and slanted lines; a flush, the reset.
    [WIN3D_CAM,
     line_op("A", (0.0, 0.0, -1.0), (0.25, 0.0, -1.0), 0xFF0000FF, 0x000000FF, 0.01),
     line_op("A", (0.1, -0.2, -1.0), (0.1, 0.2, -1.0), 0x80FFFFFF, 0x00FFFFFF, 0.005),
     line_op("A", (0.0, 0.0, -2.0), (0.3, 0.4, -2.0), 0xFF102030, 0x10102030, 0.02),
     "flush ; state ; flush ; breset ; flush"],
    [WIN3D_CAM, "get 137 1",
     line_op("B", (10.0, 10.0, 0.0), (200.0, 10.0, 0.0), 0xFFFFFFFF, 0x00FFFFFF, 1.5),
     line_op("B", (10.0, 10.0, 0.0), (10.0, 300.0, 0.0), 0xFF00FF00, 0x0000FF00, 2.0),
     line_op("A", (0.0, 0.0, -1.0), (0.0, 0.1, -1.0), 0xFFFF0000, 0x00FF0000, 0.01),
     "flush ; state"],
    # An empty flush only raises the dirty bytes.
    [WIN3D_CAM, "flush ; breset ; flush"],
    # The overlay pass: the font atlas's list flushed under the 2D projection,
    # with point filtering forced and the depth states saved and restored.
    ["get 23 4 ; get 14 1",
     "font %s ff112233 5" % fl(10.0, 20.0, 74.0, 36.0),
     "font %s ff445566 5" % fl(80.0, 20.0, 144.0, 36.0),
     "font %s ff778899 6" % fl(0.0, 40.0, 32.0, 56.0),
     "overlay ; state"],
    # An overlay with nothing queued.
    ["persp %s ; overlay ; state" % fl(50.0, 1.25, 0.1, 1000.0)],
]


# P4.13, seventh increment: the button montage, the fuzzy lines.

# The Win3D camera's x and y scales (HalfWidth / tan(27.216 degrees)).
WIN3D_SCALES = "camxs %s" % fl(622.0, -622.0)


def mtile(u, vv, step, record, scale):
    return "mtile %s %x %s" % (fl(u, vv, step), record, fb(scale))


def mdraw(index, at, rgba, size, mode, angle):
    return "mdraw %s %s %d %d %d %d %s %d %s" % (
        index, fl(*at), rgba[0], rgba[1], rgba[2], rgba[3], fb(size), mode, fb(angle))


def fuzzy(curved, texture, start, end, control, eye, colour, width, jitter, side):
    return "fuzzy %d %x %s" % (curved, texture, fl(*(list(start) + list(end) + list(control) +
                                                   list(eye) + list(colour) +
                                                   [width, jitter, side])))


CASES += [
    # Montage icons: the tile's square inset and mirrored per mode, a mode that keeps the
    # frame's rect, a rotated icon, a tile without a shape and none, behind the camera, under a
    # pixel, clipped at the viewport's edge, a rotated icon across the edge; then the flush of
    # the "buttons" list with the depth states saved and restored.
    [WIN3D_CAM, WIN3D_SCALES, "get 23 4 ; get 14 1",
     "frame 7 77 %s" % " ".join(fb(x) for x in (0.1, 0.2, 0.9, 0.8)),
     mtile(0.0, 0.0, 0.25, 7, 0.05), mtile(0.25, 0.5, 0.25, 7, 0.0),
     mtile(0.0, 0.0, 0.5, 0, 0.02), mtile(0.75, 0.25, 0.25, 8, 0.0),
     mdraw("0", (0.01, 0.02, -1.0), (16, 32, 48, 64), 0.05, 0, 0.0),
     mdraw("1", (-0.1, 0.05, -1.0), (255, 255, 255, 255), 0.03, 1, 0.0),
     mdraw("1", (0.1, -0.05, -1.2), (1, 2, 3, 4), 0.03, 2, 0.0),
     mdraw("1", (0.0, 0.1, -0.8), (200, 100, 50, 25), 0.03, 3, 0.0),
     mdraw("0", (0.05, 0.05, -1.0), (9, 8, 7, 6), 0.04, 5, 0.0),
     mdraw("3", (-0.05, -0.1, -1.0), (128, 128, 128, 255), 0.06, 0, 0.3),
     mdraw("2", (0.0, 0.0, -1.0), (255, 0, 0, 255), 0.05, 0, 0.0),
     mdraw("-", (0.0, 0.0, -1.0), (255, 0, 0, 255), 0.05, 0, 0.0),
     mdraw("0", (0.0, 0.0, 1.0), (255, 0, 0, 255), 0.05, 0, 0.0),
     mdraw("0", (0.0, 0.0, -1.0), (255, 0, 0, 255), 0.001, 0, 0.0),
     mdraw("0", (0.5, 0.0, -1.0), (0, 255, 0, 255), 0.08, 0, 0.0),
     mdraw("0", (0.0, -0.385, -1.0), (0, 0, 255, 255), 0.08, 2, 0.0),
     mdraw("3", (0.51, 0.0, -1.0), (0, 255, 255, 255), 0.08, 0, -0.7),
     "mflush ; state"],
    # A flush with nothing queued.
    [WIN3D_CAM, "mflush"],
    # Fuzzy lines: a line strip with its jitter, textured quads facing the eye, an arc as a
    # strip and as quads, an arc too small to draw.
    [WIN3D_CAM, "srand 7",
     fuzzy(0, 0, (0.0, 0.0, -10.0), (10.0, 1.0, -12.0), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0),
           (1.0, 0.5, 0.25), 2.5, 0.1, 1.0),
     "state"],
    [WIN3D_CAM, "frame 9 99 %s" % fl(0.0, 0.0, 1.0, 1.0), "known 99",
     fuzzy(0, 9, (-2.0, 1.0, -8.0), (3.0, -0.5, -9.5), (0.0, 0.0, 0.0), (0.0, 0.5, 1.0),
           (0.2, 0.4, 0.6), 1.25, 0.2, 0.75),
     "state"],
    [WIN3D_CAM, "srand 12345",
     fuzzy(1, 0, (5.0, 0.0, -10.0), (0.0, 5.0, -10.0), (0.0, 0.0, -10.0), (0.0, 0.0, 0.0),
           (0.9, 0.8, 0.7), 1.5, 0.05, 1.0),
     fuzzy(1, 0, (0.0, 0.0, -3.0), (1.0, 0.0, -3.0), (0.0, 0.0, -3.0), (0.0, 0.0, 0.0),
           (1.0, 1.0, 1.0), 1.0, 0.1, 1.0),
     "state"],
    [WIN3D_CAM, "srand 99", "known 99",
     fuzzy(1, 9, (2.0, 2.0, -6.0), (-2.0, 2.5, -7.0), (0.0, 1.0, -6.5), (0.0, 0.0, 0.0),
           (0.5, 0.5, 0.5), 0.8, 0.15, 2.0),
     "state"],
]


def write(here, name, lines, check):
    data = ("\n".join(lines) + "\n").encode("latin-1")
    path = os.path.join(here, name)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != data:
                sys.exit("%s is out of date; run make_ui_immediate_fixtures.py" % name)
        return
    with open(path, "wb") as out:
        out.write(data)


def main():
    check = "--check" in sys.argv[1:]
    here = os.path.dirname(os.path.abspath(__file__))
    lines = [
        "# P4.13 immediate-mode layer oracle inputs, written by make_ui_immediate_fixtures.py.",
        "# One case per line: operations separated by \" ; \" (oracle_ui_immediate.cpp).",
    ]
    for case in CASES:
        lines.append(" ; ".join(case))
    write(here, "ui_immediate.list", lines, check)


if __name__ == "__main__":
    main()
