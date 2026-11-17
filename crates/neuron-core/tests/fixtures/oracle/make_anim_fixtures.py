"""Writes the oracle inputs of the anim suites (work package P1.7).

Everything here is synthetic: small UTF archives that hold animation
channels and scripts in the layout CppSource/SceneRuntime/SceneRuntime.cpp
reads (Animation\\Script\\<name>\\<map>\\{Parent name, Child name,
Channel\\{Header, Frames}}), and the lists the suites walk. Run it with
Python 3 from any directory; it rewrites anim/ under this directory. The
oracle harness (CppSource/Oracle/oracle_anim.cpp) and the Rust test
tests/oracle_anim.rs read the same files.

Axis-angle channels (0x80, Tier B) appear only in the channel suite.
No input below makes the reference read outside its buffers: no event
channel whose file is shorter than its stamped frames, no name file without
a terminator or of size 0, no packed channel with inconsistent flags inside
a script (its frames would alias the shared unpacking buffer), and no
partially read Frames file inside a script.
"""

import os
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, 'anim')

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
    def __init__(self, name, data, size=None):
        self.name, self.data = name, data
        self.size = len(data) if size is None else size


class Dir:
    def __init__(self, name, children):
        self.name, self.children = name, children


def archive_header(node_size, tree, strings, data):
    fields = [MAGIC, VERSION, tree, 0, 0, node_size, strings, 0, 0, data, 0, 0, 0, 0]
    return struct.pack('<14I', *fields)


def build_full(children):
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
                struct.pack_into('<I', tree, at + 0x1C, item.size)
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
    return bytes(archive_header(0x2C, t, s, d) + tree + strings + data)


def build_compact(children):
    """A 0x0C archive: children contiguous, names 4 bytes after their offset."""
    tree = bytearray(0x0C)
    strings = bytearray()
    data = bytearray()

    def name_offset(name):
        off = len(strings)
        strings.extend(struct.pack('<I', len(name)) + name + b'\0')
        return off

    def place(items):
        first = len(tree)
        tree.extend(bytes(0x0C * len(items)))
        for i, item in enumerate(items):
            at = first + i * 0x0C
            if isinstance(item, File):
                struct.pack_into('<III', tree, at, name_offset(item.name), len(data), item.size)
                data.extend(item.data)
            else:
                child = place(item.children) if item.children else 0
                struct.pack_into('<III', tree, at, name_offset(item.name) | 0x80000000, child,
                                 len(item.children))
        return first

    struct.pack_into('<I', tree, 0, name_offset(b'\\') | 0x80000000)
    first = place(children)
    struct.pack_into('<II', tree, 4, first, len(children))
    t = HEADER
    s = t + len(tree)
    d = s + len(strings)
    return bytes(archive_header(0x0C, t, s, d) + tree + strings + data)


# --- channel data -----------------------------------------------------------

def f32(value):
    return struct.pack('<f', value)


def f32_bits(bits):
    return struct.pack('<I', bits)


def header(count, interval, flags):
    if isinstance(interval, int):
        return struct.pack('<I', count) + f32_bits(interval) + struct.pack('<I', flags)
    return struct.pack('<IfI', count, interval, flags)


def shorts(*values):
    return struct.pack('<%dh' % len(values), *values)


class Rng:
    """A fixed linear congruential generator, so the inputs never change."""

    def __init__(self, seed):
        self.state = seed

    def next(self):
        self.state = (self.state * 1103515245 + 12345) & 0x7FFFFFFF
        return self.state

    def short(self):
        return (self.next() >> 8) % 65536 - 32768

    def unit(self):
        return (self.next() % 2001 - 1000) / 1000.0


SPECIAL_SHORTS = [0, 1, -1, 32767, -32767, -32768, 16384, -16384, 23170, 18918, 100, -12345]


def packed_frames(rng, count, per_frame, stamp, pos):
    """Frames of a packed channel: time stamp, position, then packed words."""
    out = bytearray()
    specials = [(a, b, c, d) for a in SPECIAL_SHORTS[:6] for b in SPECIAL_SHORTS[6:9]
                for c in (0, 32767, -32768) for d in (0, -1)]
    for i in range(count):
        if stamp:
            out += f32(i * 0.125)
        if pos:
            out += f32(rng.unit()) + f32(rng.unit()) + f32(rng.unit())
        if i < len(specials):
            values = specials[i]
        else:
            values = (rng.short(), rng.short(), rng.short(), rng.short())
        out += shorts(*values[:per_frame])
    return bytes(out)


