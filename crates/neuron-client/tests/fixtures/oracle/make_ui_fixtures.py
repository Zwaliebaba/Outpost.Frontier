"""Writes ui_page_desc.list and ui_quad_batch.list, the inputs of the P4.13
page descriptor and screen-quad batcher suites (Oracle.exe neuron-client,
oracle_ui.cpp and oracle_ui_quads.cpp, and
crates/neuron-client/tests/oracle_vectors.rs). Run from any folder:

    python make_ui_fixtures.py [--check]

With --check nothing is written; the script fails when a list differs from
what it would write.

ui_page_desc.list, one case per line: the bytes of a PAGE file in hex,
which the suite writes to a file and loads with FUiPageDesc::LoadFromFile
(fopen "rt", fgets into 0x100 bytes, sscanf per class token).

Line endings are LF, or CR LF where a CR can only meet white space: the
reference reads in text mode, and a local build on a C library without
text mode then sees the same fields.

ui_quad_batch.list, one case per line: batcher operations separated by
" ; " (the format is in oracle_ui_quads.cpp).

ui_controls.list, one case per line: control operations separated by " ; "
(the format is in oracle_ui_controls.cpp).
"""

import os
import struct
import sys


def fb(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def rec(x0, y0, x1, y1, color, da=1.0, db=1.0, uv=(0.0, 0.0, 1.0, 1.0)):
    fields = [x0, y0, x1, y1, da, db] + list(uv)
    return "rec " + " ".join(fb(v) for v in fields) + " %08x" % color


def frame(tex, blend, find, uv=(0.0, 0.0, 0.0, 0.0)):
    return "frame %x %s %d %d" % (tex, " ".join(fb(v) for v in uv), blend, find)


def clip(e0, e1, a0, a1, lo, hi, span):
    return "clip " + " ".join(fb(v) for v in (e0, e1, a0, a1, lo, hi, span))


ATLAS = 0xC0FFEE01
QUAD_CASES = [
    # One list, two textures, a flush of the list by key.
    " ; ".join([
        "key %x" % ATLAS,
        frame(5, 2, 0),
        rec(10.0, 20.0, 74.0, 36.0, 0xFF808080, 0.5, 2.0, (0.0, 0.0, 0.5, 0.25)),
        rec(80.0, 20.0, 144.0, 36.0, 0x80FFFFFF),
        frame(6, 1, 0),
        rec(-1.5, 0.25, 3.75, 9.0, 0x01020304),
        "flush %x" % ATLAS,
    ]),
    # Drop and find, and the blend of a kept node.
    " ; ".join([
        "key 7",
        frame(5, 2, 0), rec(0, 0, 1, 1, 1),
        frame(6, 2, 0), rec(1, 1, 2, 2, 2),
        frame(5, 0, 0), rec(2, 2, 3, 3, 3),
        frame(6, 1, 1), rec(3, 3, 4, 4, 4),
        frame(6, 0, 0), rec(4, 4, 5, 5, 5),
        "flush 7",
    ]),
    # The current list by its key, with saved states the recorder answers.
    " ; ".join([
        "get 22 3", "get 139 12345678", "get 136 1",
        "key 9", frame(0, 0, 1, (0.0, 0.0, 1.0, 1.0)), rec(0, 0, 8, 8, 0xFF000000),
        "flush ffffffff",
    ]),
    # A key change leaves the old list queued; a flush of another key draws nothing.
    " ; ".join([
        "key 1", frame(3, 2, 0), rec(0, 0, 1, 1, 0xA),
        "key 2", frame(4, 2, 0), rec(5, 5, 6, 6, 0xB),
        "key 1", rec(7, 7, 8, 8, 0xC),
        "flush 3", "flush 2", "flush 1", "flush 1",
    ]),
    # The current list made by the flush itself, then a reset.
    "key 44 ; flush ffffffff ; reset ; key 44 ; " + frame(9, 2, 0) + " ; " + rec(0, 0, 2, 2, 7) + " ; reset ; flush 44",
    # Records before any frame go to the new list's first node.
    "key 5 ; " + rec(1, 2, 3, 4, 0xDEADBEEF) + " ; " + frame(8, 2, 1) + " ; " + rec(5, 6, 7, 8, 9) + " ; flush 5",
    # The menu path's state cache.
    "menu ; menu ; invalidate ; menu ; key 3 ; flush 3 ; menu",
    # Many records in one node (the growth by 0x21).
    " ; ".join(["key 6", frame(2, 2, 0)] + [rec(float(i), 0.0, float(i) + 1.0, 1.0, i) for i in range(40)] + ["flush 6"]),
    # ScreenQuadClip1D.
    " ; ".join([
        clip(10.0, 20.0, 0.0, 1.0, 0.0, 799.0, 11.0),
        clip(-5.0, 5.0, 0.0, 1.0, 0.0, 799.0, 10.0),
        clip(790.0, 810.0, 0.0, 1.0, 0.0, 799.0, 20.0),
        clip(800.0, 810.0, 0.0, 1.0, 0.0, 799.0, 10.0),
        clip(-20.0, -10.0, 0.0, 1.0, 0.0, 799.0, 10.0),
        clip(-0.1, 0.3, 0.25, 0.75, 0.0, 0.2, 0.4),
        clip(-3.0, 1000.0, 0.1, 0.9, 0.0, 639.0, 1003.0),
        clip(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0),
    ]),
]

def fbs(*values):
    return " ".join(fb(v) for v in values)


def text(s):
    return "".join("%04x" % ord(c) for c in s)


def xform(tx, ty, tz, m=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)):
    return "xform " + fbs(*(list(m) + [tx, ty, tz]))


