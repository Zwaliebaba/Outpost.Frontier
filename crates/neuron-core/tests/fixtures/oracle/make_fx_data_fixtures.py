"""Writes the oracle inputs of the fx_data suite (work package P1.7).

Everything here is synthetic: small UTF archives holding an
AlchemyNodeLibrary and an ALEffectLib stream built from the layouts in
crates/neuron-core/src/fx_data/, with made-up names and values chosen to
reach the loaders' rules: both versions, every property kind, owned and
unowned hashes, the hash and kind remaps, unnamed and duplicate nodes, the
collide-field alias, transforms, colour packing, record de-duplication,
unknown versions and truncation at many points. Run it with Python 3 from
any directory; it rewrites fx/ under this directory. The oracle harness
(CppSource/Oracle/oracle_fx_data.cpp) and the Rust test
tests/oracle_fx_data.rs read the same files.
"""

import os
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, 'fx')

V10 = 0x3F800000
V11 = 0x3F8CCCCD

# --- UTF container (the 0x2C layout of crates/neuron-core/src/utf/mod.rs) ---

TIMES = (0, 0, 0)


class File:
    def __init__(self, name, data):
        self.name, self.data = name, data


class Dir:
    def __init__(self, name, children):
        self.name, self.children = name, children


def utf(children):
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
            flags = 0x80 if isinstance(item, File) else 0x10
            struct.pack_into('<III', tree, at, nxt, name_offset(item.name), flags)
            struct.pack_into('<III', tree, at + 0x20, *TIMES)
            if isinstance(item, File):
                struct.pack_into('<I', tree, at + 0x10, len(data))
                struct.pack_into('<I', tree, at + 0x1C, len(item.data))
                data.extend(item.data)
            else:
                struct.pack_into('<I', tree, at + 0x10, place(item.children))
        return first

    struct.pack_into('<III', tree, 0, 0, name_offset(b'\\'), 0x10)
    struct.pack_into('<I', tree, 0x10, place(children))
    t = 0x38
    s = t + len(tree)
    d = s + len(strings)
    head = struct.pack('<14I', 0x20465455, 0x101, t, 0, 0, 0x2C, s, 0, 0, d, 0, 0, 0, 0)
    return bytes(head + tree + strings + data)


def ale(nodes=None, effects=None, extra=()):
    children = []
    if nodes is not None:
        children.append(Dir(b'AlchemyNodeLibrary', [File(b'AlchemyNodeLibrary', nodes)]))
    if effects is not None:
        children.append(Dir(b'ALEffectLib', [File(b'ALEffectLib', effects)]))
    children.extend(extra)
    return utf(children)


# --- Stream pieces ---

def u8(v):
    return struct.pack('<B', v & 0xFF)


def u16(v):
    return struct.pack('<H', v & 0xFFFF)


def u32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def f32(v):
    return struct.pack('<f', v)


def fb(bits):
    return u32(bits)


class Bits(int):
    """A float given as its bit pattern."""


def fv(value):
    """A float, or the bit pattern of a Bits value."""
    return fb(value) if isinstance(value, Bits) else f32(value)


def string(text, length=None):
    """The chunk string: a 16-bit length (strlen + 1), the bytes with their
    NUL, one pad byte after an odd length."""
    body = text + b'\0'
    n = len(body) if length is None else length
    out = u16(n) + body[:n] if n <= len(body) else u16(n) + body + bytes(n - len(body))
    if n % 2 == 1:
        out += b'\x5a'
    return out


def single(mode, keys):
    out = u8(mode) + u8(len(keys))
    for t, v in keys:
        out += fv(t) + fv(v)
    return out


def color(mode, keys):
    out = u8(mode) + u8(len(keys))
    for t, rgb in keys:
        out += fv(t) + b''.join(fv(c) for c in rgb)
    return out


def animated(weight, loop, keys):
    out = fv(weight) + u16(loop) + u16(len(keys))
    for t, v, i, o in keys:
        out += fv(t) + fv(v) + fv(i) + fv(o)
    return out


def nested11(mode, keys):
    """A 1.1 nested ramp: keys are (time, child block bytes)."""
    out = u8(mode) + u8(len(keys))
    for t, child in keys:
        out += fv(t) + child
    return out


def transform(a, b, c, channels=None):
    """Header bytes a, b, c and a fourth byte that is negative when the nine
    channels (1.1 nested animated ramps) follow."""
    if channels is None:
        return bytes([a & 0xFF, b & 0xFF, c & 0xFF, 0x01])
    assert len(channels) == 9
    return bytes([a & 0xFF, b & 0xFF, c & 0xFF, 0x80]) + b''.join(channels)


