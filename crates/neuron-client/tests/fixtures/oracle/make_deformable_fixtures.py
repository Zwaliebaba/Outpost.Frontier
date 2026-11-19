"""Writes the input of the P4.7 deformable suites (Oracle.exe neuron-client,
CppSource/Oracle/oracle_deformable.cpp and oracle_deformable_parts.cpp, and
crates/neuron-client/tests/oracle_vectors.rs): deformable.list,
deformable_parts.list with the archives in deformable/, and deformable_anim.list
with the skeleton and animations in deformable/ (oracle_deformable_anim.cpp).
Run from any folder:

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



# --- the second suite: parts from archives and the EyeMaterial in the library ---------------------
#
# deformable_parts.list (CppSource/Oracle/oracle_deformable_parts.cpp): ConnectInstanceParts and
# LoadSkeleton over UTF archives written here into deformable/, read by an in-memory file system
# (the oracle) and by neuron-core's UTF file system (the port); and the EyeMaterial made by its
# factory and driven through the material interface, loaded from and saved into an archive.
#
# The archives keep to what the reference defines: every part loads a skeleton and at least one
# level (a part without a holder is read through null), every skin pair names a bone the engine
# links, every UV-deform row's bone is bound, and no leaf is empty.

MAGIC = 0x20465455
UTF_VERSION = 0x101
UTF_HEADER = 0x38
TIMES = (0x2C210021, 0, 0x2C216000)


class Leaf:
    def __init__(self, name, data):
        self.name = name
        self.data = bytes(data)


class Node:
    def __init__(self, name, children):
        self.name = name
        self.children = children


def utf(children):
    """A UTF archive (0x2C-byte entries) whose root holds `children`."""
    tree = bytearray(0x2C)
    strings = bytearray()
    data = bytearray()

    def name_offset(name):
        off = len(strings)
        strings.extend(name + b'\0')
        return off

    def place(items):
        if not items:
            return 0
        first = len(tree)
        tree.extend(bytes(0x2C * len(items)))
        for i, item in enumerate(items):
            at = first + i * 0x2C
            nxt = at + 0x2C if i + 1 < len(items) else 0
            if isinstance(item, Leaf):
                struct.pack_into('<III', tree, at, nxt, name_offset(item.name), 0x80)
                struct.pack_into('<I', tree, at + 0x10, len(data))
                struct.pack_into('<I', tree, at + 0x1C, len(item.data))
                data.extend(item.data)
            else:
                struct.pack_into('<III', tree, at, nxt, name_offset(item.name), 0x10)
                struct.pack_into('<I', tree, at + 0x10, place(item.children))
            struct.pack_into('<III', tree, at + 0x20, *TIMES)
        return first

    struct.pack_into('<III', tree, 0, 0, name_offset(b'\\'), 0x10)
    struct.pack_into('<III', tree, 0x20, *TIMES)
    struct.pack_into('<I', tree, 0x10, place(children))
    t = UTF_HEADER
    s = t + len(tree)
    d = s + len(strings)
    head = bytearray(UTF_HEADER)
    for at, value in ((0, MAGIC), (4, UTF_VERSION), (8, t), (0x14, 0x2C), (0x18, s), (0x24, d)):
        struct.pack_into('<I', head, at, value)
    return bytes(head + tree + strings + data)


def u16s(values):
    return struct.pack('<%dH' % len(values), *values)


def u32s(values):
    return struct.pack('<%dI' % len(values), *[v & 0xFFFFFFFF for v in values])


def f32s(values):
    return struct.pack('<%df' % len(values), *values)


def bone_node(name, matrix, skeleton=False):
    leaf = Leaf(b'Bone to root', f32s(matrix))
    if skeleton:
        return Node(name, [Node(b'DFMSkeleton', [leaf]), Leaf(b'Lod Bits', b'\xff')])
    return Node(name, [leaf, Leaf(b'Lod Bits', b'\xff')])


def bone_matrix(i):
    return mul(rot("z", 0.1 * i), rot("x", -0.05 * i)) + [0.1 * i, 0.4 * i, -0.02 * i]


def dfm_mesh_node(name, points, groups, skin, uv_rows=None, scale=1.0):
    """groups: (node, material or None, indices); skin: per point [(bone, weight), ...];
    uv_rows: the nine UV-deform columns' rows."""
    face = [Leaf(b'Count', u32s([len(groups)]))]
    for node, material, indices in groups:
        kids = []
        if material is not None:
            kids.append(Leaf(b'Material_name', material + b'\0'))
        kids.append(Leaf(b'Face_indices', u16s(indices)))
        face.append(Node(node, kids))
    pts = []
    normals = []
    for i in range(points):
        pts += [scale * math.cos(i), scale * (0.5 * i - 1.0), scale * math.sin(i * 0.7)]
        normals += [math.cos(i * 0.3), math.sin(i * 0.3), 0.0]
    corners = [i % points for i in range(points + 3)]
    firsts, counts, ids, weights = [], [], [], []
    for pairs in skin:
        firsts.append(len(ids))
        counts.append(len(pairs))
        for b, w in pairs:
            ids.append(b)
            weights.append(w)
    geometry = [
        Leaf(b'Point_indices', u32s(corners)),
        Leaf(b'Points', f32s(pts)),
        Leaf(b'Vertex_normals', f32s(normals)),
        Leaf(b'UV0_indices', u32s(list(range(len(corners))))),
        Leaf(b'UV0', f32s([i / 16.0 for i in range(2 * len(corners))])),
        Leaf(b'Point_bone_first', u32s(firsts)),
        Leaf(b'Point_bone_count', u32s(counts)),
        Leaf(b'Bone_id_chain', u32s(ids)),
        Leaf(b'Bone_weight_chain', f32s(weights)),
    ]
    if uv_rows:
        columns = [b'UV_bone_id', b'UV_vertex_count', b'UV_plane_distance', b'Bone_X_to_U_scale',
                   b'Bone_Y_to_V_scale', b'Min_du', b'Max_du', b'Min_dv', b'Max_dv']
        for c, column in enumerate(columns):
            values = [row[c] for row in uv_rows]
            geometry.append(Leaf(column, u32s(values) if c < 2 else f32s(values)))
    return Node(name, [Node(b'Face_groups', face), Node(b'Geometry', geometry)])


