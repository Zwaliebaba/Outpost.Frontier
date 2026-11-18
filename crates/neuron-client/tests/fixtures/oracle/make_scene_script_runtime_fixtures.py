"""Writes the inputs of the P4.14 run-time suites: scene_script_curve.list,
scene_script_spline.list and scene_script_playback.list (Oracle.exe
neuron-client, CppSource/Oracle/oracle_scene_script_runtime.cpp, and
crates/neuron-client/tests/oracle_vectors.rs). Run from any folder:

    python make_scene_script_runtime_fixtures.py

Curve lines: a factory name, then operations: a<t>,<v>,<e0>,<e1> (AddKey),
r<id>,<t>,<v>,<e0>,<e1> (Replace), d<id> (DeleteKey), e<t> (Evaluate),
w<a>,<b> (WrapEvaluate, decimal), s (the keys). Words are f32 bits in hex.

Spline lines: p (CV_CRSplinePath) or o (CV_CROrientationSplinePath), then
operations: P<words> (LoadPoints), L<hex text> or L! (Load, null text),
C<0|1> (closed), S<i>,<xyz> (point), Q<i>,<wxyz> (orientation), E<t>, K<t>,
O<t> (position, tangent, orientation), T<t>,<12 words> (transform from an
initial record), N (length), G<i> (the getters), Z<size> (Save), I (counts).

Playback lines: attachments ("-" or handle:mask,...), a program
(s<arg> start, t<dt> tick, x stop, p pause, l<0|1> loop, d<h> detach,
a<h>:<mask> attach, r<h> remove entity, P<x>/<y>/<z> and R<9 words> the
scene frame), then a compiled script in hex. Masks are the interface bits
of neuron_client::scene_script::playback::interface.

The inputs stay clear of what the reference leaves undefined: Hermite curves
are not evaluated past a NaN key (divergence D245), path texts end after a
separator (D246), and no event reads an unwritten local (D248).
"""

import os
import random
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_scene_script_build_fixtures import G, IDENT, Add, ent, ev, script  # noqa: E402


def w(x):
    """The f32 bits of x."""
    if isinstance(x, str):
        return x
    return "%08x" % struct.unpack("<I", struct.pack("<f", x))[0]


def ws(*xs):
    return ",".join(w(x) for x in xs)


NAN = "7fc00000"

CURVES = [
    "CatmullRomPCurve",
    "SceneScriptParamCurve",
    "LinearPCurve",
    "SceneScriptLPCurve",
    "SmoothPCurve",
    "StepPCurve",
    "RampUpPCurve",
    "RampDownPCurve",
    "BumpOutPCurve",
    "BumpInPCurve",
    "FreeFormPCurve",
]

TIMES = [0, 0.1, 0.25, 0.3, 0.5, 0.6, 0.75, 0.9, 0.999, 1, -0.1, 1.5]


