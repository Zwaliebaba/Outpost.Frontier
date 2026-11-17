"""Writes the oracle inputs of the scene suites (work package P1.8).

Everything here is synthetic: small UTF archives in the layouts
CppSource/SceneRuntime/SceneRuntime.cpp reads (Cmpnd\\{Root, Part_*}\\{Object
name, File name}, Cmpnd\\Cons\\<type>, Hardpoints\\<type>\\<name>\\{Position,
Orientation, Axis, Min, Max}, Animation\\Script\\<name>\\<map>), and the lists
the suites walk. Run it with Python 3 from any directory; it rewrites scene/
under this directory. The oracle harness (CppSource/Oracle/oracle_scene.cpp)
and the Rust test tests/oracle_scene.rs read the same files.

No input below makes the reference misbehave: no Cons file of an unknown
type (a division by zero), no constraint that closes a cycle or connects a
part twice (an endless loop, a corrupted list), no track without a channel,
no stamped channel sampled before its first stamp, and no event-key range
whose bounds no frame sets.
"""

import os
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, 'scene')

MAGIC = 0x20465455
VERSION = 0x101
HEADER = 0x38
TIMES = (0x34851A3C, 0x34851A3C, 0x34851A3C)


def write(name, data):
    path = os.path.join(HERE, *name.split('/'))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def write_lines(name, header, lines):
    write(name, ('\n'.join(['# ' + header] + lines) + '\n').encode())


class File:
    def __init__(self, name, data):
        self.name, self.data = name, data


class Dir:
    def __init__(self, name, children):
        self.name, self.children = name, children


def build(children):
    """A 0x2C archive: siblings chained, file data in depth-first order."""
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
    struct.pack_into('<III', tree, 0x20, *TIMES)
    struct.pack_into('<I', tree, 0x10, place(children))
    t = HEADER
    s = t + len(tree)
    d = s + len(strings)
    header = struct.pack('<14I', MAGIC, VERSION, t, 0, 0, 0x2C, s, 0, 0, d, 0, 0, 0, 0)
    return bytes(header + tree + strings + data)


def f32(*values):
    return struct.pack('<%df' % len(values), *values)


def bits(value):
    return '%08x' % struct.unpack('<I', struct.pack('<f', value))[0]


def cstr(text, size=None):
    raw = text.encode() + b'\0'
    if size is not None:
        raw = raw.ljust(size, b'\0')[:size]
    return raw


# --- compounds --------------------------------------------------------------

def part(entry, obj, file_name):
    children = []
    if obj is not None:
        children.append(File(b'Object name', cstr(obj)))
    if file_name is not None:
        children.append(File(b'File name', cstr(file_name)))
    children.append(File(b'Index', struct.pack('<I', 0)))
    return Dir(entry.encode(), children)


