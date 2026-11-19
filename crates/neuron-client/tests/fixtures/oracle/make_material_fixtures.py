"""Writes the inputs of the P4.5 material suites (Oracle.exe neuron-client,
CppSource/Oracle/oracle_material.cpp, oracle_material_kinds.cpp,
oracle_material_surface.cpp and oracle_material_appearance.cpp, and
crates/neuron-client/tests/oracle_vectors.rs): material_regex.list,
material_library.list, material_global.list, material_animation.list,
material_batcher.list, material_kinds.list, material_surface.list,
material_appearance.list and the INI
files they load. Run from any folder:

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
was given, and the scratch arena (BufferSize) holds every frame's copies. The material_kinds cases read no vertex, normal or index past the
arrays they give (the vertex sources hold every stride the materials read
them at).
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


# --- material_kinds -------------------------------------------------------------------------
# The 22 kinds of Material.cpp and BatchedMaterial over the recording services of
# oracle_material_kinds.cpp (its header states the contract and the line formats).

def words(values):
    return ",".join("%08x" % v for v in values)


def kvsrc(name, flags, fvf, count, pos, normal, diffuse, tex0, index=None):
    def part(values, fn):
        return "-" if values is None else fn(values)
    return "vsrc %s %d %d %d %s %s %s %s %s" % (
        name, flags, fvf, count, part(pos, floats), part(normal, floats), part(diffuse, words),
        part(tex0, floats), "-" if index is None else ",".join(str(i) for i in index))


QUAD = [0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0]
NORMALS = [0, 0, 1, 0, 0.6, 0.8, 0.6, 0, 0.8, 0, 0, -1]
UV = [0, 0, 1, 0, 0, 1, 1, 1]
# fvf 0x112 is 32 bytes a vertex: the normals at the stride the back faces read them with.
WIDE_NORMALS = []
for n in range(4):
    WIDE_NORMALS += NORMALS[3 * n:3 * n + 3] + [9, 9, 9, 9, 9]
# fvf 0x152 is 36 bytes a vertex: the back faces read s1's normals at that stride, the atmosphere
# at 12 bytes; the stream holds both reaches.
S1_NORMALS = NORMALS + [0.125 * k for k in range(1, 25)]
V_NEAR = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, -1.01]
V_FAR = [0.6, 0, -0.8, 0, 1, 0, 0.8, 0, 0.6, 0.5, -0.25, -10]
W_SPIN = [0, 1, 0, -1, 0, 0, 0, 0, 1, 0.5, 0.25, -2]

KIND_SETUP = [
    "globals 1 1 1",
    "tex %s 1 11 3 0" % hx("diffuse.tga"),
    "tex %s 1 12 7 0" % hx("alpha.tga"),
    "tex %s 1 13 6 0" % hx("mask.tga"),
    "tex %s 4 14 3 0" % hx("anim.tga"),
    "tex %s 1 15 3 0" % hx("detail.tga"),
    "tex %s 1 16 3 1" % hx("broken.tga"),
    "tex %s 1 17 3 0" % hx("glow.tga"),
    kvsrc("s1", 0, 0x152, 4, QUAD, S1_NORMALS, [0xff102030, 0x80405060, 0x00708090, 0xffa0b0c0], UV),
    kvsrc("s2", 0, 0x112, 4, QUAD, WIDE_NORMALS, None, UV),
    kvsrc("s3", 4, 0x112, 4, QUAD, NORMALS, None, UV, [3, 2, 1, 0]),
    "idx i1 0,1,2,2,1,3",
    "xf id " + floats(IDENTITY), "xf w1 " + floats(W1), "xf v1 " + floats(V1),
    "xf vnear " + floats(V_NEAR), "xf vfar " + floats(V_FAR), "xf wspin " + floats(W_SPIN),
    "ctx c1 s1 i1 5,338,10 6,3 w1 v1 7",
    "ctx c2 s2 i1 - 6,3 w1 v1 7",
    "ctx c3 s3 i1 - - w1 v1 7",
    "ctx cn - i1 5,338,10 6,3 w1 v1 7",
    "ctx cnear s1 i1 - - id vnear 7",
    "ctx cfar s1 i1 - - wspin vfar 7",
]

# The texture-name properties of each kind (schema index), in block order.
KIND_TEXTURES = {
    "DcDt": [6], "DcDtEc": [6], "DcDtOcOt": [6], "DcDtEcOcOt": [6], "DcDtTwo": [6],
    "DcDtEcTwo": [6], "DcDtOcOtTwo": [6], "DcDtEcOcOtTwo": [6], "DcDtEt": [1, 5], "EcEt": [1],
    "EcEtTwo": [1], "AtmosphereMaterial": [2], "Nebula": [0], "NebulaTwo": [0],
    "NullMaterial": [], "LightMap": [0], "DetailMapMaterial": [2, 8],
    "DetailMap2Dm1Msk2PassMaterial": [2, 8], "IllumDetailMapMaterial": [2, 8, 13],
    "Masked2DetailMapMaterial": [2, 8, 13], "BtDetailMapMaterial": [6, 10],
    "BtDetailMapTwoMaterial": [6, 10],
}
KIND_NAMES = ["diffuse.tga", "detail.tga", "glow.tga"]


def tex_sets(kind, names=None):
    names = names or KIND_NAMES
    return ["set %d 7 %s" % (i, hx(n)) for i, n in zip(KIND_TEXTURES[kind], names)]


def kind_body(kind, names=None):
    return ["new " + kind, "name " + hx("Mat_" + kind)] + tex_sets(kind, names) + [
        "flags", "verify", "state", "resolve", "state", "bind c1", "submit c1 4 0 4 0 6 0",
        "submit c2 4 1 3 0 3 0", "teardown c1", "bind -", "teardown -", "flags", "passes",
        "params", "release"]


KINDS_CASES = [KIND_SETUP + kind_body(k) for k in KIND_TEXTURES] + [
    # Single pass: an alpha texture blends, a one-bit alpha tests (the submit's scope, and flag
    # 0x400 skips it), an opacity below 1 blends; parameters out of range or of the wrong type.
    KIND_SETUP + ["rs 24 33", "rs 25 6", "rs 15 1", "new DcDt", "name " + hx("Masked"),
                  "set 6 7 " + hx("mask.tga"), "verify", "state", "flags", "submit c1 4 0 4 0 6 0",
                  "submit c1 4 0 4 0 6 400", "set 5 2 " + f(0.5), "state", "flags", "verify",
                  "state", "set 5 2 " + f(1.0), "state", "set 5 2 " + f(1.5), "set 5 1 1",
                  "set 1 3 " + ",".join([f(0.25), f(0.5), f(0.75)]), "set 1 3 " +
                  ",".join([f(2), f(0), f(0)]), "set 10 2 0", "set 6 7 " + hx("alpha.tga"),
                  "verify", "state", "flags", "set 5 2 " + f(1.0), "state", "params", "release"],
    # Animation: the library answers, the instance fills (or does not fill) the matrix; a
    # second verify releases the first instance.
    KIND_SETUP + ["animlib", "anim %s 1" % hx("spinner"), "anim %s 0" % hx("still"),
                  "new DcDtOcOt", "name " + hx("SPINNER"), "set 6 7 " + hx("anim.tga"), "verify",
                  "state", "resolve", "state", "bind c1", "teardown c1", "verify", "bind -",
                  "teardown -", "release", "new DcDtEc", "name " + hx("still"), "verify",
                  "bind -", "teardown -", "release", "new DcDtTwo", "name " + hx("other"),
                  "verify", "state", "release"],
    # Textures that are missing, "(None)", a failing playback and a null name; a verify that
    # fails; Setup run twice releases the first playbacks.
    KIND_SETUP + ["verify -1", "new DcDtEt", "name " + hx("Holes"), "set 1 7 " + hx("missing.tga"),
                  "set 5 7 " + hx("(None)"), "verify", "state", "set 1 7 " + hx("broken.tga"),
                  "set 5 7 -", "verify", "state", "set 1 7 " + hx("diffuse.tga"),
                  "set 5 7 " + hx("glow.tga"), "verify", "state", "verify", "state", "release"],
    # The kernel's routes: the device's buffers (and their failure), the caller's buffer (and its
    # failure), a copy (and a lock that fails), no source.
    KIND_SETUP + ["new EcEt", "name " + hx("routes"), "set 1 7 " + hx("glow.tga"), "verify",
                  "submit c1 4 0 4 0 6 100", "rpdraw -5", "submit c1 4 0 4 0 6 100",
                  "submit cn 4 0 4 0 6 100", "rpdraw 0", "dvb -2", "submit c1 4 0 4 0 6 0",
                  "submit cn 4 0 4 0 6 0", "dvb 0", "submit c2 4 0 4 0 6 0",
                  "submit c2 4 0 0 0 6 0", "lockfail 1", "submit c2 4 0 4 0 6 0", "lockfail 0",
                  "submit c3 5 0 4 0 6 0", "release"],
    # Two-sided kinds: back faces with the cull mode flipped (clockwise, counter-clockwise,
    # none), normals from interleaved and indexed sources, a first pass that fails.
    KIND_SETUP + ["rs 22 2", "new DcDtTwo", "name " + hx("Fin"), "set 6 7 " + hx("mask.tga"),
                  "verify", "submit c2 4 1 3 0 3 0", "rs 22 3", "submit c3 4 0 4 0 6 0",
                  "rs 22 1", "submit c2 4 0 4 0 6 0", "submit cn 4 0 4 0 6 0", "dvb -1",
                  "submit c1 4 0 4 0 6 0", "dvb 0", "release", "new BtDetailMapTwoMaterial",
                  "name " + hx("BtFin")] + tex_sets("BtDetailMapTwoMaterial") + [
                  "verify", "rs 22 3", "submit c3 4 1 3 0 3 0", "rs 22 2",
                  "submit c2 4 0 4 0 6 100", "release"],
    # Detail kinds: the second stage disabled by global record 0, a failing first or second
    # verify, the one-pass fallbacks.
    KIND_SETUP + ["globals 0 1 1", "new DetailMap2Dm1Msk2PassMaterial", "name " + hx("d0")] +
    tex_sets("DetailMap2Dm1Msk2PassMaterial") + ["verify", "state", "passes",
                                                 "submit c1 4 0 4 0 6 0", "release",
                                                 "new IllumDetailMapMaterial", "name " + hx("i0")] +
    tex_sets("IllumDetailMapMaterial") + ["verify", "state", "passes", "submit c1 4 0 4 0 6 0",
                                          "release", "new Masked2DetailMapMaterial",
                                          "name " + hx("m0")] +
    tex_sets("Masked2DetailMapMaterial") + ["verify", "state", "submit c1 4 0 4 0 6 0",
                                            "release", "new DetailMapMaterial",
                                            "name " + hx("dm0")] +
    tex_sets("DetailMapMaterial") + ["verify", "state", "bind c1", "release",
                                     "new BtDetailMapMaterial", "name " + hx("bt0")] +
    tex_sets("BtDetailMapMaterial") + ["verify", "state", "bind c1", "release"],
    KIND_SETUP + ["verify 0,-1", "new DetailMap2Dm1Msk2PassMaterial", "name " + hx("d1")] +
    tex_sets("DetailMap2Dm1Msk2PassMaterial") + ["verify", "state", "release", "verify -1,0",
                                                 "new IllumDetailMapMaterial", "name " + hx("i1")] +
    tex_sets("IllumDetailMapMaterial") + ["verify", "state", "passes", "submit c1 4 0 4 0 6 0",
                                          "release", "verify 0,-1",
                                          "new IllumDetailMapMaterial", "name " + hx("i2")] +
    tex_sets("IllumDetailMapMaterial") + ["verify", "state", "passes", "submit c1 4 0 4 0 6 0",
                                          "release", "verify -1",
                                          "new DetailMapMaterial", "name " + hx("dm1")] +
    tex_sets("DetailMapMaterial") + ["verify", "state", "release", "verify -1",
                                     "new BtDetailMapMaterial", "name " + hx("bt1")] +
    tex_sets("BtDetailMapMaterial") + ["verify", "state", "release", "verify -1",
                                       "new Masked2DetailMapMaterial", "name " + hx("m1")] +
    tex_sets("Masked2DetailMapMaterial") + ["verify", "state", "passes", "release"],
    # DcDtEt: the second texture switched off by global record 2, at verify and at resolve.
    KIND_SETUP + ["globals 1 1 0", "new DcDtEt", "name " + hx("e0")] + tex_sets("DcDtEt") + [
        "verify", "state", "bind -", "globals 1 1 1", "resolve", "state", "bind -",
        "verify -1,0", "verify", "state", "release"],
    # The atmosphere: not ready (global record 1), the view near and far, pipeline state 7 set,
    # clear and failing, the parameters that shape it.
    KIND_SETUP + ["new AtmosphereMaterial", "name " + hx("Air"), "set 2 7 " + hx("alpha.tga"),
                  "verify", "state", "globals 1 0 1", "resolve", "state", "bind c1",
                  "submit cnear 4 0 4 0 6 0", "globals 1 1 1", "resolve", "bind c1",
                  "submit cnear 4 0 4 0 6 0", "submit cfar 4 1 3 0 3 0", "mode 0",
                  "submit cfar 4 0 4 0 6 0", "mode none", "set 8 2 " + f(1.5),
                  "set 7 2 " + f(0.25), "set 6 2 " + f(2.0), "submit cnear 4 0 4 0 6 0",
                  "set 8 2 " + f(0.5), "submit cfar 4 0 4 0 6 0", "submit c3 4 0 4 0 6 0",
                  "lockfail 1", "submit cfar 4 0 4 0 6 0", "verify -3", "verify", "params",
                  "release"],
    # BatchedMaterial: its bin from the inner flags at the first good verify, a bin of its own,
    # the depth of flag 0x40.
    KIND_SETUP + ["batched DcDtOcOt", "name " + hx("Glass"), "set 6 7 " + hx("alpha.tga"),
                  "flags", "verify", "bin", "flags", "bsubmit c1 4 0 4 0 6 c",
                  "bsubmit c1 4 0 4 0 6 4c", "bsubmit cfar 4 1 3 0 3 40", "verify", "bin",
                  "release", "batched DcDt", "name " + hx("Opaque"),
                  "set 6 7 " + hx("diffuse.tga"), "verify -1", "verify", "bin", "verify 0",
                  "verify", "bin", "setbin 5", "verify", "bin", "bsubmit c1 4 0 4 0 6 40",
                  "passes", "params", "release", "batched NullMaterial", "verify", "bin", "flags",
                  "release"],
]


# --- material_surface -----------------------------------------------------------------------
# The 19 materials of surfacematerials.dll over the recording services of
# oracle_material_surface.cpp (its header states the contract and the line formats). The cases
# keep to what the reference defines: no ApplyVerify of a material without texture records whose
# opacity is 1 (ShadowMaterial, WireMaterial), no ApplyVerify or Teardown of BackdropMaterial or
# StarsphereMaterial without a candidate, no submission without a context and no flag 0x100
# without an index buffer, no Veilborn submission before the engine is set, no candidate
# descriptor without a leaf name.

def interleaved(vertices):
    return ",".join(f(v) for vertex in vertices for v in vertex)


SI_VERTS = [QUAD[3 * n:3 * n + 3] + NORMALS[3 * n:3 * n + 3] + UV[2 * n:2 * n + 2] for n in range(4)]
SC_WORDS = []
for n in range(4):
    SC_WORDS += [f(v) for v in QUAD[3 * n:3 * n + 3] + NORMALS[3 * n:3 * n + 3]]
    SC_WORDS += ["%08x" % [0xff102030, 0x80405060, 0x00708090, 0xffa0b0c0][n]]
    SC_WORDS += [f(v) for v in UV[2 * n:2 * n + 2]]


def cand(batched=0, passes="1", cls="DcDtOcOt", desc=0, tag="Oc", param=0.5, bind=0, teardown=0,
         submit=0):
    return "cand %d %s %s %d %s %s %d %d %d" % (
        batched, passes, hx(cls), desc, "-" if tag is None else hx(tag), f(param), bind, teardown,
        submit)


SURF_SETUP = [
    "tex %s 1 11 3 0" % hx("diffuse.tga"),
    "tex %s 1 12 7 0" % hx("alpha.tga"),
    "tex %s 1 13 6 0" % hx("mask.tga"),
    "tex %s 4 14 3 0" % hx("anim.tga"),
    "tex %s 1 18 7 0" % hx("veilbornrgb1_veilbornalpha1"),
    "tex %s 1 19 7 0" % hx("efxshadow"),
    "tex %s 1 20 3 0" % hx("envmapglass"),
    "tex %s 1 21 3 0" % hx("chrome.tga"),
    "vsil si 0 274 4 " + interleaved(SI_VERTS),
    "vsil sc 0 338 4 " + ",".join(SC_WORDS),
    kvsrc("s3", 4, 0x112, 4, QUAD, NORMALS, None, UV, [3, 2, 1, 0]),
    "idx i1 0,1,2,2,1,3",
    "xf id " + floats(IDENTITY), "xf w1 " + floats(W1), "xf v1 " + floats(V1),
    "xf w2 " + floats(W2), "xf v2 " + floats(V2),
    "ctx c1 si i1 5,274,10 6,3 w1 v1 7 -",
    "ctx cc sc i1 - - w1 v1 7 -",
    "ctx c3 s3 i1 - - w2 v2 8 -",
    "ctx cn - i1 5,274,10 6,3 w1 v1 7 -",
    "ctx k1 si i1 5,274,10 6,3 w1 v1 7 c",
    "ctx kc sc i1 - - w1 v1 9 c",
    "ctx k0 - i1 - - w1 v1 7 c",
]


def surf_body(kind, texture="diffuse.tga", verify=True):
    ops = ["new " + kind, "name " + hx("Mat_" + kind), "class", "flags"]
    if texture is not None:
        ops.append("set 6 7 " + hx(texture))
    if verify:
        ops.append("verify")
    ops += ["resolve 00000000", "flags", "bind c1", "submit c1 4 0 4 0 6 0", "teardown c1",
            "bind cc", "submit cc 4 1 3 0 3 0", "submit c1 4 1 3 0 3 100", "submit cn 4 0 4 0 6 0", "teardown cc", "bind -",
            "teardown -", "flags", "passes", "params", "release"]
    return ops


SURFACE_CASES = [
    # The glass family: two passes, the opacity raised to 0.8 at bind.
    SURF_SETUP + surf_body("GlassMaterial") + surf_body("HighGlassMaterial", "alpha.tga") +
    surf_body("GFGlassMaterial", "mask.tga"),
    SURF_SETUP + ["new HighGlassMaterial", "name " + hx("Thin"), "set 6 7 " + hx("diffuse.tga"),
                  "set 5 2 " + f(0.5), "verify", "flags", "bind c1", "bind c1",
                  "set 5 2 " + f(1.5), "set 1 3 " + floats([0.25, 0.5, 0.75]), "bind cc",
                  "dvb -3", "submit cc 4 0 4 0 6 0", "dvb 0", "rpdraw -2",
                  "submit c1 4 0 4 0 6 100", "params", "release"],
    # HUD icons: blended by opacity or texture alpha, tested by a one-bit alpha; the instance's
    # colour and opacity from the engine's user data.
    SURF_SETUP + surf_body("HUDIconMaterial") + surf_body("HUDIconMaterial", "alpha.tga") +
    surf_body("HUDIconMaterial", "mask.tga") + [
        "engine 1", "ud 7 " + floats([0.25, 0.5, 0.75, 1.0, 0.5]), "new HUDIconMaterial",
        "name " + hx("Radar"), "set 6 7 " + hx("diffuse.tga"), "verify", "flags", "bind -",
        "bind c1", "flags", "bind c3", "set 5 2 " + f(0.25), "flags", "bind c3", "release"],
    # HUD animation: global record 4 switches it (its unset value counts as on), the animation
    # instance follows the record at resolve.
    SURF_SETUP + ["animlib", "anim %s 1" % hx("hudspin"), "new HUDAnimMaterial",
                  "name " + hx("HudSpin"), "set 6 7 " + hx("anim.tga"), "verify", "bind cc",
                  "submit cc 4 0 4 0 6 0", "submit c1 4 0 4 0 6 100", "resolve 00000000",
                  "bind cc", "submit c1 4 0 4 0 6 100", "teardown cc", "global 4 0",
                  "resolve 00000000", "bind cc", "submit c1 4 0 4 0 6 100", "teardown cc",
                  "flags", "global 4 1", "resolve 00000000", "resolve 00000000", "bind c1",
                  "teardown c1", "set 6 7 " + hx("alpha.tga"), "verify", "flags", "release"],
    SURF_SETUP + ["global 4 0", "new HUDAnimMaterial", "name " + hx("Still"),
                  "set 6 7 " + hx("diffuse.tga"), "verify", "resolve 00000000", "bind c1",
                  "teardown c1", "global 4 1", "resolve 00000000", "bind c1", "teardown c1",
                  "release"],
    # Exclusion zones: full ambient while drawing, the zone's colour from the user data.
    SURF_SETUP + surf_body("ExclusionZoneMaterial", "alpha.tga") + [
        "engine 1", "ud 7 " + floats([0.1, 0.2, 0.3, 0.4, 0.6]), "rs 139 8421504",
        "new ExclusionZoneMaterial", "name " + hx("Zone"), "set 6 7 " + hx("diffuse.tga"),
        "verify", "flags", "submit c1 4 0 4 0 6 0", "submit c3 4 0 4 0 6 0", "bind c1",
        "release"],
    # Veilborn: the second texture verified (or not), the clocks, the fade of the instance's
    # root, interleaved and indexed sources.
    SURF_SETUP + ["engine 1", "new VeilbornMaterial", "name " + hx("Veil"), "set 1 7 " + hx("diffuse.tga"),
                  "flags", "verify", "params", "resolve 3f000000", "resolve 40400000",
                  "bind c1", "submit c1 4 0 4 0 6 0", "submit c3 4 1 3 0 3 0", "teardown c1",
                  "ival 7 " + f(0.25), "submit c1 4 0 4 0 6 0", "root 7 3",
                  "ival 3 " + f(0.75), "submit c1 4 0 4 0 6 0", "lockfail 1",
                  "submit c1 4 0 4 0 6 0", "lockfail 0", "verify -1,0", "verify", "bind c1",
                  "submit c1 4 0 4 0 6 0", "submit c3 4 0 4 0 6 0", "ival 3 " + f(1.5),
                  "submit c1 4 0 4 0 6 0", "teardown -", "release",
                  "new VeilbornMaterialNoBendy", "name " + hx("Still"), "class",
                  "set 5 7 " + hx("missing.tga"), "verify", "resolve 3f000000", "bind c1",
                  "submit c1 4 0 4 0 6 0", "set 0 3 " + floats([0.5, 0.5, 0.5]), "params",
                  "release"],
    # Planet water: its animation made at verify, its matrix at bind; depth writes off.
    SURF_SETUP + ["animlib", "anim %s 1" % hx("waves"), "new PlanetWaterMaterial",
                  "name " + hx("Waves"), "set 6 7 " + hx("anim.tga"), "verify", "resolve 00000000",
                  "bind cc", "submit c1 4 0 4 0 6 0", "teardown cc", "set 5 2 " + f(0.5), "bind c1",
                  "teardown c1", "verify", "flags", "release", "new PlanetWaterMaterial",
                  "name " + hx("Calm"), "verify", "bind c1", "teardown c1", "release"],
    # Engine glow: ApplyVerify replaces the material block.
    SURF_SETUP + surf_body("EngineGlowMaterial", "alpha.tga"),
    # Colorize: its own texture with option 1, else the candidate's opacity times its own.
    SURF_SETUP + [cand(), "new ColorizeMaterial", "name " + hx("Paint"), "verify", "flags",
                  "params", "bind -", "bind c1", "bind k1", "teardown k1", "teardown c1",
                  "set 0 4 " + floats([0.2, 0.4, 0.6, 0.5]), "flags", "bind k1",
                  cand(1, tag="Dc"), "bind kc", cand(0, desc=-1), "bind k1", "set 10 1 1", "set 6 7 " + hx("diffuse.tga"), "bind k1", "teardown k1",
                  "submit kc 4 0 4 0 6 0", "params", "release"],
    # Cloak: the root's value times the candidate's opacity; the candidate draws with flag 0x400
    # while blended.
    SURF_SETUP + ["new CloakMaterial", "name " + hx("Cloak"), "verify", "flags", "bind c1",
                  "submit c1 4 0 4 0 6 0", cand(1), "bind k1", "submit k1 4 0 4 0 6 100",
                  "ival 7 " + f(0.5), "bind k1", "submit k1 4 0 4 0 6 100", "teardown k1",
                  "engine 1", "root 7 4", "ival 4 " + f(1.0), cand(0), "bind kc", "flags",
                  "submit kc 4 0 4 0 6 0", "teardown kc", "teardown c1", "params", "release"],
    # Wire: the mesh's own wire, else its edges (a line list); the face-group cursor.
    SURF_SETUP + ["new WireMaterial", "name " + hx("Wire"), "class", "flags", "params",
                  "bind c1", "set 0 4 " + floats([1.0, 0.5, 0.25, 0.5]), "params", "bind c1",
                  "submit c1 2 0 4 0 6 0", "engine 1", "submit c1 2 0 4 0 6 0", "wire 7 1",
                  "submit c1 2 0 4 0 6 0", "submit c1 2 1 3 0 3 0", "submit cn 2 0 4 0 6 0",
                  "submit c1 2 0 0 0 6 0", "wire 7 0",
                  "mesh 7 0 %s 0,1,2 %s 0,1,1,2,2,3 6" % (floats([0.5, 0.05, 1.0]),
                                                       floats(QUAD)),
                  "submit c1 2 0 4 0 6 0", "mopen 1,0", "submit c1 2 0 4 0 6 0", "mopen 1,-1",
                  "submit c1 2 0 4 0 6 0", "mesh 7 -8 - - - - 0", "submit c1 2 0 4 0 6 0",
                  "teardown cc", "release"],
    SURF_SETUP + ["engine 1", "mesh 7 0 %s 0,1 %s 0,1,1,2 4" % (floats([2.0, 3.0]), floats(QUAD)),
                  "new WireMaterial", "name " + hx("Edges"),
                  "set 0 4 " + floats([0.5, 1.0, 0.75, 1.0]), "submit c1 2 0 4 0 6 0",
                  "lockfail 1", "submit c1 2 0 4 0 6 0", "lockfail 0", "dpvb -1",
                  "submit c1 2 0 4 0 6 0", "release"],
    # Wire2: four triangles an edge, a reduced colour for the shifted points.
    SURF_SETUP + ["new WireMaterial2", "name " + hx("Wire2"), "verify", "verify -1", "verify",
                  "set 0 4 " + floats([0.75, 0.5, 0.25, 1.0]), "flags", "bind cc", "teardown cc",
                  "engine 1", "wire 7 1",
                  "mesh 7 0 %s 0,1 %s 0,1,2,3 4" % (floats([1.0, 0.25]), floats(QUAD)),
                  "submit c1 4 0 4 0 6 0", "submit c1 4 0 4 0 6 0", "params", "release"],
    # EnvMap: a glass candidate draws with the reflection on the second pass; another candidate
    # takes its texture from the instance's root value.
    SURF_SETUP + ["new EnvMapMaterial", "name " + hx("Chrome"), "verify", "flags",
                  cand(0, cls="GlassMaterial"), "bind k1", "teardown k1",
                  "submit k1 4 0 4 0 6 0", "tss 0 1 1", "submit k1 4 0 4 0 6 0", "tss 0 1 4",
                  "tss 1 1 1", "submit k1 4 0 4 0 6 0", "tss 1 1 3", "submit k1 4 0 4 0 6 0",
                  "dvb -1", "submit kc 4 0 4 0 6 0", "dvb 0", "bind k1", cand(0), "bind k1",
                  "iname 7 " + hx("chrome.tga"), "bind k1", "teardown k1", "submit k1 4 0 4 0 6 0",
                  "tss 0 1 1", "bind k1", "engine 1", "root 7 2", "iname 2 " + hx("chrome.tga"),
                  cand(1), "bind k1", "iname 2 " + hx("missing.tga"), "bind k1", "bind c1",
                  "submit c1 4 0 4 0 6 0", "teardown c1", "release"],
    # Backdrop and starsphere: the candidate drawn without depth.
    SURF_SETUP + [cand(1, submit=3), "new BackdropMaterial", "name " + hx("Sky"), "class",
                  "flags", "params", "bind k1", "submit k1 4 0 4 0 6 0", "teardown k1",
                  "bind c1", "bind -", "submit c1 4 0 4 0 6 0", "release", cand(0, teardown=-2),
                  "new StarsphereMaterial", "name " + hx("Stars"), "class", "rs 7 1", "rs 14 1",
                  "rs 23 4", "bind kc", "submit kc 4 0 4 0 6 0", "teardown kc", "release"],
    # Shadows: efxshadow on the stage after the candidate's; a second pass over a candidate of
    # more than one pass.
    SURF_SETUP + [cand(0, passes="1", submit=5), "new ShadowMaterial", "name " + hx("Shadow"),
                  "class", "flags", "bind -", "bind k1", "submit k1 4 0 4 0 6 0", "teardown k1",
                  "teardown -", "teardown c1", "submit c1 4 0 4 0 6 0", cand(1, passes="2"),
                  "bind k1", "submit k1 4 0 4 0 6 100", "submit kc 4 0 4 0 6 0",
                  cand(1, passes="-"), "bind k1", "submit k0 4 0 4 0 6 0", "release"],
    ["new ShadowMaterial", "name " + hx("NoTexture"), "bind -", "bind -", "release"],
    # Visible counters.
    SURF_SETUP + ["new VisibleCounterMaterial", "name " + hx("Counter"), "class", "verify",
                  "flags", "counters", "submit c1 4 0 4 0 6 0", "submit cc 4 1 3 0 3 0", "counters",
                  "bind c1", "teardown c1", "release", "counters"],
]


# --- material_appearance --------------------------------------------------------------------
# FLAppearance.cpp over the recording fakes of oracle_material_appearance.cpp (its header states
# the contract and the line formats). Every case starts from the statics as the process loads
# them, makes the appearance with an outer that answers IID_IEngine (as the engine aggregates
# it) and ends with its last Release. The cases keep to what the reference defines: override
# indices below 9, no draw before a successful Initialize but the renderer-less ones, no shadow
# instance whose bounds succeed without writing, and RenderInstance probes only the slots
# FCameraBlob has.


def dbl(value):
    return "%016x" % struct.unpack("<Q", struct.pack("<d", value))[0]


def mat12(values):
    return " ".join(f(v) for v in values)


def yaw(angle, x, y, z, scale=1.0):
    import math
    c = math.cos(angle) * scale
    s = math.sin(angle) * scale
    return [c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c, x, y, z]


def spheres(values):
    return "/".join(",".join(f(v) for v in sphere) for sphere in values)


def shadows(pairs):
    return ",".join("%d:%s" % (inst, f(radius)) for inst, radius in pairs)


APP_CAMERA = [
    "cam vp 0 0 800 600", "cam xscale " + dbl(1.7320508075688772), "cam yscale " + dbl(1.299038105676658),
    "cam znear " + f(0.25), "cam zfar " + f(12000.0), "cam aspect " + f(1.3333334), "cam fovy " + f(52.5),
    "cam hfov " + f(37.5), "cam pos " + " ".join(f(v) for v in (10.5, -4.0, 2200.0)),
    "cam orient " + " ".join(f(v) for v in (0.0, 0.6, -0.8, -1.0, 0.0, 0.0, 0.0, 0.8, 0.6)),
    "cam view " + mat12([0.0, -1.0, 0.0, 0.6, 0.0, 0.8, -0.8, 0.0, 0.6, 3.0, -2.0, -1500.0]),
    "cam unproj " + " ".join(f(v) for v in (0.125, -0.25, 0.96)),
    "cam proj 1 " + " ".join(f(v) for v in (401.5, 299.25, 0.875)), "cam classify 4",
]

APP_PROBE = "probe " + spheres([
    (0.0, 0.0, -10.0, 1.0), (0.0, 0.0, 5.0, 1.0), (0.0, 0.0, -0.5, 1.0), (1000.0, 0.0, -10.0, 1.0),
    (0.0, 1000.0, -10.0, 1.0), (0.0, 0.0, -300000.0, 10.0), (0.0, 0.0, -199999.0, 5.0),
    (-3.0, 2.0, -10.0, 0.5), (7.5, -5.25, -9.0, 2.0), (-12.0, 0.0, -10.0, 3.0), (0.0, -8.0, -10.0, 0.0),
    (0.0, 0.0, 0.0, 0.0), (10000.0, 0.0, -10.0, 1.0), (4600.0, 0.0, -10.0, 1.0),
    (0.0, 10000.0, -10.0, 1.0), (0.0, 3460.0, -10.0, 1.0), (-4615.0, 0.0, -10.0, 0.25),
])

APP_SHADOW_SCENE = [
    "ability 1", "shadowtex 0 0 77",
    # Fallback extents (no archetype), turned 0.3 rad about y.
    "xf 31 " + mat12(yaw(0.3, 12.0, 20.0, -7.5)),
    # Bounds from the renderer, turned into the third quadrant and scaled.
    "xf 32 " + mat12(yaw(-2.5, -40.25, 3.5, 18.0, 1.25)), "arch 32 5",
    "bounds 5 0 " + ",".join(f(v) for v in (2.5, -1.75, -0.5, 0.75, 4.0, -3.0)),
    # Bounds that fail: the fallback again.
    "xf 33 " + mat12(yaw(1.1, 0.0, 41.0, 0.0)), "arch 33 6", "bounds 6 -1 -",
    # Too high to cast a shadow (fade > 50), and below the ground (fade 0).
    "xf 34 " + mat12(yaw(0.0, 5.0, 80.0, 5.0)),
    "xf 35 " + mat12(yaw(2.0, -5.0, -12.0, 9.0)), "arch 35 5",
]

APP_SHADOW_LIST = shadows([(31, 1.5), (32, 0.25), (33, 2.0), (34, 1.0), (35, 0.5)])

APPEARANCE_CASES = [
    # The whole life: the overrides, global record 4, the engine published, the wrapper made;
    # instance values, the light, the overrides read back, and the releases.
    ["create", "init", "blob"] + ["override %d" % i for i in range(9)] +
    ["set 5 3f000000", "get 5", "lookup 5", "set 5 3e800000", "get 5", "set -1 1", "get -1",
     "lookup -1", "remove -1", "get 6", "lookup 6", "remove 5", "get 5", "remove 99", "set 7 12345678",
     "set 8 0", "lookup 8", "destroy 7", "get 7", "destroy 7", "light " + " ".join(
         f(v) for v in (0.25, -0.5, 0.75, 0.9, 0.8, 0.7)), "lighton 0", "lighton 3", "release"],
    # Create without the engine, then without the system.
    ["fail engine", "create"],
    ["fail system", "create"],
    # Initialize without each interface in turn.
    ["fail matlib", "create", "init", "blob", "release"],
    ["fail pipeline", "create", "init", "blob", "release"],
    ["fail vbm", "create", "init", "blob", "release"],
    ["fail texlib", "create", "init", "blob", "release"],
    # Without the renderer: no override, no wrapper; the draws return at once.
    ["fail renderer", "create", "init", "blob", "backdrop 21", "stars 22", "count 23",
     "override 5", "release"],
    # Overrides that fail (one written anyway), one made null.
    ["mat 2 -1 -", "mat 4 -1 13", "mat 6 0 -", "mat 7 1 14", "create", "init", "override 2",
     "override 4", "override 6", "override 7", "release"],
    # The backdrop: its own perspective, the wrapper's axes and sphere test, the camera's
    # perspective again.
    APP_CAMERA + [APP_PROBE, "create", "init", "backdrop 21", "blob", "cam aspect " + f(1.6),
                  "cam xscale " + dbl(0.4142135623730951), "cam vp 16 8 1040 776", "backdrop 24", "blob",
                  "release"],
    # The starsphere: the ambient state kept, the camera's orientation without its position.
    APP_CAMERA + [APP_PROBE, "create", "init", "stars 22", "blob", "backdrop 21", "blob",
                  "cam orient " + " ".join(f(v) for v in (-0.5, 0.0, 0.8660254, 0.0, -1.0, 0.0,
                                                          0.8660254, 0.0, 0.5)),
                  "stars 25", "blob", "release"],
    # A probe-less draw of each and a count: what the renderer is given.
    APP_CAMERA + ["create", "init", "backdrop 21", "stars 22", "vis 3 18 27", "count 26",
                  "count 27", "release"],
    # Fading shadows, then fixed ones with the scale the fading draw left.
    APP_SHADOW_SCENE + ["create", "init", "fading " + APP_SHADOW_LIST, "fixed " + APP_SHADOW_LIST,
                        "release"],
    # Fixed shadows first (the scale as loaded), then one fading shadow near the ground.
    APP_SHADOW_SCENE + ["create", "init", "fixed " + APP_SHADOW_LIST,
                        "fading " + shadows([(35, 0.5)]), "fixed " + shadows([(31, 1.5), (32, 4.0)]),
                        "release"],
    # Shadow early exits: no ability (written 0, not written), no instances, no texture, a frame
    # that fails, a null handle, a lock that fails or answers a positive result.
    APP_SHADOW_SCENE + ["create", "init", "ability 0", "fading " + shadows([(31, 1.5)]),
                        "ability -", "fixed " + shadows([(31, 1.5)]), "ability 1", "fading -",
                        "shadowtex -1 0 0", "fading " + shadows([(31, 1.5)]),
                        "shadowtex 0 -1 77", "fixed " + shadows([(31, 1.5)]),
                        "shadowtex 0 0 0", "fading " + shadows([(31, 1.5)]),
                        "shadowtex 0 0 77", "lock -1", "fading " + shadows([(31, 1.5)]),
                        "lock 1", "fixed " + shadows([(31, 1.5)]), "lock 0",
                        "fading " + shadows([(34, 1.0)]), "release"],
    # The visible counter without its override.
    ["mat 8 -1 -", "vis 1 2 3", "create", "init", "count 26", "release"],
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
    write("material_kinds.list",
          listing("material kinds", [" ; ".join(c) for c in KINDS_CASES]), check)
    write("material_surface.list",
          listing("material surface", [" ; ".join(c) for c in SURFACE_CASES]), check)
    write("material_appearance.list",
          listing("material appearance", [" ; ".join(c) for c in APPEARANCE_CASES]), check)
    write("material_map.ini", MATERIAL_MAP_INI, check)
    write("material_defaults.ini", MATERIAL_DEFAULTS_INI, check)
    write("material_batcher.ini", MATERIAL_BATCHER_INI, check)
    write("material_sort.ini", MATERIAL_SORT_INI, check)
    write("material_words.ini", MATERIAL_WORDS_INI, check)


if __name__ == "__main__":
    main()
