"""Writes scene_script_save.list, the inputs of the P4.14 save suite
(Oracle.exe neuron-client, CppSource/Oracle/oracle_scene_script_save.cpp,
and crates/neuron-client/tests/oracle_vectors.rs). Run from any folder:

    python make_scene_script_save_fixtures.py [--check]

--check compares the list with what the script would write and fails when
they differ.

Each line is one case on a fresh script engine and module state: a
program, then compiled scripts in hex (buffer i is script<i>.thn to the
engine's path loads). The program's tokens, comma-separated:
c<k> (Slot40SetCom: 0 none, 1 the context that hands the text back, 2 the
one that asks for 4 bytes and fails), L<i> or L- (Slot4Load from buffer i
or from no file system), l<i>, lx or l- (Slot43LoadPath: script<i>.thn,
missing.thn, a null path), S, s or N (Slot5Save to a writer that takes
every byte, one that takes half, or no file system), W (Slot44SavePath
"saved.thn"), t<dt> (Slot26Tick), g<arg> (Slot22Start), D<ms>
(Slot18SetDuration), u<h>:<key hex>:<value hex> (Slot34SetUserProp),
r<h> (Slot36RemoveEntity), e<i> (Slot39RemoveEvent of the i-th event in
master order; ids are the module's, D249), P<x>/<y>/<z> and
O<9 words> (the engine's SetPosition and SetOrientation).

The inputs stay clear of what the reference leaves undefined (divergences
D540 to D545): axes are 0 to 5 (no SUB_SCENE entity, every SCENE gives up
and front), no AUDIO_PROP_ANIM event with a secondary entity sets ain or
aout, no path object is saved without points, no string value formats to
509 to 511 bytes, no context leaves its buffer unterminated, every save to
a path gets a file system, no number is NaN (the C runtimes spell it
differently), and no load walks an earlier script's events whose entities
the engine has cleared (the parser faults there, D240).
"""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_scene_script_build_fixtures import G, IDENT, Add, ent, ev, script  # noqa: E402
from make_scene_script_runtime_fixtures import CURVE, PATH_TEXT, ROT90, sp, w  # noqa: E402

CLOSED_TEXT = "CLOSED, {0,0,0}, {10,0,0}, {10,10,0}, {0,10,5}, "