SKIN6 = [[(0, 1.0)], [(0, 0.25), (1, 0.75)], [(1, 0.5), (2, 0.5)],
         [(0, 0.1), (1, 0.2), (2, 0.3), (2, 0.4)], [(2, 1.0)], [(1, 0.6), (0, 0.4)]]


def man_dfm():
    bones = [bone_node(b'Root.3db', bone_matrix(0)), bone_node(b'Spine.3db', bone_matrix(1)),
             bone_node(b'Head.3db', bone_matrix(2), skeleton=True)]
    mesh0 = dfm_mesh_node(
        b'Mesh0', 6,
        [(b'Group0', b'body', [0, 1, 2, 2, 1, 3]), (b'Group1', b'eyeball', [3, 4, 5]),
         (b'Group2', b'nomat', [5, 4, 3])],
        SKIN6, uv_rows=[[2, 3, 2.0, 0.5, 0.4, -0.1, 0.1, -0.05, 0.05]])
    mesh1 = dfm_mesh_node(b'Mesh1', 4, [(b'Group0', b'body', [0, 1, 2, 2, 1, 3])],
                          SKIN6[:4], scale=0.5)
    multi = Node(b'MultiLevel', [Leaf(b'Fractions', f32s([1.0, 0.5])), mesh0, mesh1])
    return utf(bones + [Node(b'Skeleton', [Leaf(b'Name', b'man\0')]), multi])


def hat_dfm():
    mesh = dfm_mesh_node(b'Mesh0', 3, [(b'Group0', b'hat', [0, 1, 2])],
                         [[(0, 1.0)], [(0, 1.0)], [(0, 0.5), (0, 0.5)]], scale=0.25)
    return utf([bone_node(b'Hat.3db', bone_matrix(3)), mesh])


def levels_dfm():
    bones = [bone_node(b'Pelvis.3db', bone_matrix(0)), bone_node(b'Leg.3db', bone_matrix(1)),
             bone_node(b'Foot.3db', bone_matrix(2))]
    meshes = [dfm_mesh_node(b'Mesh%d' % k, 6 - k, [(b'Group0', b'skin', [0, 1, 2])],
                            SKIN6[:6 - k], scale=1.0 - 0.2 * k) for k in range(3)]
    vmesh = Node(b'Mesh3', [Node(b'Geometry', [Leaf(b'Points', f32s([0.0] * 9))])])
    multi = Node(b'MultiLevel', [Leaf(b'Fractions', f32s([1.0, 0.6, 0.3, 0.1])), meshes[0],
                                 meshes[1], vmesh, meshes[2], Leaf(b'Mesh_note', b'not a mesh')])
    return utf(bones + [multi])