def curve_cases():
    cases = []
    evals = ["e" + w(t) for t in TIMES]
    for name in CURVES:
        cases.append([name] + evals + ["s"])
        ops = [
            "a" + ws(0.25, 0.8, 1.5, -0.5),
            "a" + ws(0.5, 0.3, 0, 2),
            "a" + ws(0.75, 1.2, 0.25, 0.25),
            "s",
        ]
        ops += evals
        ops += ["w1250,1000", "w-250,1000", "w3000,1000", "w7,3"]
        ops += [
            "r0," + ws(0.5, 2, 0, 0),
            "r1," + ws(0.5, -1, 0, 0),
            "r2," + ws(1, 0, 0, 0),
            "r2," + ws(-0.5, 0, 0, 0),
            "r3," + ws(1.5, 0, 0, 0),
            "r3," + ws(0.4, 0.4, 1, 1),
            "r9," + ws(0.4, 0, 0, 0),
            "s",
        ]
        ops += evals
        ops += [
            "d3",
            "d0",
            "d9",
            "a" + ws(0, 0.5, 0, 0),
            "a" + ws(1, 0.5, 0, 0),
            "a" + ws(-0.5, 0, 0, 0),
            "a" + ws(1.5, 0, 0, 0),
            "s",
        ]
        ops += evals
        cases.append([name] + ops)
    # The free-form keys the shipped scripts give, and a curve that falls.
    cases.append(["FreeFormPCurve", "a" + ws(0, 0, 0, 0.241379), "a" + ws(0.5, 0.7, 1.2, 1.2),
                  "a" + ws(1, 1, 0.1, 0), "s"] + evals)
    for name in ("SceneScriptParamCurve", "SceneScriptLPCurve", "BumpOutPCurve", "BumpInPCurve"):
        cases.append([name, "a" + ws(0.2, 0.9, 0, 0), "a" + ws(0.4, 0.1, 0, 0), "a" + ws(0.6, -0.5, 0, 0),
                      "a" + ws(0.8, 0.5, 0, 0), "s"] + evals)
    # More than sixteen keys, out of order, some at equal times: the merge
    # sort's buckets and its stability, and the knot array's growth.
    rng = random.Random(14)
    many = []
    for i in range(40):
        t = rng.choice([0.5, 0.125, 0.875]) if i % 7 == 0 else round(rng.uniform(0.01, 0.99), 3)
        many.append("a" + ws(t, round(rng.uniform(-2, 2), 3), round(rng.uniform(-1, 1), 3),
                             round(rng.uniform(-1, 1), 3)))
    for name in ("CatmullRomPCurve", "LinearPCurve", "SceneScriptLPCurve", "FreeFormPCurve", "StepPCurve",
                 "SmoothPCurve"):
        cases.append([name] + many + ["s", "d5", "d17", "s"] + evals + ["e" + w(i / 64) for i in range(65)])
    # NaN keys: the list walks of the Catmull-Rom and linear curves, the
    # Hermite knots below the NaN key, and NaN times for the first two.
    for name in ("CatmullRomPCurve", "SceneScriptLPCurve"):
        cases.append([name, "a" + NAN + "," + ws(5, 0, 0), "a" + ws(0.5, 2, 0, 0), "s", "e" + NAN] + evals)
    cases.append(["FreeFormPCurve", "a" + ws(0.5, 2, 1, 1), "a" + NAN + "," + ws(5, 0, 0), "s"]
                 + ["e" + w(t) for t in (0, 0.1, 0.25, 0.5, 0.6, 0.9, -0.1)])
    cases.append(["SmoothPCurve", "a" + ws(0.3, 1, 0, 0), "r2," + NAN + "," + ws(2, 0, 0), "s"]
                 + ["e" + w(t) for t in (0, 0.2, 0.5, -1)])
    # A name SceneScript.dll does not register.
    cases.append(["NoSuchCurve", "e" + w(0.5)])
    return [" ".join(c) for c in cases]


def text(s):
    return "L" + s.encode().hex()


TS = [0, -0.5, 0.05, 0.1, 0.25, 0.3, 0.5, 0.5001, 0.77, 0.99, 1, 2]
ROT = [0.8, -0.6, 0, 0.6, 0.8, 0, 0, 0, 1]


def evaluations(orientation_ops=True):
    ops = []
    for t in TS:
        ops += ["E" + w(t), "K" + w(t)]
        if orientation_ops:
            ops.append("O" + w(t))
    ops += ["T" + ws(0.4, *IDENT_WORDS), "T" + ws(0.4, *ROT, 1, 2, 3), "T" + ws(0, *ROT, 0, 0, 0), "N", "I"]
    return ops


IDENT_WORDS = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]

PATH_TEXT = (
    "OPEN, {-1.234567,0.500000,10.000000}, {1.000000,0.000000,0.000000,0.000000}, "
    "{2.500000,1.000000,12.250000}, {0.923880,0.000000,0.382683,0.000000}, "
    "{6.000000,0.000000,15.000000}, {0.707107,0.000000,0.707107,0.000000}, "
    "{9.000000,-2.000000,20.000000}, {0.500000,0.500000,0.500000,0.500000}, "
)


