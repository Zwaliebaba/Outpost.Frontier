"""Writes the oracle inputs of the utf and vfs suites (work package P1.3).

Everything here is synthetic: small UTF archives built from the layout in
crates/neuron-core/src/utf/mod.rs, a directory tree, path and pattern lists,
and operation scripts. Run it with Python 3 from any directory; it rewrites
vfs/ under this directory. The oracle harness (CppSource/Oracle/oracle_vfs.cpp)
and the Rust test tests/oracle_vfs.rs read the same files.

It also runs the independent Python walker CppSource/ReadFile/utf_walk_once.py
over the well-formed archives and writes its view to vfs/walker.txt, which
tests/oracle_vfs.rs compares with the Rust reader (design/verification.md,
cross-implementation tests).
"""

import importlib.util
import os
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, 'vfs')
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..', '..'))
WALKER = os.path.join(REPO, 'CppSource', 'ReadFile', 'utf_walk_once.py')

MAGIC = 0x20465455
VERSION = 0x101
HEADER = 0x38


def write(name, data):
    path = os.path.join(ROOT, *name.split('/'))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def write_lines(name, header, lines):
    write(name, ('\n'.join(['# ' + header] + lines) + '\n').encode())


def dos_date(year, month, day):
    return ((year - 1980) << 9) | (month << 5) | day