def eye_library():
    def material(name, leaves):
        return Node(name, [Leaf(b'Type', b'EyeMaterial\0')] + leaves)

    return utf([Node(b'Material library', [
        material(b'eyeball_l', [
            Leaf(b'Dc', f32s([0.5, 0.25, 0.75])), Leaf(b'Ec', f32s([0.1, 0.2, 0.3])),
            Leaf(b'Oc', f32s([0.5])), Leaf(b'Sp', f32s([0.25])),
            Leaf(b'Dt_name', b'eyeball\0'), Leaf(b'Dt_flags', u32s([0x3B]))]),
        material(b'eyeball_r', [Leaf(b'Sc', f32s([1.0, 0.5])), Leaf(b'Dt_flags', u32s([0x46]))]),
    ])])


ARCHIVES = {
    'man.dfm': man_dfm,
    'hat.dfm': hat_dfm,
    'levels.dfm': levels_dfm,
    'eyes.mat': eye_library,
}



def case_parts():
    ops = []
    ops.append("links 1024 1024:256,1025:257,1026:258 | 1040 1040:259")
    ops.append("mat body MatA")
    ops.append("mat eyeball EyeMaterial")
    ops.append("mat hat MatC")
    ops.append("hp 258 head")
    ops.append("hp 259 head")
    ops.append("parent 1025 1024")
    ops.append("create deformable\\man.dfm:8:0:8,deformable\\hat.dfm:8:0:8")
    ops.append("meshes")
    pose(ops, 3)
    ops.append("lod 0 0")
    ops.append("drawpart 0")
    ops.append("lod 1 0")
    ops.append("drawpart 1")
    ops.append("lod 0 1")
    ops.append("drawpart 0")
    ops.append("stats")
    ops.append("sphere")
    ops.append("first")
    ops.append("lod 0 0")
    ops.append("draw 0 0")
    ops.append("destroy")
    ops.append("meshes")
    return ops


def case_levels_flags():
    """Three parts of one file: levels skipped, stopped after and limited, the VMesh retry of a
    node without face groups and of a leaf among the levels (UTF makes a file system of a leaf;
    their archetypes take handles 0x103 and 0x104), and meshes shared through the cache."""
    ops = []
    ops.append("links 1056 1056:256,1057:257,1058:258 | 1072 1072:261,1073:262,1074:263"
               " | 1088 1088:264,1089:265,1090:266")
    ops.append("mat skin MatB")
    ops.append("parent 1057 1056")
    ops.append("parent 1058 1057")
    ops.append("parent 1073 1072")
    ops.append("create deformable\\levels.dfm:3:2:1,deformable\\levels.dfm:2:0:0,"
               "deformable\\levels.dfm:8:1:0")
    ops.append("meshes")
    pose(ops, 3)
    ops.append("lod 0 0")
    ops.append("drawpart 0")
    ops.append("lod 0 1")
    ops.append("drawpart 0")
    ops.append("lod 1 1")
    ops.append("drawpart 1")
    ops.append("lod 2 0")
    ops.append("drawpart 2")
    ops.append("stats")
    ops.append("destroy")
    ops.append("meshes")
    return ops


def case_eye_library():
    ops = []
    ops.append("eyelib map")
    ops.append("eyelib refuse Foo")
    ops.append("eyelib new eye")
    ops.append("eyelib info eye")
    ops.append("eyelib name eye " + b"EyeBall_L".hex())
    ops.append("tex eyeball 1 77")
    ops.append("texfmt 77 7")
    ops.append("eyelib load eye deformable\\eyes.mat eyeball_l")
    ops.append("eyelib verify eye")
    ops.append("eyelib resolve eye")
    ops.append("eyelib flags eye")
    for i in range(11):
        ops.append("eyelib get eye %d" % i)
    ops.append("eyelib set eye 0 3 %s" % hs([0.5, 0.5, 0.5]))
    ops.append("eyelib set eye 3 3 %s" % hs([2.0, 0.5, 0.5]))
    ops.append("eyelib set eye 6 7 " + b"eyeball".hex())
    ops.append("eyelib set eye 8 9 1")
    ops.append("eyelib save eye deformable\\eyes.mat eyeball_r")
    ops.append("eyelib load eye deformable\\eyes.mat eyeball_r")
    ops.append("eyelib flags eye")
    skeleton(ops, 2, 0x460, [(0x460, 0x100), (0x461, 0x101)])
    ops.append("holder")
    ops.append("parent 1121 1120")
    ops.append("ori 1120 " + hs(rot("z", 0.3)))
    ops.append("bone 1 mat " + hs(xform(mul(rot("x", 0.4), rot("y", -0.6)), (0, 0, 0))))
    ops.append("eyelib bind eye 1 " + hs([1, 3, 2.0, 0.5, 0.4, -0.1, 0.1, -0.05, 0.05]))
    ops.append("eyelib bind eye -")
    ops.append("eyelib submit eye")
    ops.append("eyelib release eye")
    return ops