def spline_cases():
    cases = []
    four = ws(0, 0, 0, 1, 0.5, 2, 3, 0, 4, 6, 1, 2)
    getters = ["G%d" % i for i in range(6)]
    cases.append(["p", "I", "N", "E" + w(0.5), "P" + four] + evaluations() + getters
                 + ["C1"] + evaluations() + getters
                 + ["S1," + ws(1, 1, 1), "S9," + ws(0, 0, 0)] + evaluations(False)
                 + ["Z1000", "Z10", "I"] + evaluations(False) + getters)
    cases.append(["o", text(PATH_TEXT)] + evaluations() + getters
                 + ["Q1," + ws(0, 0, 1, 0), "Q7," + ws(1, 0, 0, 0)] + evaluations() + ["C1"] + evaluations()
                 + getters + ["Z2000", "I"] + evaluations())
    # One point: the end points only.
    cases.append(["p", "P" + ws(1, 2, 3), "I", "E" + w(0), "E" + w(1), "E" + w(0.5), "K" + w(0.5),
                  "O" + w(0.5), "N", "G0", "G1"])
    # Three points with orientations set one by one.
    cases.append(["o", "P" + ws(0, 0, 0, 10, 0, 0, 10, 10, 0), "I", "Q0," + ws(1, 0, 0, 0),
                  "Q1," + ws(0.707107, 0, 0.707107, 0), "Q2," + ws(0, 0, 1, 0), "Q3," + ws(1, 0, 0, 0)]
                 + evaluations() + getters + ["C1", "Q0," + ws(0.5, 0.5, 0.5, 0.5)] + evaluations() + getters)
    # Texts: closed, without quaternions, numbers the reader cuts short,
    # too few points, an early brace, a null pointer.
    closed = "CLOSED, {0,0,0}, {1,0,0,0}, {10,0,0}, {1,0,0,0}, {10,10,0}, {0.7071,0,0,0.7071}, "
    cases.append(["o", text(closed)] + evaluations() + getters + ["Z500"])
    cases.append(["o", text("OPEN, {0,0,0}, {5,0,0}, {5,5,5}, ")] + evaluations() + getters)
    cases.append(["p", text("OPEN, {1e2,0,0}, {-.5,3.,-0}, {0x10,7,8}, ")] + evaluations(False) + getters)
    cases.append(["p", text("OPEN, {1,2,3}, "), "I", text("{1,2}, {3,4,5}, {6,7,8}, "), "I", "N", "L!", "I",
                  text(""), "I", text("OPEN, {1,2,3}, {4,5,6}, "), "I", "N"] + evaluations(False))
    cases.append(["o", text("OPEN, {0,0,0}, {1,0,0,0}, {4,0,0}, {0.5,0.5}, {8,0,0}, ")] + evaluations()
                 + getters)
    return [" ".join(c) for c in cases]


# ------------------------------------------------------------------- playback

GEO, CAMERA, COMPOUND, IK = 0x1, 0x2, 0x4, 0x8
LIGHT, D3D, SOUND, USER = 0x10, 0x20, 0x40, 0x80
SUBTITLE, FOG, REVERB, PSYS, PATH, SCRIPT = 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000
ALL = 0x3fff

CURVE = {"CLSID": "FreeFormPCurve", "points": [[0, 0, 0, 0.241379], [0.5, 0.7, 1.2, 1.2], [1, 1, 0.1, 0]]}

ROT90 = [[0, -1, 0], [1, 0, 0], [0, 0, 1]]


def sp(pos, orient=IDENT):
    return {"spatialprops": {"pos": pos, "orient": orient}}


