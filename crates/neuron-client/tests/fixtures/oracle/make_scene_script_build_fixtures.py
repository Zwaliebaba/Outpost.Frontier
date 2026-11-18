"""Writes scene_script_build.list, the inputs of the P4.14 build suite.

Each line is one case: "c" when Build gets a COM context or "-" when it
gets none, then buffers in hex, separated by spaces, loaded one after the
other into one module state (Oracle.exe neuron-client, oracle
SceneScriptBuildSuite, and crates/neuron-client/tests/oracle_vectors.rs).
A buffer is a compiled Lua 3.2 chunk, assembled with the Chunk class of
make_scene_script_fixtures.py from the values below. Run from any folder:

    python make_scene_script_build_fixtures.py

The cases stay clear of the inputs on which the reference dereferences a
null pointer (divergence D240). Those go to ../scene_script_build_faults.list
with the error the port reports, for crates/neuron-client/tests/
scene_script_data.rs.
"""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_scene_script_fixtures import Chunk  # noqa: E402


class G:
    """A global, read with GETGLOBAL (constants such as CAMERA, or nil)."""

    def __init__(self, name):
        self.name = name.encode()


NIL = G("NOT_A_GLOBAL")


class Num:
    """A number constant stored as this text."""

    def __init__(self, text):
        self.text = text.encode()


class Add:
    def __init__(self, a, b):
        self.a, self.b = a, b


class T:
    """A table constructor: positional items, then key-value fields."""

    def __init__(self, items=(), fields=()):
        self.items, self.fields = list(items), list(fields)


def emit(c, v):
    if isinstance(v, G):
        c.get(v.name)
    elif isinstance(v, Num):
        c.text_number(v.text)
    elif isinstance(v, Add):
        emit(c, v.a)
        emit(c, v.b)
        c.add()
    elif isinstance(v, bool):
        raise TypeError("no booleans in Lua 3.2")
    elif isinstance(v, int):
        if -65535 <= v <= 65535:
            c.number(v)
        else:
            c.text_number(str(v).encode())
    elif isinstance(v, float):
        c.text_number(repr(v).encode())
    elif isinstance(v, str):
        c.string(v.encode())
    elif isinstance(v, bytes):
        c.string(v)
    elif isinstance(v, list):
        emit(c, T(items=v))
    elif isinstance(v, dict):
        emit(c, T(fields=list(v.items())))
    elif isinstance(v, T):
        c.table(len(v.items) + len(v.fields))
        for i in range(0, len(v.items), 64):
            block = v.items[i : i + 64]
            for item in block:
                emit(c, item)
            c.setlist(i // 64, len(block))
        for i in range(0, len(v.fields), 32):
            block = v.fields[i : i + 32]
            for key, value in block:
                emit(c, key)
                emit(c, value)
            c.setmap(len(block))
    else:
        raise TypeError(v)


def script(**globals_):
    c = Chunk()
    for name, value in globals_.items():
        emit(c, value)
        c.set(name.encode())
    return c.bytes()


IDENT = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]


def ent(name, kind, **fields):
    d = {"entity_name": name, "type": G(kind)}
    d.update(fields)
    return d


