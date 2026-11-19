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