def parts_list():
    return [" ; ".join(case()) for case in (case_parts, case_levels_flags, case_eye_library)]


# --- the third suite: animations and aim events through the animation system ----------------------
#
# deformable_anim.list (CppSource/Oracle/oracle_deformable_anim.cpp): a skeleton compound
# (deformable/skel.cmp: a pelvis, a spine, neck and head on sphere joints and an arm on a revolute
# one) loaded and instanced by the scene engine, its instances bound to an instance's bone runtimes,
# and the scripts of deformable/moves.anm played on it through IDeformable (PlayInstanceAnim,
# PlayInstanceAnimChain, the list, OnKeys and OnLoop into the caller's sink) and aim events whose
# typed channels the animation system ticks, each case walked as FEngineCharacterBody::Advance does
# (MixerWalk under the root bone, UpdateInstance, GetListCount). Both read the archives with their own
# UTF readers (the oracle's in-memory one of the parts suite, neuron-core's).
#
# The cases keep to what the reference defines: every aim chain is of sphere joints without an
# axis (the alternative solver with a position task reads past its joint array), no aim's first
# joint is locked, PlayInstanceAnimChain names a joint whose chain to the root bone every channel of
# the playback reaches, every aim is stopped and swept before the case ends (the handle registry
# outlives the instances), and the event channel binds to the instance's sink.

def quat(axis, angle):
    c, s = math.cos(angle / 2), math.sin(angle / 2)
    v = {"x": (s, 0.0, 0.0), "y": (0.0, s, 0.0), "z": (0.0, 0.0, s)}[axis]
    return [c, v[0], v[1], v[2]]


def cstr(text, size):
    raw = text.encode() + b'\0'
    return raw.ljust(size, b'\0')[:size]


def joint_record(parent, child, words, size):
    data = cstr(parent, 0x40) + cstr(child, 0x40) + f32s(words)
    return data.ljust(size, b'\0')[:size]


def sphere_record(parent, child, origin, limits):
    # origin, offset (none: no axis), rotation, yaw and pitch limits, roll limits.
    return joint_record(parent, child, list(origin) + [0.0, 0.0, 0.0] + IDENT9 + list(limits), 0xD4)


def rev_record(parent, child, origin, axis, lo, hi):
    return joint_record(parent, child, list(origin) + [0.0, 0.0, 0.0] + IDENT9 + list(axis) + [lo, hi],
                        0xD0)


SKEL_BONES = ["pelvis", "spine", "neck", "head", "arm"]


def skel_cmp():
    def part(entry, name):
        return Node(entry.encode(), [Leaf(b'Object name', name.encode() + b'\0'),
                                     Leaf(b'File name', (name + ".3db").encode() + b'\0'),
                                     Leaf(b'Index', u32s([0]))])
    limits = (-0.8, 0.8, -0.6, 0.6, -0.4, 0.4)
    spheres = (sphere_record("pelvis", "spine", (0.0, 0.5, 0.0), limits)
               + sphere_record("spine", "neck", (0.0, 0.6, 0.05), limits)
               + sphere_record("neck", "head", (0.0, 0.25, 0.02), limits))
    revs = rev_record("spine", "arm", (0.3, 0.4, 0.0), (0.0, 0.0, 1.0), -1.5, 1.5)
    cmpnd = Node(b'Cmpnd', [part("Root", "pelvis")] + [part("Part_" + n, n) for n in SKEL_BONES[1:]]
                 + [Node(b'Cons', [Leaf(b'Sphere', spheres), Leaf(b'Rev', revs)])])
    files = [Node((n + ".3db").encode(), [Leaf(b'Bone', u32s([i]))]) for i, n in enumerate(SKEL_BONES)]
    return utf([cmpnd] + files)


