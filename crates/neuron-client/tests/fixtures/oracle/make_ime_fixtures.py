"""Writes ime_ui.list, the input of the P4.11 to P4.13 IME suite (Oracle.exe
neuron-client, oracle_ime_ui.cpp, and crates/neuron-client/tests/oracle_vectors.rs).
Run from any folder:

    python make_ime_fixtures.py [--check]

With --check nothing is written; the script fails when the list differs from what
it would write.

ime_ui.list, one case per line: operations separated by " ; " (the format is in
oracle_ime_ui.cpp). Texts are UTF-16 units in hex, "-" for none.
"""

import os
import sys


def u(text):
    if text == "":
        return "-"
    data = text.encode("utf-16-le")
    return "".join("%02x%02x" % (data[i + 1], data[i]) for i in range(0, len(data), 2))


def typed(box, text):
    return ["wc %d %04x" % (box, ord(c)) for c in text]


VK_BACK, VK_END, VK_HOME, VK_LEFT, VK_RIGHT, VK_INSERT, VK_DELETE = 8, 35, 36, 37, 39, 45, 46

CASES = [
    # The appearance ImeUi.dll's strings give, as set; the number parser's quirks.
    ["focus", "appearance", "parse " + u("0x1g2"), "parse " + u("0XfF"), "parse " + u("123abc"),
     "parse " + u("x12"), "parse -", "parse " + u("0x"), "parse " + u("4294967296")],
    # The orientation and symbol of each language; English keeps the orientation Korean left.
    ["focus", "langstate", "lang 04110411", "langstate", "lang 04120412", "langstate",
     "lang 04090409", "langstate", "lang 08040804", "langstate", "lang 04040404", "langstate",
     "lang 0c040c04", "langstate", "lang 10041004", "langstate", "lang e0010411", "langstate"],
    # Candidate pages: the page of the selection, the formats, Korean and Simplified Chinese.
    ["focus", "box", "cand 0 9 %s %s %s" % (u("a"), u("bb"), u("ccc")), "candstate",
     "cand 12 5 " + " ".join(u("w%d" % i) for i in range(15)), "candstate",
     "cand 3 20 " + " ".join(u(c) for c in "abcdefghijkl"), "candstate",
     "lang 04120412", "cand 1 9 %s %s" % (u("x"), u("y")), "candstate",
     "lang 08040804", "cand 2 4 %s %s %s" % (u("p"), u("q"), u("r")), "candstate",
     "nocand", "candstate"],
    # Typing into the focused box: the owner's text, the caret, refused control characters,
    # the multibyte entry, Delete, Backspace, Left, Home, End and overwrite mode.
    ["focus", "box", "resize 0 80 5 7 200 30", "state 0"] + typed(0, "hi") +
    ["state 0", "mb 0 33", "wc 0 0008", "wc 0 0085", "key 0 %d" % VK_LEFT, "state 0",
     "key 0 %d" % VK_DELETE, "state 0", "wc 0 0058", "state 0", "key 0 %d" % VK_BACK, "state 0",
     "key 0 %d" % VK_HOME, "key 0 %d" % VK_HOME, "key 0 %d" % VK_END, "key 0 %d" % VK_RIGHT,
     "key 0 %d" % VK_INSERT, "state 0", "key 0 %d" % VK_HOME, "wc 0 004d", "state 0",
     "key 0 %d" % VK_INSERT, "key 0 65", "state 0", "fstate"],
    # A buffer of four units: the last unit overwritten; the buffer grown and filled again, its
    # last unit overwritten; shrunk with the caret at the start; a cut tail typed over. (Typing
    # into the middle of a full buffer, or a buffer smaller than the caret, is undefined
    # behaviour in the reference, D282.)
    ["focus", "box", "resize 0 4 0 0 100 20"] + typed(0, "abcde") +
    ["state 0", "resize 0 6 0 0 100 20", "state 0", "key 0 %d" % VK_END, "onchar 0062",
     "onchar 0063", "onchar 0064", "state 0", "key 0 %d" % VK_HOME, "resize 0 5 0 0 100 20",
     "state 0", "key 0 %d" % VK_DELETE, "wc 0 0051", "state 0"],
    # The focus between boxes: the composition goes to the box losing it, nobody takes it over,
    # the window's focus, a forced focus, the IME's characters and keys, a focused box deleted.
    ["focus", "box", "box", "fstate", "comp %s 0001 1" % u("xy"), "setfocus 1", "state 0",
     "state 1", "fstate", "setfocus 1", "force 1", "fstate", "onchar 0041", "onkey %d" % VK_LEFT,
     "onchar 0042", "state 1", "comp %s 00 1" % u("k"), "win 0", "state 1", "fstate", "win 1",
     "state 1", "setfocus -", "fstate", "state 1", "setfocus 0", "comp %s 00 1" % u("q"),
     "del 0", "fstate", "box", "fstate", "state 2"],
    # The caret's blink: shown and hidden by the tick count, hidden while composing, steady in
    # overwrite mode, the tick count wrapping.
    ["blink 400", "focus", "box", "tick 1000", "caret 0", "tick 1300", "caret 0", "tick 1500",
     "caret 0", "comp %s 00 1" % u("a"), "caret 0", "endcomp", "caret 0", "key 0 %d" % VK_INSERT,
     "caret 0", "tick 900000", "caret 0", "key 0 %d" % VK_INSERT, "tick 100", "caret 0",
     "tick 600", "caret 0"],
    # The box's font: extents, the height made negative, a face made again, deleted with the
    # box; a smaller font for the next box.
    ["focus", "box", "extent 0 -", "extent 0 %s" % u("ab"), "extent 0 %s" % u("il.M"), "gdi",
     "fheight 0 20", "extent 0 %s" % u("MW"), "fheight 0 20", "extent 0 %s" % u("x"), "gdi",
     "del 0", "gdi", "lf 10 %s" % u("Tahoma"), "box", "extent 1 %s" % u("ab"), "gdi"],
    # The toolbar: on (the rainbow), off, turning every 250 ms, beside the box or before it.
    ["focus", "box", "resize 0 80 5 7 200 30", "draw", "tick 251", "draw", "tick 400", "draw",
     "tick 502", "draw", "imestate 0 1", "draw"],
    ["screen 220 100", "focus", "box", "resize 0 80 0 0 200 40", "draw", "lang 04120412",
     "draw"],
    # The composition at level 2: each unit's background, the caret in its blink, the text
    # through the box, moved left by the box's own text.
    ["focus", "box", "resize 0 80 0 0 300 40", "comp %s 000102 2" % u("abc"), "draw",
     "tick 600", "draw", "comp %s 0304 0" % u("Mi"), "tick 1200", "draw"],
    ["focus", "box", "resize 0 80 0 0 300 40"] + typed(0, "hey") +
    ["tick 501", "comp %s 0001 2" % u("ab"), "draw", "key 0 %d" % VK_HOME, "draw"],
    # Korean draws the composition only in the caret's visible phase.
    ["focus", "box", "resize 0 80 0 0 300 40", "lang 04120412", "comp %s 00 1" % u("k"),
     "draw", "tick 501", "draw"],
    # The candidate list: vertical below the conversion target, its selection; horizontal
    # (Korean); above the line near the bottom; clipped at the surface's edges.
    ["focus", "box", "resize 0 80 0 0 300 40", "comp %s 000100 1" % u("abc"),
     "cand 1 9 %s %s %s" % (u("x"), u("yy"), u("zzz")), "draw", "nocand", "draw"],
    ["focus", "box", "resize 0 80 0 0 300 40", "lang 04120412", "tick 501",
     "comp %s 00 1" % u("k"), "cand 0 9 %s %s %s" % (u("a"), u("bb"), u("c")), "draw"],
    ["screen 400 60", "focus", "box", "resize 0 80 0 0 390 50", "lang 04110411",
     "comp %s 01 0" % u("W"), "cand 4 3 %s %s %s %s %s" % (u("m"), u("nn"), u("o"), u("p"), u("qq")),
     "draw"],
    ["screen 100 100", "focus", "box", "resize 0 80 0 0 90 20"] + typed(0, "abcdefgh") +
    ["comp %s 0000 1" % u("MM"), "draw"],
    # Nothing draws once the window loses the focus or no box has it.
    ["focus", "box", "resize 0 80 0 0 300 40", "comp %s 00 1" % u("a"), "win 0", "draw",
     "win 1", "setfocus -", "draw"],
]


def render():
    return "".join(" ; ".join(case) + "\n" for case in CASES)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "ime_ui.list")
    text = render()
    if "--check" in sys.argv[1:]:
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() != text:
                sys.exit("ime_ui.list differs from make_ime_fixtures.py; run it without --check")
        return
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


if __name__ == "__main__":
    main()