def ray(x, y, z=-1.0):
    return "ray " + fbs(x, y, z)


# A list box at (-0.25, 0.2, -1), 0.5 by 0.3: on screen x 220..420, y 160..280 with the
# oracle's fixed projection, rows of 16 pixels and characters of 7.
LIST = ["new UIListBox " + fbs(0.5, 0.3), xform(-0.25, 0.2, -1.0)]
# A vertical scroll bar at (0.6, 0.2, -1), 0.02 by 0.4, range 0..100, page 14, step 1: the
# local point (lx, ly) is under the ray (0.6 + lx, 0.2 + ly, -1).
VSCROLL = ["new UIScroll " + fbs(0.02, 0.4, 0.0, 100.0, 14.0, 1.0) + " 0", xform(0.6, 0.2, -1.0)]


def vray(lx, ly):
    return ray(0.6 + lx, 0.2 + ly)


# A horizontal one at (-0.3, -0.1, -1), 0.5 by 0.03, range 0..20, page 4, step 2.
HSCROLL = ["new UIScroll " + fbs(0.5, 0.03, 0.0, 20.0, 4.0, 2.0) + " 1", xform(-0.3, -0.1, -1.0)]


def hray(lx, ly):
    return ray(-0.3 + lx, -0.1 + ly)


ROT = (0.8, -0.6, 0.0, 0.6, 0.8, 0.0, 0.0, 0.0, 1.0)