def anim_channel(count, interval, flags, frames):
    return Node(b'Channel', [Leaf(b'Header', struct.pack('<IfI', count, interval, flags)),
                             Leaf(b'Frames', frames)])


def anim_map(name, parent, child, channel):
    kids = []
    if parent is not None:
        kids.append(Leaf(b'Parent name', parent.encode() + b'\0'))
    if child is not None:
        kids.append(Leaf(b'Child name', child.encode() + b'\0'))
    kids.append(channel)
    return Node(name.encode(), kids)


def rows(values):
    return b''.join(f32s(r) for r in values)


def moves_anm():
    walk = Node(b'Walk', [
        anim_map("Object map", "pelvis", None, anim_channel(4, 0.25, 6, rows([
            [0.0, 0.0, 0.0] + quat("y", 0.0), [0.2, 0.02, 0.3] + quat("y", 0.1),
            [0.4, 0.0, 0.6] + quat("y", 0.2), [0.6, 0.02, 0.9] + quat("y", 0.3)]))),
        anim_map("Joint map 0", "pelvis", "spine", anim_channel(3, 0.375, 4, rows([
            quat("x", 0.0), quat("x", 0.1), quat("x", 0.0)]))),
        anim_map("Joint map 1", "spine", "neck", anim_channel(3, 0.375, 4, rows([
            quat("z", 0.0), quat("z", -0.15), quat("z", 0.0)]))),
        Leaf(b'Root height', f32s([0.95])),
    ])
    # A key at 0 bounds every range a tick collects (CollectRange reads outside the frames for a
    # range no frame bounds).
    events = b''.join(f32s([t]) + u32s([4 * i]) for i, t in enumerate([0.0, 0.3, 0.45]))
    events += b'STEPWAVEDONE'
    wave = Node(b'Wave', [
        anim_map("Joint map", "spine", "arm", anim_channel(4, 0.2, 1, f32s([0.0, 0.8, 1.2, 0.4]))),
        # The loader drops event maps (LoadAnimScriptMaps adds tracks for the other maps only);
        # a joint map without names whose channel holds events binds to the instance's sink.
        anim_map("Event map", None, None, anim_channel(3, -1.0, 8, events)),
        anim_map("Joint map E", None, None, anim_channel(3, -1.0, 8, events)),
    ])
    nod = Node(b'Nod', [
        anim_map("Joint map 0", "neck", "head", anim_channel(3, 0.3, 4, rows([
            quat("x", 0.0), quat("x", 0.3), quat("x", 0.1)]))),
        anim_map("Joint map 1", "spine", "neck", anim_channel(2, 0.6, 4, rows([
            quat("y", 0.0), quat("y", 0.2)]))),
        anim_map("Joint map 2", "pelvis", "spine", anim_channel(2, 0.6, 4, rows([
            quat("z", 0.0), quat("z", 0.1)]))),
        anim_map("Object map", "pelvis", None, anim_channel(2, 0.6, 6, rows([
            [0.0, 0.0, 0.0] + quat("y", 0.0), [0.0, 0.1, 0.2] + quat("y", 0.25)]))),
    ])
    return utf([Node(b'Animation', [Node(b'Script', [walk, wave, nod])])])


def play(anim, time=0.0, blend=0.0, speed=1.0, scale=1.0, flags=0, twist=-1.0):
    return "play %s %s %s %s %s %x %s" % (anim, h(time), h(blend), h(speed), h(scale), flags, h(twist))


def walks(*dts):
    return ["walk " + h(dt) for dt in dts]


def model():
    return ["model deformable\\skel.cmp deformable\\moves.anm 5c", "walk " + h(0.0)]


def case_anim_loop():
    # A looping walk: the root's object channel wraps (OnLoop resets its height and heading), the
    # caller's sink hears it; then the list is dropped.
    ops = model() + ["sink", play("Walk", flags=2)]
    ops += walks(0.1, 0.2, 0.2, 0.2, 0.15, 0.3, 0.4)
    ops += ["count", "destroyall", "count"] + walks(0.1)
    return ops


