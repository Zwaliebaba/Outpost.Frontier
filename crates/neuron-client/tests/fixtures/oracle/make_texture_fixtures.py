"""Writes texture_runtime.list and big_image.list, the inputs of the P4.4
suites (Oracle.exe neuron-client, CppSource/Oracle/oracle_texture.cpp and
oracle_big_image.cpp, and crates/neuron-client/tests/oracle_vectors.rs).
Run from any folder:

    python make_texture_fixtures.py

Both lists hold one case per line: operations separated by " ; ", their
fields by single spaces; floats are eight hex digits of the f32. The
operations are listed at the top of the two C++ files.

The cases keep to what the reference defines: no record is used after its
destruction, no null child is retained or released, and no frame is read
past its table (the Rust test checks the first and the last). Rates of
zero are not advanced, so no NaN reaches the vectors.
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def f(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def frames(*rects):
    """child:u0:v0:u1:v1 per frame."""
    return "|".join("%d:%s" % (c, ":".join(f(v) for v in uv)) for c, uv in rects)


QUARTERS = [
    (0, (0.0, 0.0, 0.5, 0.5)),
    (1, (0.5, 0.0, 1.0, 0.5)),
    (2, (0.0, 0.5, 0.5, 1.0)),
    (1, (0.5, 0.5, 1.0, 1.0)),
]


def runtime_cases():
    cases = []
    # Records, frames and the record queries.
    cases.append([
        "tex a 11", "tex b 12", "tex c 13",
        "anim an %s a,b,c %s" % (f(24.0), frames(*QUARTERS)),
        "dump",
        "tframe a 0", "tframe an 2", "tframe an -2", "tframe an 7", "tframe - 0",
        "count an", "count a", "count -", "gettile an", "gettile -",
        "copy an 256", "copy an 2", "copy an 1", "copy - 8",
        "fmt an", "fmt a", "fmt -",
        "pick 7", "pick 3", "pick 1", "pick 6", "pick 5", "pick 10", "pick 12",
        "pick 0", "pick 9", "pick 2", "pick 4", "pick 8",
    ])
    # A looping playback over time.
    cases.append([
        "tex a 1", "tex b 2", "tex c 3",
        "anim w %s a,b,c %s" % (f(10.0), frames(*QUARTERS[:3])),
        "play w", "dump",
        "adv 1 %s" % f(0.1), "getidx 1", "getel 1",
        "adv 1 %s" % f(0.25), "getidx 1", "frame 1 -1", "frame 1 1", "frame 1 9",
        "adv 1 %s" % f(1.0), "frame 1 -1", "dump",
        "tick 0 %s" % f(1.0 / 30.0), "tick 1 %s" % f(1.0), "tick 0 %s" % f(1.0 / 30.0),
        "getel 1", "getidx 1",
        "setel 1 %s" % f(1.5), "getidx 1", "setidx 1 2", "getidx 1", "setidx 1 5",
        "getidx 1",
        "getrate 1", "setrate 1 %s" % f(5.0), "getrate 1", "dump",
        "adv 1 %s" % f(0.3), "getidx 1", "frame 1 -1",
        "setrate 1 %s" % f(1e-6), "getrate 1", "adv 1 %s" % f(0.7), "getidx 1",
        "getel 1", "dump",
        "getel 9", "getidx -1", "getrate 9", "adv 9 %s" % f(1.0), "setel 9 0",
        "frame 9 0", "setidx 9 0", "setrate 9 0", "setwrap 9 0", "getwrap -1",
    ])
    # Clamp, ping-pong, an unknown wrap mode and a negative rate.
    cases.append([
        "tex a 1", "tex b 2", "tex c 3", "tex d 4",
        "anim p %s a,b,c,d %s" % (f(8.0), frames(*QUARTERS)),
        "play p", "play p", "play p",
        "setwrap 1 0", "setwrap 2 2", "getwrap 1", "getwrap 2", "getwrap 9",
        "adv 1 %s" % f(0.5), "getidx 1", "getel 1",
        "adv 1 %s" % f(1.0), "getidx 1", "frame 1 -1",
        "adv 2 %s" % f(0.25), "getidx 2", "adv 2 %s" % f(0.375), "dump",
        "getidx 2", "frame 2 -1", "adv 2 %s" % f(0.25), "adv 2 %s" % f(0.5), "dump",
        "getidx 2", "frame 2 -1", "adv 2 %s" % f(0.125), "getidx 2",
        "setwrap 1 7", "adv 1 %s" % f(0.25), "getidx 1", "getel 1",
        "setrate 3 %s" % f(-2.0), "getrate 3", "adv 3 %s" % f(0.3), "getidx 3",
        "getel 3", "setel 3 %s" % f(0.75), "getidx 3", "dump",
        "tick 0 %s" % f(0.0625), "dump",
    ])
    # Reference counts, groups, enumeration, release and garbage collection.
    cases.append([
        "tex a 21", "tex b 22", "tex c 23",
        "anim m %s a,b %s" % (f(12.0), frames(*QUARTERS[:2])),
        "play m", "play a", "playnull b", "dump",
        "group", "group", "gname 1 first", "getname 1 256", "getname 1 3",
        "getname 2 8", "getname 0 8", "getname 9 8", "gname 0 x", "gname 2 -",
        "gname 7 x",
        "gadd 1 m", "gadd 1 b", "gadd 0 a", "gadd 9 a", "gadd 2 -", "dump",
        "subcount 0", "subcount 1", "subcount 2", "subcount 7",
        "enumg 0", "enumg 1", "enumg 2", "enumg 3", "enumg -1", "peek",
        "enumt 0 0 1", "enumt 0 1 0", "enumt 0 9 1", "enumt 1 0 1", "enumt 1 1 1",
        "enumt 1 5 1", "enumt 8 0 1", "dump",
        "resolve 3", "resolve 99", "resolve -1",
        "gdel 1 b", "dump", "gdel 1 a", "gdel 0 a", "gdel 9 a", "dump",
        "addref 1", "addref 99", "decref 1", "decref 1", "dump", "decref 1",
        "decref 99", "dump",
        "release c", "release -", "dump",
        "gc 1", "dump", "gc 0", "dump", "gc 5", "dump",
    ])
    # Sub-textures, renaming, lookups, frame edits and the generic slots.
    cases.append([
        "tex a 31", "tex b 32",
        "anim s %s a,b %s" % (f(1.0), frames(*QUARTERS[:2])),
        "play b", "play s",
        "named fresh", "named fresh", "named a", "find a 1", "find a 0",
        "find zz 1", "dump",
        "sub s 1 31", "dump", "sub a 0 77", "dump", "sub - 0 31",
        "tframe s 0", "tframe s 1",
        "fhandle s 0 1", "fhandle s -2 0", "fhandle s 5 0", "fhandle - 0 0",
        "frect s 1 %s %s %s %s" % (f(0.25), f(0.25), f(0.75), f(0.75)),
        "frect s -2 %s %s %s %s" % (f(0.0), f(0.125), f(1.0), f(0.875)),
        "frect s 9 0 0 0 0",
        "settile s %s" % f(15.0), "settile - 0", "gettile s",
        "tframe s 0", "tframe s 1", "frame 2 0", "frame 2 -1", "dump",
        "sub s 3 32", "dump",
        "rename a renamed", "rename a b", "rename - a", "dump",
        "copy a 256", "find b 0",
        "slot 2 1", "slot 3 2", "getslot 2", "getslot 0", "getslot 4",
        "slot 6 9", "getslot 6", "getslot 1",
    ])
    # A playback of one frame, and of a record without texture.
    cases.append([
        "named n", "play n", "tex t 5", "play t", "dump",
        "tick 0 %s" % f(1.0), "frame 1 -1", "frame 1 3", "getel 1",
        "adv 1 %s" % f(1.0), "getel 1", "setel 1 %s" % f(2.0), "getidx 1", "getel 1",
        "frame 2 -1", "frame 2 0", "fmt t", "fmt n",
        "decref 1", "dump", "gc 0", "dump", "decref 2", "dump", "gc 0", "dump",
    ])
    return cases


def big_image_cases():
    cases = []
    cases.append([
        "image 0 4 4 3 3 10 6 20 120", "size", "count",
        "draw - 0 0 10 6", "draw 1 1 9 5 20 30 40 50", "draw 9 5 1 1 0 0 100 60",
        "draw 0 0 0 0 0 0 10 6", "draw 0 3 10 3 0 0 10 6", "draw 4 0 8 6 0 0 8 12",
        "tile 0", "tile 5", "tile 6", "tile -1",
        "alpha %s" % f(0.5), "draw - 0 0 5 3",
        "color %s %s %s" % (f(1.0), f(0.5), f(0.0)), "draw - 0 0 5 3",
        "fill 3 10 6 20", "fill 3 9 6 20", "fill 7 10 6 40",
    ])
    cases.append(["image 1 4 4 7 7 10 6 40 240", "count", "draw - 0 0 10 6",
                  "draw 2 1 10 6 -5 -5 15 15"])
    cases.append(["image 1 4 4 7 7 6 10 24 240", "draw - 0 0 6 10"])
    cases.append(["image 0 4 4 3 3 8 8 16 128", "draw 2 2 6 6 0 0 64 64",
                  "draw - 0 0 8 8"])
    cases.append(["image 0 256 256 7 7 100 50 400 20000", "count",
                  "draw - 10 10 110 60", "draw 20 10 80 40 0 0 300 150"])
    cases.append(["image 0 4 4 3 3 0 0 0 0", "count", "size", "draw - 0 0 10 10",
                  "tile 0"])
    cases.append(["image 0 4 4 12 12 8 8 32 64", "draw - 0 0 8 8"])
    cases.append(["image 0 4 4 3 0 10 6 20 120"])
    cases.append(["image 0 4 4 6 6 5 5 10 40", "draw - 0 0 5 5"])
    cases.append([
        "image 0 8 8 7 7 8 8 32 256",
        "alpha %s" % f(1.0), "draw - 0 0 8 8",
        "alpha %s" % f(-1.0), "draw - 0 0 8 8",
        "alpha %s" % f(255.0), "draw - 0 0 8 8",
        "color %s %s %s" % (f(0.25), f(0.75), f(1.0)), "draw - 0 0 8 8",
        "draw - 3 7 2 9",
    ])
    return cases


def write(name, header, cases):
    with open(os.path.join(HERE, name), "w", newline="\n") as out:
        out.write("# " + header + "\n")
        for ops in cases:
            out.write(" ; ".join(ops) + "\n")


def main():
    write("texture_runtime.list",
          "P4.4 texture library oracle inputs, written by make_texture_fixtures.py.",
          runtime_cases())
    write("big_image.list",
          "P4.4 BigImage oracle inputs, written by make_texture_fixtures.py.",
          big_image_cases())


if __name__ == "__main__":
    main()