def main_scene():
    """Every entity type but SUB_SCENE, and every event type."""
    ents = [
        ent("Scene_1", "SCENE", ambient=[20, 30, 40], up=G("Y_AXIS"), front=G("NEG_Z_AXIS"), fogon=1,
            fogcolor=[100, 110, 120], fogmode=G("F_EXP2"), fogstart=10, fogend=400, fogdensity=0.02),
        ent("Ship", "COMPOUND", template_name="ge_fighter", compoundprops={"floor_height": 1.5},
            lt_grp=3, srt_grp=200, usr_flg=7, userprops={"Faction": "li_n_grp", "Rank": 4},
            **sp([10, 0, 5], ROT90)),
        ent("Char", "DEFORMABLE", compoundprops={"floor_height": -0.5}, **sp([1, 2, 3])),
        ent("Cam", "CAMERA", cameraprops={"fovh": 35, "hvaspect": 1.6, "nearplane": 0.5, "farplane": 9000},
            **sp([0, 5, -20])),
        ent("Mon", "MONITOR", **sp([0, 0, 0])),
        ent("Snd", "SOUND", audioprops={"attenuation": -6, "pan": 0.25, "dmin": 5, "dmax": 800, "ain": 90,
                                         "aout": 180, "atout": -2, "rmix": 0.5}, **sp([3, 0, 0])),
        ent("Lamp", "LIGHT", lightprops={"type": G("L_POINT"), "color": [200, 150, 100], "range": 300,
                                          "diffuse": [0.8, 0.6, 0.4], "direction": [0, -1, 0], "atten": [1, 0, 0],
                                          "cutoff": 30, "theta": 20, "on": 1, "specular": [0.1, 0.2, 0.3],
                                          "ambient": [0.05, 0.05, 0.05]}, **sp([0, 10, 0])),
        ent("Fx", "PSYS", psysprops={"sparam": 0.25}, **sp([5, 5, 5])),
        ent("Mark", "MARKER", **sp([-4, 1, 2])),
        ent("Path_1", "MOTION_PATH", pathprops={"path_type": "CV_CROrientationSplinePath",
                                                "path_data": PATH_TEXT}, **sp([0, 0, 0])),
        ent("Path_2", "MOTION_PATH", pathprops={"path_type": "CV_CRSplinePath", "path_data": CLOSED_TEXT}),
        ent("Path_3", "MOTION_PATH", pathprops={"control_pts": [[1, 2, 3], [4, 5, 6], [7, 8, 9.5]]}),
        ent("Path_4", "MOTION_PATH", pathprops={"path_type": "NoSuchPath"}),
        ent("Path_5", "MOTION_PATH"),
        ent("Lamp2", "LIGHT", lightprops={"color": [10, 20, 30], "range": 50, "cutoff": 45, "type": 3},
            **sp([2, 2, 2])),
        ent("Hp", "HARDPOINT", **sp([1, 1, 1])),
        ent("Snd2", "SOUND", audioprops={"attenuation": -20, "pan": -0.5, "ain": 10, "aout": 300}),
        ent("Cam2", "CAMERA", cameraprops={"fovh": 60, "hvaspect": 2}),
    ]
    evs = [
        ev(0, "SET_CAMERA", ["Mon", "Cam"]),
        ev(0, "START_MOTION", ["Char"], {"animation": "Sc_Walk", "duration": 1.2, "trans_time": 0.25,
                                         "locked_bone": "Root", "start_time": 0.5, "time_scale": 1.5,
                                         "weight": 0.75, "heading": 90}),
        ev(0.1, "START_SOUND", ["Snd"], {"duration": 0.6, "start_time": 0.05, "flags": Add(G("SPATIAL"), G("LOOP"))}),
        ev(0.1, "START_PSYS", ["Fx"], {"duration": 0.9, "event_flags": 3}),
        ev(0.15, "START_LIGHT_PROP_ANIM", ["Lamp"], {"duration": 0.5, "lightprops": {
            "color": [255, 51, 0], "range": 80, "diffuse": [0.1, 0.2, 0.3], "on": 0, "atten": [0.5, 0.25, 0],
            "direction": [1, 0, 0], "theta": 10, "cutoff": 40, "specular": [1, 1, 1], "ambient": [0.2, 0.2, 0.2],
            "type": 3}}),
        ev(0.2, "START_LIGHT_PROP_ANIM", ["Lamp", "Lamp2"], {"duration": 0.3, "param_curve": CURVE,
                                                              "pcurve_period": 250,
                                                              "lightprops": {"color": [0, 0, 0], "range": 10}}),
        ev(0.25, "START_CAMERA_PROP_ANIM", ["Cam"], {"duration": 0.5, "event_flags": 2, "cameraprops": {
            "fovh": 50, "aspect": 1.5, "near": 1, "far": 100}}),
        ev(0.3, "START_CAMERA_PROP_ANIM", ["Cam", "Cam2"], {"cameraprops": {"near": 10, "far": 20}}),
        ev(0.3, "START_SPATIAL_PROP_ANIM", ["Mark", "Ship"], {"duration": 0.6, "spatialprops": {
            "pos": [1, 2, 3], "orient": ROT90}, "param_curve": CURVE}),
        ev(0.35, "START_SPATIAL_PROP_ANIM", ["Char"], {"duration": 0.4, "spatialprops": {
            "axisrot": [90, G("Y_AXIS")], "pos": [0, 0, 1], "q_orient": [0.5, 0.5, 0.5, 0.5]}}),
        ev(0.35, "START_SPATIAL_PROP_ANIM", ["Fx", "Char"], {"duration": 0.3, "target_part": "Head",
                                                             "target_type": 2, "event_flags": 1,
                                                             "spatialprops": {"orient": IDENT,
                                                                              "axisrot": [45, G("NEG_X_AXIS")]}}),
        ev(0.4, "ATTACH_ENTITY", ["Mark", "Ship"], {"duration": 0.3, "offset": [0, 1, 2],
                                                    "flags": Add(G("POSITION"), G("ORIENTATION"))}),
        ev(0.4, "ATTACH_ENTITY", ["Fx", "Char"], {"duration": 0.3, "offset": [1, 0, 0], "target_part": "Hand",
                                                  "target_type": 1, "flags": Add(G("POSITION"), G("ENTITY_RELATIVE"))}),
        ev(0.45, "ATTACH_ENTITY", ["Lamp2", "Char"], {"duration": 0.2, "event_flags": 5, "flags": Add(Add(Add(
            G("POSITION"), G("ORIENTATION")), G("PARENT_CHILD")), G("ORIENTATION_RELATIVE"))}),
        ev(0.5, "START_PATH_ANIMATION", ["Ship", "Path_1"], {"duration": 0.5, "start_percent": 0.1,
                                                             "stop_percent": 0.9, "offset": [0, 1, 0],
                                                             "flags": Add(G("POSITION"), G("ORIENTATION"))}),
        ev(0.5, "START_PATH_ANIMATION", ["Mark", "Path_2"], {"duration": 0.4, "param_curve": CURVE,
                                                             "flags": Add(Add(G("POSITION"), G("LOOK_AT")),
                                                                          G("ENTITY_RELATIVE")),
                                                             "up": 2, "front": 3}),
        ev(0.55, "CONNECT_HARDPOINTS", ["Ship", "Char"], {"duration": 0.2, "hardpoint": "HpMount",
                                                          "parent_hardpoint": "HpRoot", "flags": 1}),
        ev(0.6, "START_IK", ["Char", "Ship"], {"end_effector": "Hand", "target_part": "Gun", "target_type": 1,
                                               "duration": 0.3, "up": 1, "front": 2, "damping": 0.5,
                                               "count_to_root": 3, "point_at": 1, "move_to": 0,
                                               "transition_duration": 0.125}),
        ev(0.6, "START_SUB_SCENE", ["Scene_1"], {"start_time": 1.5, "duration": 0.2, "flags": 1}),
        ev(0.62, "START_SUB_SCENE", ["Scene_1"], {"start_time": 1.1, "duration": 0.2}),
        ev(0.65, "START_PSYS_PROP_ANIM", ["Fx"], {"duration": 0.3, "psysprops": {"sparam": 0.75}}),
        ev(0.65, "START_PSYS_PROP_ANIM", ["Fx", "Hp"], {"duration": 0.2, "event_flags": 9, "psysprops": {}}),
        ev(0.7, "START_AUDIO_PROP_ANIM", ["Snd"], {"duration": 0.3, "audioprops": {
            "attenuation": -30, "pan": -1, "dmin": 1, "dmax": 100, "ain": 45, "aout": 360, "atout": 0,
            "rmix": 1}}),
        ev(0.7, "START_AUDIO_PROP_ANIM", ["Snd", "Snd2"], {"duration": 0.2, "param_curve": CURVE,
                                                           "audioprops": {"pan": 1, "rmix": 0.5}}),
        ev(0.75, "START_FOG_PROP_ANIM", ["Scene_1"], {"duration": 0.2, "event_flags": 4, "fogprops": {
            "fogon": 0, "fogcolor": [1, 2, 3], "fogmode": 1, "fogstart": 20, "fogend": 500, "fogdensity": 0.5,
            "fogtable": 1}}),
        ev(0.8, "START_REVERB_PROP_ANIM", ["Scene_1"], {"duration": 0.2, "reverbprops": {
            "env": 12, "vol": 0.25, "decay": 2.5, "damping": 0.5}}),
        ev(0.8, "START_FLR_HEIGHT_ANIM", ["Char", "Mark"], {"duration": 0.2, "event_flags": 6}),
        ev(0.85, "START_FLR_HEIGHT_ANIM", ["Char", "Ship"], {"duration": 0.1, "target_part": "Deck",
                                                             "target_type": G("PART"), "floor_height": 2.25}),
        ev(0.9, "SUBTITLE", ["Scene_1"], {"strid": 65538, "duration": 0.05, "event_flags": 8}),
        ev(0.9, "USER_EVENT", ["Ship"], {"duration": 0.05, "user_event_string": "hello"}),
        ev(0.92, "USER_EVENT", ["Char", "Ship"], {"duration": -0.5, "event_flags": 7,
                                                  "user_event_string": 12}),
        ev(0.95, "START_MOTION", ["Ship"], {"animation": "Sc_Idle", "flags": 1, "trans_scale": 2}),
        ev(0.97, "START_MOTION", ["Ship"], {"duration": 0.25}),
        ev(1.0, "START_PSYS", ["Fx"], {"duration": 0.1, "flags": 1}),
        ev(4000.123, "START", ["Snd2"], {"duration": 0.01}),
    ]
    return script(duration=1.1, entities=ents, events=evs)