def case_anim_once():
    # A walk from 0.3 s with the root data's motion and a twist, run out: the sweep drops it and
    # the resting height follows the root.
    ops = model() + ["sink", play("Walk", time=0.3, speed=1.5, flags=0, twist=0.5)]
    ops += walks(0.1, 0.1, 0.2, 0.2) + ["count"]
    ops += [play("Walk", time=0.0, scale=0.5, flags=0)] + walks(0.25, 0.25, 0.3) + ["count"]
    return ops


def case_anim_keys():
    # The wave's event keys reach the caller's sink with the animation's name, while a walk blends
    # in over it; a second wave without a sink.
    ops = model() + ["sink", play("Walk", flags=2), "walk " + h(0.1),
                     play("Wave", blend=0.2, flags=0)]
    ops += walks(0.05, 0.1, 0.1, 0.1, 0.2, 0.2) + ["count"]
    ops += ["nosink", play("Wave", flags=0)] + walks(0.2, 0.2) + ["count", "destroyall"]
    return ops


def case_anim_chain():
    # PlayInstanceAnimChain moves the playback so that the head starts where it stands.
    ops = model() + [play("Walk", flags=2)] + walks(0.2, 0.1)
    ops += ["chain Nod %s %s %s %s 0 %s 3" % (h(0.0), h(0.0), h(1.0), h(1.0), h(-1.0))]
    ops += walks(0.1, 0.2, 0.2, 0.2) + ["count"]
    ops += ["chain Nod %s %s %s %s 2 %s 3" % (h(0.3), h(0.1), h(1.0), h(1.0), h(0.4))]
    ops += walks(0.1, 0.3) + ["count", "destroyall"]
    return ops


def case_anim_aim_chain():
    # An aim of the head through the neck and spine (the damped-least-squares solver), its target
    # moved, then stopped and swept; a second aim on a joint the first holds is blocked.
    ops = model()
    target = [0.4, 1.6, 0.6]
    ops.append("aim 0 3 21 %s %s %s - 0" % (hs(target), hs(IDENT9), h(0.05)))
    ops += walks(0.1, 0.1)
    ops.append("aim 1 3 21 %s %s %s - 0" % (hs(target), hs(IDENT9), h(0.05)))
    ops.append("target 0 %s %s" % (hs([-0.3, 1.4, 0.5]), hs(IDENT9)))
    ops += walks(0.1, 0.1)
    ops += ["stopaim 0"] + walks(0.1) + ["count"] + walks(0.1) + ["count"]
    return ops


def case_anim_aim_single():
    # A look of the head alone (the single-joint solver with the Euler clamp) over a looping walk,
    # with a blend in; a point aim of the neck through the spine, the neck locked.
    ops = model() + [play("Walk", flags=2)] + walks(0.1)
    orient = mul(rot("y", 0.5), rot("x", 0.2))
    ops.append("aim 2 3 5 %s %s %s - %s" % (hs([0.5, 2.0, 1.0]), hs(orient), h(0.1), h(0.2)))
    ops += walks(0.1, 0.1, 0.2)
    ops.append("target 0 %s %s" % (hs([-1.0, 1.0, 0.5]), hs(orient)))
    ops += walks(0.1)
    ops += ["stopaim 0"] + walks(0.1, 0.1) + ["count"]
    ops.append("aim 0 2 21 %s %s %s 2 0" % (hs([0.3, 1.0, 0.4]), hs(orient), h(0.1)))
    ops += walks(0.1, 0.1)
    ops += ["stopaim 1"] + walks(0.1, 0.1) + ["count", "destroyall"]
    return ops


def anim_list():
    return [" ; ".join(case()) for case in (case_anim_loop, case_anim_once, case_anim_keys,
                                            case_anim_chain, case_anim_aim_chain,
                                            case_anim_aim_single)]


def write_bytes(relative, data, check):
    path = os.path.join(HERE, relative)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != data:
                sys.exit("%s is out of date; run make_deformable_fixtures.py" % relative)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as out:
        out.write(data)

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
    write("deformable_parts.list", listing("deformable parts", parts_list()), check)
    write("deformable_anim.list", listing("deformable animation", anim_list()), check)
    for name, build in ARCHIVES.items():
        write_bytes(os.path.join("deformable", name), build(), check)
    for name, build in (('skel.cmp', skel_cmp), ('moves.anm', moves_anm)):
        write_bytes(os.path.join("deformable", name), build(), check)


if __name__ == "__main__":
    main()
