"""Writes the inputs of the P4.5 material suites (Oracle.exe neuron-client,
CppSource/Oracle/oracle_material.cpp, and crates/neuron-client/tests/
oracle_vectors.rs): material_regex.list, material_library.list,
material_global.list, material_animation.list, material_batcher.list and the
INI files they load. Run from any folder:

    python make_material_fixtures.py [--check]

The lists hold one case per line: operations separated by " ; ", their fields
by single spaces; floats are eight hex digits of the f32. The operations are
those of oracle_material.cpp. The patterns and INI lines are written for these
suites in the shapes of the game's [MaterialMap] and [BatchedMaterials]; the
game's own lines are exercised by tests/material_data.rs from outdir/ when it
runs (decision O15).

The cases keep to what the reference defines: no map entry is used after its
material was cleared or destroyed, RemoveMaterial is only asked for CRCs the
map holds, no animation group is unloaded twice, only materials with the
batched face are queued, every queued submission reads inside the arrays it
was given, and the scratch arena (BufferSize) holds every frame's copies.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def f(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def hx(text):
    if isinstance(text, str):
        text = text.encode("latin-1")
    return text.hex()


# --- material_regex -------------------------------------------------------------------------

REGEX = [
    ("ab", ["ab", "xab", "a", "", "abab", "aab"]),
    ("^alpha_.*", ["alpha_x", "xalpha_", "alpha_", "alpha"]),
    ("^mask_.*2side", ["mask_a2side", "mask_2side", "mask_a2sidex", "mask_", "xmask_2side"]),
    (".*OcOt", ["DcDtOcOt", "OcOt", "DcDt", "OcO"]),
    ("^tlr_glow$", ["tlr_glow", "tlr_glows", "atlr_glow"]),
    ("^n-tex.*$", ["n-tex", "n-texture01", "ntex"]),
    ("^planet.*_glass$", ["planet01_glass", "planet_glass", "planetglass", "planet_glass_"]),
    ("^ui_.*", ["ui_", "ui_icon", "aui_"]),
    ("[a-c]+x", ["zzbbcx", "x", "cx", "dx"]),
    ("[^a-c]x", ["ax", "dx", "x"]),
    ("(ab|cd)+e", ["xxcdabe", "e", "abe", "abcd"]),
    ("a?b", ["b", "ab", "aab", "a"]),
    ("\\<word\\>", ["a word here", "swordfish", "word", "words", "_word", "word_"]),
    ("[\\d]+z", ["x9z", "z", "12z"]),
    ("[\\s\\-\\\\]", ["a b", "a-b", "a\\b", "ab", "a\tb"]),
    ("[\\q\\n]", ["q", "\n", "\\"]),
    ("a|b|c", ["c", "d", "abc"]),
    ("a\nb", ["b", "c"]),
    ("x*", ["", "y", "xxx"]),
    ("x*y", ["xxy", "y", "x"]),
    ("(xy)*z", ["xyxyz", "z", "xyx"]),
    ("(xy)+z", ["xyxyz", "z", "xz"]),
    ("(xy)?z", ["xyz", "z", "xxz"]),
    ("a\\.b", ["a.b", "axb"]),
    ("a\\.*b", ["a..b", "ab", "axb"]),
    ("\\$x", ["$x", "x"]),
    ("abc*d", ["abd", "abccd", "acd"]),
    ("ab+c", ["abbc", "ac"]),
    ("a.c", ["abc", "ac", "a\nc"]),
    (".*", ["", "anything"]),
    (".+x", ["x", "ax"]),
    ("^$", ["", "a"]),
    ("$", ["", "a"]),
    ("^", ["", "a"]),
    ("[]a]", ["]", "a", "b"]),
    ("[-a]", ["-", "a", "b"]),
    ("[a-]", ["-", "b"]),
    ("[^]x]", ["]", "x", "y"]),
    ("[a-a]x", ["ax", "x"]),
    ("x(a)(b)(c)(d)(e)(f)(g)(h)(i)", ["xabcdefghi", "xabc"]),
    ("(a|ab)(c|bcd)(d*)", ["abcd", "acd", "abd"]),
    ("a*bcdef", ["bcdef", "aabcdefg", "bcde"]),
    (".*hello.*world", ["hello world", "world hello", "say hello to the world"]),
    ("hello|world", ["world", "hell"]),
    ("\xe9t\xe9", ["\xe9t\xe9", "ete"]),
    ("\\<x", ["\xe9x", "ax", "x"]),
    ("y\\>", ["y\xe9", "ya", "y"]),
    ("(" + "a" * 300 + ")b", ["a" * 300 + "b", "a" * 299 + "b"]),
    ("", ["", "abc"]),
    ("*a", ["a"]),
    ("a**", ["a"]),
    ("a+*", ["a"]),
    ("(a", ["a"]),
    ("a)", ["a"]),
    ("[a", ["a"]),
    ("[b-a]", ["a"]),
    ("(a*)*", ["a"]),
    ("(a*)+", ["a"]),
    ("(a?)?", ["", "a"]),
    ("((((((((((a))))))))))", ["a"]),
    ("a\\", ["a"]),
    ("\\", [""]),
    ("a|*", ["a"]),
    ("a" * 32760, ["a"]),
]


def regex_list():
    return ["%s %s" % (hx(p), ",".join(hx(s) for s in subjects)) for p, subjects in REGEX]


# --- material_library -----------------------------------------------------------------------

LONG = "averyveryverylongmaterialnamethatgoesonandonpastthesixtythreebytelimit"
LONG_TYPE = "AnExtremelyLongTypeNameThatIsWellPastTheSixtyThreeByteTypeLimitX"

LIBRARY = [
    # Map rules, remapping and type resolution.
    [
        "library", "rule 1 DcDtEcEt DcDtEt", "rule 1 EcEtOcOt DcDtOcOt", "rule 0 ^alpha_.* DcDt",
        "rule 0 ^alpha_.*2side DcDtTwo", "rule 5 odd Plain", "rule 1 ( Broken",
        "rule 0 ^long_.* " + LONG_TYPE, "dump", "findrule 1 EcEtOcOt", "findrule 0 EcEtOcOt",
        "findrule 5 odd", "findrule 0 ^long_.*", "remap alpha_x DcDt", "remap alpha_x2side DcDt",
        "remap foo EcEtOcOt", "remap foo DcDtEcEt", "remap foo Zz", "remap odd odd", "remap long_x Zz",
        "resolve alpha_x -", "resolve foo DcDtEcEt", "resolve DcDtEcEt -", "resolve foo Zz",
        "resolve odd -", "unrule 1 EcEtOcOt", "unrule 1 EcEtOcOt", "unrule 7 none",
        "remap foo EcEtOcOt", "unsup", "dump",
    ],
    # Creation, FindOrCreate, FindMaterialByCrc and reference counts.
    [
        "library", "rule 0 ^glass_.* HighGlassMaterial", "create Hull_A DcDt 1",
        "create glass_Top DcDt 1", "create glass_Top DcDt 0", "create bad BadInit 1",
        "create none Unknown 1", "create nm NotMaterial 1", "generic Panel NebulaTwo 1",
        "generic glass_Side Plain 0", "generic glass_Rim Plain 1", "foc Hull_A Plain",
        "foc hull_a Plain", "foc Fresh DcDtTwo", "foc Ghost Unknown", "foc Bad2 BadInit",
        "find Hull_A", "find nothing", "dump", "release #1", "release #1", "release #1",
        "release #8", "addref #5", "release #5", "create " + LONG + " Plain 1",
        "foc " + LONG + " Plain", "dump",
    ],
    # Groups: allocation, names, enumeration, members, removal and deletion.
    [
        "library", "gcount", "group", "group", "gcount", "enumg 0", "enumg 1", "enumg 2",
        "enumg 3", "gname 1 first", "gname 0 x", "gname -1 x", "gname 9 x", "getname 1 256",
        "getname 1 3", "getname 1 1", "getname 2 8", "getname 0 8", "getname 9 8",
        "gname 2 second", "gname 1 -", "getname 1 8", "add 1 m1 new:DcDt", "add 1 m2 new:Plain",
        "add 0 m3 new:DcDtTwo", "add 2 m1 #1", "add 0 m1 #1", "add 9 m4 new:Plain",
        "dump", "mcount 0", "mcount 1", "mcount 2", "mcount 9", "mcount -1",
        "enumm 1 0", "enumm 1 5", "enumm 0 2", "enumm 0 9", "enumm 9 0", "enumm 2 1",
        "enumm -1 0", "verify 1", "verify 0", "verify 9", "verify -1",
        "resolvet 2 3f800000", "resolvet 0 bf000000", "resolvet -1 00000000", "release #1",
        "release #1", "dump", "remove 1 m2", "remove 2 m1", "remove 0 m3", "remove 9 m1", "dump",
        "gdel 1", "gdel 1", "gdel -1", "gcount", "enumg 1", "dump", "add 0 q new:Plain",
        "add 2 r new:Plain", "gdel 0", "dump", "gdel 2", "dump", "group", "gcount", "enumg 1",
    ],
    # A batcher's library: [MaterialMap] and [BatchedMaterials] from the INI, wrapping.
    [
        "ini material_map.ini", "batcher", "dump", "brule 3 anything", "create Hull DcDtOcOt 1",
        "create glow_Ring Plain 1", "create Plain1 Plain 1", "create Mask BadType 1",
        "create X ZzOcOt 1", "create mask_a Plain 1", "create mask_b2side Plain 1",
        "create Weird Weird 1", "create c_glass DcDt 1", "create EcEtOcOt EcEtOcOt 0",
        "create " + LONG + " Plain 1", "foc " + LONG + " Plain", "foc Hull Plain",
        "bunrule 2 ^glow_.*", "bunrule 2 ^glow_.*", "create glow_Two Plain 1", "bclear",
        "create Hull2 DcDtOcOt 1", "dump",
    ],
    # A library of its own reading [MaterialMap].
    [
        "ini material_map.ini", "library", "parsemap", "dump", "remap mask_a Plain",
        "remap mask_x2side Plain", "remap tlr_glow X", "remap Weird Weird", "remap q Weird",
        "resolve mask_a -", "resolve x EcEtOcOt", "findrule 1 justkey", "create mask_q Plain 1",
        "create q Plain 1", "dump",
    ],
    # The defaults INI: no sections.
    ["ini material_defaults.ini", "batcher", "dump", "create Hull DcDtOcOt 1", "dump"],
]

MATERIAL_MAP_INI = """; P4.5 material suites: an engine INI in the shapes of the game's.
[MaterialBatcher]
BufferSize = 0x20000
Bin1SortOrder = 3

