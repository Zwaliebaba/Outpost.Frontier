"""Writes the input of the P4.7 deformable suite (Oracle.exe neuron-client,
CppSource/Oracle/oracle_deformable.cpp, and
crates/neuron-client/tests/oracle_vectors.rs): deformable.list. Run from any
folder:

    python make_deformable_fixtures.py [--check]

Everything is synthetic. One case per line, operations separated by " ; ",
arguments by spaces (those of oracle_deformable.cpp): the skeleton's bones and
the engine's bone link, meshes as tables (points and normals, corners, UVs,
skin, face groups and UV-deform rows), materials (recording fakes and real
EyeMaterials), parts, instances, bone poses, draws, levels, the animation
list, aim events and the EyeMaterial's parameters, with the engine, animation,
channel, texture and camera answers they need. Floats are the eight hex digits
of an f32.

The cases keep to what the reference defines: every skin pair names a bound
bone, every corner names a point, every UV-deform row's bone is bound, every
part has its own holder, the alternative IK solver runs without a position
task (with one it reads past its joint array), no aim's first joint is locked
(its step would read uninitialised stack), and every texture call names a
texture the fake holds.
"""

import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def h(x):
    return "%08x" % struct.unpack("<I", struct.pack("<f", x))[0]


def hs(values):
    return ",".join(h(v) for v in values)


def ds(values):
    return ",".join(str(v) for v in values) if values else "-"