def raw_frames(rng, count, words, stamp):
    out = bytearray()
    for i in range(count):
        if stamp:
            out += f32(i * 0.25)
        for _ in range(words):
            out += f32(rng.unit())
    return bytes(out)


def channel_dir(name, head, frames, header_name=b'Header', frames_name=b'Frames'):
    items = []
    if head is not None:
        items.append(File(header_name, head))
    if frames is not None:
        items.append(File(frames_name, frames))
    return Dir(name, items)


def channel_cases():
    rng = Rng(0x5EED)
    cases = []

    def add(name, head, frames, **kw):
        cases.append(channel_dir(name.encode(), head, frames, **kw))

    # Raw layouts, uniform and stamped.
    add('scalar', header(5, 1.0 / 30.0, 1), raw_frames(rng, 5, 1, False))
    add('scalar_stamped', header(4, -1.0, 1), raw_frames(rng, 4, 1, True))
    add('vector', header(3, 0.5, 2), raw_frames(rng, 3, 3, False))
    add('vector_stamped', header(4, -1.0, 2), raw_frames(rng, 4, 3, True))
    add('quat', header(3, 0.04, 4), raw_frames(rng, 3, 4, False))
    add('posquat', header(2, -2.0, 6), raw_frames(rng, 2, 7, True))
    add('scalar_vector', header(2, 0.1, 3), raw_frames(rng, 2, 4, False))
    add('all_raw', header(2, 0.1, 7), raw_frames(rng, 2, 8, False))
    add('flags_9', header(3, 0.1, 9), raw_frames(rng, 3, 1, False))
    add('high_bits', header(2, 0.1, 0x206), raw_frames(rng, 2, 7, False))
    add('one_frame', header(1, 0.1, 2), raw_frames(rng, 1, 3, False))
    add('nan_interval', header(2, 0x7FC00000, 2), raw_frames(rng, 2, 3, False))
    add('negzero_interval', header(2, 0x80000000, 2), raw_frames(rng, 2, 3, False))
    add('big_count_interval', header(3, 1e30, 1), raw_frames(rng, 3, 1, False))
    add('case_names', header(2, 0.1, 1), raw_frames(rng, 2, 1, False),
        header_name=b'HEADER', frames_name=b'frames')
    add('long_header', header(2, 0.1, 1) + b'\xAA\xBB\xCC\xDD', raw_frames(rng, 2, 1, False))

    # Event channels (flags exactly 8): offsets into data after the frames.
    event_data = b'step\0jump\0land\0'
    add('event', header(3, -1.0, 8),
        f32(0.0) + struct.pack('<I', 0) + f32(0.5) + struct.pack('<I', 5)
        + f32(1.5) + struct.pack('<I', 10) + event_data)
    add('event_uniform', header(2, 0.25, 8), struct.pack('<II', 0, 5) + event_data)
    add('event_short_uniform', header(10, 0.25, 8), struct.pack('<II', 0, 4))

    # Packed quaternions (0x40), axis-angle (0x80), four shorts (0x100).
    for flags, per, label in ((0x40, 3, 'comp3'), (0x80, 3, 'axis'), (0x100, 4, 'comp4')):
        for extra, extra_label in ((0, ''), (0x2, '_pos'), (0x10, '_zeropos')):
            for stamp in (False, True):
                count = 80 if extra == 0 and not stamp else 9
                frames = packed_frames(rng, count, per, stamp, extra == 0x2)
                name = '%s%s%s' % (label, extra_label, '_stamped' if stamp else '')
                add(name, header(count, -1.0 if stamp else 1.0 / 15.0, flags | extra), frames)
    add('zeropos_only', header(3, -1.0, 0x10), f32(0.0) + f32(1.0) + f32(2.0))
    add('zeropos_uniform', header(1, 0.5, 0x10), b'')
    add('ident_only', header(2, -1.0, 0x20), f32(0.0) + f32(3.0))
    add('vector_ident', header(2, 0.2, 0x22), raw_frames(rng, 2, 3, False))
    add('zeropos_ident', header(2, -1.0, 0x30), f32(0.0) + f32(0.5))
    add('comp3_high_bits', header(2, 0.1, 0x440), shorts(1000, 2000, 3000, -1000, -2000, -3000))

    # Inconsistent packed flags: reported as fatal, frames left as read.
    add('fatal_scalar', header(2, 0.1, 0x41), raw_frames(rng, 2, 1, False) + shorts(1, 2, 3, 4, 5, 6))
    add('fatal_vector', header(1, 0.1, 0x52), raw_frames(rng, 1, 3, False) + shorts(7, 8, 9))
    add('fatal_quat', header(1, 0.1, 0x44), raw_frames(rng, 1, 4, False) + shorts(7, 8, 9))
    add('fatal_two_packed', header(1, 0.1, 0xC0), shorts(1, 2, 3, 4, 5, 6))
    add('fatal_ident_packed', header(1, -1.0, 0x60), f32(0.0) + shorts(1, 2, 3))

    # Load failures.
    add('no_header', None, raw_frames(rng, 2, 1, False))
    add('short_header', header(2, 0.1, 1)[:8], raw_frames(rng, 2, 1, False))
    add('no_frames', header(2, 0.1, 1), None)
    add('size_mismatch', header(3, 0.1, 2), raw_frames(rng, 2, 3, False))
    add('size_mismatch_packed', header(3, 0.1, 0x40), shorts(1, 2, 3))
    add('zero_frames', header(0, 0.1, 2), b'')
    add('zero_frames_stamped', header(0, -1.0, 2), b'')
    add('flags_zero', header(4, 0.1, 0), b'')
    add('empty', None, None)
    # Last in the data section: Frames claims more bytes than the archive has.
    cases.append(Dir(b'zz_cut', [File(b'Header', header(4, 0.1, 2)),
                                 File(b'Frames', raw_frames(rng, 2, 3, False), size=48)]))
    return cases