def entities_case():
    """Every entity type, and the defaults and conversions of each key."""
    ents = [
        ent("Scene_1", "SCENE", template_name="", up=G("Y_AXIS"), front=G("NEG_Z_AXIS"),
            ambient=[128, 64.9, -3.5], start_time=1.25, fogon=1, fogtable=0, fogcolor=[10, 20, 30.7],
            fogmode=G("F_EXP2"), fogstart=5.5, fogend=900, fogdensity=0.0125,
            spatialprops={"pos": [0, 0, 0], "orient": IDENT}),
        ent("Sub", "SUB_SCENE", ambient=[1, 2], fogcolor=5, fogon=Num("0"), fogtable="x"),
        ent("Ship", "COMPOUND", template_name="ge_fighter", flags=Add(G("LIT_DYNAMIC"), G("LIT_AMBIENT")),
            lt_grp=300, srt_grp=-1, usr_flg=2.9, compoundprops={"floor_height": -12.5},
            spatialprops={"pos": [6.739478, -2.368005, 10.086178],
                          "orient": [[0.831549, 0, -0.555451], [0.10431, 0.982209, 0.156159],
                                     [0.545569, -0.187793, 0.816755]]},
            userprops={"category": "Spaceship", "b": 7, 3: "three", "t": {"x": 1}, "neg": -0.5}),
        ent("Body", "DEFORMABLE", srt_grp=-2.9, compoundprops=5,
            spatialprops={"pos": [1, 2], "orient": [NIL, [1, 2], [0, 0, 1]]}),
        ent("Null_template", "COMPOUND", template_name="NULL", compoundprops={"floor_height": "2.5"}),
        ent("Cam", "CAMERA", cameraprops={"fovh": 35, "hvaspect": 1.85, "nearplane": "abc"}),
        ent("Mon", "MONITOR", spatialprops={"pos": 4}),
        ent("Lamp_typed", "LIGHT", lightprops={
            "type": G("L_DIRECT"), "on": 1, "color": [255, 128.9, -1.5], "diffuse": [0.5, 0.25, 1],
            "specular": [1, 1], "ambient": [0.1, 0.2, 0.3], "direction": [0, -1, 0], "range": 250,
            "atten": [1, 0.5, 0.25], "cutoff": 30, "theta": 20, "infinite": 0}),
        ent("Lamp_point", "LIGHT", lightprops={"color": [200, 100, 50], "range": 40, "cutoff": 45}),
        ent("Lamp_wide", "LIGHT", lightprops={"cutoff": 100, "range": 0.00001}),
        ent("Lamp_clamp", "LIGHT", lightprops={"cutoff": 170}),
        ent("Lamp_spot", "LIGHT", lightprops={"cutoff": -1, "range": "x"}),
        ent("Lamp_sun", "LIGHT", lightprops={"infinite": 3, "theta": 10}),
        ent("Lamp_default", "LIGHT"),
        ent("Lamp_empty", "LIGHT", lightprops={}),
        ent("Snd", "SOUND", audioprops={"archetype": "ambience_loop", "attenuation": -6, "pan": 0.5,
                                         "dmin": 10, "dmax": 2000, "ain": 90.7, "aout": -45.2,
                                         "atout": -3, "rmix": 0.25}),
        ent("Snd_named", "SOUND", template_name="music", audioprops={"archetype": "ignored"}),
        ent("Snd_bare", "SOUND", audioprops={"pan": 1}),
        ent("Mark", "MARKER", userprops={"main_object": "main_object", 1.5: 2.25}),
        ent("Hp", "HARDPOINT", spatialprops={"orient": [[0, 1, 0]]}),
        ent("Path_points", "MOTION_PATH", pathprops={"control_pts": [[1, 2, 3], [4, 5], 7, [-1.5, 0.25, 9]]}),
        ent("Path_text", "MOTION_PATH", pathprops={"path_type": "CV_CROrientationSplinePath",
                                                     "path_data": "OPEN, {1,2,3}"}),
        ent("Path_number", "MOTION_PATH", pathprops={"path_type": "CV_CRSplinePath", "path_data": 12.5}),
        ent("Path_null", "MOTION_PATH", pathprops={"path_type": "NULL", "path_data": "x"}),
        ent("Path_curve", "MOTION_PATH", pathprops={"path_type": "LinearPCurve"}),
        ent("Path_empty", "MOTION_PATH", pathprops={}),
        ent("Path_bare_points", "MOTION_PATH", pathprops={"control_pts": 5}),
        ent("Path_none", "MOTION_PATH", pathprops=3),
        ent("Fx", "PSYS", psysprops={"sparam": 0.75}),
        ent("Fx_named", "PSYS", template_name="smoke"),
        ent("Gone", "DELETED"),
        {"entity_name": "Untyped"},
        ent("Zero", "UNKNOWN_ENTITY"),
        {"entity_name": "Fourteen", "type": 14},
        {"entity_name": "Lamp_string_type", "type": "5", "flags": "16"},
        {"type": G("MARKER")},
        {"entity_name": 3.5, "type": G("MARKER"), "template_name": 7},
        "not a table",
        ent("Dup", "MARKER"),
        ent("Dup", "MARKER", flags=1),
    ]
    return script(duration=24.937, entities=ents)


def ev(time, kind, names, props=None):
    items = [time, G(kind) if isinstance(kind, str) else kind, names]
    if props is not None:
        items.append(props)
    return items


CURVE = {"CLSID": "FreeFormPCurve", "points": [[0, 0, 0, 0.241379], [NIL, 5, 5, 5], 9, [1, 1, 0]]}