def word(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def rot(axis, angle):
    c, s = math.cos(angle), math.sin(angle)
    if axis == "x":
        return [1, 0, 0, 0, c, -s, 0, s, c]
    if axis == "y":
        return [c, 0, s, 0, 1, 0, -s, 0, c]
    return [c, -s, 0, s, c, 0, 0, 0, 1]


def mul(a, b):
    return [sum(a[r * 3 + k] * b[k * 3 + c] for k in range(3)) for r in range(3) for c in range(3)]


def xform(m, p):
    return m + list(p)


IDENT9 = [1, 0, 0, 0, 1, 0, 0, 0, 1]
IDENT12 = IDENT9 + [0, 0, 0]


def relative(kind, arm, axis=(0, 0, 0), limits=(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0)):
    """23 joint payload words: type, arm (words 1..3), axis (4..6), a matrix
    (7..15), roll (16, 17), yaw (19, 20), pitch (21, 22)."""
    w = [0] * 23
    w[0] = kind & 0xFFFFFFFF
    for k in range(3):
        w[1 + k] = word(arm[k])
        w[4 + k] = word(axis[k])
    for k in range(9):
        w[7 + k] = word(IDENT9[k])
    w[16], w[17] = word(limits[0]), word(limits[1])
    w[19], w[20] = word(limits[2]), word(limits[3])
    w[21], w[22] = word(limits[4]), word(limits[5])
    return ",".join("%x" % v for v in w)


# --- case 0: the math helpers and the SVD --------------------------------------------------------

def case_math():
    ops = []
    full3 = [4.0, 1.0, 0.5, 1.0, 3.0, 0.2, 0.5, 0.2, 2.0]
    ops.append("svd 3 3 " + hs(full3))
    ops.append("svd 3 3 " + hs([1, 2, 3, 2, 4, 6, 1, 0, 1]))
    ops.append("svd 3 3 " + hs([0] * 9))
    six = []
    for r in range(6):
        for c in range(6):
            six.append(1.0 / (r + c + 1) + (0.5 if r == c else 0.0))
    ops.append("svd 6 6 " + hs(six))
    nine = [math.sin(r * 1.3 + c * 0.7) * 0.5 + (2.0 if r == c else 0.0) for r in range(9) for c in range(9)]
    ops.append("svd 9 9 " + hs(nine))
    twelve = [math.cos(r * 0.4 - c * 1.1) * 0.3 + (1.5 if r == c else 0.0) for r in range(12) for c in range(12)]
    ops.append("svd 12 12 " + hs(twelve))
    ops.append("svd 4 3 " + hs([1, 0, 2, 0, 1, 1, 2, 1, 0, 1, 1, 1]))
    ops.append("svd 3 4 " + hs([1, 0, 2, 3, 0, 1, 1, -1, 2, 1, 0, 0.5]))
    ops.append("svd 3 3 " + hs([1e-7, 0, 0, 0, 2, 0, 0, 0, 3]))
    limits = [-0.5, 0.5, -1.0, 1.0, -0.3, 0.3]
    for m in (mul(rot("z", 0.2), rot("y", 0.4)), mul(rot("z", 0.9), rot("x", 0.7)),
              rot("y", math.pi / 2), mul(rot("x", 2.5), rot("z", -0.4)), IDENT9):
        ops.append("euler %s %s" % (hs(limits), hs(m)))
    parent = mul(rot("z", 0.3), rot("x", -0.2))
    d = [0.6, 0.48, 0.64]
    for flags in ("4", "8", "10", "0", "1c"):
        ops.append("look %s %s %s" % (flags, hs(d), hs(parent)))
    q1 = [math.cos(0.3), 0.0, math.sin(0.3), 0.0]
    q2 = [math.cos(0.5), math.sin(0.5) * 0.6, 0.0, math.sin(0.5) * 0.8]
    q3 = [-q2[0], -q2[1], -q2[2], -q2[3]]
    ops.append("qlog %s %s" % (hs(q1), hs(q2)))
    ops.append("qlog %s %s" % (hs(q1), hs(q3)))
    ops.append("qlog %s %s" % (hs(q1), hs(q1)))
    ops.append("qexp %s" % hs([0.1, -0.2, 0.3]))
    ops.append("qexp %s" % hs([1e-6, 0, 0]))
    ops.append("qexp %s" % hs([1.5, 0.5, -2.0]))
    return ops


# --- the skeleton and meshes ----------------------------------------------------------------------

def skeleton(ops, count, link_root, links):
    bones = []
    for i in range(count):
        m = mul(rot("z", 0.1 * i), rot("x", -0.05 * i))
        bones += xform(m, (0.1 * i, -0.5 * i, 0.02 * i))
    ops.append("bones " + hs(bones))
    ops.append("link %d %s" % (link_root, ",".join("%d:%d" % p for p in links) if links else "-"))


def mesh_ops(name, flag70, groups, uvd=None, points=6, scale=1.0):
    ops = ["mesh %s %d" % (name, flag70)]
    pts = []
    for i in range(points):
        p = (scale * math.cos(i), scale * (0.5 * i - 1.0), scale * math.sin(i * 0.7))
        n = (math.cos(i * 0.3), math.sin(i * 0.3), 0.0)
        pts += list(p) + list(n)
    ops.append("pts " + hs(pts))
    corners = [0, 1, 2, 3, 4, 5, 2, 0, 5, 1]
    ops.append("pidx " + ds(corners))
    ops.append("uv " + hs([0.1 * i for i in range(2 * points)]))
    ops.append("uvidx " + ds([c % points for c in corners]))
    skin = []
    for i in range(points):
        k = i % 5
        if k == 0:
            skin.append("1:%d/%s" % (i % 4, h(1.0)))
        elif k == 1:
            skin.append("2:0/%s:1/%s" % (h(0.25), h(0.75)))
        elif k == 2:
            skin.append("3:1/%s:2/%s:3/%s" % (h(0.5), h(0.3), h(0.2)))
        elif k == 3:
            skin.append("4:0/%s:1/%s:2/%s:3/%s" % (h(0.1), h(0.2), h(0.3), h(0.4)))
        else:
            skin.append("5:3/%s:2/%s:1/%s:0/%s:1/%s" % (h(0.6), h(0.1), h(0.1), h(0.1), h(0.1)))
    ops.append("skin " + ",".join(skin))
    for mat, indices in groups:
        ops.append("grp %s %s" % (mat, ds(indices)))
    if uvd:
        ops.append("uvd " + hs(uvd))
    ops.append("endmesh")
    return ops


def pose(ops, count):
    for i in range(count):
        m = mul(rot("y", 0.15 * (i + 1)), rot("x", 0.05 * i))
        ops.append("bone %d mat %s" % (i, hs(xform(m, (0.2 * i, 1.0 + 0.1 * i, -0.3 * i)))))


# --- case 1: skinning and the draw ----------------------------------------------------------------

def case_skin():
    ops = []
    skeleton(ops, 5, 0x400, [(0x400, 0x100), (0x401, 0x101), (0x402, 0x102), (0x403, 0x103)])
    ops.append("matrix 1024 " + hs(xform(rot("z", 0.3), (1.0, 2.0, 3.0))))
    ops.append("parent 1027 1026")
    ops.append("ori 1026 " + hs(rot("y", 0.2)))
    ops.append("holder")
    ops.append("mat body MatA")
    ops.append("eyemat eye_l")
    ops.append("tex eyeball 1 77")
    ops.append("eye set eye_l 6 7 " + b"eyeball".hex())
    ops.append("eye setup eye_l")
    groups = [("body", [0, 1, 2, 2, 1, 3]), ("eye_l", [3, 4, 5]), ("-", []), ("missing", [5, 4, 3])]
    ops += mesh_ops("skinme0", 1, groups, uvd=[3, 2, 1.5, 0.8, 0.6, -0.1, 0.1, -0.05, 0.05])
    ops.append("part skinme0")
    ops.append("inst 5150")
    ops.append("cam view " + hs(xform(rot("x", 0.1), (0.0, -1.0, 10.0))))
    ops.append("lod 0 0")
    ops.append("drawpart 0")
    pose(ops, 5)
    ops.append("drawpart 0")
    ops.append("dirty 2")
    ops.append("dirty 0")
    ops.append("showbone 1")
    ops.append("drawpart 0")
    ops.append("bone 3 ori " + hs(rot("x", 0.4)))
    ops.append("bone 0 pos " + hs([0.5, 0.25, -0.125]))
    ops.append("drawpart 0")
    ops.append("stats")
    ops.append("sphere")
    ops.append("first")
    ops.append("draw 3 20")
    ops.append("destroy")
    return ops


# --- case 2: levels, parts, the rate and the hardpoints -------------------------------------------

def case_levels():
    ops = []
    skeleton(ops, 4, 0x410, [(0x410, 0x100), (0x411, 0x101), (0x412, 0x102), (0x413, 0x103)])
    ops.append("holder")
    ops.append("mat skin MatB")
    ops.append("mat cloth MatC")
    ops += mesh_ops("lv0", 1, [("skin", [0, 1, 2, 3, 4, 5]), ("cloth", [5, 4, 0])])
    ops += mesh_ops("lv1", 0, [("cloth", [0, 2, 4])], points=5, scale=0.5)
    ops.append("part lv0,lv1")
    skeleton(ops, 4, 0x420, [(0x420, 0x100), (0x421, 0x101), (0x422, 0x102), (0x423, 0x103)])
    ops.append("holder")
    ops += mesh_ops("arm0", 1, [("skin", [0, 1, 2])], points=4, scale=2.0)
    ops.append("part arm0")
    ops.append("inst 0")
    pose(ops, 4)
    ops.append("lod 0 0")
    ops.append("lod 1 0")
    ops.append("pstate -")
    ops.append("drawpart 0")
    ops.append("setlod %s 1,0 %s" % (h(0.05), h(0.5)))
    ops.append("drawpart 0")
    ops.append("setlod %s 1,0 %s" % (h(0.3), h(0.5)))
    ops.append("setlod %s 1,0 %s" % (h(0.25), h(0.5)))
    ops.append("setlod %s 9,0 %s" % (h(0.0), h(0.0)))
    ops.append("lod 0 1")
    ops.append("pstate 1")
    ops.append("cam class 2")
    ops.append("draw 0 0")
    ops.append("cam class 4")
    ops.append("rank 2")
    ops.append("draw 7 1")
    ops.append("rank 5")
    ops.append("draw 7 1")
    ops.append("sphere")
    ops.append("stats")
    ops.append("hp 256 head,hand")
    ops.append("hp 257 hand")
    ops.append("hp 258 hand,foot")
    ops.append("hp 259 foot")
    ops.append("parent 1041 1040")
    ops.append("parent 1042 1040")
    ops.append("connres 1")
    ops.append("walkhp")
    ops.append("connect")
    ops.append("first")
    ops.append("nohp")
    ops.append("walkhp")
    ops.append("destroy")
    return ops


# --- case 3: the animation list and the pose calls ------------------------------------------------

def case_anim():
    ops = []
    skeleton(ops, 4, 0x430, [(0x430, 0x100), (0x431, 0x101), (0x432, 0x102), (0x433, 0x103)])
    ops.append("holder")
    ops.append("mat skin MatB")
    ops += mesh_ops("an0", 1, [("skin", [0, 1, 2])], points=3)
    ops.append("part an0")
    ops.append("inst 99")
    ops.append("bone 0 mat " + hs(xform(rot("y", 0.6), (1.0, 0.75, -2.0))))
    ops.append("rootres 0 " + hs([0.25, 0.5, 0.125]))
    ops.append("named -")
    one = h(1.0)
    ops.append("play 11 22 %s 4 %s 6 7 %s 0" % (h(0.5), h(2.0), h(-1.0)))
    ops.append("rootres 1 " + hs([0.25, 0.5, 0.125]))
    ops.append("named " + h(0.4))
    ops.append("play 12 23 %s 4 %s 6 7 %s 33" % (h(0.5), one, h(0.8)))
    ops.append("play 13 24 %s 4 %s 6 7 %s 0" % (h(0.0), one, h(0.0)))
    ops.append("playres -1")
    ops.append("play 14 25 %s 4 %s 6 7 %s 0" % (h(0.0), one, h(0.0)))
    ops.append("evalres -1")
    ops.append("play 15 26 %s 4 %s 6 7 %s 0" % (h(0.0), one, h(0.0)))
    ops.append("evalres 0")
    ops.append("durn 1792 " + h(0.3))
    ops.append("touch 1792 " + hs([0.1, 0.2, 0.3, 0.4, 0.5, 0.6]))
    ops.append("height " + h(0.2))
    ops.append("posepos " + hs([3.0, 4.0, 5.0]))
    ops.append("poseori " + hs(rot("y", 0.25)))
    ops.append("filter 1793")
    ops.append("count")
    ops.append("object 1792 abc")
    ops.append("firstf 1792 77")
    ops.append("onkeys abc")
    ops.append("sink")
    ops.append("onkeys abc")
    ops.append("onkeys def")
    ops.append("onloop abc " + hs(xform(rot("y", 0.7), (1.0, 2.0, 3.0))))
    ops.append("onloop def " + hs(IDENT12))
    ops.append("destroyh 1792")
    ops.append("count")
    # A chain animation from the second bone.
    ops.append("parent 1073 1072")
    ops.append("rel 1073 " + relative(2, (0.0, 0.5, 0.0), (0.1, 0.2, 0.3)))
    ops.append("pos 1073 " + hs([0.3, 1.4, -1.9]))
    key = 0x700 + 5
    ops.append("samp %x %s %s %s" % (key * 0x100 + 1073 * 2, h(0.25),
                                     hs([1, 0, 0, 0, 0, 0, 0, 0]), hs([math.cos(0.2), 0, math.sin(0.2), 0, 0, 0, 0, 0])))
    ops.append("samp %x %s %s %s" % (key * 0x100 + 1072 * 2 + 1, h(0.5),
                                     hs([0.1, 0.2, 0.3, 1, 0, 0, 0]), hs([0.2, 0.4, 0.5, math.cos(0.1), math.sin(0.1), 0, 0])))
    ops.append("playalt 16 27 %s 4 %s 6 7 %s 1073" % (h(0.5), one, h(0.0)))
    ops.append("count")
    ops.append("destroyall")
    ops.append("count")
    ops.append("destroy")
    return ops


# --- cases 4 and 5: aim solving -------------------------------------------------------------------

def chain(ops, joints, kinds=None, axes=None):
    """A chain from instance 10 (the root) through 11.. upwards along y, each
    joint turned a little."""
    kinds = kinds or [2] * joints
    axes = axes or [(0, 0, 0)] * joints
    ori = IDENT9
    pos = [0.0, 0.0, 0.0]
    ops.append("pos 10 " + hs(pos))
    ops.append("ori 10 " + hs(ori))
    for j in range(joints):
        inst = 11 + j
        parent = 10 + j
        arm = (0.05 * j, 1.0, -0.02 * j)
        step = mul(rot("z", 0.1 + 0.05 * j), rot("x", 0.03 * j))
        pos = [pos[k] + sum(ori[k * 3 + c] * arm[c] for c in range(3)) for k in range(3)]
        ori = mul(ori, step)
        ops.append("parent %d %d" % (inst, parent))
        ops.append("rel %d %s" % (inst, relative(kinds[j], arm, axes[j],
                                                 (-0.4, 0.4, -0.8, 0.8, -0.6, 0.6))))
        ops.append("pos %d %s" % (inst, hs(pos)))
        ops.append("ori %d %s" % (inst, hs(ori)))
        half = (0.1 + 0.05 * j) / 2
        ops.append("jp %d %s" % (inst, hs([math.cos(half), 0, 0, math.sin(half)])))


def aim_base(ops):
    skeleton(ops, 4, 0x440, [(0x440, 0x100), (0x441, 0x101), (0x442, 0x102), (0x443, 0x103)])
    ops.append("holder")
    ops.append("mat skin MatB")
    ops += mesh_ops("aim0", 1, [("skin", [0, 1, 2])], points=3)
    ops.append("part aim0")
    ops.append("inst 0")


def case_aim_single():
    ops = []
    aim_base(ops)
    chain(ops, 1)
    orient = mul(rot("y", 0.5), rot("x", 0.2))
    aims = [("5", 0.0), ("9", 0.0), ("11", 0.0), ("41", 0.0), ("85", 0.0), ("40", 0.0)]
    for index, (flags, _) in enumerate(aims):
        ops.append("aim 10 11 %s %s %s %s - 3" % (flags, hs([0.7, 1.6, 0.4]), hs(orient), h(0.1)))
        ops.append("apply %d 0 0 11" % index)
        ops.append("apply %d 0 0 11" % index)
    ops.append("target 0 %s %s" % (hs([-0.5, 0.2, 0.9]), hs(IDENT9)))
    ops.append("apply 0 0 0 11")
    ops.append("apply 0 0 1 11")
    ops.append("stopaim 0")
    ops.append("apply 0 0 0 11")
    ops.append("count")
    ops.append("mixer 900")
    ops.append("count")
    # A blocked joint, and a chain too long.
    ops.append("block 11")
    ops.append("aim 10 11 5 %s %s %s - 0" % (hs([1, 1, 1]), hs(IDENT9), h(0.1)))
    chain_long = []
    chain(chain_long, 5)
    ops += chain_long
    ops.append("aim 10 15 21 %s %s %s - 0" % (hs([1, 1, 1]), hs(IDENT9), h(0.1)))
    ops.append("count")
    ops.append("destroy")
    return ops


def case_aim_dls():
    ops = []
    aim_base(ops)
    chain(ops, 3)
    orient = mul(rot("z", 0.6), rot("y", -0.3))
    target = [0.9, 2.2, 0.5]
    ops.append("aim 10 13 21 %s %s %s - 0" % (hs(target), hs(orient), h(0.05)))
    for _ in range(4):
        ops.append("apply 0 0 0 11")
    ops.append("apply 0 2 0 11")
    ops.append("aim 10 13 61 %s %s %s - 0" % (hs(target), hs(orient), h(0.2)))
    ops.append("apply 1 1 0 11")
    ops.append("aim 10 13 41 %s %s %s - 0" % (hs(target), hs(orient), h(0.1)))
    ops.append("apply 2 0 0 11")
    ops.append("aim 10 13 a1 %s %s %s 12 0" % (hs(target), hs(orient), h(0.1)))
    ops.append("apply 3 0 0 11")
    ops.append("aim 10 13 e1 %s %s %s 12 0" % (hs([0.2, 3.0, 0.1]), hs(orient), h(0.3)))
    ops.append("apply 4 2 0 11")
    ops.append("aim 10 13 21 %s %s %s - 0" % (hs([0.0, 2.6, 1.4]), hs(orient), h(0.5)))
    ops.append("apply 5 0 0 11")
    # Already on target: no solve past the start.
    ops.append("aim 10 13 21 %s %s %s - 0" % (hs([0.15, 2.99, -0.06]), hs(orient), h(0.1)))
    ops.append("apply 6 0 0 11")
    ops.append("destroyall")
    # A sphere joint with an axis: the alternative solver (orientation only).
    ops.append("rel 12 " + relative(4, (0.05, 1.0, -0.02), (0.0, 0.0, 1.0)))
    ops.append("aim 10 13 41 %s %s %s - 0" % (hs(target), hs(orient), h(0.1)))
    ops.append("apply 7 0 0 11")
    ops.append("aim 10 13 c1 %s %s %s - 0" % (hs(target), hs(orient), h(0.2)))
    ops.append("apply 8 1 0 11")
    ops.append("destroy")
    return ops


# --- case 6: the EyeMaterial ----------------------------------------------------------------------

def case_eye():
    ops = []
    ops.append("eyemat eye")
    ops.append("eye desc eye")
    for i in range(11):
        ops.append("eye get eye %d" % i)
    ops.append("eye set eye 0 3 %s" % ",".join(h(v) for v in (0.5, 0.25, 0.125)))
    ops.append("eye set eye 1 3 %s" % ",".join(h(v) for v in (0.75, 0.5, 1.0)))
    ops.append("eye set eye 1 3 %s" % ",".join(h(v) for v in (1.5, 0.5, 1.0)))
    ops.append("eye set eye 1 2 %s" % h(0.5))
    ops.append("eye set eye 4 2 %s" % h(1.0000001))
    ops.append("eye set eye 4 2 %s" % h(-0.0000001))
    ops.append("eye set eye 7 8 6")
    ops.append("eye set eye 8 9 1")
    ops.append("eye set eye 9 9 3")
    ops.append("eye set eye 11 2 0")
    ops.append("eye get eye 1")
    ops.append("eye get eye 7")
    ops.append("eye name eye " + b"Eye_Material_With_A_Rather_Long_Name_That_Runs_Past_Sixty_Three_Bytes".hex())
    ops.append("eye name eye -")
    ops.append("eye name eye " + b"EyeBall".hex())
    ops.append("tex single 1 1234")
    ops.append("tex multi 4 5678")
    ops.append("texfmt 1234 6")
    ops.append("texfmt 5678 7")
    ops.append("eye setup eye")
    ops.append("eye set eye 6 7 " + b"single".hex())
    ops.append("eye setup eye")
    ops.append("eye flags eye")
    ops.append("texfmt 1234 7")
    ops.append("eye setup eye")
    ops.append("eye flags eye")
    ops.append("eye set eye 6 7 " + b"multi".hex())
    ops.append("eye setup eye")
    ops.append("eye resolve eye")
    ops.append("eye set eye 6 7 " + b"nothere".hex())
    ops.append("eye setup eye")
    ops.append("eye set eye 5 2 %s" % h(0.5))
    ops.append("eye flags eye")
    ops.append("eye set eye 5 2 %s" % h(1.0))
    ops.append("texfmt 1234 3")
    ops.append("eye set eye 6 7 " + b"single".hex())
    ops.append("eye setup eye")
    ops.append("eye verify eye")
    ops.append("eye bind eye null")
    skeleton(ops, 2, 0x450, [(0x450, 0x100), (0x451, 0x101)])
    ops.append("holder")
    ops.append("parent 1105 1104")
    ops.append("ori 1104 " + hs(rot("z", 0.3)))
    ops.append("bone 1 mat " + hs(xform(mul(rot("x", 0.4), rot("y", -0.6)), (0, 0, 0))))
    ops.append("eye bind eye 1 " + hs([1, 3, 2.0, 0.5, 0.4, -0.1, 0.1, -0.05, 0.05]))
    ops.append("eye bind eye 1 " + hs([1, 3, 2.0, 0.01, 0.01, -0.1, 0.1, -0.05, 0.05]))
    ops.append("eye bind eye 1 " + hs([1, 3, 0.0, 0.5, 0.4, -0.1, 0.1, -0.05, 0.05]))
    ops.append("bone 0 mat " + hs(xform(rot("x", -0.3), (0, 0, 0))))
    ops.append("eye bind eye 0 " + hs([0, 3, 1.0, 0.2, 0.1, -1.0, 1.0, -1.0, 1.0]))
    ops.append("eye submit eye 0")
    ops.append("eye submit eye -1")
    return ops


def deformable_list():
    return [" ; ".join(case()) for case in (case_math, case_skin, case_levels, case_anim,
                                            case_aim_single, case_aim_dls, case_eye)]


def write(name, text, check):
    path = os.path.join(HERE, name)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != text.encode("latin-1"):
                sys.exit("%s is out of date; run make_deformable_fixtures.py" % name)
        return
    with open(path, "wb") as out:
        out.write(text.encode("latin-1"))


def listing(title, lines):
    return "# P4.7 %s oracle inputs, written by make_deformable_fixtures.py.\n" % title + "".join(
        line + "\n" for line in lines)


def main():
    check = "--check" in sys.argv[1:]
    write("deformable.list", listing("deformable", deformable_list()), check)


if __name__ == "__main__":
    main()