CONTROL_CASES = [
    # A list: items, data, texts, the hover row, a press and a double click, the draw.
    " ; ".join(LIST + [
        "state",
        "item " + text("Alpha") + " 11", "item " + text("Beta") + " 22",
        "item " + text("A row far too long for the box it is in") + " 33",
        "data 0", "data 2", "data 3", "data -1",
        "settext 1 " + text("B2"), "settext 3 " + text("x"),
        "cursor 250 190", "dt " + fb(0.25), "update", "state",
        "btn -1 2", "state", "btn -1 3", "btn -1 1", "btn -2 2", "state",
        "f80 2", "rules 1", "draw",
        "over", "state",
    ]),
    # The hover at the box's edges and past the items; nothing hovered selects no row.
    " ; ".join(LIST + [
        "item " + text("one") + " 1", "item " + text("two") + " 2",
        "cursor 219 170", "update", "state",
        "cursor 420 280", "update", "state",
        "cursor 220 160", "update", "state",
        "cursor 300 200", "update", "state", "btn -1 2", "state",
        "cursor 421 170", "update", "state",
        "first 1", "cursor 300 170", "update", "state",
    ]),
    # Visible rows, the first row, the frame off, the rules with no rows, not selectable.
    " ; ".join(LIST + [
        "item " + text("r0") + " 0", "item " + text("r1") + " 0", "item " + text("r2") + " 0",
        "item " + text("") + " 0",
        "rows 2", "first 1", "draw",
        "rows 5", "first 0", "frame 0", "rules 1", "draw",
        "rows -1", "draw",
        "sel 0", "cursor 300 170", "update", "btn -1 2", "state",
        "f80 0", "pulse " + fb(0.1), "rows 3", "frame 1", "rules 0", "draw",
        "pulse " + fb(0.35), "draw",
    ]),
    # A rotated page transform projects the extent; a full list refuses the 257th row.
    " ; ".join(["new UIListBox " + fbs(0.4, 0.25), xform(0.1, -0.2, -1.5, ROT), "state"] +
               ["item " + text("i%d" % i) + " %d" % i for i in range(257)] +
               ["data 255", "data 256", "settext 255 " + text("last"), "settext 256 " + text("no"),
                "state"]),
    # A vertical scroll bar: the layout, the hot parts, the arrows and their repeat, the track,
    # the thumb drag and its release.
    " ; ".join(VSCROLL + [
        "state",
        vray(0.01, -0.01), "update", "state",
        "btn -1 2", "state",
        "dt " + fb(0.125), "draw", "draw", "draw", "state", "draw", "state",
        "leave", "state",
        vray(0.01, -0.39), "btn -1 2", "state",
        vray(0.01, -0.3), "btn -1 2", "state",
        vray(0.01, -0.025), "btn -1 2", "state",
        "value " + fb(40.0), "state",
        vray(0.01, -0.17), "update", "btn -1 2", "state",
        vray(0.01, -0.25), "over", "state", "update", "draw",
        vray(0.01, -0.2), "btn -1 1", "state",
        vray(0.05, -0.2), "btn -1 2", "state",
        "btn -2 2",
    ]),
    # The draw's option gate and custom colour, inactive bars, the hit test, the properties.
    " ; ".join(VSCROLL + [
        "opt 0", "draw", "opt 1", "custom 1", "draw", "custom 0",
        vray(0.01, -0.1), "hit", vray(0.03, -0.1), "hit", vray(0.0, 0.0), "hit",
        vray(0.01, -0.4), "hit",
        "active 0", vray(0.01, -0.01), "btn -1 2", "over", "active 1",
        "prop 37 0 5", "state", "prop 37 0 50", "state", "prop 38 7 0", "state",
        "prop 38 99 0", "prop 3b 0 0",
        "offset " + fbs(0.01, -0.02, 0.0), "state",
        "soff " + fbs(0.03, 0.01, 0.5), "state",
        "pos " + fbs(0.2, 0.1, -2.0), "state",
        "range " + fbs(10.0, 5.0), "state", "range " + fbs(-5.0, 5.0), "page " + fbs(20.0), "state",
        "page " + fbs(-1.0), "step " + fbs(0.5), "size " + fbs(0.05, 0.03), "state",
        "value " + fb(3.25), "state",
    ]),
    # A horizontal bar: the layout, the draw with a hot thumb, the arrows, a drag.
    " ; ".join(HSCROLL + [
        "state",
        hray(0.1, -0.015), "update", "draw",
        hray(0.01, -0.01), "btn -1 2", "state",
        hray(0.49, -0.01), "btn -1 3", "btn -1 2", "state",
        hray(0.3, -0.01), "btn -1 2", "state",
        hray(0.06, -0.01), "btn -1 2", "state",
        "value " + fb(8.0), "state",
        hray(0.25, -0.01), "update", "btn -1 2", "state", "update", "draw",
        hray(0.35, -0.01), "over", "state",
        "btn -1 1", "state",
    ]),
    # Step mode: the thumb's block, the track's tenth jumps, the drag's truncation.
    " ; ".join(VSCROLL + [
        "stepmode 1", "range " + fbs(0.0, 10.0), "value " + fb(0.0), "state",
        vray(0.01, -0.3), "btn -1 2", "state", "btn -1 2", "state",
        vray(0.01, -0.03), "btn -1 2", "state",
        "range " + fbs(0.0, 4.0), "value " + fb(2.0), "state",
        vray(0.01, -0.3), "btn -1 2", "state",
        "value " + fb(1.0), "update", "draw",
        "horiz 1", "size " + fbs(0.4, 0.02), "state",
    ]),
    # A cycle: only the arrows, a press on each, their repeat, a click elsewhere inside, outside.
    " ; ".join(["new UICycle " + fbs(0.3, 0.03, 0.0, 5.0, 1.0, 1.0) + " 1",
                xform(0.1, 0.2, -1.0), "state",
                ray(0.1 + 0.29, 0.2 - 0.01), "btn -1 2", "state",
                "dt " + fb(0.25), "draw", "state", "draw", "state", "draw", "state",
                ray(0.1 + 0.01, 0.2 - 0.01), "btn -1 2", "state",
                ray(0.1 + 0.15, 0.2 - 0.01), "btn -1 2", "state", "btn -1 1",
                ray(0.1 + 0.5, 0.2 - 0.01), "btn -1 2", "state",
                "stepmode 1", ray(0.1 + 0.29, 0.2 - 0.01), "btn -1 2", "btn -1 2", "btn -1 2",
                "btn -1 2", "btn -1 2", "state",
                "active 0", "btn -1 2"]),
    # A vertical cycle with the arrow hot while drawn.
    " ; ".join(["new UICycle " + fbs(0.03, 0.2, 0.0, 3.0, 1.0, 1.0) + " 0",
                xform(-0.4, 0.0, -1.0), ray(-0.4 + 0.01, -0.01), "update", "draw", "state"]),
    # A framed panel: the mesh load, the placement of the page transform, the draw passes.
    " ; ".join(["new UIWinObject " + fbs(0.6, 0.4, -1.2) + " " +
                "interface\\intro\\objects\\box.cmp".encode().hex(),
                xform(-0.3, 0.2, -1.0), "state", "draw",
                "flags 1", "tint 10203040", "draw", "flags 3", "draw", "flags 2", "draw",
                xform(0.05, -0.1, -1.5, ROT), "state",
                "pos " + fbs(0.1, 0.2, -3.0), "state"]),
    # A panel without its mesh path still loads (the loader decides).
    " ; ".join(["new UIWinObject " + fbs(1.0, 1.0, -1.0), "state"]),
]

def hx(s):
    return s.encode().hex()


def fbits(v):
    return "%d" % struct.unpack("<i", struct.pack("<f", v))[0]


IDENT = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)