def events_case():
    """Every event type, its properties and its defaults."""
    ents = [
        ent("Scene_1", "SCENE"),
        ent("Ship", "COMPOUND"),
        ent("Char", "DEFORMABLE"),
        ent("Snd", "SOUND", flags=5),
        ent("Cam", "CAMERA"),
        ent("Mon", "MONITOR"),
        ent("Lamp", "LIGHT"),
        ent("Mark", "MARKER"),
        ent("Path_1", "MOTION_PATH"),
        ent("Fx", "PSYS"),
        ent("Sub", "SUB_SCENE"),
    ]
    evs = [
        ev(0.5, "SET_CAMERA", ["Mon", "Cam"]),
        ev(1, "START_SOUND", ["Snd"], {"duration": 2.5, "start_time": 3.7}),
        ev(1.5, "START_SOUND", ["Snd"], {"flags": Add(G("POSITION"), G("ORIENTATION")), "duration": 1}),
        ev(2, "START", ["Snd"], {"flags": G("POSITION")}),
        ev(2.25, "START", ["Char"], {"animation": "Sc_Walk", "loop": 1, "heading": 90, "trans_scale": 3}),
        ev(2.5, "START_MOTION", ["Ship"], {"animation": 12, "locked_bone": "Root", "start_time": 0.5,
                                           "trans_time": 0.25, "time_scale": 2, "weight": 0.5, "loop": 0,
                                           "trans_scale": 1.5, "heading": 45}),
        ev(2.75, "START_MOTION", ["Char"], {"animation": {"x": 1}, "loop": -1}),
        ev(2.8, "START_MOTION", ["Char"]),
        ev(3, "START_IK", ["Char", "Mark"], {"end_effector": "Hand", "target_part": "", "target_type": 2,
                                             "ik_id": 7.9, "count_to_root": 3, "damping": 0.5, "up": 2,
                                             "front": 4, "point_at": 0.5, "move_to": 0,
                                             "transition_duration": 0.25, "duration": 4}),
        ev(0, "START_IK", ["Char"], {"end_effector": 5, "target_part": "Head"}),
        ev(0, "START_IK", ["Char"], 5),
        ev(3.5, "START_PATH_ANIMATION", ["Mark", "Path_1"], {"start_percent": 0.1, "percent1": 0.2,
                                                              "stop_percent": 0.9, "up": 2, "front": 3,
                                                              "offset": [1, 2, 3], "param_curve": CURVE,
                                                              "pcurve_period": -1000}),
        ev(3.6, "START_PATH_ANIMATION", ["Mark", "Lamp"], {"offset": [1, 2], "target_part": "Gun",
                                                            "target_type": G("HARDPOINT"), "up": 0}),
        ev(3.7, "START_PATH_ANIMATION", ["Mark", "Fx"], {"percent2": 0.5}),
        ev(3.8, "START_PATH_ANIMATION", ["Mark", "Nobody"]),
        ev(4, "ATTACH_ENTITY", ["Ship", "Char"], {"offset": [0, 1, 2], "target_part": 3, "target_type": 1,
                                                  "up": 4, "front": 5}),
        ev(4.5, "START_SPATIAL_PROP_ANIM", ["Mark"], {
            "spatialprops": {"axisrot": [45, G("Y_AXIS")], "pos": [1, 2, 3], "q_orient": [0.5, 0.5, 0.5, 0.5],
                             "orient": IDENT},
            "target_part": "Part", "target_type": 2, "flags": G("POSITION")}),
        ev(4.6, "START_SPATIAL_PROP_ANIM", ["Mark"], {
            "spatialprops": {"axisrot": 5, "pos": 7, "orient": [[0, 1, 0], NIL, [1, 0]]}}),
        ev(4.7, "START_SPATIAL_PROP_ANIM", ["Mark"], {"spatialprops": {"q_orient": [1, 2, 3]}}),
        ev(4.8, "START_SPATIAL_PROP_ANIM", ["Mark"]),
        ev(5, "START_LIGHT_PROP_ANIM", ["Lamp"], {"lightprops": {
            "color": [255, 51, 0], "infinite": 0, "cutoff": -5, "theta": 10, "range": 80,
            "diffuse": [0.1, 0.2, 0.3], "on": 0}}),
        ev(5.1, "START_LIGHT_PROP_ANIM", ["Lamp"], {"lightprops": {"infinite": 1, "cutoff": 30}}),
        ev(5.2, "START_LIGHT_PROP_ANIM", ["Lamp"], {"lightprops": {"type": 3, "specular": [1, 1, 1]}}),
        ev(5.3, "START_LIGHT_PROP_ANIM", ["Lamp"], {"lightprops": 4}),
        ev(5.5, "START_CAMERA_PROP_ANIM", ["Cam"], {"cameraprops": {"fovh": 50, "aspect": 1.5, "far": "1e4",
                                                                    "fovy": 3}}),
        ev(6, "CONNECT_HARDPOINTS", ["Ship", "Char"], {"hardpoint": "HpMount", "parent_hardpoint": {"t": 1}}),
        ev(6.5, "START_SUB_SCENE", ["Sub"], {"start_time": 1.5}),
        ev(6.6, "START_SUB_SCENE", ["Sub"]),
        ev(7, "START_PSYS", ["Fx"], {"duration": 3, "event_flags": 4}),
        ev(7.5, "START_PSYS_PROP_ANIM", ["Fx"], {"psysprops": {"sparam": 0.3}}),
        ev(7.6, "START_PSYS_PROP_ANIM", ["Fx"], {"psysprops": {}}),
        ev(8, "START_AUDIO_PROP_ANIM", ["Snd"], {"audioprops": {"attenuation": -20, "ain": 45.5, "rmix": 1}}),
        ev(8.5, "START_FOG_PROP_ANIM", ["Scene_1"], {"fogprops": {"fogon": 1, "fogcolor": [1, 2, 3],
                                                                  "fogmode": 3, "fogend": 500}}),
        ev(9, "START_REVERB_PROP_ANIM", ["Scene_1"], {"reverbprops": {"env": 12.7, "vol": 0.5, "decay": 1.5,
                                                                      "damping": 0.25}}),
        ev(9.1, "START_REVERB_PROP_ANIM", ["Scene_1"], {"reverbprops": {"vol": 1}}),
        ev(9.5, "START_FLR_HEIGHT_ANIM", ["Ship"], {"floor_height": 2.5, "target_part": "Deck",
                                                     "target_type": G("PART")}),
        ev(9.6, "START_FLR_HEIGHT_ANIM", ["Ship"]),
        ev(10, "SUBTITLE", ["Scene_1"], {"strid": 65538, "duration": 2}),
        ev(10.5, "SUBTITLE", ["Scene_1"], {"strid": -1}),
        ev(11, "USER_EVENT", ["Scene_1"], {"user_event_string": "hello", "param_curve": {
            "CLSID": "CatmullRomPCurve", "points": [[0, 1, 2, 3]]}}),
        ev(11.5, "USER_EVENT", ["Scene_1"], {"user_event_string": 2.5}),
        ev(11.6, "USER_EVENT", ["Scene_1"], {"user_event_string": {"a": 1}}),
        ev(11.7, "USER_EVENT", ["Scene_1"], {"other": 1}),
        ev(11.8, "USER_EVENT", ["Scene_1"]),
        ev(12, "STOP", ["Char"], {"param_curve": CURVE}),
        ev(12.5, "STOP_IK", ["Char"]),
        ev(13, "UNDEFINED_EVENT", ["Char"]),
        ev(13.5, -4, ["Char"]),
        ev(14, "SET_CAMERA", "Mon"),
        ev(14.5, "SET_CAMERA", []),
        ev(15, "SET_CAMERA", ["Nobody", "Cam"], {"duration": 1, "flags": 6, "event_flags": "3"}),
        ev(15.5, "SET_CAMERA", [NIL, "Nobody"]),
        ev(-2.5, "SET_CAMERA", ["Mon"], {"param_curve": {"CLSID": "NoSuchCurve", "points": [[0, 0, 0, 0]]}}),
        ev(0.0015, "SET_CAMERA", ["Mon"], {"param_curve": {"CLSID": "LinearPCurve"}}),
        ev(2.0, "SET_CAMERA", ["Mon"], {"param_curve": {"CLSID": "LinearPCurve", "points": 4},
                                         "duration": "1.5"}),
        ev(3.0, "SET_CAMERA", ["Mon"], {"param_curve": {"CLSID": 5, "points": [[0, 0, 0, 0]]}}),
        ev(3000000, "SET_CAMERA", ["Mon"], {"duration": 1e10}),
        ev(1e20, "SET_CAMERA", ["Mon"], {"duration": -1e20}),
        ev(-0.0004, "SET_CAMERA", ["Mon"], {"duration": 0.0005, "pcurve_period": 2.5}),
        ev("2.5", "SET_CAMERA", ["Mon"], {"duration": "x", "flags": "y"}),
        ev(NIL, "SET_CAMERA", ["Mon"]),
        "not an event",
        [],
        [1, G("SET_CAMERA")],
    ]
    return script(duration=30, entities=ents, events=evs)