def full_scene():
    ents = [
        ent("Scene_1", "SCENE", ambient=[20, 30, 40], up=G("Y_AXIS"), front=G("NEG_Z_AXIS"), fogon=1,
            fogcolor=[100, 110, 120], fogmode=G("F_EXP2"), fogstart=10, fogend=400, fogdensity=0.02),
        ent("Ship", "COMPOUND", compoundprops={"floor_height": 1.5}, **sp([10, 0, 5], ROT90)),
        ent("Char", "DEFORMABLE", compoundprops={"floor_height": -0.5}, **sp([1, 2, 3])),
        ent("Cam", "CAMERA", cameraprops={"fovh": 35, "hvaspect": 1.6, "nearplane": 0.5, "farplane": 9000},
            **sp([0, 5, -20])),
        ent("Mon", "MONITOR", **sp([0, 0, 0])),
        ent("Snd", "SOUND", audioprops={"attenuation": -6, "pan": 0.25, "dmin": 5, "dmax": 800, "ain": 90,
                                         "aout": 180, "atout": -2, "rmix": 0.5}, **sp([3, 0, 0])),
        ent("Lamp", "LIGHT", lightprops={"type": G("L_POINT"), "color": [200, 150, 100], "range": 300,
                                          "diffuse": [0.8, 0.6, 0.4], "direction": [0, -1, 0], "atten": [1, 0, 0],
                                          "cutoff": 30, "theta": 20, "on": 1}, **sp([0, 10, 0])),
        ent("Fx", "PSYS", psysprops={"sparam": 0.25}, **sp([5, 5, 5])),
        ent("Mark", "MARKER", **sp([-4, 1, 2])),
        ent("Path_1", "MOTION_PATH", pathprops={"path_type": "CV_CROrientationSplinePath",
                                                "path_data": PATH_TEXT}, **sp([0, 0, 0])),
        ent("Lamp2", "LIGHT", lightprops={"color": [10, 20, 30], "range": 50, "cutoff": 45}, **sp([2, 2, 2])),
        ent("Fx2", "PSYS", psysprops={"sparam": 0.9}),
        ent("Snd2", "SOUND", audioprops={"attenuation": -20, "pan": -0.5, "ain": 10, "aout": 300}),
        ent("Cam2", "CAMERA", cameraprops={"fovh": 60, "hvaspect": 2}),
        ent("Sub", "SUB_SCENE"),
    ]
    evs = [
        ev(0, "SET_CAMERA", ["Mon", "Cam"]),
        ev(0, "START_MOTION", ["Char"], {"animation": "Sc_Walk", "duration": 1.2, "trans_time": 0.25}),
        ev(0.1, "START_SOUND", ["Snd"], {"duration": 0.6, "start_time": 0.05}),
        ev(0.1, "START_PSYS", ["Fx"], {"duration": 0.9}),
        ev(0.15, "START_LIGHT_PROP_ANIM", ["Lamp"], {"duration": 0.5, "lightprops": {
            "color": [255, 51, 0], "range": 80, "diffuse": [0.1, 0.2, 0.3], "on": 0, "atten": [0.5, 0.25, 0],
            "direction": [1, 0, 0], "theta": 10, "cutoff": 40, "specular": [1, 1, 1], "ambient": [0.2, 0.2, 0.2],
            "type": 3}}),
        ev(0.2, "START_LIGHT_PROP_ANIM", ["Scene_1", "Lamp2"], {"duration": 0.4, "lightprops": {
            "color": [5, 6, 7]}}),
        ev(0.2, "START_LIGHT_PROP_ANIM", ["Lamp", "Lamp2"], {"duration": 0.3, "param_curve": CURVE,
                                                              "lightprops": {"color": [0, 0, 0], "range": 10}}),
        ev(0.25, "START_CAMERA_PROP_ANIM", ["Cam"], {"duration": 0.5, "cameraprops": {"fovh": 50, "aspect": 1.5,
                                                                                    "near": 1, "far": 100}}),
        ev(0.3, "START_CAMERA_PROP_ANIM", ["Cam", "Cam2"], {"duration": 0.2, "cameraprops": {"fovh": 10}}),
        ev(0.3, "START_SPATIAL_PROP_ANIM", ["Mark", "Ship"], {"duration": 0.6, "spatialprops": {
            "pos": [1, 2, 3], "orient": ROT90}, "param_curve": CURVE}),
        ev(0.35, "START_SPATIAL_PROP_ANIM", ["Char"], {"duration": 0.4, "spatialprops": {
            "axisrot": [90, G("Y_AXIS")], "pos": [0, 0, 1], "q_orient": [0.5, 0.5, 0.5, 0.5]}}),
        ev(0.35, "START_SPATIAL_PROP_ANIM", ["Fx2", "Char"], {"duration": 0.3, "target_part": "Head",
                                                              "target_type": 2, "spatialprops": {"pos": [0, 0, 0]}}),
        ev(0.4, "ATTACH_ENTITY", ["Mark", "Ship"], {"duration": 0.3, "offset": [0, 1, 2],
                                                    "flags": Add(G("POSITION"), G("ORIENTATION"))}),
        ev(0.4, "ATTACH_ENTITY", ["Fx", "Char"], {"duration": 0.3, "offset": [1, 0, 0], "target_part": "Hand",
                                                  "target_type": 1, "flags": Add(G("POSITION"), G("ENTITY_RELATIVE"))}),
        ev(0.45, "ATTACH_ENTITY", ["Snd", "Ship"], {"duration": 0.3, "offset": [0, 0, 5], "up": 1, "front": 5,
                                                    "flags": Add(G("POSITION"), G("LOOK_AT"))}),
        ev(0.45, "ATTACH_ENTITY", ["Lamp2", "Char"], {"duration": 0.2, "flags": Add(Add(
            G("POSITION"), G("ORIENTATION")), G("PARENT_CHILD"))}),
        ev(0.45, "ATTACH_ENTITY", ["Cam2", "Ship"], {"duration": 0.2, "offset": [3, 0, 0],
                                                     "flags": Add(Add(Add(G("POSITION"), G("ORIENTATION")),
                                                                      G("ENTITY_RELATIVE")),
                                                                  G("ORIENTATION_RELATIVE"))}),
        ev(0.5, "START_PATH_ANIMATION", ["Ship", "Path_1"], {"duration": 0.5, "start_percent": 0.1,
                                                             "stop_percent": 0.9, "offset": [0, 1, 0],
                                                             "flags": Add(G("POSITION"), G("ORIENTATION"))}),
        ev(0.5, "START_PATH_ANIMATION", ["Mark", "Path_1"], {"duration": 0.4, "param_curve": CURVE,
                                                             "flags": Add(Add(G("POSITION"), G("ORIENTATION")),
                                                                          G("LOOK_AT")), "up": 1, "front": 5}),
        ev(0.55, "CONNECT_HARDPOINTS", ["Ship", "Char"], {"duration": 0.2, "hardpoint": "HpMount",
                                                          "parent_hardpoint": "HpRoot"}),
        ev(0.6, "START_IK", ["Char", "Ship"], {"end_effector": "Hand", "target_part": "Gun", "target_type": 1,
                                               "duration": 0.3, "up": 1, "front": 2, "damping": 0.5}),
        ev(0.6, "START_SUB_SCENE", ["Sub"], {"start_time": 1.5, "duration": 0.2}),
        ev(0.65, "START_PSYS_PROP_ANIM", ["Fx"], {"duration": 0.3, "psysprops": {"sparam": 0.75}}),
        ev(0.65, "START_PSYS_PROP_ANIM", ["Fx", "Fx2"], {"duration": 0.2, "psysprops": {"sparam": 0.1}}),
        ev(0.7, "START_AUDIO_PROP_ANIM", ["Snd"], {"duration": 0.3, "audioprops": {
            "attenuation": -30, "pan": -1, "dmin": 1, "dmax": 100, "ain": 45, "aout": 360, "atout": 0,
            "rmix": 1}}),
        ev(0.7, "START_AUDIO_PROP_ANIM", ["Snd", "Snd2"], {"duration": 0.2, "audioprops": {"pan": 1, "ain": 0}}),
        ev(0.75, "START_FOG_PROP_ANIM", ["Scene_1"], {"duration": 0.2, "fogprops": {
            "fogon": 0, "fogcolor": [1, 2, 3], "fogmode": 1, "fogstart": 20, "fogend": 500, "fogdensity": 0.5,
            "fogtable": 1}}),
        ev(0.8, "START_REVERB_PROP_ANIM", ["Scene_1"], {"duration": 0.2, "reverbprops": {
            "env": 12, "vol": 0.25, "decay": 2.5, "damping": 0.5}}),
        ev(0.8, "START_FLR_HEIGHT_ANIM", ["Char", "Mark"], {"duration": 0.2}),
        ev(0.85, "START_FLR_HEIGHT_ANIM", ["Char", "Ship"], {"duration": 0.1, "target_part": "Deck",
                                                             "target_type": G("PART")}),
        ev(0.9, "SUBTITLE", ["Scene_1"], {"strid": 65538, "duration": 0.05}),
        ev(0.9, "USER_EVENT", ["Ship"], {"duration": 0.05, "user_event_string": "hello"}),
        ev(0.95, "START_MOTION", ["Ship"], {"animation": "Sc_Idle", "flags": 1}),
        ev(1.0, "START_PSYS", ["Fx2"], {"duration": 0.1, "flags": 1}),
    ]
    return script(duration=1.1, entities=ents, events=evs)