def flags_scene():
    """Entity flags of each type, escaped and long template names, long
    user properties."""
    long_value = "v" * 600
    exact_value = "w" * 507  # formats to exactly 512 bytes
    exact_key = "k" * 506  # formats to exactly 512 bytes
    ents = [
        ent("C1", "COMPOUND", flags=0x40, template_name="a\\b\r\nc\bd"),
        ent("D1", "DEFORMABLE", flags=0x16, template_name="x" * 1100),
        ent("S1", "SOUND", flags=14 + 0x100, template_name="x" * 1022 + "\\\\"),
        ent("Sc", "SCENE", flags=0x20 + 0x10, up=G("X_AXIS"), front=G("Z_AXIS")),
        ent("M1", "MARKER", flags=1 + 4, userprops={"Long": long_value, "Exact": exact_value,
                                                   exact_key: "1", "a": "b"}),
        ent("P1", "PSYS", flags=6, psysprops={"sparam": 1e6}),
        ent("L1", "LIGHT", flags=0x200, lightprops={"type": G("L_SPOT"), "color": [1, 2, 3], "range": 12.5,
                                                     "theta": 15.5, "cutoff": 89.999, "atten": [0.0001, 2, 3e-6]}),
        ent("Cam", "CAMERA", flags=2),
    ]
    evs = [
        ev(0.25, "START_MOTION", ["D1"], {"animation": "A", "duration": 0.5}),
        ev(0.125, "START_SPATIAL_PROP_ANIM", ["C1"], {"duration": 1, "spatialprops": {
            "pos": [123456.789, -0.0000004, 1e-7], "q_orient": [0.7071068, 0, 0.7071068, 0]}}),
    ]
    return script(duration=2.5, entities=ents, events=evs)


