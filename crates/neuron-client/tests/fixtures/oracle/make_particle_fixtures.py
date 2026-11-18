"""Writes the inputs of the P4.8 particle suites (Oracle.exe neuron-client,
CppSource/Oracle/oracle_particles.cpp, and
crates/neuron-client/tests/oracle_vectors.rs): particle_ramp.list and
particle_sim.list. Run from any folder:

    python make_particle_fixtures.py [--check]

Everything is synthetic. Node libraries and effect libraries are built as the
streams of an .ale file's two leaves with the stream helpers of
crates/neuron-core/tests/fixtures/oracle/make_fx_data_fixtures.py (P1.7),
which this script imports; node names are hashed with the particle module's
name CRC, whose table is read from crates/neuron-core/src/hash.rs.

particle_ramp.list: one case per line, "<node library hex> <times>": every
ramp and transform property of every node is evaluated at the times in order
(each time "outer:inner", floats as eight hex digits).

particle_sim.list: one case per line, operations separated by " ; " (those of
oracle_particles.cpp): the libraries, pool sizes, the generator state,
textures, effects and the client's per-frame calls, with dumps.

The cases keep to what the reference defines: no time is unordered, every
ramp a property hands out has a key, loop types stay within 0 to 4 per nibble,
every record names a node the library holds and every pair names records of
the effect, every pool a case spawns into has room for at least one particle,
and no texture call is made on an id the fake did not hand out.
"""

import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", ".."))