def prop(kind, hashv, value=b''):
    return u16(kind) + u32(hashv) + value


def node(cls, props, end=True):
    return string(cls) + b''.join(props) + (u16(0) if end else b'')


def library(nodes, version=V11, count=None):
    return u32(version) + u32(len(nodes) if count is None else count) + b''.join(nodes)


def effect(name, refs, pairs, header=(0, 0, 0, 0), version=V11, ref_count=None, pair_count=None):
    out = string(name)
    if version is not None:
        out += b''.join(u32(w) for w in header)
    out += u32(len(refs) if ref_count is None else ref_count)
    for r in refs:
        out += b''.join(u32(w) for w in r)
    out += u32(len(pairs) if pair_count is None else pair_count)
    for p in pairs:
        out += b''.join(u32(w) for w in p)
    return out


def effects_lib(effects, version=V11, count=None):
    return u32(version) + u32(len(effects) if count is None else count) + b''.join(effects)


# Property hashes (the class tables in crates/neuron-core/src/fx_data/props.rs).
NAME = 0xEFA8CE01
CLASS_NAME = 0xF54EF296
LIFE = 0xF27FDE7D
XFORM = 0xE13A59A1
APP_LOD = 0x0C53B008
EMIT_LOD = 0xFE077E05
INITIAL = 0x0F9A9D52
FREQ = 0x023C350C
MAXP = 0xF5042852
COLOR = 0x048767E8
ALPHA = 0x04FC9016
TEXNAME = 0x1BA23359
BLEND = 0x1506EB6C
COMMON_TEX = 0xF52B8DD5
TRI_TEX = 0xF863872E
QUAD_TEX = 0xF87B5FD5
NAN = Bits(0x7FC00001)


def flat_anim(v):
    """A 1.1 nested animated ramp with one keyless value of weight v."""
    return nested11(4, [(0.0, animated(v, 0, []))])


def channels(values):
    return [flat_anim(v) for v in values]


# --- The archives ---