# The later controls. With the Win3D camera of a 640 x 480 surface, the pixel (cx, cy) looks
# along ((cx - 320) / 622.2252, (240 - cy) / 622.2252, -1): (382, 116) is about (0.1, 0.2, -1).
LATER_CASES = [
    # A text control: placement, a draw, the clickable control's hover, press, release and
    # right press, the typewriter, the blink, the colours, the trims, the cached draw, the
    # number text, the option gate.
    " ; ".join([
        "new TextControl", "init - " + fbs(0.0, 0.0, 0.0) + " - " + fbs(0.4, 0.05) + " 0 1 0 0",
        "state", "pos " + fbs(0.1, 0.2, -1.0), "state",
        "tset " + text("Hello"), "draw",
        "prop 24 1 0", "cursor 380 116", "draw",
        ray(0.1, 0.2), "hit",
        "btn -1 2", "state", "btn -1 1", "btn -1 3", "btn -2 2", "state",
        "prop 50 1 100", "dt " + fb(0.25), "update", "state", "draw",
        "update", "update", "state",
        "prop 50 1 50", "update", "inactive", "state",
        "prop 51 1 0", "colour 11223344 0", "dt " + fb(0.6), "update", "update", "state", "draw",
        "prop 51 0 0", "prop c 1 0", "draw", "prop c 0 0",
        "prop 2 1 0", "draw", "prop 2 0 0",
        "prop 23 1065353216 0", "draw",
        "prop 21 0 0", "prop 20 0 0",
        "text " + text("World"), "ids 5", "width " + fb(0.5),
        "cached 1 1", "draw", "draw", "cached 0 0",
        "gopt 19 0", "draw", "gopt 19 1",
        "number " + hx("%d"), "draw",
        "offset " + fbs(0.01, 0.02, 0.0), "state",
    ]),
    # A text control anchored at its corner, hit without being clickable, a tooltip.
    " ; ".join([
        "new TextControl", "init " + hx("Normal") + " " + fbs(0.01, -0.02, 0.0) + " - " +
        fbs(0.2, 0.1) + " 7 2 11 1",
        "pos " + fbs(-0.3, 0.0, -1.0), "state", ray(-0.2, -0.05), "hit", ray(0.3, 0.0), "hit",
        "draw",
    ]),
    # An edit control: the blinking cursor, characters, the numeric keypad, Backspace, Enter,
    # digits only, the maximum length, the text property, not editable.
    " ; ".join([
        "new EditControl", "init - " + fbs(0.0, 0.0, 0.0) + " - " + fbs(0.3, 0.04) + " 0 0 0 1",
        "pos " + fbs(-0.2, 0.1, -1.0),
        "dt " + fb(0.3), "update", "update", "state",
        "key 65 2", "key 65 1", "key 98 1", "key 8 2", "key 8 1", "key 8 1", "key 8 1",
        "key 13 1", "key 13 2",
        "prop 44 1 0", "key 65 1", "key 49 1",
        "prop 43 1 3", "key 50 1", "key 51 1", "key 52 1",
        "dt " + fb(0.6), "update", "state",
        "text " + text("xyz"), "state",
        "prop 43 0 0", "update", "key 65 1", "state",
    ]),
    # A static control: the mesh, the placement, the passes, the force radius, the flags, the
    # colours, inactive, the option gate, the angles, the clicks, the tooltip.
    " ; ".join([
        "new StaticControl",
        "init " + hx("box.cmp") + " " + fbs(0.0, 0.0, 0.0) + " - " + fbs(0.1, 0.1, 0.1) +
        " 0 0 0 0 0 0 1 0 0 0 " + fbs(0.0, 0.0, 0.0),
        "state", xform(0.1, 0.2, -1.0, ROT), "state", "draw",
        "prop 4d 2 0", "draw",
        "prop 19 0 65536", "draw",
        "prop 19 1 1", "draw",
        "colours 10203040 50607080", "draw",
        "prop 2 1 0", "draw", "prop 2 0 0",
        "gopt 21 0", "draw", "gopt 21 1",
        "prop c 1 0", "prop d " + fbits(0.5) + " 0", "state", "prop e 0 0",
        "prop 1c 0 0", "state",
        "btn -1 3", "btn -1 2", "btn -2 2",
        "prop 3 7 0", ray(0.1, 0.2), "hit", ray(0.5, 0.5), "hit", "state",
    ]),
    # A turning static control and its snap back.
    " ; ".join([
        "new StaticControl",
        "init " + hx("spin.cmp") + " " + fbs(0.0, 0.0, 0.0) + " - " + fbs(0.1, 0.1, 0.1) +
        " 1 0 0 0 1 0 1 0 2 1 " + fbs(0.1, 0.2, 0.3),
        xform(0.0, 0.0, -1.0), "state", "dt " + fb(0.5), "update", "state",
        "prop 1c 0 0", "update", "state", "update", "state", "draw",
    ]),
    # A button: placement, the draw, the hover, a press followed on the button and released
    # over it, a right press released off it, the selection, the glow, the blink, inactive,
    # the colour.
    " ; ".join([
        "new ButtonControl",
        "init " + hx("btn.cmp") + " - " + hx("click") + " - " + fbs(0.0, 0.0, 0.0) + " - " +
        fbs(0.05, 0.05, 0.05) + " 0 0 0 0 7 0 0",
        xform(0.1, 0.2, -1.0), "state", "draw",
        "enter", "state", "draw", "leave",
        "cursor 382 116", "btn -1 2", "update", "state", "btn -1 1", "state",
        "cursor 100 100", "btn -1 3", "update", "btn -1 1", "state",
        "getcmd",
        "prop c 1 0", "draw", "prop c 0 0",
        "prop 35 1 0", "draw",
        "prop 51 1 0", "dt " + fb(0.6), "update", "update", "draw", "state", "prop 51 0 0",
        "prop 2 1 0", "draw", "prop 34 1 0", "draw", "state", "prop 2 0 0",
        "colour 11223344 1", "draw", "prop 2c 0 0", "prop 2d 0 0", "prop 32 1 0", "draw",
        "gopt 20 0", "draw",
    ]),
    # A meshless button of a key-map command: no body, the hit through its position, the post.
    " ; ".join([
        "new ButtonControl",
        "init - - - " + hx("next") + " " + fbs(0.0, 0.0, 0.0) + " - " +
        fbs(0.05, 0.05, 0.05) + " 1 1 1 1 0 0 0",
        xform(0.1, 0.2, -1.0), "state", "draw", "getcmd",
        "cursor 382 116", "btn -1 2", "btn -1 1", "state",
        "prop 36 1 0", "state",
    ]),
    # A toggle button: both meshes, the glow's half turn, a click flips the state, the state-1
    # draw, the state sounds and properties.
    " ; ".join([
        "new ToggleButtonControl",
        "init " + hx("a.cmp") + " " + hx("b.cmp") + " " + hx("snd") + " - " +
        fbs(0.0, 0.0, 0.0) + " - " + fbs(0.05, 0.05, 0.05) + " 0 0 0 0 7 8",
        "state", xform(0.1, 0.2, -1.0), "state", "draw",
        "prop 35 1 0", "draw",
        ray(0.1, 0.2), "cursor 382 116", "btn -1 2", "update", "btn -1 1", "state",
        "gettoggle", "update", "draw",
        "sounds - " + hx("x"), "btn -1 2", "btn -1 1", "state",
        "prop 30 0 0", "gettoggle", "prop 30 1 0", "gettoggle",
        "offset " + fbs(0.01, 0.0, 0.0), "state",
    ]),
    # A wire box: the edges, lit, focused, inactive, the colour, the blink; never hit.
    " ; ".join([
        "new WireStaticControl", "init " + fbs(0.0, 0.0, 0.0) + " " + fbs(0.1, 0.05, 0.0) +
        " 0 0 0 0",
        xform(0.1, 0.2, -1.0), "state", "draw",
        "prop c 1 0", "draw", "prop c 0 0",
        "prop 2c 0 0", "draw", "prop 2d 0 0",
        "prop 2 1 0", "draw", "prop 34 1 0", "draw", "prop 2 0 0",
        "colour 11223344 1", "draw",
        "prop 51 1 0", "dt " + fb(0.6), "update", "update", "draw", "state",
        ray(0.1, 0.2), "hit", "btn -1 2",
    ]),
    # A wire button: two edges, the hover, a right click, a press released off it.
    " ; ".join([
        "new WireButtonControl", "init " + hx("ws") + " " + fbs(0.0, 0.0, 0.0) + " " +
        fbs(0.1, 0.05, 0.02) + " 1 0 9 0 5",
        xform(0.1, 0.2, -1.0), "draw", "enter", "leave",
        "cursor 382 116", ray(0.1, 0.2), "hit",
        "btn -1 3", "update", "btn -1 1", "state",
        "btn -1 2", "cursor 100 100", "update", "btn -1 1", "state",
        "getcmd", "draw",
    ]),
    # A mesh scroll bar: the layout, the draw, the hover, the buttons and their repeat, the
    # range and value properties, the thumb drag, the back plate's paging.
    " ; ".join([
        "new ScrollControl",
        "init " + hx("back") + " " + fbs(0.0, 0.0, 0.0) + " - " + fbs(0.02, 0.2, 0.01) + " 0 " +
        hx("next") + " " + fbs(0.0, 0.21, 0.0) + " - " + fbs(0.02, 0.02, 0.01) + " 0 " +
        hx("prev") + " " + fbs(0.0, -0.21, 0.0) + " - " + fbs(0.02, 0.02, 0.01) + " 0 " +
        hx("thumb") + " " + fbs(0.0, 0.18, 0.0) + " " + fbs(0.0, -0.18, 0.0) + " - - " +
        fbs(0.02, 0.02, 0.01) + " 0 0 10 3 0 0",
        "state", xform(0.5, 0.0, -1.0), "state", "draw",
        ray(0.5, 0.21), "enter", "draw", "leave",
        "cursor 631 109", "btn -1 2", "state",
        "time " + fb(0.5), "update", "state", "btn -1 1",
        "cursor 631 371", "btn -1 2", "btn -1 1", "state",
        "prop 37 20 5", "prop 38 12 0", "prop 39 0 0", "prop 3a 0 0", "prop 3b 0 0",
        "prop 3c 4 2", "prop 3d 0 0", "state",
        "offset " + fbs(0.01, 0.0, 0.0), "state",
        "cursor 637 233", "btn -1 2", "state", "cursor 637 150", "update", "state", "btn -1 1",
        "cursor 637 333", "btn -1 2", "state", "time " + fb(1.0), "update", "state", "btn -1 1",
        "draw",
    ]),
    # A table: the edges, the hover, the selection, a column edge dragged, the edge setters,
    # a row that is not hoverable, the hit test.
    " ; ".join([
        "new UITable " + fbs(0.6, 0.4) + " 3 2", "rows 1 1 1 1 0 1",
        xform(-0.3, 0.2, -1.0), "state", "draw",
        ray(-0.2, 0.15), "update", "state", "btn -1 2", "draw",
        ray(0.002, 0.15), "update", "state", "btn -1 2", "state",
        ray(0.1, 0.15), "update", "state", "btn -1 1", "state",
        "cedge 1 " + fb(0.7), "cedge 0 " + fb(0.1), "redge 1 " + fb(-0.05),
        "redge 2 " + fb(-0.3), "state",
        ray(-0.2, -0.13), "update", "state",
        "hit", ray(0.5, 0.15), "hit", "btn -2 2",
    ]),
]