def _load_fx_data_helpers():
    path = os.path.join(REPO, "crates", "neuron-core", "tests", "fixtures", "oracle",
                        "make_fx_data_fixtures.py")
    spec = importlib.util.spec_from_file_location("make_fx_data_fixtures", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fx = _load_fx_data_helpers()


def _crc_table():
    with open(os.path.join(REPO, "crates", "neuron-core", "src", "hash.rs")) as source:
        text = source.read()
    block = text[text.index("const CRC_TABLE"):]
    block = block[block.index("[") + 1:block.index("];")]
    words = [int(w.replace("_", ""), 16) for w in re.findall(r"0x[0-9A-Fa-f_]+", block)]
    assert len(words) == 256
    return words


CRC = _crc_table()


def name_hash(name):
    """HashNameCrc: the engine CRC over the bytes as they are."""
    crc = 0xFFFFFFFF
    for byte in name:
        crc = CRC[(byte ^ crc) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def f(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def hx(data):
    return data.hex()


# --- Property helpers ---------------------------------------------------------------------

prop, node, string, f32, u32 = fx.prop, fx.node, fx.string, fx.f32, fx.u32
nested11, animated, single, color, transform = (fx.nested11, fx.animated, fx.single, fx.color,
                                                fx.transform)
Bits = fx.Bits
STEP = Bits(0xFFFFFFFF)

ANIM, RAMP, CRAMP = 0x202, 0x200, 0x201
NAME, LIFE, XFORM = 0xEFA8CE01, 0xF27FDE7D, 0xE13A59A1

FREQ, MAXP, COUNT, LIFESPAN, PRESSURE, APPROACH = (0x023C350C, 0xF5042852, 0xE7221F95,
                                                   0x0A635880, 0x0AB180C5, 0x0681233E)
INITIAL, EMIT_LOD = 0x0F9A9D52, 0xFE077E05
CONE_MIN_R, CONE_MAX_R, CONE_MIN_S, CONE_MAX_S = 0x1C73D16B, 0xF636F07F, 0x100E7F55, 0xFA4B5E41
CUBE_W, CUBE_H, CUBE_D, CUBE_MIN_S, CUBE_MAX_S = (0xFD22A11C, 0xEFB0F8CF, 0x14B74EB2,
                                                  0xE282006B, 0x08C7217F)
SPHERE_MIN_R, SPHERE_MAX_R = 0xF231FF60, 0x1874DE74
GRAVITY = 0xE02B8BD4
AIR_MAG, AIR_APPROACH = 0xE5E3524C, 0x10423CEB
RAD_RADIUS, RAD_ATTEN, RAD_MAG, RAD_APPROACH = 0xFF28F620, 0x1A433168, 0x0064B3B9, 0xF3CF5EA5
TURB_MAG, TURB_APPROACH = 0x1CEAC6D1, 0x1B970B9A
COLLIDE_REFLECT = 0xF2468210
DUST_RADIUS = 0x1874DE74
TEXNAME, BLEND, COLOR, ALPHA, SIZE = 0x1BA23359, 0x1506EB6C, 0x048767E8, 0x04FC9016, 0xF7C2EBA9
USE_COMMON_FRAME, COMMON_FRAME = 0xE2F60EEB, 0xF52B8DD5
APP_LOD = 0x0C53B008

OVERRIDE_FOLDER = 0xEE223B51
ROOT = 0x8000


def anim(value):
    """A nested animated ramp with one keyless value."""
    return nested11(4, [(0.0, animated(value, 0, []))])


def anim_keys(keys, loop=0, weight=1.0, mode=4):
    """A nested animated ramp whose one child has keys (time, value, in, out)."""
    return nested11(mode, [(0.0, animated(weight, loop, keys))])


def ramp(value):
    return nested11(4, [(0.0, single(4, [(0.0, value)]))])


def named(cls, name, props):
    return node(cls, [prop(0x103, NAME, string(name))] + props)


def channels(values):
    return [anim(v) for v in values]


def bool_prop(hashv, value=True):
    return prop(0x8001 if value else 0x0001, hashv)


def texture_name(text):
    return prop(0x103, TEXNAME, string(text))


def name_crc(text):
    """NEURONCOMPONENTS_CRC::GetCRC32: the case-folded name CRC the billboard sets keep."""
    crc = 0xFFFFFFFF
    for byte in text.lower():
        crc = CRC[(byte ^ crc) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


# --- particle_ramp ------------------------------------------------------------------------

def ramp_library():
    """Nodes whose properties reach every evaluation rule."""
    loops = [0x00, 0x11, 0x22, 0x33, 0x44, 0x12, 0x30]
    hermite = [(0.0, 0.0, 0.0, 2.0), (1.0, 4.0, -1.0, 0.5), (1.0, 5.0, 0.0, 0.0),
               (2.5, -3.0, 0.25, STEP), (4.0, 1.0, 1.0, 1.0)]
    emitter_props = [
        prop(ANIM, FREQ, anim_keys(hermite, loops[0])),
        prop(ANIM, MAXP, anim_keys(hermite, loops[1])),
        prop(ANIM, COUNT, anim_keys(hermite, loops[2])),
        prop(ANIM, LIFESPAN, anim_keys(hermite, loops[3])),
        prop(ANIM, PRESSURE, anim_keys(hermite, loops[4])),
        prop(ANIM, APPROACH, anim_keys(hermite, loops[5])),
        prop(ANIM, SPHERE_MIN_R, anim_keys([(0.5, 2.0, 0.0, 0.0)], 0x44)),
        prop(ANIM, SPHERE_MAX_R, anim_keys([(0.5, 2.0, 3.0, -3.0)], 0x00)),
        # Outer keys blended in each mode.
        prop(RAMP, EMIT_LOD, nested11(0, [
            (0.0, single(1, [(0.0, 0.0), (1.0, 1.0), (1.0, 5.0), (2.0, 6.0)])),
            (2.0, single(4, [(0.0, 10.0), (2.0, 0.0)]))])),
    ]
    nested_modes = []
    for mode in range(8):
        nested_modes.append(nested11(mode if mode < 7 else 9, [
            (0.0, animated(1.0, 0, [(0.0, 0.0, 0.0, 0.0), (2.0, 8.0, 0.0, 0.0)])),
            (1.0, animated(3.0, 0, [])),
            (3.0, animated(-2.0, 0x30, [(0.0, 1.0, 0.0, 0.0), (1.0, 2.0, 0.0, 0.0)]))]))
    flat = []
    for mode in range(7):
        flat.append(nested11(mode, [
            (0.0, single(mode, [(-1.0, 2.0), (0.5, -1.0), (0.5, 3.0), (2.0, 0.25)])),
            (1.5, single((mode + 3) % 7, [(0.0, 1.0), (1.0, 7.0)])),
            (2.0, single(mode, [(0.0, 4.0)]))]))
    colors = []
    for mode in range(7):
        colors.append(nested11(mode, [
            (0.0, color(mode, [(0.0, (0.1, 0.9, 0.5)), (0.5, (1.0, 0.0, 0.25)),
                               (0.5, (0.2, 0.2, 0.2)), (1.0, (0.0, 1.0, 1.0))])),
            (1.0, color((mode + 4) % 7, [(0.0, (1.0, 1.0, 1.0)), (2.0, (0.0, 0.5, 0.0))])),
            (2.5, color(mode, [(0.0, (0.3, 0.6, 0.9))]))]))
    rotate = channels([0.0] * 9)
    rotate[0] = anim_keys([(0.0, 1.0, 0.0, 0.0), (2.0, -3.0, 0.0, 0.0)])
    rotate[2] = anim_keys([(0.0, 0.5, 0.0, 0.0)])
    rotate[3] = anim_keys([(0.0, 0.0, 0.0, 0.0), (2.0, 90.0, 10.0, 10.0)])
    rotate[4] = anim(30.0)
    rotate[5] = anim_keys([(0.0, -45.0, 0.0, 0.0), (1.0, 200.0, 0.0, 0.0)], 0x11)
    rotate[6] = anim_keys([(0.0, 1.0, 0.0, 0.0), (2.0, 2.0, 0.0, 0.0)])
    rotate[8] = anim(0.5)
    scale_only = channels([0.0] * 6 + [2.0, 1.0, 1.0])
    nodes = [
        named(b"FxSphereEmitter", b"ramp_emitter", emitter_props + [
            prop(0x105, XFORM, transform(1, 2, 3, rotate))]),
        named(b"FxBasicAppearance", b"ramp_basic", [
            prop(CRAMP, COLOR, colors[0]),
            prop(RAMP, ALPHA, flat[1]),
            prop(RAMP, SIZE, flat[2]),
            prop(RAMP, 0x05DB630E, flat[3]),
            prop(RAMP, 0x015A6F47, flat[4]),
            prop(RAMP, 0xE0E97650, flat[5]),
            prop(RAMP, APP_LOD, flat[6]),
            prop(ANIM, COMMON_FRAME, nested_modes[0]),
        ]),
        named(b"FxRectAppearance", b"ramp_rect", [
            prop(CRAMP, COLOR, colors[1]),
            prop(ANIM, COMMON_FRAME, nested_modes[1]),
            prop(0x105, XFORM, transform(0, 0, 0, scale_only)),
        ]),
        named(b"FxPerpAppearance", b"ramp_perp", [
            prop(CRAMP, COLOR, colors[2]),
            prop(ANIM, COMMON_FRAME, nested_modes[2]),
        ]),
        named(b"FxOrientedAppearance", b"ramp_oriented", [
            prop(CRAMP, COLOR, colors[3]),
            prop(ANIM, COMMON_FRAME, nested_modes[3]),
            prop(0x105, 0xF0004D58, transform(4, 3, 5, rotate)),
        ]),
        named(b"FLDustAppearance", b"ramp_dust", [
            prop(CRAMP, COLOR, colors[4]),
            prop(ANIM, COMMON_FRAME, nested_modes[4]),
        ]),
        named(b"FLBeamAppearance", b"ramp_beam", [
            prop(CRAMP, COLOR, colors[5]),
            prop(ANIM, COMMON_FRAME, nested_modes[5]),
        ]),
        named(b"FxRadialField", b"ramp_radial", [
            prop(RAMP, RAD_ATTEN, flat[0]),
            prop(ANIM, RAD_MAG, nested_modes[6]),
            prop(ANIM, RAD_RADIUS, nested_modes[7]),
        ]),
        named(b"FxCubeEmitter", b"ramp_cube", [
            prop(ANIM, CUBE_W, anim_keys(hermite, loops[6])),
            prop(ANIM, CUBE_H, anim_keys([(1.0, 3.0, 0.0, 0.0), (1.0, 4.0, 0.0, 0.0)], 0x44)),
        ]),
        named(b"FxParticleAppearance", b"ramp_particle", [prop(RAMP, APP_LOD, flat[3])]),
    ]
    return fx.library(nodes)


def ramp_times():
    times = [(0.0, 0.0), (0.5, 0.5), (1.0, 1.0), (0.25, 0.75), (1.0, 0.5), (2.0, 2.0),
             (1.75, -1.5), (3.0, 4.0), (0.6, 1.0), (-1.0, 9.5), (2.4, -7.25), (1.2, 2.5),
             (0.5, 0.5), (5.0, 1.999), (1.5, 13.0)]
    return ",".join("%s:%s" % (f(a), f(b)) for a, b in times)


def ramp_list():
    lines = [hx(ramp_library()) + " " + ramp_times()]
    # The 1.0 library: one key per nested ramp.
    v10 = fx.library([
        node(b"FxConeEmitter", [prop(0x103, NAME, string(b"old")),
                                prop(0x100, EMIT_LOD, single(2, [(0.0, 1.0), (1.0, 3.0)])),
                                prop(0x102, FREQ, animated(5.0, 0x11, [(0.0, 1.0, 0.0, 0.0),
                                                                         (1.0, 2.0, 0.0, 0.0)]))]),
    ], version=fx.V10)
    lines.append(hx(v10) + " " + ramp_times())
    return lines


# --- particle_sim -------------------------------------------------------------------------

def effect(name, refs, pairs):
    return fx.effect(name, refs, pairs)


def ale_op(nodes, effects):
    return "ale %s %s" % (hx(fx.library(nodes)), hx(fx.effects_lib(effects)))


def ref(name, parent, ident, kind=0):
    return (kind, name_hash(name), parent, ident)


DT = f(1.0 / 30.0)
XF_ID = ",".join(f(v) for v in (1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0))


def xf(rot, pos):
    return ",".join(f(v) for v in list(rot) + list(pos))


def steps(slot, count, dt=DT):
    return ["step %d %s" % (slot, dt)] * count


def sim_basic():
    """Sphere emitter into a basic appearance under gravity; stop and finish."""
    nodes = [
        named(b"FxSphereEmitter", b"spark_emit", [
            prop(ANIM, FREQ, anim(40.0)), prop(ANIM, LIFESPAN, anim(0.5)),
            prop(ANIM, PRESSURE, anim(3.0)), prop(2, INITIAL, u32(3)),
            prop(ANIM, SPHERE_MIN_R, anim(0.25)), prop(ANIM, SPHERE_MAX_R, anim(1.5)),
            prop(3, LIFE, f32(10.0))]),
        named(b"FxBasicAppearance", b"spark_app", [prop(CRAMP, COLOR, nested11(4, [
            (0.0, color(1, [(0.0, (1.0, 0.5, 0.0)), (1.0, (0.0, 0.0, 1.0))]))]))]),
        named(b"FxGravityField", b"spark_gravity", [prop(ANIM, GRAVITY, anim(4.0))]),
    ]
    effects = [effect(b"sparks", [ref(b"spark_emit", ROOT, 1), ref(b"spark_app", ROOT, 2),
                                  ref(b"spark_gravity", ROOT, 3)], [(1, 2), (2, 3)])]
    ops = [ale_op(nodes, effects), "effect 0 " + hx(b"sparks"), "dump 0",
           "xform 0 0 " + XF_ID] + steps(0, 1) + ["dump 0"] + steps(0, 5) + [
        "dump 0", "pools", "stop 0 1", "done 0 4"] + steps(0, 4) + ["dump 0", "done 0 4"] + \
        steps(0, 14) + ["done 0 4", "dump 0", "rand", "release 0", "pools"]
    return ops


def sim_cone_motion():
    """Cone emitter moving each frame (motion compensation and velocity
    approach), air and turbulence fields on a perp appearance."""
    nodes = [
        named(b"FxConeEmitter", b"jet_emit", [
            prop(ANIM, FREQ, anim(25.0)), prop(ANIM, LIFESPAN, anim(0.4)),
            prop(ANIM, PRESSURE, anim_keys([(0.0, 2.0, 0.0, 0.0), (1.0, 6.0, 0.0, 0.0)])),
            prop(ANIM, APPROACH, anim(0.5)),
            prop(ANIM, CONE_MIN_R, anim(0.1)), prop(ANIM, CONE_MAX_R, anim(0.4)),
            prop(ANIM, CONE_MIN_S, anim(5.0)), prop(ANIM, CONE_MAX_S, anim(30.0))]),
        named(b"FxPerpAppearance", b"jet_app", []),
        named(b"FxAirField", b"jet_air", [prop(ANIM, AIR_MAG, anim(-2.0)),
                                          prop(ANIM, AIR_APPROACH, anim(1.5))]),
        named(b"FxTurbulenceField", b"jet_turb", [prop(ANIM, TURB_MAG, anim(3.0)),
                                                  prop(ANIM, TURB_APPROACH, anim(0.25))]),
    ]
    effects = [effect(b"jet", [ref(b"jet_app", ROOT, 1), ref(b"jet_emit", ROOT, 2),
                               ref(b"jet_air", ROOT, 3), ref(b"jet_turb", ROOT, 4)],
                      [(2, 1), (1, 3), (1, 4)])]
    ops = [ale_op(nodes, effects), "seed 4660 7", "effect 0 " + hx(b"jet")]
    rot = (0.8, 0.0, -0.6, 0.0, 1.0, 0.0, 0.6, 0.0, 0.8)
    for frame in range(8):
        ops.append("xform 0 0 " + xf(rot, (0.5 * frame, 0.25 * frame, -frame)))
        ops += steps(0, 1)
    ops += ["dump 0", "rand", "stop 0 7", "dump 0", "release 0", "pools"]
    return ops


def sim_cube_pool():
    """Cube emitter into a rect appearance with radial and collide fields,
    through a pool of four items (eviction), with a second effect sharing it."""
    nodes = [
        named(b"FxCubeEmitter", b"box_emit", [
            prop(ANIM, FREQ, anim(60.0)), prop(ANIM, LIFESPAN, anim(1.0)),
            prop(ANIM, PRESSURE, anim(1.0)), prop(ANIM, CUBE_W, anim(2.0)),
            prop(ANIM, CUBE_H, anim(1.0)), prop(ANIM, CUBE_D, anim(0.5)),
            prop(ANIM, CUBE_MIN_S, anim(0.0)), prop(ANIM, CUBE_MAX_S, anim(80.0))]),
        named(b"FxRectAppearance", b"box_app", []),
        named(b"FxRadialField", b"box_radial", [
            prop(ANIM, RAD_RADIUS, anim(2.0)), prop(ANIM, RAD_MAG, anim(5.0)),
            prop(ANIM, RAD_APPROACH, anim(0.5)),
            prop(RAMP, RAD_ATTEN, nested11(4, [(0.0, single(1, [(0.0, 1.0), (1.0, 0.0)]))]))]),
        named(b"FxGravityField", b"box_gravity", [prop(ANIM, GRAVITY, anim(-30.0))]),
        named(b"FxCollideField", b"box_floor", [prop(ANIM, COLLIDE_REFLECT, anim(0.6)),
                                                prop(0x105, XFORM, transform(0, 0, 0, channels(
                                                    [0.0, 0.5, 0.0, 10.0, 0.0, 0.0, 1, 1, 1])))]),
    ]
    effects = [effect(b"box", [ref(b"box_app", ROOT, 1), ref(b"box_emit", ROOT, 2),
                               ref(b"box_radial", ROOT, 3), ref(b"box_floor", ROOT, 4),
                               ref(b"box_gravity", ROOT, 5)],
                      [(2, 1), (1, 3), (1, 5), (1, 4)])]
    ops = [ale_op(nodes, effects), "pool rect 4", "seed 99 3", "effect 0 " + hx(b"box"),
           "effect 1 " + hx(b"box"), "xform 1 0 " + xf((1, 0, 0, 0, 1, 0, 0, 0, 1), (3, 0, 0))]
    for _ in range(8):
        ops += steps(0, 1) + steps(1, 1)
    ops += ["dump 0", "dump 1", "pools", "release 0", "pools"] + steps(1, 3) + [
        "dump 1", "release 1", "pools"]
    return ops


def sim_folders():
    """Folders (a kind 1 group and the transform override), animated node
    transforms, outer time, LOD input, bursts, initial particles, the
    particle cap, lifetimes, and a beam with a beam field and a dust
    appearance with a dust field."""
    move = channels([0.0] * 9)
    move[0] = anim_keys([(0.0, 0.0, 0.0, 0.0), (1.0, 2.0, 0.0, 0.0)])
    move[4] = anim_keys([(0.0, 0.0, 0.0, 0.0), (1.0, 90.0, 0.0, 0.0)])
    nodes = [
        named(b"FxSphereEmitter", b"burst_emit", [
            prop(ANIM, FREQ, anim(0.0)), prop(ANIM, LIFESPAN, anim(2.0)),
            prop(ANIM, COUNT, anim_keys([(0.05, 4.0, 0.0, 0.0), (0.1, 2.5, 0.0, 0.0),
                                         (0.2, 6.0, 0.0, 0.0)])),
            prop(ANIM, MAXP, anim(7.0)), prop(2, INITIAL, u32(2)),
            prop(RAMP, EMIT_LOD, nested11(4, [(0.0, single(1, [(0.0, 0.5), (1.0, 1.5)]))])),
            prop(3, LIFE, f32(0.4)), prop(0x105, XFORM, transform(0, 0, 0, move))]),
        named(b"FLBeamAppearance", b"burst_beam", []),
        named(b"FLBeamField", b"burst_beamfield", []),
        named(b"FxConeEmitter", b"dust_emit", [
            prop(ANIM, FREQ, anim(30.0)), prop(ANIM, LIFESPAN, anim(0.3)),
            prop(ANIM, PRESSURE, anim(4.0)), prop(ANIM, CONE_MAX_S, anim(60.0))]),
        named(b"FLDustAppearance", b"dust_app", []),
        named(b"FLDustField", b"dust_field", [prop(ANIM, DUST_RADIUS, anim(4.0))]),
        named(b"FxOrientedAppearance", b"dust_oriented", []),
        named(b"FxSphereEmitter", b"dust_feed", [
            prop(ANIM, FREQ, anim(45.0)), prop(ANIM, LIFESPAN, anim(20.0)),
            prop(ANIM, PRESSURE, anim(12.0)), prop(ANIM, SPHERE_MAX_R, anim(0.3))]),
    ]
    effects = [effect(b"groups", [
        ref(b"group", ROOT, 1, kind=1),
        ref(b"burst_emit", 1, 2), ref(b"burst_beam", 1, 3), ref(b"burst_beamfield", 1, 4),
        (2, OVERRIDE_FOLDER, ROOT, 5),
        ref(b"dust_emit", 5, 6), ref(b"dust_app", 5, 7), ref(b"dust_field", 5, 8),
        ref(b"dust_oriented", ROOT, 9), ref(b"dust_feed", 5, 10)],
        [(2, 3), (3, 4), (6, 7), (7, 8), (6, 9), (10, 7)])]
    ops = [ale_op(nodes, effects), "seed 51966 0", "effect 0 " + hx(b"groups"), "dump 0",
           "outer 0 " + f(0.5), "lod 0 " + f(0.75),
           "xform 0 0 " + xf((0, 1, 0, -1, 0, 0, 0, 0, 1), (1, 2, 3)),
           "xform 0 1 " + xf((1, 0, 0, 0, 1, 0, 0, 0, 1), (0, 0.25, 0))]
    ops += steps(0, 3) + ["dump 0"] + steps(0, 6, f(0.05)) + ["dump 0", "done 0 1", "done 0 2",
                                                              "done 0 7", "age 0 " + f(0.0)]
    ops += steps(0, 2) + ["dump 0", "release 0", "pools"]
    return ops


def sim_textures():
    """Texture frames: a common-frame lookup, an advancing playback, a texture
    the library does not find, and the handle released with the node."""
    nodes = [
        named(b"FxSphereEmitter", b"tex_emit", [prop(ANIM, FREQ, anim(10.0)),
                                                prop(ANIM, LIFESPAN, anim(0.25))]),
        named(b"FxBasicAppearance", b"tex_frame", [
            texture_name(b"anim.tga"), bool_prop(USE_COMMON_FRAME),
            prop(ANIM, COMMON_FRAME, anim_keys([(0.0, 0.0, 0.0, 0.0), (1.0, 1.0, 0.0, 0.0)]))]),
        named(b"FxRectAppearance", b"tex_play", [texture_name(b"play.tga")]),
        named(b"FxPerpAppearance", b"tex_missing", [texture_name(b"missing.tga")]),
        named(b"FLBeamAppearance", b"tex_beam", [texture_name(b"anim.tga")]),
    ]
    effects = [effect(b"textured", [ref(b"tex_frame", ROOT, 1), ref(b"tex_play", ROOT, 2),
                                    ref(b"tex_missing", ROOT, 3), ref(b"tex_beam", ROOT, 4),
                                    ref(b"tex_emit", ROOT, 5)],
                      [(5, 1), (5, 2)])]
    ops = [ale_op(nodes, effects),
           "tex %08x 4" % name_crc(b"anim.tga"), "tex %08x 3" % name_crc(b"play.tga"),
           "effect 0 " + hx(b"textured")]
    ops += steps(0, 4, f(0.2)) + ["dump 0", "stop 0 7", "release 0"]
    return ops


def sim_errors():
    """An unknown effect, a record whose node is missing, and nodes made
    and released without ever updating."""
    nodes = [named(b"FxBasicAppearance", b"lonely", [])]
    effects = [effect(b"broken", [ref(b"lonely", ROOT, 1), ref(b"nowhere", ROOT, 2)], []),
               effect(b"idle", [ref(b"lonely", ROOT, 1)], [])]
    return [ale_op(nodes, effects), "effect 0 " + hx(b"absent"), "effect 1 " + hx(b"broken"),
            "release 1", "effect 2 " + hx(b"idle"), "dump 2", "done 2 4"] + steps(2, 1) + [
        "dump 2", "done 2 4", "release 2", "pools"]


def sim_list():
    return [" ; ".join(case()) for case in (sim_basic, sim_cone_motion, sim_cube_pool,
                                            sim_folders, sim_textures, sim_errors)]


def write(name, text, check):
    path = os.path.join(HERE, name)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != text.encode("latin-1"):
                sys.exit("%s is out of date; run make_particle_fixtures.py" % name)
        return
    with open(path, "wb") as out:
        out.write(text.encode("latin-1"))


def listing(title, lines):
    return "# P4.8 %s oracle inputs, written by make_particle_fixtures.py.\n" % title + "".join(
        line + "\n" for line in lines)


def main():
    check = "--check" in sys.argv[1:]
    write("particle_ramp.list", listing("particle ramp", ramp_list()), check)
    write("particle_sim.list", listing("particle sim", sim_list()), check)


if __name__ == "__main__":
    main()