def basic():
    """One node of every class with typical properties, a 1.1 effect library."""
    nodes = [
        node(b'FxNode', [prop(0x103, NAME, string(b'folder')), prop(3, LIFE, f32(2.5))]),
        node(b'FxSphereEmitter', [
            prop(0x103, NAME, string(b'sphere')),
            prop(2, INITIAL, u32(7)),
            prop(0x200, EMIT_LOD, nested11(4, [(0.0, single(1, [(0.0, 1.0), (1.0, 0.25)]))])),
            prop(0x202, FREQ, nested11(4, [(0.0, animated(50.0, 0, [])), (2.0, animated(1.0, 16, [
                (0.0, 0.0, 0.0, 0.0), (1.0, 10.0, 1.5, -1.5), (1.0, 11.0, 0.0, Bits(0xFFFFFFFF))]))])),
            prop(0x202, 0x1874DE74, flat_anim(3.0)),
        ]),
        node(b'FxConeEmitter', [
            prop(0x103, NAME, string(b'cone')),
            prop(0x202, 0xFA4B5E41, flat_anim(45.0)),
            prop(0x202, 0x0AB180C5, flat_anim(-2.0)),
            prop(0x105, XFORM, transform(1, 2, 3)),
        ]),
        node(b'FxCubeEmitter', [
            prop(0x103, NAME, string(b'cube')),
            prop(0x202, 0x14B74EB2, flat_anim(0.5)),
            prop(0x202, 0x0681233E, flat_anim(0.75)),
        ]),
        node(b'FxAirField', [prop(0x103, NAME, string(b'air')), prop(0x202, 0xE5E3524C, flat_anim(-4.0))]),
        node(b'FxGravityField', [prop(0x103, NAME, string(b'gravity')), prop(0x202, 0xE02B8BD4, flat_anim(1.0))]),
        node(b'FxColideField', [
            prop(0x103, NAME, string(b'collide')),
            prop(0x202, 0xE462B789, flat_anim(0.1)),
            prop(0x202, 0x138BDB51, flat_anim(20.0)),
            prop(0x202, 0x16FF3474, flat_anim(30.0)),
        ]),
        node(b'FxRadialField', [
            prop(0x103, NAME, string(b'radial')),
            prop(0x200, 0x1A433168, nested11(2, [(0.0, single(3, [])), (0.5, single(0, [(0.0, -1.0)]))])),
        ]),
        node(b'FxTurbulenceField', [prop(0x103, NAME, string(b'turbulence')), prop(0x202, 0x1CEAC6D1, flat_anim(2.0))]),
        node(b'FLDustField', [prop(0x103, NAME, string(b'dustfield')), prop(0x202, 0x1874DE74, flat_anim(9.0))]),
        node(b'FLBeamField', [prop(0x103, NAME, string(b'beamfield')), prop(3, LIFE, f32(-1.0))]),
        node(b'FxBasicAppearance', [
            prop(0x103, NAME, string(b'basic')),
            prop(0x8001, TRI_TEX), prop(0x8001, QUAD_TEX), prop(0x0001, QUAD_TEX),
            prop(0x201, COLOR, nested11(4, [(0.0, color(1, [(0.0, (1.0, 0.5, 0.0)), (1.0, (0.2, 0.4, 0.6))])),
                                            (0.5, color(4, []))])),
            prop(0x200, ALPHA, nested11(4, [(0.0, single(4, [(0.0, 0.0), (0.5, 1.0), (1.0, 0.0)]))])),
            prop(0x103, TEXNAME, string(b'Fx\\Sample.tga')),
            prop(0x104, BLEND, u32(5) + u32(6)),
            prop(0x202, COMMON_TEX, flat_anim(0.125)),
        ]),
        node(b'FxParticleAppearance', [
            prop(0x103, NAME, string(b'particle')),
            prop(0x103, 0x0EC7A290, string(b'trail_fx')),
            prop(0x8001, 0x049A6DBE),
            prop(0x200, APP_LOD, nested11(4, [(0.0, single(4, [(0.0, 2.0)]))])),
        ]),
        node(b'FxRectAppearance', [prop(0x103, NAME, string(b'rect')), prop(0x8001, 0xFB7958C1),
                                   prop(0x200, 0xE48DDAE2, nested11(4, [(0.0, single(4, [(0.0, 3.0)]))]))]),
        node(b'FxPerpAppearance', [prop(0x103, NAME, string(b'perp')), prop(0x202, COMMON_TEX, flat_anim(0.5))]),
        node(b'FxMeshAppearance', [
            prop(0x103, NAME, string(b'mesh')),
            prop(2, 0x0B7EE41C, u32(0x12345678)),
            prop(0x103, 0x18DADCA8, string(b'debris_a')),
            prop(0x8001, 0x001FC112),
            prop(0x105, 0xF0004D58, transform(4, 3, 5, channels([0, 0, 1, 0, 90, 0, 2, 2, 2]))),
        ]),
        node(b'FxOrientedAppearance', [prop(0x103, NAME, string(b'oriented')),
                                       prop(0x105, 0xF0004D58, transform(0, 0, 0, channels([0] * 6 + [1] * 3)))]),
        node(b'FLDustAppearance', [prop(0x103, NAME, string(b'dustapp')), prop(0x8001, 0xF3D52EE4)]),
        node(b'FLBeamAppearance', [
            prop(0x103, NAME, string(b'beamapp')),
            prop(0x8001, 0x08508C35), prop(0x8001, 0x03503B61), prop(0x0001, QUAD_TEX),
            prop(0x105, 0x0BA0B3BB, transform(15, 15, 15, channels([1, 2, 3, 4, 5, 6, 7, 8, 9]))),
        ]),
    ]
    effects = [
        effect(b'fx_basic', [(0, 0x11111111, 0x8000, 1), (1, 0xEE223B51, 0x8000, 2), (0, 0x22222222, 2, 3)],
               [(1, 3), (3, 4)], header=(1, 2, 3, 0xDEADBEEF)),
        effect(b'fx_auto', [(0, 0xAAAA0001, 0x8000, 0), (0, 0xAAAA0002, 1, 0), (0, 0xAAAA0003, 0x8000, 7),
                            (0, 0xAAAA0004, 7, 0), (0, 0xAAAA0003, 0x8000, 7), (0, 0xAAAA0005, 0x8000, 7),
                            (0, 0xAAAA0006, 0x8000, 0x80000000), (0, 0xAAAA0007, 0x8000, 0)],
               [(1, 2), (1, 2), (2, 1), (0, 0)]),
        effect(b'', [], []),
        effect(b'fx_basic', [(0, 1, 2, 3)], []),
    ]
    return ale(library(nodes), effects_lib(effects))