# --- scripts ------------------------------------------------------------------

def name_file(name, text):
    return File(name, text + b'\0')


def channel(head, frames):
    return Dir(b'Channel', [File(b'Header', head), File(b'Frames', frames)])


def joint(name, parent, child, chan):
    items = []
    if parent is not None:
        items.append(name_file(b'Parent name', parent))
    if child is not None:
        items.append(name_file(b'Child name', child))
    if chan is not None:
        items.append(chan)
    return Dir(name, items)


def script_tree():
    rng = Rng(0xA11CE)

    def uniform(count, rate, flags, words):
        return channel(header(count, rate, flags), raw_frames(rng, count, words, False))

    def stamped(count, flags, words):
        return channel(header(count, -1.0, flags), raw_frames(rng, count, words, True))

    def packed(count, flags, per, stamp):
        return channel(header(count, -1.0 if stamp else 0.1, flags),
                       packed_frames(rng, count, per, stamp, flags & 2 != 0))

    walk = Dir(b'Walk', [
        joint(b'Joint map 0', b'Bip01', b'Bip01 Pelvis', packed(6, 0x40, 3, False)),
        joint(b'Joint map 1', b'Bip01 Pelvis', b'Bip01 Spine', packed(5, 0x50, 3, True)),
        joint(b'Joint map 2', b'Bip01 Spine', b'Bip01 Neck', packed(4, 0x102, 4, False)),
        joint(b'Object map', b'Root', None, stamped(7, 6, 7)),
        File(b'Root height', f32(0.97)),
        joint(b'Event map', b'Root', None, stamped(9, 2, 3)),
        joint(b'Event mapping', None, None, uniform(2, 0.1, 1, 1)),
        joint(b'Joint map no child', b'Bip01', None, uniform(3, 0.1, 4, 4)),
        joint(b'Joint map no channel', b'Bip01', b'Bip01 L Foot', None),
        Dir(b'Joint map channel file', [name_file(b'Parent name', b'Bip01'),
                                        name_file(b'Child name', b'Bip01 R Foot'),
                                        File(b'Channel', b'not a directory')]),
        joint(b'Joint map bad', b'Bip01', b'Bip01 Head',
              channel(header(5, 0.1, 2), raw_frames(rng, 4, 3, False))),
        Dir(b'Extra', [name_file(b'Parent name', b'ignored')]),
        File(b'notes', b'not a map'),
        Dir(b'.hidden', [joint(b'Joint map', b'a', b'b', uniform(2, 0.1, 1, 1))]),
        joint(b'joint map lower', b'Bip01', b'Bip01 Tail', uniform(2, 0.1, 1, 1)),
        joint(b'Object map 2', b'Prop', b'ignored child', uniform(50, 0.1, 2, 3)),
        Dir(b'Joint mapX', [File(b'Parent name', b'Bip01\0junk after'),
                            File(b'Child name', b'\0'),
                            channel(header(1, 0.1, 1), f32(4.0))]),
    ])
    walk_upper = Dir(b'WALK', [
        joint(b'Joint map', b'Upper', b'Case', uniform(3, 0.2, 1, 1)),
        File(b'Root height', b'\x11\x22'),
    ])
    idle = Dir(b'Idle', [
        File(b'Root height', b'\x00\x00'),
        joint(b'Object map', b'Root', None, uniform(4, 0.5, 2, 3)),
        joint(b'Object map b', b'Root', None, packed(3, 0x110, 4, True)),
    ])
    run = Dir(b'Run', [
        joint(b'Joint map a', b'P', b'A', uniform(3, 0.5, 1, 1)),
        joint(b'Joint map b', b'P', b'B', uniform(3, 0.5, 4, 4)),
        joint(b'Joint map c', b'P', b'C', uniform(4, 0.5, 2, 3)),
        joint(b'Joint map d', b'P', b'D', uniform(4, 0.5, 6, 7)),
        Dir(b'Root height', []),
        File(b'Root Height', f32(5.0)),
    ])
    negative = Dir(b'Negative', [
        joint(b'Joint map nan', b'P', b'N', channel(header(2, 0x7FC00000, 1), raw_frames(rng, 2, 1, False))),
        joint(b'Joint map neg', b'P', b'M', channel(header(2, -1.0, 1), f32(-9.0) + f32(1.0) + f32(-5.0) + f32(2.0))),
        File(b'Root height', b''),
    ])
    return [
        Dir(b'Animation', [
            Dir(b'Script', [
                walk,
                walk_upper,
                idle,
                run,
                negative,
                Dir(b'Empty', []),
                File(b'readme', b'a file in Script'),
                Dir(b'.dot', [joint(b'Joint map', b'x', b'y', uniform(2, 0.1, 1, 1))]),
                Dir(b'Nested', [Dir(b'Script', [Dir(b'Inner', [])])]),
            ]),
            File(b'Script.txt', b'beside Script'),
        ]),
        Dir(b'Cmpnd', [Dir(b'Root', [File(b'Object name', b'Root\0')])]),
    ]