def attach_all(n, mask=ALL, skip=()):
    return ",".join("%d:%x" % (h, mask) for h in range(n) if h not in skip)


def ticks(*dts):
    return ["t%d" % d for d in dts]


def playback_cases():
    cases = []
    scene = full_scene()
    run = ["s0"] + ticks(100, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 100)
    # Everything attached except the path (its own spline drives the path events).
    cases.append((attach_all(15, skip=(9,)), run + ["x", "s0"] + ticks(300, 400, 500)))
    # Nothing attached: the timeline runs on data.
    cases.append(("-", run))
    # The path answered by the engine object; a few interfaces only.
    cases.append(("0:%x,1:%x,2:%x,5:%x,6:%x,8:1,9:%x,10:%x" % (ALL, GEO | COMPOUND, GEO | COMPOUND | IK,
                                                             SOUND, LIGHT | GEO, GEO | PATH, D3D),
                  run))
    # Pause, resume, loop, a step back, detach, remove, the scene frame.
    cases.append((attach_all(15, skip=(9,)), ["s0", "t120", "p", "t50", "s30", "t100", "l1", "t700", "t200",
                                              "t-50", "t300", "d1", "t100", "a1:5", "t50", "r2", "t100",
                                              "P" + "/".join(w(x) for x in (100, 0, -50)), "t50",
                                              "R" + "/".join(w(x) for x in (0, 0, 1, 0, 1, 0, -1, 0, 0)),
                                              "t50", "l0", "t2000", "s0", "t1500"]))
    # One monitor and one camera without SET_CAMERA: the default event.
    two = script(duration=0.5, entities=[ent("Cam", "CAMERA", cameraprops={"fovh": 20}, **sp([1, 2, 3])),
                                         ent("Mon", "MONITOR"), ent("Mark", "MARKER")],
                 events=[ev(0.1, "START_CAMERA_PROP_ANIM", ["Cam"], {"duration": 0.2,
                                                                    "cameraprops": {"fovh": 80}})])
    cases.append(("0:%x,1:%x" % (GEO | CAMERA, GEO | CAMERA), ["s0", "t50", "t100", "t100", "t400"]))
    cases[-1] = cases[-1] + (two,)
    # More than sixteen events at one start on one entity, and in the master
    # list: the partitions of both sorts.
    kinds = ["START_SOUND", "START_AUDIO_PROP_ANIM", "USER_EVENT", "SUBTITLE"]
    many = [ev(0.1 if i % 3 else 0.2, kinds[i % 4], ["Snd"], {"duration": 0.05 * (1 + i % 5),
                                                               "user_event_string": "u%d" % i})
            for i in range(40)]
    many += [ev(0.1, "START_MOTION", ["Ship"], {"animation": "A%d" % i, "duration": 0.1}) for i in range(20)]
    crowd = script(duration=0.6, entities=[ent("Snd", "SOUND"), ent("Ship", "COMPOUND")], events=many)
    cases.append(("0:%x,1:%x" % (ALL, ALL), ["s0", "t100", "t50", "t60", "t200"], crowd))
    out = []
    for case in cases:
        attach, program = case[0], case[1]
        chunk = case[2] if len(case) > 2 else scene
        out.append("%s %s %s" % (attach, ",".join(program), chunk.hex()))
    return out


def write(name, header, lines):
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, name), "w", newline="\n") as f:
        f.write(header)
        for line in lines:
            f.write(line + "\n")


def main():
    write("scene_script_curve.list",
          "# P4.14 param-curve oracle inputs, written by make_scene_script_runtime_fixtures.py.\n",
          curve_cases())
    write("scene_script_spline.list",
          "# P4.14 spline-path oracle inputs, written by make_scene_script_runtime_fixtures.py.\n",
          spline_cases())
    write("scene_script_playback.list",
          "# P4.14 playback oracle inputs, written by make_scene_script_runtime_fixtures.py.\n",
          playback_cases())


if __name__ == "__main__":
    main()