def version10():
    """Both libraries in version 1.0: the flat kinds become nested ones with
    one key at time 0 and no mode; the effects have no header."""
    nodes = [
        node(b'FxSphereEmitter', [
            prop(0x103, NAME, string(b'old_sphere')),
            prop(0x100, 0xEE1429BA, single(2, [(0.0, 0.5)])),
            prop(0x102, FREQ, animated(25.0, 0, [(0.0, 1.0, 0.0, 0.0)])),
            prop(0x105, XFORM, transform(4, 3, 5, [animated(1.0, 0, [])] * 9)),
        ]),
        node(b'FxBasicAppearance', [
            prop(0x103, NAME, string(b'old_basic')),
            prop(0x101, COLOR, color(1, [(0.0, (0.0, 1.0, 0.0))])),
            prop(0x100, 0xF0382D6B, single(4, [])),
            prop(0x101, COLOR, color(3, [])),
            prop(0x8100, ALPHA, single(1, [(0.0, 0.75)])),
        ]),
    ]
    effects = [
        effect(b'old_effect', [(0, 0x12345678, 0x8000, 1)], [(1, 1)], version=None),
        effect(b'old_second', [], [(5, 6)], version=None),
    ]
    return ale(library(nodes, version=V10), effects_lib(effects, version=V10))


def names():
    """Unnamed nodes, duplicate names, odd and even lengths, a 256-byte name,
    then a class name longer than 256 bytes, which ends the load."""
    nodes = [
        node(b'FxNode', []),
        node(b'FxNode', [prop(0x103, NAME, string(b''))]),
        node(b'FxNode', [prop(0x103, NAME, string(b'x'))]),
        node(b'FxNode', [prop(0x103, NAME, string(b'x')), prop(3, LIFE, f32(9.0))]),
        node(b'FxNode', [prop(0x103, NAME, string(b'ab')), prop(0x103, NAME, string(b'abc'))]),
        node(b'FxNode', [prop(0x103, NAME, string(b'n' * 255))]),
        node(b'FxNode', [prop(0x103, NAME, string(b'unnamed#1'))]),
        node(b'FxNode', [prop(0x103, NAME, u16(0))]),
        node(b'F' * 300, []),
        node(b'FxNode', [prop(0x103, NAME, string(b'never'))]),
    ]
    effects = [
        effect(b'e' * 255, [], []),
        u16(0x200) + effect(b'after_long', [], []),
        effect(b'odd', [], []),
        effect(b'even', [], []),
    ]
    return ale(library(nodes), effects_lib(effects, count=5))


def unknown_class():
    nodes = [
        node(b'FxNode', [prop(0x103, NAME, string(b'first'))]),
        node(b'FxImaginaryEmitter', [prop(0x103, NAME, string(b'second'))]),
        node(b'FxNode', [prop(0x103, NAME, string(b'third'))]),
    ]
    return ale(library(nodes), effects_lib([effect(b'still_loaded', [(0, 1, 0x8000, 1)], [])]))


def unknown_props():
    """Records whose hash or kind the class does not take. Unowned
    scalars, strings and pairs are read and dropped; an unowned nested scalar
    or animated ramp is read into a throw-away ramp; an unowned transform,
    colour ramp, flat ramp kind or unknown kind reads nothing, so the bytes
    that follow are read as the next record."""
    filler_name = prop(0x103, NAME, string(b'hidden'))
    nodes = [
        node(b'FxSphereEmitter', [
            prop(0x103, NAME, string(b'probe')),
            prop(0x8001, 0x12345678),
            prop(2, 0x12345678, u32(1)),
            prop(3, 0x12345678, f32(1.0)),
            prop(0x103, 0x12345678, string(b'dropped')),
            prop(0x104, 0x12345678, u32(1) + u32(2)),
            prop(0x103, CLASS_NAME, string(b'FxRenamed')),
            prop(0x200, 0x12345678, nested11(4, [(0.0, single(4, [(0.0, 5.0)]))])),
            prop(0x202, 0x12345678, flat_anim(5.0)),
            prop(0x105, 0x12345678), filler_name,
            prop(0x201, 0x12345678), prop(3, LIFE, f32(4.0)),
            prop(0x100, 0x12345678), prop(2, INITIAL, u32(3)),
            prop(0x300, 0x12345678), prop(0x8001, 0x12345678),
            prop(3, 0x9999, f32(0.0)),
        ]),
        node(b'FxCubeEmitter', [
            prop(0x103, NAME, string(b'probe2')),
            prop(0x202, 0x1874DE74, flat_anim(8.0)),
            prop(0x202, 0x1C73D16B, flat_anim(8.0)),
            prop(0x202, 0xFD22A11C, flat_anim(6.0)),
        ]),
        node(b'FxBasicAppearance', [
            prop(0x103, NAME, string(b'probe3')),
            prop(0x202, ALPHA, flat_anim(1.0)),
            prop(0x200, COMMON_TEX, nested11(4, [(0.0, single(4, [(0.0, 7.0)]))])),
            prop(0x105, 0xF0004D58), prop(0x103, NAME, string(b'hidden3')),
        ]),
    ]
    return ale(library(nodes), effects_lib([]))