ROT_Z90 = (0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
ROT_SKEW = (0.36, 0.48, -0.8, -0.8, 0.6, 0.0, 0.48, 0.64, 0.6)
IDENT = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


def record(parent, child, words, size):
    data = cstr(parent, 0x40) + cstr(child, 0x40) + f32(*words)
    return data.ljust(size, b'\0')[:size]


def fix(parent, child, offset, rot):
    return record(parent, child, list(offset) + list(rot), 0xB0)


def rev(parent, child, origin, offset, rot, axis, lo, hi):
    return record(parent, child, list(origin) + list(offset) + list(rot) + list(axis) + [lo, hi], 0xD0)


def cyl(parent, child, origin, offset, rot, axis, lo, hi, alo, ahi):
    words = list(origin) + list(offset) + list(rot) + list(axis) + [lo, hi, alo, ahi]
    return record(parent, child, words, 0xD8)


def sphere(parent, child, origin, offset, rot, limits):
    words = list(origin) + list(offset) + list(rot) + list(limits)
    return record(parent, child, words, 0xD4)


def hardpoint(name, kind, pos=None, orient=None, axis=None, lo=None, hi=None, short=False):
    children = []
    if pos is not None:
        children.append(File(b'Position', f32(*pos)[:8] if short else f32(*pos)))
    if orient is not None:
        children.append(File(b'Orientation', f32(*orient)))
    if axis is not None:
        children.append(File(b'Axis', f32(*axis)))
    if lo is not None:
        children.append(File(b'Min', f32(lo)))
    if hi is not None:
        children.append(File(b'Max', f32(hi)))
    return Dir(name.encode(), children)


def mesh_node(name, hardpoints=None, extra=None):
    children = [File(b'VMeshPart', b'\0' * 4)]
    if hardpoints is not None:
        children.append(Dir(b'Hardpoints', hardpoints))
    if extra:
        children.extend(extra)
    return Dir(name.encode(), children)


def ship_tree(prefix='ship'):
    hp_root = [
        Dir(b'Fixed', [
            hardpoint('HpEngine01', 0, (0.0, -1.5, 12.25), IDENT),
            hardpoint('hpMount', 0, (1.0, 2.0, 3.0), ROT_Z90),
            hardpoint('HpShort', 0, (4.0, 5.0, 6.0), IDENT, short=True),
            hardpoint('HpNoOrient', 0, (7.0, 8.0, 9.0)),
            File(b'NotADir', b'x'),
        ]),
        Dir(b'Revolute', [
            hardpoint('HpTurret01', 2, (0.5, 1.25, -2.0), ROT_SKEW, (0.0, 1.0, 0.0), -0.75, 1.5),
            hardpoint('HpNoLimits', 2, (0.0, 0.0, 0.0), IDENT, (1.0, 0.0, 0.0)),
        ]),
        Dir(b'Prismatic', [
            hardpoint('HpSlide', 1, (2.0, 0.0, 0.0), IDENT, (0.0, 0.0, 1.0), 0.0, 3.0),
        ]),
        Dir(b'Swivel', [
            hardpoint('HpOdd', 7, (1.0, 1.0, 1.0), IDENT, (1.0, 0.0, 0.0), 1.0, 2.0),
        ]),
        File(b'Loose_file', b'old layout'),
    ]
    hp_a = [Dir(b'Fixed', [hardpoint('HpEngine01', 0, (9.0, 9.0, 9.0), IDENT),
                           hardpoint('HPENGINE01', 0, (8.0, 8.0, 8.0), IDENT)])]
    cmpnd = Dir(b'Cmpnd', [
        part('Root', 'Root', prefix + '_root.3db'),
        part('Part_wing', 'wing', prefix + '_wing.3db'),
        part('Part_turret', 'turret', prefix + '_turret.3db'),
        part('Part_gun', 'gun', prefix + '_gun.3db'),
        part('Part_slide', 'slide', prefix + '_slide.3db'),
        part('Part_ball', 'ball', prefix + '_ball.3db'),
        part('Part_free', 'free', prefix + '_wing.3db'),
        part('Part_trans', 'trans', prefix + '_trans.3db'),
        part('Part_cyl', 'cyl', prefix + '_cyl.3db'),
        part('Part_ext', 'ext', 'scene/external.3db'),
        part('Part_long', 'L' * 80, prefix + '_long.3db'),
        part('Part_missing', 'missing', 'nowhere.3db'),
        part('Part_noobject', None, prefix + '_wing.3db'),
        part('Part_nofile', 'nofile', None),
        part('Part_dup', 'wing', prefix + '_trans.3db'),
        Dir(b'Cons', [
            File(b'Fix', fix('Root', 'wing', (1.5, 0.0, -2.0), ROT_Z90)
                 + fix('wing', 'ext', (0.0, 0.25, 0.0), IDENT)
                 + fix('Root', 'nobody', (0.0, 0.0, 0.0), IDENT)
                 + b'\x01' * 0x30),
            File(b'rev', rev('Root', 'turret', (0.0, 2.0, 1.0), (0.0, -0.5, 0.0), IDENT,
                             (0.0, 1.0, 0.0), -1.0, 1.0)
                 + rev('turret', 'gun', (0.25, 0.5, 0.75), (0.1, 0.2, 0.3), ROT_SKEW,
                       (1.0, 0.0, 0.0), -0.5, 2.0)),
            File(b'Pris', rev('Root', 'slide', (0.0, 0.0, 3.0), (0.0, 0.0, 0.0), ROT_Z90,
                              (0.0, 0.0, 1.0), 0.0, 4.0)),
            File(b'Sphere', sphere('Root', 'ball', (1.0, 1.0, 1.0), (0.0, 0.0, -1.0), IDENT,
                                   (-1.0, 1.0, -0.5, 0.5, -0.25, 0.25))),
            File(b'Loose', fix('Root', 'free', (3.0, 2.0, 1.0), ROT_SKEW)),
            File(b'TRANS', fix('Root', 'trans', (0.0, -3.0, 0.0), IDENT)),
            File(b'Cyl', cyl('Root', 'cyl', (2.0, 0.0, 0.0), (0.5, 0.0, 0.0), IDENT,
                             (0.0, 0.0, 1.0), -2.0, 2.0, -3.0, 3.0)),
            File(b'.hidden', b'\0' * 0xB0),
            Dir(b'Fix', []),
        ]),
    ])
    return [
        cmpnd,
        mesh_node(prefix + '_root.3db', hp_root),
        mesh_node(prefix + '_wing.3db', hp_a),
        mesh_node(prefix + '_turret.3db', [Dir(b'Revolute', [
            hardpoint('HpBarrel', 2, (0.0, 0.5, 1.0), IDENT, (0.0, 0.0, 1.0), -3.0, 3.0)])]),
        mesh_node(prefix + '_gun.3db'),
        mesh_node(prefix + '_slide.3db'),
        mesh_node(prefix + '_ball.3db'),
        mesh_node(prefix + '_trans.3db'),
        mesh_node(prefix + '_cyl.3db'),
        mesh_node(prefix + '_long.3db'),
    ]


def nested_tree():
    sub_cmpnd = Dir(b'Cmpnd', [
        part('Root', 'Root', 'sub_base.3db'),
        part('Part_arm', 'arm', 'sub_arm.3db'),
        Dir(b'Cons', [File(b'Rev', rev('Root', 'arm', (0.0, 1.0, 0.0), (0.0, 0.0, 0.0), IDENT,
                                       (0.0, 0.0, 1.0), -2.0, 2.0))]),
    ])
    return [
        Dir(b'Cmpnd', [
            part('Root', 'Root', 'nested_base.3db'),
            part('Part_sub', 'sub', 'nested_sub.cmp'),
            Dir(b'Cons', [File(b'Fix', fix('Root', 'sub', (0.0, 0.0, 5.0), ROT_Z90))]),
        ]),
        mesh_node('nested_base.3db', [Dir(b'Fixed', [hardpoint('HpTop', 0, (0.0, 1.0, 0.0), IDENT)])]),
        Dir(b'nested_sub.cmp', [sub_cmpnd,
                                mesh_node('sub_base.3db'),
                                mesh_node('sub_arm.3db', [Dir(b'Fixed', [
                                    hardpoint('HpTip', 0, (0.0, 2.0, 0.0), IDENT)])])]),
    ]


def nocons_tree():
    return [
        Dir(b'Cmpnd', [
            part('Root', 'zeta', 'nc_a.3db'),
            part('Part_b', 'alpha', 'nc_b.3db'),
            part('Part_c', 'mid', 'nc_c.3db'),
        ]),
        mesh_node('nc_a.3db'), mesh_node('nc_b.3db'), mesh_node('nc_c.3db'),
    ]


def simple_tree():
    return [
        File(b'VMeshPart', b'\0' * 4),
        Dir(b'Hardpoints', [
            Dir(b'Fixed', [hardpoint('HpA', 0, (1.0, 0.0, 0.0), ROT_SKEW)]),
            Dir(b'Revolute', [hardpoint('HpB', 2, (0.0, 1.0, 0.0), IDENT, (0.0, 1.0, 0.0), -1.0, 1.0)]),
        ]),
    ]


# --- animation --------------------------------------------------------------

def channel(count, interval, flags, frames):
    return Dir(b'Channel', [File(b'Header', struct.pack('<IfI', count, interval, flags)),
                            File(b'Frames', frames)])


def joint_map(name, parent, child, chan):
    children = []
    if parent is not None:
        children.append(File(b'Parent name', cstr(parent)))
    if child is not None:
        children.append(File(b'Child name', cstr(child)))
    children.append(chan)
    return Dir(name.encode(), children)


def scalar_frames(values, stamps=None):
    out = b''
    for i, v in enumerate(values):
        if stamps is not None:
            out += f32(stamps[i])
        out += f32(v)
    return out


def words_frames(rows, stamps=None):
    out = b''
    for i, row in enumerate(rows):
        if stamps is not None:
            out += f32(stamps[i])
        out += f32(*row)
    return out


QUATS = [(1.0, 0.0, 0.0, 0.0), (0.9238795, 0.0, 0.3826834, 0.0), (0.7071068, 0.0, 0.7071068, 0.0),
         (0.5, 0.5, 0.5, 0.5), (0.0, 1.0, 0.0, 0.0)]


def anim_tree():
    rev_vals = [0.0, 0.5, 1.5, -2.5, 3.0]
    script = Dir(b'Script', [
        Dir(b'Swing', [
            joint_map('Joint map 0', 'Root', 'turret', channel(5, 0.25, 1, scalar_frames(rev_vals))),
            joint_map('Joint map 1', 'turret', 'gun', channel(
                4, -1.0, 1, scalar_frames([0.25, -0.5, 1.75, 0.0], [0.0, 0.3, 0.45, 1.2]))),
            joint_map('Joint map 2', 'Root', 'slide', channel(3, 0.5, 1, scalar_frames([0.0, 5.0, 2.0]))),
            File(b'Root height', f32(1.75)),
        ]),
        Dir(b'Spin', [
            joint_map('Joint map', 'Root', 'ball', channel(5, 0.2, 4, words_frames(QUATS))),
            joint_map('Joint map b', 'Root', 'trans', channel(
                3, -1.0, 2, words_frames([(0.0, 0.0, 0.0), (1.0, 2.0, 3.0), (-1.0, 0.5, 0.25)],
                                         [0.0, 0.6, 0.9]))),
            joint_map('Joint map c', 'Root', 'free', channel(
                3, 0.4, 6, words_frames([(0.0, 0.0, 0.0) + QUATS[0], (1.0, -1.0, 0.5) + QUATS[2],
                                         (2.0, 0.0, -1.0) + QUATS[3]]))),
        ]),
        Dir(b'Move', [
            joint_map('Object map', 'Root', None, channel(
                4, 0.3, 6, words_frames([(0.0, 0.0, 0.0) + QUATS[0], (1.0, 0.0, 0.0) + QUATS[1],
                                         (2.0, 1.0, 0.0) + QUATS[2], (2.5, 1.5, -1.0) + QUATS[3]]))),
            joint_map('Joint map', 'Root', 'turret', channel(2, 0.6, 1, scalar_frames([0.0, 1.0]))),
        ]),
        Dir(b'Broken', [
            joint_map('Joint map 0', 'Root', 'cyl', channel(2, 0.5, 1, scalar_frames([0.0, 1.0]))),
            joint_map('Joint map 1', 'Root', 'nobody', channel(2, 0.5, 1, scalar_frames([0.0, 1.0]))),
            joint_map('Joint map 2', 'wing', 'turret', channel(2, 0.5, 1, scalar_frames([0.0, 1.0]))),
            joint_map('Joint map 3', 'Root', 'ball', channel(2, 0.5, 1, scalar_frames([0.0, 1.0]))),
            joint_map('Object map', 'nobody', None, channel(
                2, 0.5, 6, words_frames([(0.0, 0.0, 0.0) + QUATS[0], (1.0, 0.0, 0.0) + QUATS[1]]))),
            joint_map('Joint map 4', 'Root', 'turret', channel(3, 0.5, 1, scalar_frames([0.0, 1.0, 2.0]))),
            Dir(b'Event map', [File(b'Parent name', cstr('Root'))]),
        ]),
    ])
    return Dir(b'Animation', [script])


def anim_compound():
    tree = ship_tree('anim')
    tree.append(anim_tree())
    return tree


# --- sampling ---------------------------------------------------------------

def sample_tree():
    events = b''
    stamps = [0.0, 0.1, 0.1, 0.35, 0.5]
    for i, t in enumerate(stamps):
        events += f32(t) + struct.pack('<I', 4 * i)
    events += b'EVT0EVT1EVT2EVT3EVT4'
    uniform_events = b''.join(struct.pack('<I', 8 * i) for i in range(4)) + b'e' * 32
    return [
        Dir(b'uniform', channel(5, 0.25, 1, scalar_frames([0.0, 1.0, 2.0, 3.0, 4.0])).children),
        Dir(b'odd', channel(4, 0.1, 2, words_frames([(0.0, 0.0, 0.0), (1.0, 1.0, 1.0),
                                                     (2.0, 0.5, 0.0), (3.0, 0.0, -1.0)])).children),
        Dir(b'stamped', channel(4, -1.0, 4, words_frames(QUATS[:4], [0.0, 0.2, 0.2, 0.7])).children),
        Dir(b'single', channel(1, 0.5, 1, scalar_frames([9.0])).children),
        Dir(b'zero', channel(3, 0.0, 1, scalar_frames([1.0, 2.0, 3.0])).children),
        Dir(b'events', channel(5, -1.0, 8, events).children),
        Dir(b'uevents', channel(4, 0.25, 8, uniform_events).children),
    ]


SAMPLES = [
    ('uniform', [0.0, 0.1, 0.25, 0.3, 0.99, 1.0, 1.01, -0.0, -0.1]),
    ('odd', [0.0, 0.05, 0.1, 0.15000001, 0.2, 0.29999998, 0.3, 0.31]),
    ('stamped', [0.0, 0.1, 0.2, 0.21, 0.69, 0.7, 0.75]),
    ('single', [0.0, 0.1]),
    ('zero', [0.0, 0.5]),
    ('events', [0.0, 0.1, 0.4, 0.5]),
    ('uevents', [0.0, 0.25, 0.6]),
]

RANGES = [
    ('events', 0.0, 0.5, 16), ('events', 0.0, 0.1, 16), ('events', 0.05, 0.35, 16),
    ('events', 0.5, 0.0, 16), ('events', 0.35, 0.1, 16), ('events', 0.0, 0.5, 2),
    ('events', 0.0, 0.0, 16), ('events', -0.5, 0.2, 16), ('events', 0.0, 0.6, 16),
    ('uevents', 0.0, 0.75, 16), ('uevents', 0.75, 0.0, 16), ('uevents', 0.1, 0.6, 1),
    ('uniform', 0.0, 0.5, 16),
]

AT = [('events', 0.1, 16), ('events', 0.1, 1), ('events', 0.0, 16), ('events', 0.2, 16),
      ('uevents', 0.5, 16), ('uniform', 0.25, 16)]


# --- lists ------------------------------------------------------------------

def joint_values(seed):
    out = []
    for k in range(7):
        out.append(((seed * 37 + k * 11) % 23 - 11) * 0.1875)
    return out


def main():
    shutil.rmtree(ROOT, ignore_errors=True)
    write('scene/ship.cmp', build(ship_tree()))
    write('scene/nested.cmp', build(nested_tree()))
    write('scene/nocons.cmp', build(nocons_tree()))
    write('scene/simple.3db', build(simple_tree()))
    write('scene/empty.cmp', build([Dir(b'Cmpnd', [])]))
    write('scene/plain.3db', build([File(b'VMeshPart', b'\0' * 4)]))
    write('scene/external.3db', build([File(b'VMeshPart', b'\0' * 4),
                                       Dir(b'Hardpoints', [Dir(b'Fixed', [
                                           hardpoint('HpExt', 0, (0.5, 0.5, 0.5), IDENT)])])]))
    write('scene/anim.cmp', build(anim_compound()))
    write('scene/sample.utf', build(sample_tree()))

    archives = ['scene/ship.cmp', 'scene/nested.cmp', 'scene/nocons.cmp', 'scene/simple.3db',
                'scene/empty.cmp', 'scene/plain.3db', 'scene/missing.cmp', 'scene/anim.cmp']
    write_lines('scene/load.list', 'archives CreateArchetypeHandle loads, one per line', archives)

    joints = []
    for archive in ['scene/ship.cmp', 'scene/nested.cmp', 'scene/anim.cmp']:
        for seed in range(3):
            values = ','.join(bits(v) for v in joint_values(seed))
            joints.append('%s|%d|%s' % (archive, seed % 2 + (seed // 2) * 2, values))
    clamp = ','.join(bits(v) for v in [5.0, -9.0, 0.5, 0.5, 0.5, 0.5, 0.5])
    joints.append('scene/ship.cmp|0|' + clamp)
    write_lines('scene/joints.list',
                'archive|index|seven f32 bit patterns: SetJointParams on every instance',
                joints)

    def p(name, flags, start, speed, blend, scale=1.0, factor=1.0):
        return 'p:%s:%x:%s:%s:%s:%s:%s' % (name.encode().hex(), flags, bits(start), bits(speed),
                                           bits(blend), bits(scale), bits(factor))

    def w(dt):
        return 'w:' + bits(dt)

    def a(dt):
        return 'a:' + bits(dt)

    steps = [w(0.0), w(0.1), w(0.3), w(0.45), w(1.0)]
    plays = [
        [p('Swing', 0, 0.0, 1.0, 0.0)] + steps,
        [p('Swing', 2, 0.0, 1.5, 0.0)] + steps,
        [p('Swing', 4, 0.0, 1.0, 0.0)] + steps + [w(2.0)],
        [p('Swing', 1, -1.0, 1.0, 0.0)] + steps,
        [p('Swing', 3, -1.0, 0.75, 0.0, 0.5, 2.0)] + steps,
        [p('Spin', 0, 0.0, 1.0, 0.0)] + steps,
        [p('Spin', 2, 0.25, 1.0, 0.0, 0.5)] + steps,
        [p('Move', 0, 0.0, 1.0, 0.0)] + steps,
        [p('Move', 2, 0.0, 1.0, 0.0, 1.0, 0.5)] + steps,
        [p('Move', 0x42, 0.3, 1.0, 0.0)] + steps,
        [p('Broken', 0, 0.0, 1.0, 0.0)] + steps,
        [p('Swing', 0, 0.0, 1.0, 0.0), w(0.2), p('Move', 0, 0.0, 1.0, 0.5), w(0.1), w(0.2),
         w(0.3), w(0.5)],
        [p('Spin', 2, 0.0, 1.0, 0.0), w(0.3), p('Spin', 0, 0.0, 1.0, 0.4), w(0.1), w(0.2),
         w(0.4)],
        [p('Move', 0, 0.0, 1.0, 0.0), w(0.2), p('Move', 0x18, 0.0, 1.0, 0.6), w(0.1), w(0.3),
         w(0.4)],
        [p('Move', 0, 0.0, 1.0, 0.0), w(0.2), p('Move', 0x80, 0.0, 1.0, 0.5), w(0.1), 's:1',
         w(0.2), w(0.5)],
        [p('Swing', 0, 0.0, 1.0, 0.25), a(0.1), a(0.2), 's:0', a(0.1), 'd:0', a(0.1)],
        [p('Swing', 0, 0.0, 1.0, 0.0), p('Spin', 0, 0.0, 1.0, 0.0), w(0.2), 'd:0', w(0.3)],
        [p('Swing', 0, 2.0, 1.0, 0.0), w(0.1)],
    ]
    write_lines('scene/play.list',
                'scene/anim.cmp scenarios: p:name:flags:start:speed:blend:scale:factor '
                'plays and evaluates, w:dt walks the root, a:dt walks all, s:n stops and '
                'd:n destroys the n-th playback',
                [' '.join(ops) for ops in plays])

    samples = ['%s|%s' % (name, bits(t)) for name, times in SAMPLES for t in times]
    write_lines('scene/sample.list', 'channel|time: Sample, SamplePair, GetFrame', samples)
    ranges = ['%s|%s|%s|%d' % (n, bits(t0), bits(t1), cap) for n, t0, t1, cap in RANGES]
    write_lines('scene/range.list', 'channel|t0|t1|capacity: CollectRange', ranges)
    at = ['%s|%s|%d' % (n, bits(t), cap) for n, t, cap in AT]
    write_lines('scene/at.list', 'channel|time|capacity: CollectAt', at)


if __name__ == '__main__':
    main()