def compact_tree():
    rng = Rng(0xC0C0)
    return [
        Dir(b'Animation', [
            Dir(b'Script', [
                Dir(b'Open', [
                    joint(b'Joint map 0', b'Door', b'Hinge',
                          channel(header(3, 0.25, 0x40), packed_frames(rng, 3, 3, False, False))),
                    joint(b'Object map', b'Door', None,
                          channel(header(2, -1.0, 2), raw_frames(rng, 2, 3, True))),
                    File(b'Root height', f32(-1.5)),
                ]),
                Dir(b'Close', []),
            ]),
        ]),
    ]


def main():
    if os.path.isdir(ROOT):
        shutil.rmtree(ROOT)
    cases = channel_cases()
    write('anim/channels.utf', build_full(cases))
    write_lines('anim/channels.list', 'channel directories of anim/channels.utf, in order',
                [c.name.decode() for c in cases] + ['missing_dir'])

    write('anim/script_full.utf', build_full(script_tree()))
    write('anim/script_compact.utf', build_compact(compact_tree()))
    write('anim/no_anim.utf', build_full([Dir(b'Cmpnd', [])]))
    write('anim/anim_file.utf', build_full([File(b'Animation', b'a file')]))
    write('anim/no_script.utf', build_full([Dir(b'Animation', [Dir(b'Other', [])])]))
    write('anim/empty_script.utf', build_full([Dir(b'Animation', [Dir(b'Script', [])])]))
    scripts = ['anim/script_full.utf', 'anim/script_compact.utf', 'anim/no_anim.utf',
               'anim/anim_file.utf', 'anim/no_script.utf', 'anim/empty_script.utf']
    write_lines('anim/scripts.list', 'archives for OpenScript and EnumFsScripts', scripts)

    def names(*items):
        return ','.join('-' if n is None else n.encode().hex() for n in items)

    add = [
        'anim/script_full.utf|' + names('Walk', 'Idle'),
        'anim/script_full.utf|' + names('Idle', None, 'Walk'),
        'anim/script_full.utf|' + names('readme', 'Missing', 'Idle') + '|' + names('Idle', 'Run'),
        'anim/script_full.utf|' + names('WALK', 'walk') + '|' + names('Negative'),
        'anim/script_full.utf|' + names('Walk\\Joint map 0', 'Nested\\Script\\Inner', '.dot'),
        'anim/script_full.utf|' + names('Empty') + '|' + names('Empty', 'Run'),
        'anim/script_compact.utf|' + names('Open', 'Close', 'open'),
        'anim/no_anim.utf|' + names('Walk'),
        'anim/no_script.utf|' + names('Walk'),
        'anim/anim_file.utf|' + names('Walk'),
    ]
    write_lines('anim/add.list', 'archive|names|names...: AddScriptNames calls on one script and file system',
                add)


if __name__ == '__main__':
    main()