def ramps():
    """Key order for equal and NaN times, colour packing at the edges and the
    animated-value failure that abandons a ramp mid-stream."""
    nodes = [
        node(b'FxBasicAppearance', [
            prop(0x103, NAME, string(b'order')),
            prop(0x200, ALPHA, nested11(7, [
                (1.0, single(4, [(1.0, 1.0), (0.0, 2.0), (1.0, 3.0), (NAN, 4.0), (-0.0, 5.0), (0.0, 6.0)])),
                (NAN, single(4, [(0.0, 0.0)])),
                (0.0, single(4, [(0.0, 0.0)])),
                (1.0, single(6, [(0.0, 1.0)])),
            ])),
            prop(0x201, COLOR, nested11(4, [(0.0, color(4, [
                (0.0, (-0.1, 1.0, 0.0)), (0.1, (1.5, 2.0, 255.0)), (0.2, (NAN, Bits(0x7F800000), Bits(0xFF800000))),
                (0.3, (0.999, 0.5, 0.003)), (0.4, (1e10, -1e10, 16777216.0))]))])),
            prop(0x202, COMMON_TEX, nested11(4, [(0.0, animated(1.0, 1, [
                (1.0, 1.0, 0.0, 0.0), (1.0, 2.0, 0.0, 0.0), (0.0, 3.0, 0.0, 0.0), (NAN, 4.0, 0.0, 0.0),
                (0.5, 5.0, 0.0, 0.0)]))])),
        ]),
        node(b'FxConeEmitter', [
            prop(0x103, NAME, string(b'channels')),
            prop(0x105, XFORM, transform(-1, 0x7F, 0x10, [
                nested11(4, []), flat_anim(0.0), nested11(4, [(0.0, animated(9.0, 0, [(0.0, 0.0, 0.0, 0.0)]))]),
                nested11(4, [(0.0, animated(0.0, 0, [(0.0, 1.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0)]))]),
                nested11(4, [(0.0, animated(-0.0, 0, []))]), flat_anim(1.0),
                flat_anim(1.0), nested11(4, [(0.0, animated(5.0, 0, [])), (1.0, animated(1.0, 0, []))]),
                nested11(4, [(0.0, animated(5.0, 0, [(0.0, 1.0, 0.0, 0.0)]))]),
            ])),
        ]),
        node(b'FxSphereEmitter', [
            prop(0x103, NAME, string(b'abandoned')),
            # The second key's value has a loop type but no key: SetLoopType
            # fails and the ramp read stops after that value; the third key's
            # bytes are then read as property records.
            prop(0x202, FREQ, nested11(4, [(0.0, animated(2.0, 0, [])), (1.0, animated(3.0, 5, [])),
                                           (2.0, animated(4.0, 0, []))])),
            prop(3, LIFE, f32(6.0)),
        ]),
    ]
    return ale(library(nodes), effects_lib([]))


def bad_version():
    """A node library of version 2.0: nested ramps reset and read nothing
    ("Unknown version"); a transform reads its header and stops at its first
    channel. An effect library of version 2.0 has headers (2.0 >= 1.1)."""
    nodes = [
        node(b'FxSphereEmitter', [
            prop(0x103, NAME, string(b'v2')),
            prop(0x202, FREQ), prop(3, LIFE, f32(7.0)),
            prop(0x200, EMIT_LOD), prop(2, INITIAL, u32(9)),
            prop(0x105, XFORM, transform(1, 1, 1, None)[:3] + b'\x80'), prop(0x103, NAME, string(b'v2b')),
        ]),
    ]
    effects = [effect(b'v2_effect', [(0, 5, 0x8000, 1)], [], header=(9, 8, 7, 6))]
    return ale(library(nodes, version=0x40000000), effects_lib(effects, version=0x40000000))