def dos_time(hour, minute, second):
    return (hour << 11) | (minute << 5) | (second // 2)


def packed(date, time):
    return (time << 16) | date


VALID_TIMES = (
    packed(dos_date(2003, 3, 4), dos_time(5, 6, 8)),
    packed(dos_date(1984, 2, 29), dos_time(23, 59, 58)),
    packed(dos_date(1980, 1, 1), 0),
)
INVALID_TIMES = (0, packed(dos_date(1990, 13, 1), 0), packed(dos_date(1990, 1, 1), 0x1F))


class File:
    def __init__(self, name, data, flags=0x80, times=VALID_TIMES, size=None):
        self.name, self.data, self.flags, self.times = name, data, flags, times
        self.size = len(data) if size is None else size


class Dir:
    def __init__(self, name, children, flags=0x10, times=VALID_TIMES):
        self.name, self.children, self.flags, self.times = name, children, flags, times


def header(node_size, tree, strings, data, extra=0):
    fields = [MAGIC, VERSION, tree, extra, extra, node_size, strings, extra, extra, data,
              extra, extra, extra, extra]
    return struct.pack('<14I', *fields)


def build_full(children, root_times=VALID_TIMES, data_base=0):
    """A 0x2C archive: records in breadth order, siblings chained."""
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
            struct.pack_into('<III', tree, at, nxt, name_offset(item.name), item.flags)
            struct.pack_into('<III', tree, at + 0x20, *item.times)
            if isinstance(item, File):
                struct.pack_into('<I', tree, at + 0x10, data_base + len(data))
                struct.pack_into('<I', tree, at + 0x1C, item.size)
                data.extend(item.data)
            else:
                struct.pack_into('<I', tree, at + 0x10, place(item.children))
        return first

    struct.pack_into('<III', tree, 0, 0, name_offset(b'\\'), 0x10)
    struct.pack_into('<III', tree, 0x20, *root_times)
    struct.pack_into('<I', tree, 0x10, place(children))
    t = HEADER
    s = t + len(tree)
    d = s + len(strings)
    return bytearray(header(0x2C, t, s, d) + tree + strings + data)


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
    return bytearray(header(0x0C, t, s, d) + tree + strings + data)


def sample_tree():
    return [
        Dir(b'Mesh', [
            Dir(b'Geometry', [
                File(b'VMeshData', b'vertex data 0123456789'),
                File(b'readme.txt', b'hello, utf'),
                File(b'Notes.TXT', b'upper case extension'),
            ]),
            File(b'Sphere', bytes(range(16))),
            Dir(b'Face_groups', [File(b'group0', b'\x01\x02')]),
        ]),
        File(b'dup', b'first of two'),
        File(b'DUP', b'second of two'),
        Dir(b'Empty', []),
        File(b'\xc9t\xe9.bin', b'\xff\xfe'),
        File(b'\x80b.dat', b'fold table'),
        File(b'noext', b'no extension'),
        File(b'zero', b''),
        Dir(b'Animation', [
            Dir(b'Script', [File(b'walk', b'w'), File(b'run', b'r'), File(b'MIP0', b'm0'),
                            File(b'MIP1', b'm1'), File(b'mip.lod1', b'lod')]),
        ]),
        File(b'odd times', b'times', times=INVALID_TIMES),
        File(b'flags', b'raw flags', flags=0x21),
    ]


def full_archive():
    return build_full(sample_tree())


def compact_archive():
    data = build_compact(sample_tree())
    # Point "Empty" (count 0) at the records of "Mesh": the find cursor still
    # yields the first of them, FindChild sees none.
    tree = HEADER
    count = struct.unpack_from('<I', data, tree + 8)[0]
    for i in range(count):
        at = tree + 0x0C * (1 + i)
        word0, off, n = struct.unpack_from('<III', data, at)
        strings = struct.unpack_from('<I', data, 0x18)[0]
        name_at = strings + (word0 & 0x7FFFFFFF) + 4
        name = bytes(data[name_at:data.index(b'\0', name_at)])
        if name == b'Mesh':
            mesh_children = off
        if name == b'Empty':
            empty_at = at
    struct.pack_into('<I', data, empty_at + 4, mesh_children)
    return data


def clamp_archive():
    """Data that runs past the end, and data that starts past it."""
    items = [
        File(b'long', b'0123456789', size=0x40),
        File(b'inside', b'abc'),
        File(b'outside', b'', size=4),
    ]
    data = build_full(items)
    length = len(data)
    # "outside" is the third record: move its data offset past the end.
    at = HEADER + 0x2C * 3
    data_section = struct.unpack_from('<I', data, 0x24)[0]
    struct.pack_into('<I', data, at + 0x10, length - data_section + 8)
    return data


def bad_archives():
    good = full_archive()
    out = {}
    b = bytearray(good)
    b[0:4] = b'UTF!'
    out['bad_magic.utf'] = b
    b = bytearray(good)
    struct.pack_into('<I', b, 4, 0x100)
    out['bad_version.utf'] = b
    b = bytearray(good)
    struct.pack_into('<I', b, 0x14, 0x10)
    out['bad_node.utf'] = b
    out['header_only.utf'] = bytearray(good[:HEADER])
    out['tiny.utf'] = bytearray(good[:10])
    out['empty.utf'] = bytearray()
    b = bytearray(good)
    struct.pack_into('<I', b, 8, len(good))
    out['tree_oob.utf'] = b
    b = bytearray(good)
    struct.pack_into('<I', b, 0x18, len(good) + 100)
    out['strings_oob.utf'] = b
    b = bytearray(good)
    struct.pack_into('<I', b, 0x14, 0x0C)
    out['wrong_layout.utf'] = b
    return out


ARCHIVES = ['full.utf', 'compact.utf', 'clamp.utf']


def archives():
    write('utf/full.utf', full_archive())
    write('utf/compact.utf', compact_archive())
    write('utf/clamp.utf', clamp_archive())
    bad = bad_archives()
    for name, data in bad.items():
        write('utf/' + name, data)
    names = ARCHIVES + sorted(bad) + ['missing.utf']
    write_lines('utf.list', 'UTF archives opened through the UTF factory (relative paths)',
                ['vfs/utf/' + n for n in names])


def tree():
    base = os.path.join(ROOT, 'tree')
    if os.path.isdir(base):
        shutil.rmtree(base)
    write('tree/Data/Sub/File.TXT', b'data sub file')
    write('tree/Data/Empty.bin', b'')
    write('tree/Data/plain.cmp', b'plain bytes, not an archive')
    write('tree/Data/Ships/Ship.utf', full_archive())
    write('tree/Data/Ships/small.utf', compact_archive())
    write('tree/Data/Deep/A/B/leaf.txt', b'leaf')
    write('tree/Patch/Sub/file.txt', b'patch sub file')
    write('tree/Patch/only.txt', b'only in patch')
    write('tree/Patch/Ships/plain.utf', b'plain again')


def h(text):
    if isinstance(text, str):
        text = text.encode('latin-1')
    return text.hex() if text else '-'


MATCH = [
    (b'Geometry', b'*'), (b'Geometry', b'geo*'), (b'Geometry', b'GEO*Y'), (b'MIP0', b'MIP*'),
    (b'Texture', b'MIP*'), (b'a.txt', b'*.TXT'), (b'a.txt', b'.txt'), (b'a.txt', b'*.'),
    (b'noext', b'*.'), (b'noext', b'noext'), (b'noext', b'noext.'), (b'abc', b'a?c'),
    (b'ac', b'a?c'), (b'abc', b'???'), (b'abcd', b'???'), (b'mip.lod1', b'*.lod?'),
    (b'a\\b', b'a/b'), (b'A/B', b'a\\b'), (b'\x80', b'B'), (b'\x80', b'b.'),
    (b'a.\xc1', b'a.*'), (b'ab.c', b'ab.c\xc1'), (b'ab.c\xc1\xc1', b'ab.c'), (b'\xc9t\xe9.bin', b'*.bin'),
    (b'\xc9t\xe9.bin', b'\xe9*'), (b'\xe9', b'\xc9'), (b'x', b''), (b'', b''), (b'', b'*'),
    (b'.hidden', b'*'), (b'.hidden', b'.*'), (b'a.b.c', b'*.c'), (b'a.b.c', b'a.*.c'),
    (b'x' * 300, b'*'), (b'x' * 300, b'x*'), (b'y' * 262, b'y' * 262), (b'y' * 263, b'y' * 263),
    (b'z' * 270 + b'.e', b'*.e'), (b'*', b'\\*'), (b'a*b', b'a*b'), (b'aXb', b'a*b*'),
    (b'a' * 40, b'*a' * 20 + b'b'), (b'name.ext', b'NAME.EXT'), (b'name', b'*?'),
]


def match_list():
    write_lines('match.hex', 'name:pattern for MatchUtfWildcard', [
        '%s:%s' % (a.hex(), b.hex()) for a, b in MATCH])


JOIN = [
    (b'\\', b'Mesh', 0x104), (b'data', b'x.txt', 0x104), (b'data\\', b'x.txt', 0x104),
    (b'data/', b'x.txt', 0x104), (b'', b'x', 0x104), (b'a', b'\\abs', 0x104),
    (b'a', b'/abs', 0x104), (b'a', b'C:\\abs', 0x104), (b'a', b'9:x', 0x104),
    (b'\\a\\', b'.\\b', 0x104), (b'\\a\\', b'./b', 0x104), (b'\\Animation\\', b'..', 0x104),
    (b'\\ab\\cd\\', b'..\\x', 0x104), (b'\\ab\\cd\\', b'..\\..\\x', 0x104),
    (b'\\a\\b\\', b'..\\..\\x', 0x104), (b'\\a\\b\\', b'..\\x', 0x104), (b'C:\\', b'..', 0x104),
    (b'C:\\games\\data\\', b'..\\..\\x', 0x104), (b'data', b'..', 0x104), (b'\\', b'..', 0x104),
    (b'a', b'b\\..\\c', 0x104), (b'a', b'.', 0x104), (b'a', b'..\\', 0x104),
    (b'\\x\\y\\', b'.\\..\\z', 0x104), (b'\\x\\y\\', b'...', 0x104), (b'\\xx\\yy', b'..', 0x104),
    (b'a', b'b', 0), (b'a', b'b', 1), (b'a', b'b', 2), (b'ab', b'cdef', 6), (b'abcde', b'x', 6),
    (b'abcd', b'x', 6), (b'abcdef', b'x', 6), (b'x', b'\\abcdef', 4), (b'\\abc\\', b'..\\d', 5),
    (b'r' * 258, b'tail', 0x104), (b'r' * 259, b'tail', 0x104), (b'r' * 260, b'tail', 0x104),
    (b's' * 200, b't' * 100, 0x104), (b'\\', b'u' * 300, 0x104), (b'\\', b'\\' + b'v' * 300, 0x104),
    (b'w' * 300, b'..\\x', 0x104), (b'\\\xc9t\xe9\\', b'..\\\xe9', 0x104),
]


def join_list():
    write_lines('join.list', 'root:rel:out_chars for JoinRoot and the memory ResolvePath', [
        '%s:%s:%d' % (a.hex(), b.hex(), n) for a, b, n in JOIN])


DOSTIME = [
    (dos_date(1980, 1, 1), 0), (dos_date(2003, 3, 4), dos_time(5, 6, 8)),
    (dos_date(1984, 2, 29), dos_time(23, 59, 58)), (dos_date(1985, 2, 29), 0),
    (dos_date(2107, 12, 31), dos_time(23, 59, 58)), (0, 0), (dos_date(1990, 13, 1), 0),
    (dos_date(1990, 1, 0), 0), (dos_date(1990, 1, 1), 0x1F), (dos_date(1990, 1, 1), dos_time(24, 0, 0)),
    (dos_date(1990, 1, 1), 60 << 5), (dos_date(2000, 2, 29), dos_time(12, 34, 56)),
]


def dostime_list():
    write_lines('dostime.list', 'DOS date and time words, hex', [
        '%04x:%04x' % (d, t) for d, t in DOSTIME])


def ops(name, header, lines):
    write_lines(name, header, lines)


def utf_ops():
    full = 'vfs/utf/full.utf'
    compact = 'vfs/utf/compact.utf'
    lines = []
    for archive in (full, compact):
        lines += [
            'archive ' + h(archive),
            'self',
            'syspath 260', 'syspath 4', 'curpath 260',
            'attr ' + h('Mesh'), 'attr ' + h('mesh\\GEOMETRY'), 'attr ' + h('mesh\\sphere'),
            'attr ' + h('none'), 'attr ' + h('\\'), 'attr ' + h('..'), 'attr ' + h('Mesh\\'),
            'find ' + h('*'), 'find ' + h('*.*'), 'find ' + h('*.txt'), 'find ' + h('Mesh\\Geometry\\*.txt'),
            'find ' + h('\\Animation\\Script\\MIP*'), 'find ' + h('Animation\\Script\\*.lod?'),
            'find ' + h('none\\*'), 'find ' + h('zero\\*'), 'find ' + h('nothing*'),
            'find ' + h('Empty\\*'), 'find ' + h('*.BIN'), 'find ' + h('\xe9*'), 'find ' + h('b*'),
            'find ' + h('?'), 'find ' + h('noext'), 'find ' + h('dup'),
            'open ' + h('MESH\\geometry\\VMeshData'), 'open ' + h('DUP'), 'open ' + h('zero'),
            'open ' + h('Mesh'), 'open ' + h('missing'), 'open ' + h('..\\x'), 'open ' + h('\\Mesh\\Sphere'),
            'resolve ' + h('x') + ' 260', 'resolve ' + h('..') + ' 260', 'resolve ' + h('abc') + ' 4',
            'node ' + h('Mesh'), 'resolve ' + h('x') + ' 260', 'find ' + h('*'), 'find ' + h('Geometry\\*'),
            'find ' + h('\\Animation\\*'), 'open ' + h('sphere'), 'open ' + h('Geometry\\readme.txt'),
            'open ' + h('\\Mesh\\Sphere'), 'attr ' + h('sphere'), 'attr ' + h('\\dup'),
            'node ' + h('Geometry'), 'resolve ' + h('y') + ' 260', 'find ' + h('*'),
            'node ' + h('..'), 'resolve ' + h('y') + ' 260', 'node ' + h('..'), 'node ' + h('..'),
            'node ' + h('missing'), 'node ' + h('Mesh\\Sphere'), 'resolve ' + h('z') + ' 260',
            'node ' + h('..'), 'node ' + h('\\'), 'find ' + h('*'),
            'clone ' + h('Mesh'), 'self', 'syspath 260', 'curpath 260', 'curpath 3',
            'find ' + h('*'), 'attr ' + h('Sphere'), 'open ' + h('sphere'),
            'clone ' + h('geometry\\'), 'syspath 260', 'curpath 260', 'find ' + h('*'),
            'clonefind ' + h('*.txt') + ' 1', 'self', 'syspath 260', 'curpath 260',
            'seek 0 4', 'readself 4', 'seek 1 2', 'readself 100', 'seek 2 3', 'readself 2',
            'seek 0 100', 'readself 1', 'map 0 0', 'map 2 3', 'map 2 100',
            'pop', 'clonefind ' + h('v*') + ' 0', 'self', 'pop',
            'clone ' + h('missing'), 'clone ' + h('readme.txt'), 'self', 'map 0 0', 'pop',
            'pop', 'node ' + h('Geometry'), 'clone ' + h('readme.txt'), 'self', 'pop',
            'pop', 'clone ' + h('Mesh\\Sphere'), 'self', 'curpath 260', 'readself 4', 'readself 100',
            'readself 1', 'pop', 'clone ' + h('\\Animation\\Script\\'), 'find ' + h('*'),
            'clonefind ' + h('w*') + ' 0', 'self', 'pop', 'pop', 'map 0 0',
            'clonefind ' + h('*') + ' 3', 'self', 'pop',
        ]
    lines += [
        'archive ' + h('vfs/utf/clamp.utf'),
        'find ' + h('*'), 'open ' + h('long'), 'open ' + h('inside'), 'open ' + h('outside'),
        'clone ' + h('long'), 'self', 'readself 5', 'seek 0 8', 'readself 4', 'pop',
        'clone ' + h('inside'), 'map 0 0', 'map 1 2', 'pop',
        'clone ' + h('outside'), 'self', 'pop',
        'archive ' + h('vfs/utf/bad_magic.utf'),
    ]
    ops('utf_ops.txt', 'UTF file-system operations: op hex-argument [number]', lines)


def dos_ops():
    lines = [
        'dos - ' + h('vfs/tree/Data'),
        'syspath 260', 'syspath 5', 'curpath 260', 'resolve ' + h('x') + ' 260',
        'open ' + h('SUB\\file.txt'), 'open ' + h('sub/FILE.TXT'), 'open ' + h('empty.bin'),
        'open ' + h('Sub'), 'open ' + h('missing.txt'), 'open ' + h('Deep\\A\\..\\A\\B\\leaf.txt'),
        'attr ' + h('Sub'), 'attr ' + h('sub\\file.txt'), 'attr ' + h('missing'),
        'find ' + h('*'),
        'node ' + h('sub'), 'syspath 260', 'curpath 260', 'resolve ' + h('x') + ' 260',
        'open ' + h('file.txt'), 'node ' + h('file.txt'), 'node ' + h('missing'),
        'node ' + h('..'), 'open ' + h('empty.bin'), 'node ' + h('Deep\\A'), 'syspath 260',
        'node ' + h('..\\..'), 'open ' + h('plain.cmp'),
        'child - ' + h('Sub'), 'syspath 260', 'open ' + h('file.txt'), 'self', 'pop',
        'child ' + h('DOS') + ' ' + h('sub\\file.txt'), 'syspath 260', 'curpath 260', 'self',
        'map 0 0', 'map 5 4', 'map 5 100', 'seek 0 5', 'readself 4', 'seek 2 4', 'readself 4',
        'readself 100', 'open ' + h('x'), 'attr ' + h('x'), 'node ' + h('x'), 'find ' + h('*'),
        'child - ' + h('x'), 'pop',
        'child - ' + h('Ships\\Ship.utf'), 'syspath 260', 'find ' + h('Mesh\\*'),
        'open ' + h('mesh\\sphere'), 'pop',
        'child ' + h('UTF') + ' ' + h('ships\\SMALL.utf'), 'syspath 260', 'find ' + h('*'), 'pop',
        'child - ' + h('plain.cmp'),
        'child ' + h('XYZ') + ' ' + h('Ships\\Ship.utf'),
        'child ' + h('dos') + ' ' + h('Ships\\Ship.utf'),
        'child - ' + h('missing.utf'),
        'child - ' + h('Deep'), 'syspath 260', 'child - ' + h('A\\B'), 'syspath 260',
        'open ' + h('leaf.txt'), 'pop', 'pop',
        'pop',
        'dos ' + h('DOS') + ' ' + h('vfs/tree/Data/Sub/File.TXT'), 'syspath 260', 'self', 'pop',
        'dos ' + h('UTF') + ' ' + h('vfs/tree/Data/Ships/Ship.utf'), 'syspath 260', 'pop',
        'dos - ' + h('vfs/tree/Data/plain.cmp'), 'syspath 260', 'self', 'pop',
        'dos - ' + h('vfs/tree/nothing'),
        'dos - ' + h('vfs\\tree\\Patch\\'), 'syspath 260', 'resolve ' + h('..\\x') + ' 260',
        'open ' + h('only.txt'), 'pop',
        'search ' + h('vfs/tree/Patch;;vfs/tree/missing;vfs\\tree\\Data'),
        'getpath 260', 'getpath 5', 'getpath 1',
        'sfind ' + h('DOS') + ' ' + h('sub\\file.txt'),
        'sfind ' + h('DOS') + ' ' + h('Empty.bin'),
        'sfind - ' + h('Ships\\Ship.utf'),
        'sfind - ' + h('Ships\\plain.utf'),
        'sfind - ' + h('Sub'),
        'sfind - ' + h('nothing'),
        'search ' + h('vfs/tree/Data/Deep/A;vfs/tree/Patch/only.txt;vfs/tree/Patch/'),
        'sfind ' + h('DOS') + ' ' + h('B\\leaf.txt'),
        'sfind ' + h('DOS') + ' ' + h('only.txt'),
        'search ' + h(''),
        'sfind - ' + h('x'),
        'search ' + h(';;;'),
        'search ' + h('vfs/../vfs/tree/Data'),
        'search ' + h('vfs/tree/' + 'x' * 300),
        'getpath 300',
    ]
    ops('dos_ops.txt', 'DOS file-system and search-path operations', lines)


def mem_ops():
    lines = [
        'mem c0000000 2 copy - ' + h('mem\\root\\'), 'state',
        'write ' + h('hello'), 'state', 'seek 0 0', 'read 8', 'read 8', 'seek 0 1', 'read 3',
        'writeat 3 ' + h('LO world'), 'state', 'readat 0 20', 'seek 2 16', 'state', 'setend', 'state',
        'seek 0 70000', 'setend', 'state', 'seek 0 5', 'setend', 'state', 'read 4',
        'resolve ' + h('x') + ' 260', 'resolve ' + h('..\\x') + ' 260', 'resolve ' + h('..\\..\\..\\x') + ' 260',
        'resolve ' + h('\\abs') + ' 260', 'resolve ' + h('x') + ' 4', 'syspath 260', 'syspath 4',
        'mem 80000000 3 copy ' + h('read only data') + ' -', 'state', 'write ' + h('x'), 'read 4',
        'seek 2 2', 'read 10', 'setend', 'seek 1 2', 'read 2',
        'mem c0000000 3 wrap ' + h('fixed') + ' ' + h('wrapped'), 'state', 'write ' + h('ab'),
        'write ' + h('cdefg'), 'state', 'write ' + h('h'), 'seek 0 9', 'setend', 'seek 0 2', 'setend',
        'state', 'readat 0 10',
        'mem c0000000 4 copy ' + h('trunc') + ' -', 'state', 'writeat 10 ' + h('gap'), 'state',
        'readat 0 20',
        'mem c0000000 1 copy ' + h('new') + ' -', 'state', 'read 3',
    ]
    ops('mem_ops.txt', 'memory file-system operations', lines)


def walker_view():
    spec = importlib.util.spec_from_file_location('utf_walk_once', WALKER)
    walker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(walker)
    lines = []

    def dump(node, depth):
        for child in node.children:
            lines.append('%d\t%s\t%d\t%08x\t%08x' % (
                depth, child.name.encode('latin-1').hex() or '-', int(child.is_dir),
                child.data_off, child.data_size))
            dump(child, depth + 1)

    for name in ARCHIVES:
        path = os.path.join(ROOT, 'utf', name)
        root, info = walker.parse_utf(path)
        lines.append('file vfs/utf/%s' % name)
        lines.append('root\t%s\t%d\t%08x\t%08x' % (
            root.name.encode('latin-1').hex() or '-', int(root.is_dir), root.data_off,
            root.data_size))
        dump(root, 0)
        info['mm'].close()
    write_lines('walker.txt', 'the view of CppSource/ReadFile/utf_walk_once.py', lines)


def main():
    archives()
    tree()
    match_list()
    join_list()
    dostime_list()
    utf_ops()
    dos_ops()
    mem_ops()
    walker_view()


if __name__ == '__main__':
    main()