CONTROL_CASES += LATER_CASES


def tray(lx, ly):
    """The ray under the table point (lx, ly) of a table at (-0.3, 0.2, -1)."""
    return ray(-0.3 + lx, 0.2 + ly)


# The rich-text windows (the fifth increment): the edit box's keys, characters, caret, runs,
# modes, focus and blink; the read-only page edit field; the fast table's hover, selection, edge
# drags, setters, tooltips and draws; the scroll table's bar and callback.
RICH_CASES = [
    # Typing, the caret keys, Delete and Backspace, the formatting keys and their runs, Enter
    # and Escape to the parent, the caret's clamp, GetText.
    " ; ".join([
        "new RichTextEditWin", "state",
        "echar 65", "echar 98", "echar 99", "state",
        "ekey 37 0 1", "echar 88", "state",
        "ekey 36 0 1", "ekey 46 0 1", "state",
        "ekey 35 0 1", "ekey 8 0 1", "state",
        "ekey 8 0 0", "echar 13", "echar 127", "state",
        "ekey 66 4 1", "echar 122", "state",
        "ekey 66 0 1", "ekey 73 4 1", "ekey 85 4 1", "echar 113", "state",
        "ekey 107 4 1", "ekey 107 4 1", "echar 114", "ekey 109 4 1", "echar 115", "state",
        "attrs fffffff9", "ekey 107 4 1", "state", "attrs ffffff01", "ekey 109 4 1", "state",
        "fmask 0", "ekey 66 4 1", "ekey 107 4 1", "state",
        "ekey 13 0 1", "ekey 27 0 1", "ekey 9 0 1", "ekey 45 0 1", "ekey 65 0 1",
        "caret 2", "state", "caret 0", "state", "caret -5", "state", "caret 99", "state",
        "gettext 0", "gettext 1", "gettext 3", "gettext 64",
        "hit", "btn -1 2",
    ]),
    # The modes: the maximum length, the case modes, a deny and an allow list with their
    # replacement, password runs, the fixed style, a prefix, text with attributes, clearing.
    " ; ".join([
        "new RichTextEditWin",
        "maxlen 3", "settext " + text("hello"), "state", "echar 120", "state",
        "maxlen -1", "maxlen 0", "case 1", "echar 97", "settext " + text("MiXed"), "state",
        "case 2", "echar 81", "settext " + text("MiXed"), "state", "case 7",
        "settext " + text("MiXed"), "state", "case 0",
        "filter " + text("xyz"), "echar 121", "settext " + text("oxo"), "state",
        "allow 1", "echar 122", "echar 97", "repl 2a", "echar 98", "state",
        "allow 0", "repl 5f",
        "password 1", "state", "password 1", "password 0", "state",
        "fixedlist ffff0008", "fixed 1", "state", "fixed 0", "state",
        "prefix " + text("> "), "state",
        "settexta " + text("abc") + " 00ff0004", "state",
        "caret 1", "echar 100", "state",
        "cleartext", "state",
    ]),
    # The input focus and the events, read-only and USER_CANCEL, the caret blink, the caret's
    # draw while it holds the focus, a deactivation that hides the box.
    " ; ".join([
        "new RichTextEditWin", "state", "draw",
        "activate", "state", "editable 0", "editable 0", "ecancel 0", "ecancel 5",
        "editable 1", "ecancel 5",
        "dt " + fb(0.2), "update", "state", "dt " + fb(0.35), "update", "state",
        "dt " + fb(1.2), "update", "state", "draw", "dt " + fb(0.25), "update", "state",
        "draw", "update", "draw",
        "deactivate 0", "state", "draw", "activate", "deactivate 1", "state", "draw",
        "ekey 37 0 1", "echar 65", "state",
    ]),
    # The page's edit field: read-only, its payload's size and caption.
    " ; ".join([
        "new UITextEdit " + fbs(0.5, 0.04) + " " + text("Player"), "state",
        "echar 33", "state", "activate", "deactivate 0",
    ]),
    # A fast table: the hover, the selection by a press and a double click, a press off the
    # rows, the edge drags, the edge setters, the draws with quads and with the group outline.
    " ; ".join([
        "new UIFastTable " + fbs(0.6, 0.4) + " 4 3", "state",
        xform(-0.3, 0.2, -1.0), "state",
        "frows 1 1 1 0 1 1 1 7 1 1 0 7 0 1 1 0", "draw",
        tray(0.25, -0.15), "update", "state", "btn -1 2", "draw",
        "btn -1 3", "btn -1 2", "selrow 2", "selrow 3", "selrow 9", "getsel", "clearsel",
        "getsel", "state",
        tray(0.25, -0.35), "update", "state", "btn -1 2",
        tray(0.2015, -0.15), "update", "state", "btn -1 2", "state",
        tray(0.3, -0.15), "update", "state", "btn -1 1", "state", "update",
        tray(0.25, -0.2015), "update", "state",
        tray(0.25, -0.1985), "update", "state", "btn -1 2",
        tray(0.25, -0.25), "update", "state", "btn -1 1",
        tray(0.1985, -0.15), "update", "state",
        "hdr 1", tray(0.1985, -0.05), "update", "state", "hdr 9", "state",
        "cedge 0 " + fb(0.1), "cedge 1 " + fb(0.001), "cedge 2 " + fb(0.9), "cedge 3 " + fb(0.1),
        "redge 0 " + fb(-0.1), "redge 1 " + fb(-0.05), "redge 3 " + fb(-0.12), "redge 4 " + fb(-0.1),
        "state",
        "setedges 4 " + fbs(0.0, -0.1, -0.2, -0.3) + " 3 " + fbs(0.0, 0.2, 0.4), "state",
        "setedges 4 " + fbs(0.0, -0.1, -0.2, -0.3) + " 2 " + fbs(0.0, 0.2),
        "setedges 4 " + fbs(0.0, -0.1, -0.2, -0.3) + " 3 " + fbs(0.1, 0.2, 0.4), "state",
        tray(0.25, -0.15), "update", "highlights 0", "draw",
        "borders 1 0 1 0", "collines 0", "draw", "collines 1", "hdr 2", "draw",
        "resizable 0 0", tray(0.2015, -0.15), "update", "state",
        "grab " + fb(-1.0), "grab " + fb(0.5), "state",
        "clearcell 1 2", "clearcell 4 0", "clearcell 0 3", "span 1 1 1", "invalidate 9 0",
        "margins 3 1", "state",
        "leave", "hit", tray(0.7, -0.1), "hit", tray(0.1, 0.01), "hit",
    ]),
    # The tooltips: the window's record, per column, per row and per cell.
    " ; ".join([
        "new UIFastTable " + fbs(0.6, 0.4) + " 2 2", xform(-0.3, 0.2, -1.0),
        "tooltip 1", tray(0.45, -0.25), "update", "wtip 21", "update",
        "tipmode 1", "state", "coltip 1 55", "update",
        "tipmode 2", "state", "rtip 1 33", "update", tray(0.45, -0.05), "update",
        "tipmode 3", "state", "ctip 1 1 44", tray(0.45, -0.25), "update",
        "tipmode 0", "state", "update", "tooltip 0", "update",
    ]),
    # A scroll table: the bar made with the table, the item counts, the callback, the bar's
    # value, the gap, the transform moving the bar, the draw.
    " ; ".join([
        "new UIScrollTable " + fbs(0.5, 0.3) + " 3 2", "state", "bstate",
        "items 10 0", "state", "bstate", "callback", "barvalue " + fb(4.6), "first", "scrolled",
        "items 10 1", "bstate", "scrolled", "barvalue " + fb(9.0), "first",
        "gap " + fb(0.01), "bstate", xform(-0.3, 0.2, -1.0), "bstate", "draw",
        "items 2 0", "state", "bstate", "first", "draw",
    ]),
]