def published_names():
    """Names published by one script stay for the next (G_LuaEntityHandles)."""
    first = script(duration=1, entities=[ent("A", "MARKER"), ent("B", "CAMERA"), ent("C", "LIGHT")])
    second = script(
        duration=2,
        entities=[ent("X", "COMPOUND")],
        events=[
            ev(0, "SET_CAMERA", ["X", "B"]),
            ev(0, "SET_CAMERA", ["C", "A"]),
            ev(0, "START_PATH_ANIMATION", ["X", "A"]),
        ],
    )
    return [first, second]


def rerun():
    """No entities and no duration: Build runs the script a second time."""
    seed = script(counter=0)
    c = Chunk()
    emit(c, Add(G("counter"), 1))
    c.set(b"counter")
    c.table(0)
    c.set(b"entities")
    c.table(1)
    c.table(3)
    c.get(b"counter")
    c.get(b"SET_CAMERA")
    emit(c, ["Nobody"])
    c.setlist(0, 3)
    c.setlist(0, 1)
    c.set(b"events")
    return [seed, c.bytes()]


def keyed():
    """A string-keyed entity (its key is its name) and events in a table
    grown by SETMAP, whose number keys are placed by value."""
    c = Chunk()
    emit(c, 5)
    c.set(b"duration")
    emit(c, T(fields=[("cam", {"type": G("CAMERA")})]))
    c.set(b"entities")
    emit(c, T(fields=[(k, ev(k, "SET_CAMERA", ["cam"])) for k in (12, 1, 7, 23, 2)]))
    c.set(b"events")
    return c.bytes()