def empty_scene():
    return script(entities=[])


def camera_scene():
    """One monitor and one camera without SET_CAMERA: the default event."""
    return script(duration=0.5, entities=[ent("Cam", "CAMERA", cameraprops={"fovh": 20}, **sp([1, 2, 3])),
                                          ent("Mon", "MONITOR"), ent("Mark", "MARKER")],
                  events=[ev(0.1, "START_CAMERA_PROP_ANIM", ["Cam"], {"duration": 0.2,
                                                                     "cameraprops": {"fovh": 80}})])


def words(*xs):
    return "/".join(w(x) for x in xs)


def cases():
    main, flags, empty, camera = main_scene(), flags_scene(), empty_scene(), camera_scene()
    out = [
        # Saved twice: the closed path is reopened by the first save.
        ("c1,L0,S,S", [main]),
        # Played, then saved: the default SET_CAMERA event.
        ("L3,g0,t100,t200,S", [main, flags, empty, camera]),
        # No context: no user events; the second context fails Slot4.
        ("c0,L0,S", [main]),
        ("c2,L0,S,c0,S", [main]),
        ("c1,L0,S,s", [flags]),
        ("L0,s", [main]),
        # Nothing to save, then a duration; no file system.
        ("L1,S,D1500,S,N,L0,N", [main, empty]),
        # The paths: a load, a missing file, a null path, a save to a path.
        ("c1,l0,S,lx,l-,S,W,l1,W", [camera, main]),
        # The scene frame, a new property, removals, a new duration.
        ("c1,L0,P" + words(100, 0, -50) + ",O" + words(0, 0, 1, 0, 1, 0, -1, 0, 0)
         + ",u1:" + b"Note".hex() + ":" + b"x\"y".hex() + ",u9:" + b"k".hex() + ":" + b"v".hex()
         + ",r7,e3,e99,D2500,S", [main]),
        # A second load replaces the first.
        ("c1,L1,L0,S,L3,S", [main, flags, empty, camera]),
    ]
    return ["%s %s" % (program, " ".join(s.hex() for s in scripts)) for program, scripts in out]


def write(name, header, lines, check):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    text = header + "".join(line + "\n" for line in lines)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != text.encode("ascii"):
                sys.exit("%s is out of date; run make_scene_script_save_fixtures.py" % name)
        return
    with open(path, "w", newline="\n") as out:
        out.write(text)


def main():
    check = "--check" in sys.argv[1:]
    write("scene_script_save.list",
          "# P4.14 save oracle inputs, written by make_scene_script_save_fixtures.py.\n",
          cases(), check)


if __name__ == "__main__":
    main()