CONTROL_CASES += RICH_CASES

# P4.13, seventh increment: the engine's interface pass.
PASS_CASES = [
    # A static control that renders to a window: the instance's box projected, the window
    # camera's rectangle, planes and angle, the passes through it, the Win3D camera's viewport
    # back; then a box off the surface, which draws nothing; then one on z-clear, and the
    # option gate.
    " ; ".join([
        "new StaticControl",
        "init " + hx("window.cmp") + " " + fbs(0.0, 0.0, 0.0) + " - " + fbs(0.1, 0.08, 0.1) +
        " 0 0 0 0 0 1 0 0 0 0 " + fbs(0.0, 0.0, 0.0),
        xform(0.1, -0.05, -1.0), "state", "draw",
        "colours 10203040 50607080", "draw",
        xform(5.0, 0.0, -1.0), "draw",
        xform(-0.2, 0.1, -1.5), "prop 2 1 0", "draw",
        "gopt 21 0", "draw", "gopt 21 1",
    ]),
]

CONTROL_CASES += PASS_CASES

CASES = [
    # A front-end page of every class, in the shipped files' style.
    "#Automatically generated. Do not edit!\n"
    "UIWinObject ServerInfoBox -.642252604166667 -.316080729166667  .882161458333333  .17578125 -1.0000 interface\\intro\\objects\\info.cmp \n"
    "UIFastTable ServerTable -.58203125  .229166666666667  1.17838541666667  .393880208333333 9.0000 8.0000 \n"
    "UITextMsgButton MainMenu -.58203125 -.241203125  .3125  6.51041666666667E-02 \"Back\" 1001.0000 \"<default>\"  393251 \n"
    "UIScroll ServerScroll  .602864583333333  .203125  2.27864583333333E-02  .367838541666667 0.0000 100.0000 14.0000 1.0000  0 \n"
    "UIRichTextWin ServerDetails -.629231770833333 -.3486328125  .836588541666667  .113932291666667 \n"
    "UIStaticText Caption -0.5 0.4 1.0 0.05 \"Select a server\"\n"
    "UITextEdit Name -0.25 0.1 0.5 0.04 \"Player\"\n"
    "UIListBox List -0.4 0.0 0.8 0.3\n"
    "UIWinDeform Deform 0 0 1 1\n"
    "UIShipViewControl Ship -0.1 -0.1 0.2 0.2\n"
    "UITable Table -0.3 0.2 0.6 0.4 5.9 -2.9\n"
    "UICycle Cycle 0.1 0.2 0.3 0.04 1 2 3 4 1\n",
    # Optional fields absent, and the stale float of a short button line.
    "UIStaticText NoCaption 0 0 1 1\n"
    "UITextMsgButton A 0 0 1 1 \"a\" 7.9 \"<none>\" -5\n"
    "UITextMsgButton B 0 0 1 1 \"b\"\n"
    "UITextMsgButton C 0 0 1 1 \"c\" 12.5\n"
    "UITextMsgButton D 0 0 1 1 \"d\" -3.99 \"\" 9\n"
    "UITextMsgButton E 0 0 1 1 \"e\" 1 \"Custom Style\"\n"
    "UIScroll S 1 2 3 4 5 6 7 8\n"
    "UIScroll T 1 2 3 4 5 6 7 8 7\n",
    # Comments, indented and empty lines, CR LF endings.
    "# comment\n"
    "   UIListBox Indented 0 0 1 1\n"
    "\n"
    "\tUIListBox Tab 0 0 1 1\n"
    "UIListBox A 1 2 3 4\r\n"
    "\r\n"
    "UIListBox B 5 6 7 8\r\n",
    # Number forms.
    "UIListBox N -.5 5. 1e3 1E-02\n"
    "UIListBox M +2.5 -0 000.125 3.4028234e38\n"
    "UIListBox P 0.1 0.2 0.3 0.7\n",
    # Captions: spaces, bytes above 0x7F, an empty caption.
    "UIStaticText Spaces 0 0 1 1 \"  two  words \"\n"
    "UIStaticText High 0 0 1 1 \"caf\xe9 \xff\x80\"\n"
    "UIStaticText Empty 0 0 1 1 \"\"\n",
    # An empty caption fails a button.
    "UIListBox Ok 0 0 1 1\nUITextMsgButton Bad 0 0 1 1 \"\" 1\nUIListBox Never 0 0 1 1\n",
    # An unknown class fails the load; the entries before it stay counted.
    "UIListBox One 0 0 1 1\nUIListBox Two 0 0 1 1\nUIButton Three 0 0 1 1\n",
    # Too few floats.
    "UIListBox Short 0 0 1\n",
    # A table without its sixth float.
    "UITable T 0 0 1 1 5\n",
    # A win object without its mesh.
    "UIWinObject W 0 0 1 1 2\n",
    # A scroll bar with nine fields.
    "UIScroll S 0 0 1 1 1 2 3\n",
    # A class token alone.
    "UIListBox\n",
    # A line longer than 255 bytes: the rest starts with white space.
    "UIListBox Long 0 0 1 1" + " " * 300 + "\nUIListBox After 0 0 1 1\n",
    # A line longer than 255 bytes whose rest is a token.
    "UIListBox Long 0 0 1 1" + " " * 233 + "x" * 40 + "\n",
    # An empty file and a file of comments only.
    "",
    "# nothing\n# here\n",
    # A last line without a newline.
    "UIListBox Last 9 8 7 6",
]