def failures():
    return [b"", script(entities=5), script(duration=3)]


def faults():
    """Scripts on which the reference dereferences a null pointer, with the
    error the port reports instead (divergence D240). Not oracle inputs."""
    ents = [ent("Char", "DEFORMABLE"), ent("Cam", "CAMERA")]

    def with_event(*e):
        return script(duration=1, entities=ents, events=[list(e)])

    return [
        ("null:entity name",
         script(duration=1, entities=T(fields=[({}, {"type": G("MARKER")})]))),
        ("null:event entity name", with_event(0, G("SET_CAMERA"), [{}])),
        ("null:event entity name", with_event(0, G("SET_CAMERA"), ["Cam", {}])),
        ("entity:START event", with_event(0, G("START"), ["Nobody"])),
        ("entity:positioned START_SOUND event",
         with_event(0, G("START_SOUND"), ["Nobody"], {"flags": G("POSITION")})),
        ("entity:START_MOTION event", with_event(0, G("START_MOTION"), ["Nobody"], {})),
        ("null:START_IK end_effector", with_event(0, G("START_IK"), ["Char"], {"target_part": "x"})),
        ("null:START_IK target_part",
         with_event(0, G("START_IK"), ["Char"], {"end_effector": "x", "target_part": {}})),
        ("null:ATTACH_ENTITY target_part",
         with_event(0, G("ATTACH_ENTITY"), ["Char", "Cam"], {"target_part": {}})),
        ("null:SPATIAL_PROP_ANIM target_part",
         with_event(0, G("START_SPATIAL_PROP_ANIM"), ["Char"], {"target_part": {}})),
        ("null:FLOOR_HEIGHT_ANIM target_part",
         with_event(0, G("START_FLR_HEIGHT_ANIM"), ["Char"], {"target_part": {}})),
    ]


def hexs(*buffers):
    return " ".join(b.hex() for b in buffers)


def main():
    cases = [
        "c " + hexs(entities_case()),
        "c " + hexs(events_case()),
        # Without a COM context the user events are dropped.
        "- " + hexs(events_case()),
        "c " + hexs(*published_names()),
        "c " + hexs(*rerun()),
        "c " + hexs(keyed()),
        "c " + hexs(*failures()),
    ]
    header = (
        "# P4.14 scene-script build oracle inputs, written by make_scene_script_build_fixtures.py.\n"
        "# One case per line: c (COM context) or - (none), then buffers in hex, loaded in one module state.\n"
    )
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "scene_script_build.list"), "w", newline="\n") as f:
        f.write(header)
        for case in cases:
            f.write(case + "\n")
    with open(os.path.join(here, "..", "scene_script_build_faults.list"), "w", newline="\n") as f:
        f.write(
            "# Scripts the reference faults on, written by oracle/make_scene_script_build_fixtures.py.\n"
            "# One per line: the error the port reports (kind:what), then the chunk in hex.\n"
        )
        for error, chunk in faults():
            f.write(error.replace(" ", "_") + " " + chunk.hex() + "\n")


if __name__ == "__main__":
    main()