def nan_version():
    """An effect library whose version is NaN has no headers (NaN is not at
    least 1.1)."""
    effects = [effect(b'nan_effect', [(0, 5, 0x8000, 1)], [(1, 1)], version=None)]
    return ale(library([], version=NAN), effects_lib(effects, version=NAN))


def counts():
    """Counts far above what the streams hold. The node loader stops at the
    first short name; the effect loader skips an effect whose name it cannot
    read (here a length above 0x100, which consumes only the length) and
    runs every remaining count as a failed read at the end of the stream."""
    nodes = [node(b'FxNode', [prop(0x103, NAME, string(b'counted'))])]
    effects = (effect(b'first', [(0, 1, 0x8000, 1)], [])
               + u16(0x101) + u16(0x101)
               + effect(b'after_skips', [(0, 1, 0x8000, 0)], [(1, 1)]))
    return ale(library(nodes, count=0x7FFFFFFF), effects_lib([effects], count=0x00100000))


def missing():
    yield 'missing_both.ale', ale()
    yield 'missing_leaf.ale', utf([Dir(b'AlchemyNodeLibrary', [File(b'Other', b'\0' * 8)]),
                                    Dir(b'ALEffectLib', [File(b'ALEffectLib', effects_lib([]))])])
    yield 'empty_leaves.ale', ale(b'', b'')
    yield 'lower_case.ale', utf([Dir(b'alchemynodelibrary', [File(b'ALCHEMYNODELIBRARY', library(
        [node(b'FxNode', [prop(0x103, NAME, string(b'cased'))])]))]),
        Dir(b'aleffectlib', [File(b'aleffectlib', effects_lib([effect(b'cased', [], [])]))])])


def truncations():
    """A valid file cut at many points of each stream."""
    nodes = library([
        node(b'FxSphereEmitter', [
            prop(0x103, NAME, string(b'cut')),
            prop(2, INITIAL, u32(4)),
            prop(0x202, FREQ, nested11(4, [(0.0, animated(5.0, 0, [(0.0, 1.0, 2.0, 3.0)])),
                                           (1.0, animated(6.0, 0, []))])),
            prop(0x105, XFORM, transform(1, 2, 3, channels([1] * 9))),
        ]),
        node(b'FxBasicAppearance', [prop(0x103, NAME, string(b'cut2')),
                                    prop(0x201, COLOR, nested11(4, [(0.0, color(4, [(0.0, (1, 1, 1))]))]))]),
    ])
    effects = effects_lib([
        effect(b'cut_effect', [(0, 1, 0x8000, 1), (0, 2, 1, 2)], [(1, 2), (2, 1)], header=(1, 2, 3, 4)),
        effect(b'cut_second', [(0, 3, 0x8000, 1)], [], header=(5, 6, 7, 8)),
    ])
    for cut in (0, 3, 4, 7, 8, 10, 20, 26, 30, 40, 55, 60, 75, 90, 120, 200, 300, len(nodes) - 3, len(nodes) - 1):
        if cut < len(nodes):
            yield 'cut_nodes_%03d.ale' % cut, ale(nodes[:cut], effects)
    for cut in (0, 4, 7, 8, 12, 20, 25, 33, 40, 44, 60, 70, 78, 85, len(effects) - 1):
        if cut < len(effects):
            yield 'cut_effects_%03d.ale' % cut, ale(nodes, effects[:cut])


def archives():
    yield 'basic.ale', basic()
    yield 'version10.ale', version10()
    yield 'names.ale', names()
    yield 'unknown_class.ale', unknown_class()
    yield 'unknown_props.ale', unknown_props()
    yield 'ramps.ale', ramps()
    yield 'bad_version.ale', bad_version()
    yield 'nan_version.ale', nan_version()
    yield 'counts.ale', counts()
    yield from missing()
    yield from truncations()


def main():
    if os.path.isdir(ROOT):
        shutil.rmtree(ROOT)
    os.makedirs(ROOT)
    listed = []
    for name, data in archives():
        with open(os.path.join(ROOT, name), 'wb') as f:
            f.write(data)
        listed.append(name)
    with open(os.path.join(ROOT, 'files.list'), 'w', newline='\n') as f:
        f.write('# The .ale archives the fx_data suite loads, in order.\n')
        f.write('\n'.join(listed) + '\n')


if __name__ == '__main__':
    main()