def write(here, name, lines, check):
    data = ("\n".join(lines) + "\n").encode("latin-1")
    path = os.path.join(here, name)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != data:
                sys.exit("%s is out of date; run make_ui_fixtures.py" % name)
        return
    with open(path, "wb") as out:
        out.write(data)


def main():
    check = "--check" in sys.argv[1:]
    here = os.path.dirname(os.path.abspath(__file__))
    lines = [
        "# P4.13 page descriptor oracle inputs, written by make_ui_fixtures.py.",
        "# One case per line: the bytes of a PAGE file in hex, - for an empty file.",
    ]
    for case in CASES:
        data = case.encode("latin-1")
        lines.append(data.hex() if data else "-")
    write(here, "ui_page_desc.list", lines, check)
    lines = [
        "# P4.13 screen-quad batcher oracle inputs, written by make_ui_fixtures.py.",
        "# One case per line: operations separated by \" ; \" (oracle_ui_quads.cpp).",
    ] + QUAD_CASES
    write(here, "ui_quad_batch.list", lines, check)
    lines = [
        "# P4.13 control oracle inputs, written by make_ui_fixtures.py.",
        "# One case per line: operations separated by \" ; \" (oracle_ui_controls.cpp).",
    ] + CONTROL_CASES
    write(here, "ui_controls.list", lines, check)


if __name__ == "__main__":
    main()