[BatchedMaterials]
Type = .*OcOt
name = ^glow_.*
other = ignored

[MaterialMap]                ; evaluated last line first
EcEtOcOt= DcDtOcOt           ; a type rule
DcDtEcEt= DcDtEt
name = ^mask_.* = DcDtOcOt   ; a name rule
name = ^mask_.*2side = DcDtTwo
NAME = ^tlr_glow$ = NebulaTwo
type = ^Weird$ = Plain
Name= ^c_glass$ = HighGlassMaterial
other = ^q$ = Plain
bare
justkey =
"""

MATERIAL_DEFAULTS_INI = """; P4.5 material suites: no material sections.
[Other]
Key = Value
"""

MATERIAL_BATCHER_INI = """; P4.5 material batcher suite.
[MaterialBatcher]
BufferSize = 0x40000
Bin1SortOrder = 3

[BatchedMaterials]
Type = .*OcOt
"""

MATERIAL_SORT_INI = """; P4.5 material batcher suite: ascending bin 1, a decimal buffer size.
[MaterialBatcher]
BufferSize = 262144
Bin1SortOrder = 2
"""

MATERIAL_WORDS_INI = """; P4.5 material batcher suite: the boolean words of the profile reader. No
; submission is queued in its case, so the one-byte arena is never used.
[MaterialBatcher]
BufferSize = true
Bin1SortOrder = yes
"""

# --- material_global ------------------------------------------------------------------------

GLOBAL = [
    ["get 0", "get 4", "value 2", "init", "get 0", "get 1", "get 2", "get 3", "get 4"],
    ["set 4 7", "get 4", "setv 4 9", "get 4", "value 4", "set 9 5", "get 5", "get 8", "get 9",
     "setv 7 3", "get 7", "act 6", "get 6", "set 20 1", "get 19", "get 20", "act 12", "get 12",
     "setv 0 4294967295", "value 0", "act 2", "value 2", "set 3 0", "get 3"],
]

# --- material_animation ---------------------------------------------------------------------


def anim(name, flags, deltas, keys):
    """NAME:COUNT:FLAGS:DELTAS:KEYS; deltas are (duration, a, b, c, d) per key, keys (a, b, c, d)
    from the second key on."""
    words = [f(v) for d in deltas for v in d]
    key_words = [f(v) for k in keys for v in k]
    return "%s:%d:%d:%s:%s" % (name, len(deltas), flags, "/".join(words), "/".join(key_words))


A = anim("anim_a", 1, [(1.0, 0.5, 0.25, -0.5, 2.0), (2.0, 1.0, 0.0, 0.0, 0.0), (0.5, 0.0, 1.0, 1.0, 0.0)],
         [(0.1, 0.2, 0.3, 0.4), (1.0, 2.0, 3.0, 4.0)])
B = anim("anim_b", 0, [(4.0, 0.125, 0.25, 0.5, 1.0)], [])
C = anim("anim_c", 3, [(0.3, 1.0 / 3.0, 0.7, -1.1, 0.01), (0.7, -0.2, 0.3, 0.9, 1e-3)],
         [(0.25, -0.75, 1.5, 0.1)])
A2 = anim("anim_a", 0, [(0.25, 2.0, 0.0, 0.0, 0.0)], [])
ZERO = anim("anim_zero", 0, [], [])
TINY = anim("anim_tiny", 0, [(1e-3, 1.0, 1.0, 1.0, 1.0), (2e-3, 0.5, 0.5, 0.5, 0.5),
                             (4e-3, 0.25, 0.25, 0.25, 0.25)],
            [(0.5, 0.5, 0.5, 0.5), (1.5, 1.5, 1.5, 1.5)])

ANIMATION = [
    ["group " + A + "," + B, "map", "inst anim_a", "inst anim_b", "inst none", "state 0", "uv 0",
     "itick 0 " + f(0.5), "state 0", "uv 0", "itick 0 " + f(1.5), "state 0", "uv 0",
     "tick -1 " + f(2.0), "state 0", "state 1", "uv 0", "uv 1", "tick 0 " + f(2.0), "state 0",
     "itick 0 " + f(-1.0), "itick 0 7fc00000", "itick 0 00000000", "state 0", "reset 0", "state 0",
     "uv 0", "addref 0", "release 0", "map"],
    ["group " + A, "group " + C, "map", "inst anim_c", "inst anim_a", "tick -1 " + f(0.1),
     "state 0", "uv 0", "tick -1 " + f(0.1), "state 0", "uv 0", "tick -1 " + f(0.1), "state 0",
     "uv 0", "tick -1 " + f(0.1), "state 0", "uv 0", "state 1", "uv 1", "tick -1 " + f(5.55),
     "state 0", "state 1", "uv 0", "uv 1", "release 1", "tick -1 " + f(0.25), "state 0", "map"],
    ["group " + A + "," + C, "group " + A2, "map", "inst anim_a", "state 0", "unload 0", "map",
     "state 0", "inst anim_a", "inst anim_c", "unload 1", "map", "inst anim_a", "state 0",
     "itick 0 " + f(0.3), "uv 0", "group ", "group " + ZERO, "map", "inst anim_zero",
     "itick 1 " + f(1.0), "state 1"],
    ["group " + TINY, "inst anim_tiny", "tick -1 " + f(0.0125), "state 0", "uv 0",
     "tick -1 " + f(3.3333333e-3), "state 0", "uv 0", "tick -1 " + f(1e-4), "state 0", "uv 0",
     "inst anim_tiny", "tick -1 " + f(7e-3), "state 0", "state 1", "uv 1"],
]

# --- material_batcher -----------------------------------------------------------------------


def floats(values):
    return ",".join(f(v) for v in values)


IDENTITY = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
W1 = [1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 2, 3]
W2 = [0.5, 0.1, 0, -0.1, 0.5, 0, 0, 0, 0.5, -4, 0.25, 7.5]
V1 = [0.6, 0, -0.8, 0, 1, 0, 0.8, 0, 0.6, 0, 0, -10]
V2 = [1, 0, 0, 0, 0.7071068, 0.7071068, 0, -0.7071068, 0.7071068, 0.3, -0.2, 2.1]

S1 = "vsrc s1 3 12 " + floats([0, 0, 0, 1, 0, 0, 0, 1, 0])
S2 = "vsrc s2 2 16 " + floats([1, 2, 3, 99, -1, -2, -3, 99])
S3 = "vsrc s3 6 12 " + floats([0, 0, 5, 1, 0, 4, 0, 1, 3, 1, 1, 2, 2, 0, 1, 0, 2, 0.1])
S3R = S3.replace("vsrc s3", "vsrc s3r") + " 5,4,3,2,1,0"
S4 = "vsrc s4 4 12 " + floats([0, 0, 0.1, 0, 0, 0.2, 0, 0, 0.3, 0, 0, 0.4])
I1 = "idx i1 0,1,2,3,4,5,0,2,4,1,3,5"
I2 = "idx i2 0,1,2,2,1,0,5,4,3,1,2,3,0,0,0"
SETUP = [S1, S2, S3, S3R, S4, I1, I2, "xf id " + floats(IDENTITY), "xf w1 " + floats(W1),
         "xf w2 " + floats(W2), "xf v1 " + floats(V1), "xf v2 " + floats(V2)]


def sub(mat, flags, depth=0.0, src="s1", idx="i1", world="w1", view="v1", prim=4, start=0,
        count=3, ibs=0, span=3, vb="1,2,0", ib="2,0"):
    return "sub #%d %d %d %d %d %d %08x %s %s %s %s %s %s %s" % (
        mat, prim, start, count, ibs, span, flags, f(depth), src, idx, world, view, vb, ib)


BATCHER = [
    # Direct draws in bin 0, then queued draws in bin 1 (descending), flushes and clears.
    ["startup material_batcher.ini", "bins"] + SETUP + [
        "mat 0 1", "mat 20 1", "mat 0 0", "select 0", sub(1, 0x0c), sub(3, 0x0c, count=4, ibs=2),
        sub(1, 0x100, vb="5,322,7", ib="6,11"), "select 1", "events on", "tag 7", "tagctx 9",
        sub(1, 0x0c), sub(2, 0x10c, src="s2", world="w2", vb="5,322,7", ib="6,11"),
        sub(1, 0x00, count=3, ibs=6, span=6), sub(2, 0x04, src="s1", count=0, ibs=3, span=3),
        "bins", "flushbin 1 0", "bins", "flush 1", "bins", "flush 1", "select 2", "sort 2 3",
        "order 2 1", "flushbin 2 0", "noop", "events off", "bins",
    ],
    # Triangle sorts (kinds 5 and 4) through shared and copied indices, with a remap array.
    ["startup material_batcher.ini"] + SETUP + [
        "mat 0 1", "mat 0 1", "select 1", sub(1, 0x0c, src="s3", idx="i2", count=6, span=9),
        sub(2, 0x00, src="s3r", idx="i2", count=6, ibs=9, span=9, world="w2", view="v2"),
        "flush 0", "bins", "flush 1", "sort 1 4",
        sub(1, 0x08, src="s3", idx="i2", ibs=3, span=12, world="id", view="v2"),
        sub(2, 0x04, src="s3r", idx="i1", count=0, ibs=12, span=12), "mode 0", "flush 1",
        "mode none", sub(1, 0x0c, src="s3", idx="i2", span=6), "flush 1",
    ],
    # The depth sort with a dozen records: the caller's depths (ties and a NaN) and centres.
    ["startup material_sort.ini"] + SETUP + [
        "mat 0 1", "mat 0 1", "mat 0 1", "select 1", "events on",
        sub(1, 0x2c, depth=3.0), sub(2, 0x2c, depth=1.0), sub(3, 0x2c, depth=3.0),
        sub(1, 0x2c, depth=-2.0), sub(2, 0x2c, depth=3.0), sub(3, 0x2c, depth=0.5),
        sub(1, 0x0c, src="s4"), sub(2, 0x0c, src="s4", world="w2"), sub(3, 0x2c, depth=1.0),
        "sub #1 4 0 3 0 3 0000002c 7fc00000 s1 i1 w1 v1 1,2,0 2,0",
        sub(2, 0x2c, depth=3.0), sub(3, 0x0c, world="w2"), "bins", "flushbin 1 0", "sort 1 3",
        "flushbin 1 0", "sort 1 5", "flushbin 1 0", "sort 1 1", "flushbin 1 0", "sort 1 6",
        "flushbin 1 0", "sort 1 7", "flushbin 1 0", "sort 1 0", "flushbin 1 0", "sort 1 9",
        "flushbin 1 0", "sort 1 2", "flush 1", "bins",
    ],
    # Bin order keys and their quirk, queueing in bin 0, sort kind 1.
    ["startup material_batcher.ini"] + SETUP + [
        "mat 0 1", "events on", "bins", "order 0 5", "bins", "order 1 -1", "bins", "order 1 3",
        "bins", "order 0 2", "bins", "order 1 9", "bins", "select 0", sub(1, 0x0c),
        "select 1", sub(1, 0x0c, world="w2"), "flush 0", "flushbin 0 1", "bins", "sort 0 2",
        "select 0", sub(1, 0x0c), "flush 1", "bins", "order 0 -4", "select 0", sub(1, 0x0c),
        "bins", "clearbins", "bins",
    ],
    # Rules, re-starting with other INI files, creation through the batcher's library.
    ["startup material_batcher.ini", "bins", "rule 1 .*OcOt", "rule 2 (", "rule 7 x",
     "unrule 1 .*OcOt", "unrule 1 .*OcOt", "unrule 1 .*OcOt", "bins", "create Hull DcDtOcOt", "create Rim Plain",
     "create glow_X Plain", "clearrules", "create Hull2 DcDtOcOt", "rule 2 ^glow_.*",
     "create glow_X Plain", "bins", "startup material_defaults.ini", "bins",
     "startup material_words.ini", "bins", "create Hull3 DcDtOcOt"],
    # Render flag 0x20 rebinds every record; tags; device buffers that fail to select.
    ["startup material_sort.ini"] + SETUP + [
        "mat 20 1", "mat 0 1", "select 1", "events on", "tag 0",
        sub(1, 0x12c, depth=1.0, vb="5,2,7", ib="6,1"), sub(1, 0x12c, depth=2.0, vb="100,2,7", ib="6,1"),
        sub(2, 0x12c, depth=3.0, vb="5,2,7", ib="100,1"), "tag 4", sub(2, 0x2c, depth=4.0),
        sub(2, 0x2c, depth=5.0), "tag 5", sub(1, 0x2c, depth=6.0), "flush 0", "bins", "flush 1",
        "bins",
    ],
]


def write(name, text, check):
    path = os.path.join(HERE, name)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != text.encode("latin-1"):
                sys.exit("%s is out of date; run make_material_fixtures.py" % name)
        return
    with open(path, "wb") as out:
        out.write(text.encode("latin-1"))


def listing(title, lines):
    return "# P4.5 %s oracle inputs, written by make_material_fixtures.py.\n" % title + "".join(
        line + "\n" for line in lines)


def main():
    check = "--check" in sys.argv[1:]
    write("material_regex.list", listing("material regex", regex_list()), check)
    write("material_library.list", listing("material library", [" ; ".join(c) for c in LIBRARY]), check)
    write("material_global.list", listing("global material record", [" ; ".join(c) for c in GLOBAL]),
          check)
    write("material_animation.list",
          listing("material animation", [" ; ".join(c) for c in ANIMATION]), check)
    write("material_batcher.list", listing("material batcher", [" ; ".join(c) for c in BATCHER]),
          check)
    write("material_map.ini", MATERIAL_MAP_INI, check)
    write("material_defaults.ini", MATERIAL_DEFAULTS_INI, check)
    write("material_batcher.ini", MATERIAL_BATCHER_INI, check)
    write("material_sort.ini", MATERIAL_SORT_INI, check)
    write("material_words.ini", MATERIAL_WORDS_INI, check)


if __name__ == "__main__":
    main()
