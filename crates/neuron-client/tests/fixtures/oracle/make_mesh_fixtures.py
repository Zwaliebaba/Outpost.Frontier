"""Writes the oracle inputs of the mesh render suites (work package P4.6).

    python make_mesh_fixtures.py [--check]

Everything here is synthetic: small UTF archives in mesh/ holding the nodes RendComp reads
(VMeshLibrary, MultiLevel, VMeshPart, VMeshWire, Mesh, Sphere), mesh_render.list and
mesh_faces.list (the IMesh and IRenderableMesh faces, second increment), the operations
CppSource/Oracle/oracle_mesh_render.cpp and tests/support/mesh_render.rs run on them. The name CRCs the archives hold are the engine's (NEURONCOMPONENTS_CRC::GetCRC32), whose
table this script reads from crates/neuron-core/src/hash.rs. With --check nothing is written;
the script fails when a file differs from what it would write.
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..', '..'))

MAGIC = 0x20465455
VERSION = 0x101
HEADER = 0x38
TIMES = (0x2C210021, 0, 0x2C216000)


def crc_table():
    text = open(os.path.join(REPO, 'crates', 'neuron-core', 'src', 'hash.rs')).read()
    body = text[text.index('const CRC_TABLE'):]
    body = body[body.index('[', body.index('=')) + 1:body.index('];')]
    values = [int(v.replace('_', ''), 16) for v in re.findall(r'0x[0-9A-Fa-f_]+', body)]
    assert len(values) == 256
    return values


TABLE = crc_table()


def name_crc(name):
    crc = 0xFFFFFFFF
    for b in name.encode('latin-1'):
        assert b < 0x80
        b = ord(chr(b).lower())
        crc = TABLE[(b ^ crc) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


class File:
    def __init__(self, name, data):
        self.name = name.encode()
        self.data = bytes(data)


class Dir:
    def __init__(self, name, children):
        self.name = name.encode()
        self.children = children


def build(children):
    """A UTF archive whose root directory holds `children` (0x2C-byte entries)."""
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
            if isinstance(item, File):
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
    t = HEADER
    s = t + len(tree)
    d = s + len(strings)
    head = bytearray(HEADER)
    for at, value in ((0, MAGIC), (4, VERSION), (8, t), (0x14, 0x2C), (0x18, s), (0x24, d)):
        struct.pack_into('<I', head, at, value)
    return bytes(head + tree + strings + data)


def u16s(values):
    return struct.pack('<%dH' % len(values), *values)


def u32s(values):
    return struct.pack('<%dI' % len(values), *[v & 0xFFFFFFFF for v in values])


def f32s(values):
    return struct.pack('<%df' % len(values), *values)


def fbits(value):
    return '%08x' % struct.unpack('<I', struct.pack('<f', value))[0]


def dbits(value):
    return '%016x' % struct.unpack('<Q', struct.pack('<d', value))[0]


def floats(values):
    return ','.join(fbits(v) for v in values)


def stride(fvf):
    position = 0x1C if fvf & 0xC == 0xC else 0x10 if fvf & 4 else 0xC if fvf & 2 else 0
    size = position + (0xC if fvf & 0x10 else 0) + (4 if fvf & 0x40 else 0) + (4 if fvf & 0x80 else 0)
    for t in range((fvf >> 8) & 0xF):
        size += (8, 0xC, 0x10, 4)[(fvf >> ((16 + 2 * t) & 31)) & 3]
    return size


def vmesh_data(fvf, groups, indices, vertex_count, seed=1):
    """groups: (material CRC, first vertex, last vertex, index count)."""
    out = bytearray(struct.pack('<BBBBIHHHH', 1, 0, 0, 0, 4, len(groups), len(indices), fvf,
                                vertex_count))
    for material, begin, end, count in groups:
        out += struct.pack('<IHHHH', material, begin, end, count, 0xCC)
    out += u16s(indices)
    x = seed
    for _ in range(stride(fvf) * vertex_count):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


def vmesh_ref(crc, vertex_start, vertex_count, index_start, index_count, group_start, group_count,
              radius=8.0):
    out = struct.pack('<II6H', 0x3C, crc, vertex_start, vertex_count, index_start, index_count,
                      group_start, group_count)
    out += f32s([4.0, -4.0, 3.5, -3.5, 2.0, -2.25, 0.5, -0.25, 1.0 / 3.0, radius])
    return out


def part(crc, *ranges, **kw):
    return Dir('VMeshPart', [File('VMeshRef', vmesh_ref(crc, *ranges, **kw))])


HULL0 = name_crc('hull.lod0.vms')
HULL1 = name_crc('hull.lod1.vms')
GLOW = name_crc('glow.vms')


def ship_cmp():
    library = Dir('VMeshLibrary', [
        Dir('hull.lod0.vms', [File('VMeshData', vmesh_data(
            0x112, [(0xA0000001, 0, 5, 6), (0xA0000002, 6, 8, 3), (0, 0, 2, 3)],
            [0, 1, 2, 3, 4, 5, 0, 1, 2, 2, 1, 0], 9, seed=11))]),
        Dir('glow.vms', [File('VMeshData', vmesh_data(
            0x142, [(0xA0000003, 0, 3, 6)], [0, 1, 2, 2, 3, 0], 4, seed=13))]),
        Dir('hull.lod1.vms', [File('VMeshData', vmesh_data(
            0x112, [(0xA0000001, 0, 2, 3)], [0, 1, 2], 3, seed=12))]),
    ])
    multi = Dir('MultiLevel', [
        File('Switch2', f32s([0.0, 120.5, 1000.0])),
        Dir('Level0', [part(HULL0, 0, 9, 0, 12, 0, 3)]),
        Dir('Level1', [part(HULL1, 0, 3, 0, 3, 0, 1, radius=2.5)]),
    ])
    wire = Dir('VMeshWire', [File('VWireData', struct.pack('<HHIHHHH', 0x10, 0, HULL0, 2, 7, 6, 9)
                                  + u16s([0, 1, 1, 2, 2, 0]))])
    return build([
        File('Exporter Version', b'synthetic\0'), library, multi, wire,
        Dir('Part2.3db', [part(HULL1, 1, 2, 0, 3, 0, 1)]),
        Dir('Glow.3db', [part(GLOW, 0, 4, 0, 6, 0, 1)]),
        Dir('Bad.3db', [Dir('Hardpoints', [])]),
        Dir('Multi0.3db', [Dir('MultiLevel', [File('Switch2', f32s([0.0, 10.0, 20.0])),
                                              Dir('Level0', [part(HULL1, 0, 3, 0, 3, 0, 1)])])]),
        Dir('NoWire.3db', [Dir('VMeshWire', [File('readme', b'x')])]),
    ])


def tube_3db(name, points, interpolate, frames, fps, groups, lookup):
    corners = len(points) and 9
    geometry = Dir('Geometry', [
        File('Points', f32s([c for p in points for c in p])),
        File('Point_indices', u32s([i % len(points) for i in range(corners)])),
        File('Vertex_normals', f32s([0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0])),
        File('Vertex_normal_indices', u32s([i % 3 for i in range(corners)])),
        File('UV0', f32s([i / 8.0 for i in range(2 * corners)])),
        File('UV0_indices', u32s(list(range(corners)))),
        File('Color0', u32s([0xFF102030, 0x80FFFFFF])),
        File('Color0_indices', u32s([i % 2 for i in range(corners)])),
        File('UV0_anim_lookup', u32s(lookup)),
        File('UV0_frame_count', u32s([frames])),
        File('UV0_anim', f32s([i / 16.0 + 0.03125 for i in range(2 * len(lookup) * frames)])),
        File('UV0_fps', f32s([fps])),
        File('UV0_interpolate', u32s([interpolate])),
    ])
    nodes = [File('Count', u32s([len(groups)]))]
    for index, group in enumerate(groups):
        material, kind, indices = group[:3]
        children = []
        if material is not None:
            children.append(File('Material_name', material.encode() + b'\0'))
        children.append(File(kind, u16s(indices)))
        children += list(group[3]) if len(group) > 3 else []
        nodes.append(Dir('Group%d' % index, children))
    return build([Dir('Mesh', [geometry, Dir('Face_groups', nodes)])])


TUBE_POINTS = [(0.0, 0.0, 0.0), (10.0, 0.0, 0.0), (0.0, 8.0, 0.0), (0.0, 0.0, 6.0),
               (7.0, 7.0, 7.0), (-3.0, 2.0, 9.0)]


def tube():
    return tube_3db('tube', TUBE_POINTS, 1, 3, 12.0, [
        ('TubeOuter', 'Face_indices', [0, 1, 2, 2, 3, 0]),
        ('tube_glass', 'Tristrip_indices', [3, 4, 5, 6]),
        (None, 'Face_indices', [6, 7, 8]),
        ('tube_missing', 'Face_indices', [1, 2, 3]),
        ('tube_decal', 'Face_indices', [4, 5, 6]),
        ('tube_plain', 'Face_indices', [5, 6, 7]),
    ], [0, -1, 2, 5])


def tube2():
    points = [(1.5, -2.0, 0.25), (-4.0, 3.0, 2.0), (2.0, 2.5, -6.0), (0.5, -0.5, 7.5),
              (-1.0, -5.0, -1.0), (3.25, 1.0, 1.0)]
    return tube_3db('tube2', points, 0, 4, 10.0, [
        ('TubeOuter', 'Face_indices', [0, 1, 2, 3, 4, 5]),
    ], [1, 3])


def edges():
    """A TriMesh whose groups have edges: the first with angles, the second with indices only,
    the third with angles only (a longer leaf than the edges need), the fourth none."""
    return tube_3db('edges', TUBE_POINTS, 1, 3, 12.0, [
        ('TubeOuter', 'Face_indices', [0, 1, 2, 2, 3, 0],
         [File('Edge_indices', u16s([0, 1, 1, 2, 2, 0, 0, 3])), File('Edge_angles', f32s([0.75, 0.25, 1.5, 0.0625]))]),
        ('tube_glass', 'Face_indices', [3, 4, 5],
         [File('Edge_indices', u16s([3, 4, 4, 5]))]),
        (None, 'Face_indices', [6, 7, 8],
         [File('Edge_angles', f32s([0.5, 2.0, 3.0]))]),
        ('tube_plain', 'Face_indices', [5, 6, 7, 1, 2, 3]),
    ], [0, -1, 2, 5])


def planet_sph():
    names = ['planet_side%d' % i for i in range(6)] + ['planet_atmo']
    return build([Dir('Sphere', [File('Radius', f32s([1000.0])), File('Sides', u32s([7]))]
                      + [File('M%d' % i, n.encode()) for i, n in enumerate(names)])])


def bare_sph():
    return build([Dir('Sphere', [File('Sides', u32s([2])), File('M1', b'Planet_Side1')])])


ARCHIVES = {
    'mesh/ship.cmp': ship_cmp,
    'mesh/tube.3db': tube,
    'mesh/tube2.3db': tube2,
    'mesh/edges.3db': edges,
    'mesh/planet.sph': planet_sph,
    'mesh/bare.sph': bare_sph,
}

I12 = floats([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0])


def xf(tx, ty, tz, rot=(1, 0, 0, 0, 1, 0, 0, 0, 1)):
    return floats(list(rot) + [tx, ty, tz])


QUARTER = (0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -1.0, 0.0, 0.0)
SHIP_MATS = [
    'mat hull #a0000001 8 Shading', 'mat trim #a0000002 1 Shading', 'mat glow #a0000003 0 Shading',
    'mat over #a0000099 18 Shading',
]

CASES = [
    # The levels: MultiLevel, the plugin walk, failures, ranges, geometry queries.
    ['fs mesh/ship.cmp'] + SHIP_MATS + [
        'lib a', 'arch 1 .', 'arch 1 .', 'lods 1', 'range 1 0', 'range 1 1', 'range 1 2',
        'setrange 1 1 %s %s' % (fbits(150.0), fbits(900.0)),
        'setrange 1 0 %s %s' % (fbits(9.0), fbits(1.0)), 'range 1 1', 'root 1 3', 'root 1 2',
        'root 9 1', 'arch 2 Part2.3db', 'arch 3 Bad.3db', 'arch 4 Multi0.3db',
        'geom 1 %s 0 4003' % fbits(100.0), 'geom 1 %s 3 4003' % fbits(140.0),
        'geom 1 %s 2 3' % fbits(100.0), 'geom 1 %s 0 4003' % fbits(-1.0),
        'geom 1 %s 0 4003' % fbits(2000.0), 'geomat 1 1 3 4003', 'geomat 1 2 3 4003',
        'bounds 1 %s' % fbits(10.0), 'centroid 2 %s' % fbits(0.0), 'bounds 2 %s' % fbits(-2.0),
        'qarch 1 vmesh', 'qarch 1 renderable', 'qarch 1 sphere', 'setlod 1 2', 'qarch 1 mesh',
        'setlod 1 3', 'lods 1', 'dup 6 1', 'dup 6 2', 'destroy 6', 'destroy 1', 'destroy 1',
        'lods 1', 'unlib a', 'end',
    ],
    # Instances and draws of the VMesh levels: culling, picking, children, matrices, overrides.
    ['fs mesh/ship.cmp'] + SHIP_MATS + [
        'lib a', 'arch 1 .', 'arch 2 Part2.3db', 'arch 3 Glow.3db',
        'instarch 10 1', 'instarch 11 2', 'instarch 12 3',
        'ext 10 00000000 %s' % floats([0, 0, 0]), 'ext 11 %s %s' % (fbits(3.0), floats([0.25, 0.5, -1.0])),
        'ext 12 %s %s' % (fbits(-1.0), floats([0, 0, 0])), 'inst 10 1', 'inst 11 2', 'inst 12 3',
        'xform 10 %s' % xf(0.0, 0.0, 50.0), 'xform 11 %s' % xf(2.0, -1.0, 60.0, QUARTER),
        'kids 10 11,12', 'ilods 10', 'qinst 10 hook', 'qinst 10 trianim',
        'draw 10 %s 8' % fbits(0.005), 'ilods 10', 'draw 10 %s 8' % fbits(0.01), 'ilods 10',
        'draw 10 %s 2008' % fbits(0.1), 'ilods 10', 'draw 10 %s 2088' % fbits(0.1),
        'setlod 1 1', 'draw 10 %s 2080' % fbits(1.0), 'cam class 1',
        'draw 10 %s 2000' % fbits(1.0), 'cam class 3',
        'draw 10 %s 2130 m=%s' % (fbits(1.0), xf(1.0, 2.0, 3.0, (0.0, 2.0, 0.0, -2.0, 0.0, 0.0, 0.0, 0.0, 2.0))),
        'draw 11 %s 1000 x=over' % fbits(1.0), 'pstate 0', 'draw 11 %s 6000' % fbits(1.0),
        'draw 11 %s 6000 m=%s' % (fbits(1.0), xf(0.5, 0.0, 0.0)), 'pstate -',
        'draw 12 %s 8000' % fbits(1.0), 'draw 10 %s 0' % fbits(-1.0),
        'cam vp 10 20 329 259', 'cam xs %s' % dbits(1.75), 'cam zn %s' % fbits(0.5),
        'root 1 1', 'draw 10 %s 2008' % fbits(0.002), 'ilod 10 1', 'ilod 10 2', 'ilod 10 3', 'ilods 10',
        'uninst 11', 'uninst 10', 'uninst 13', 'unlib a', 'end',
    ],
    # The registry and groups: two loads of one library, a part before its library, wires.
    ['fs mesh/ship.cmp'] + SHIP_MATS + [
        'arch 7 Part2.3db', 'lib a', 'lib b', 'wire 1 .', 'wire 2 NoWire.3db', 'wire 3 Bad.3db',
        'wdraw 1', 'wdraw 1', 'wdraw 9', 'ext 20 %s %s' % (fbits(2.0), floats([1, 1, 1])),
        'ext 21 %s %s' % (fbits(0.0005), floats([0, 1, 0])), 'instarch 20 7', 'inst 20 7', 'draw 20 %s 0' % fbits(1.0),
        'arch 8 Part2.3db', 'instarch 21 8', 'inst 21 8', 'draw 21 %s 0' % fbits(1.0),
        'draw 21 %s 0' % fbits(1.0), 'uninst 21', 'draw 21 %s 0' % fbits(1.0), 'unlib a',
        'wdraw 1', 'destroy 8', 'unlib b', 'wdraw 1', 'end',
    ],
    # A planet: hook-up, the atmosphere's scale, levels, the face and shell passes.
    ['fs mesh/planet.sph'] + ['mat side%d planet_side%d 8 Shading' % (i, i) for i in (0, 1, 2, 4, 5)] + [
        'mat atmo planet_atmo 1 AtmosphereMaterial %s' % fbits(1.05), 'mat over #a0000099 0 Shading',
        'arch 1 .', 'bounds 1 %s' % fbits(5.0), 'centroid 1 %s' % fbits(5.0),
        'geom 1 %s 2 4003' % fbits(5.0), 'qarch 1 hook', 'qarch 1 sphere', 'dup 2 1',
        'instarch 30 1', 'inst 30 1', 'qinst 30 sphere', 'qinst 30 mesh',
        'sphlevel 30 %s' % fbits(200.0), 'sphlevel 30 %s' % fbits(200.0),
        'sphlevel 30 %s' % fbits(50.0), 'sphlevel 30 %s' % fbits(10.0), 'sphlevel 30 %s' % fbits(1.0),
        'sphlevel 30 %s' % fbits(25.0), 'sphmax 30 4', 'sphlevel 30 %s' % fbits(25.0),
        'sphmax 30 9', 'sphradius 30 %s' % fbits(1200.0), 'sphlevel 30 %s' % fbits(25.0),
        'sphradius 30 %s' % fbits(1000.0),
        'xform 30 %s' % xf(10.0, -20.0, 5.0, QUARTER), 'cam pos %s' % floats([310.0, 180.0, 1105.0]),
        'cam xs %s' % dbits(66.0), 'draw 30 %s 100' % fbits(1.0),
        'cam pos %s' % floats([2500.0, 900.0, -1500.0]), 'cam xs %s' % dbits(300.0),
        'sphscale 30 %s' % fbits(1.25), 'draw 30 %s 100' % fbits(1.0),
        'draw 30 %s 100 x=over' % fbits(1.0), 'pstate 0',
        'draw 30 %s 100' % fbits(1.0), 'uninst 30', 'destroy 1', 'end',
    ],
    # A sphere without a radius and with a missing name.
    ['fs mesh/bare.sph', 'mat s1 planet_side1 0 Shading', 'arch 1 .', 'bounds 1 %s' % fbits(1.0),
     'instarch 31 1', 'inst 31 1', 'sphlevel 31 %s' % fbits(1.0), 'cam pos %s' % floats([0.0, 0.0, 9.0]),
     'draw 31 %s 100' % fbits(1.0), 'uninst 31', 'destroy 1', 'end'],
    # A TriMesh: buckets, bounds, the sphere, the UV0 animation with interpolation, the draw.
    ['fs mesh/tube.3db', 'mat outer tubeouter 8 Shading', 'mat glass tube_glass 1 Shading',
     'mat decal tube_decal 18 Shading', 'mat plain tube_plain 0 Shading', 'mat over #a0000099 0 Shading',
     'arch 1 .', 'centroid 1 %s' % fbits(1.0), 'bounds 1 %s' % fbits(1.0),
     'geom 1 %s 0 4003' % fbits(1.0), 'geom 1 %s 3 4003' % fbits(1.0), 'qarch 1 renderable',
     'instarch 40 1', 'inst 40 1', 'inst 40 1', 'centroid 1 %s' % fbits(1.0), 'qinst 40 trianim',
     'xform 40 %s' % xf(1.0, 2.0, 3.0), 'draw 40 %s 0' % fbits(1.0),
     'tick 40 %s' % fbits(0.05), 'draw 40 %s 4000' % fbits(1.0), 'tick 40 %s' % fbits(0.1),
     'notify %s' % fbits(0.07), 'draw 40 %s 9000 x=over' % fbits(1.0), 'failvm',
     'draw 40 %s 0 m=%s' % (fbits(1.0), xf(0.0, 1.0, 0.0)), 'pstate 0', 'draw 40 %s 0' % fbits(1.0),
     'dup 2 1', 'instarch 41 2', 'inst 41 2', 'tick 41 %s' % fbits(0.05), 'draw 41 %s 0' % fbits(1.0),
     'uninst 41', 'destroy 2', 'end'],
    # A TriMesh without interpolation: frames copied when they change.
    ['fs mesh/tube2.3db', 'mat outer tubeouter 8 Shading', 'arch 1 .', 'instarch 50 1', 'inst 50 1',
     'tick 50 %s' % fbits(0.04), 'draw 50 %s 0' % fbits(1.0), 'tick 50 %s' % fbits(0.08),
     'draw 50 %s 0' % fbits(1.0), 'tick 50 %s' % fbits(0.02), 'draw 50 %s 0' % fbits(1.0),
     'tick 50 %s' % fbits(0.3), 'draw 50 %s 0' % fbits(1.0), 'bounds 1 %s' % fbits(1.0),
     'uninst 50', 'destroy 1', 'end'],
]


SHIP_MATS = ['mat hull #a0000001 8 Shading', 'mat trim #a0000002 1 Shading',
             'mat glow #a0000003 0 Shading', 'mat over #a0000099 18 Shading']
TUBE_MATS = ['mat outer tubeouter 8 Shading', 'mat glass tube_glass 1 Shading',
             'mat plain tube_plain 0 Shading', 'mat over #a0000099 0 Shading']


def faces(target, *slots):
    return ['face %s %s' % (target, slot) for slot in slots]


# The IMesh and IRenderableMesh faces (second increment): "face a <archetype> <slot ...>" asks
# QueryArchetype, "face i <instance> <slot ...>" QueryInstance.
FACE_CASES = [
    # A VMesh part of three face groups over a mesh of three groups (position, normal, tex0).
    ['fs mesh/ship.cmp'] + SHIP_MATS + ['lib a', 'arch 1 .', 'arch 2 Part2.3db', 'arch 3 Glow.3db']
    + faces('a 1', 'count', 'prim', 'getmat', 'close', 'chopen 1', 'chclose', 'data 1', 'idx 1',
            'idxcount 1', 'getctx', 'setctx 7', 'open 3 0', 'open -1 0', 'open 1 5', 'open 0 0',
            'count', 'prim', 'getmat', 'chopen 1', 'chopen 10', 'chopen 180', 'chopen 3',
            'chlock 8', 'chlock 800', 'chunlock', 'chclose', 'data 1', 'data 8', 'data 2',
            'data 4', 'data 10', 'data 180', 'idx 1', 'idx 80', 'idxcount 1', 'idxcount 2',
            'getctx', 'setctx 7', 'close', 'open 2 1', 'getmat', 'idx 1', 'idxcount 1',
            'close', 'close', 'add 4', 'remove 0', 'setmat over', 'setdata 1', 'setidx 1',
            'setidxcount', 'addch 1', 'rmch 1', 'reset', 'noop', 'refresh', 'reject',
            'reject1', 'vcount', 'icount', 'copyidx 12', 'copyidx 3', 'copyvd 112 0 9',
            'copyvd 42 2 4')
    + faces('a 2', 'count', 'open 0 0', 'data 1', 'data 2', 'idx 1', 'getmat', 'vcount',
            'icount', 'copyidx 3', 'copyvd 112 0 2', 'close')
    + faces('a 3', 'open 0 0', 'data 10', 'data 1', 'idx 1', 'getmat', 'close')
    + ['setlod 1 1'] + faces('a 1', 'count', 'open 0 0', 'getmat', 'data 1', 'close')
    + ['setlod 1 2'] + faces('a 1', 'count')
    + faces('a 9', 'count')
    + ['instarch 10 1', 'instarch 11 2', 'ext 11 40400000 00000000,00000000,00000000', 'inst 11 2']
    + faces('i 11', 'count', 'open 0 0', 'getmat', 'data 1', 'vcount', 'copyidx 3', 'close')
    + faces('i 13', 'count')
    + ['unlib a', 'end'],
    # A TriMesh archetype and its instance; the archetype's cursor starts at the group count.
    ['fs mesh/tube.3db'] + TUBE_MATS + ['arch 1 .']
    + faces('a 1', 'count', 'prim', 'getmat', 'close', 'open 6 1', 'open 1 1',
            'open 0 1', 'prim', 'getmat', 'getctx', 'setctx 2a', 'getctx', 'chopen 1b',
            'chopen 4', 'chopen 80', 'chopen 4000', 'data 1', 'data 8', 'data 2', 'data 4',
            'data 10', 'data 80', 'data 100', 'data 4000', 'data 3', 'idx 1', 'idx 80',
            'idxcount 1', 'idxcount 2', 'idxcount 3', 'chlock 1b', 'idx 1', 'idx 8', 'idx 2',
            'idx 10', 'idx 4', 'idx 100', 'idx 40', 'chlock 4', 'chlock 1', 'chunlock', 'idx 1',
            'setdata 1', 'setidx 1', 'setidxcount', 'addch 1', 'rmch 1', 'close', 'open 2 1',
            'getmat', 'close', 'open 0 1', 'chlock 1', 'close', 'idx 1', 'vcount', 'icount',
            'copyidx 6', 'copyidx 5', 'copyvd 152 0 9', 'noop', 'reject', 'reject1')
    + ['instarch 40 1', 'inst 40 1']
    + faces('i 40', 'count', 'reject', 'reject1', 'open 0 1', 'prim', 'getmat', 'close', 'vcount')
    + ['dup 2 1'] + faces('a 2', 'count', 'open 0 1', 'getmat', 'close')
    + faces('a 1', 'open 0 1', 'setmat over', 'getmat', 'setmat -', 'getmat', 'setmat glass',
            'getmat', 'close', 'refresh', 'open 0 1', 'getmat', 'close', 'add 5', 'count',
            'open 6 1', 'prim', 'getmat', 'idxcount 1', 'add 4', 'close', 'add 4',
            'remove 7', 'count', 'reset', 'count', 'add 4', 'close',
            'add 4', 'count', 'open 0 1', 'prim', 'close', 'remove 0', 'count', 'vcount')
    + ['uninst 40', 'destroy 2', 'end'],
    # TriMesh groups with edges and edge angles.
    ['fs mesh/edges.3db'] + TUBE_MATS + ['arch 1 .', 'dup 2 1']
    + faces('a 1', 'open 0 1', 'chopen 80', 'chopen 100', 'chopen 180', 'data 80', 'data 100',
            'data 180', 'idx 80', 'idx 100', 'idxcount 2', 'chlock 180', 'idx 100', 'idx 80',
            'chunlock', 'idx 100', 'close', 'open 1 1', 'chopen 180', 'chopen 80', 'data 100',
            'idx 80', 'idxcount 2', 'chlock 80', 'chlock 100', 'close', 'open 3 1',
            'chopen 100', 'data 100', 'idx 100', 'chlock 100', 'idx 100', 'idxcount 2',
            'chunlock', 'close', 'open 2 1', 'chopen 80', 'idx 80', 'idxcount 2', 'close')
    + faces('a 2', 'open 0 1', 'data 100', 'idx 80', 'close', 'open 3 1', 'chopen 100',
            'data 100', 'close')
    + ['destroy 2', 'end'],
]


def write(name, data, check):
    path = os.path.join(HERE, name)
    if check:
        with open(path, 'rb') as existing:
            if existing.read() != data:
                sys.exit('%s is out of date; run make_mesh_fixtures.py' % name)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as out:
        out.write(data)


def main():
    check = '--check' in sys.argv[1:]
    for name, make in ARCHIVES.items():
        write(name, make(), check)
    text = '# P4.6 mesh render oracle inputs, written by make_mesh_fixtures.py.\n' + ''.join(
        ' ; '.join(case) + '\n' for case in CASES)
    write('mesh_render.list', text.encode('latin-1'), check)
    text = '# P4.6 mesh face oracle inputs, written by make_mesh_fixtures.py.\n' + ''.join(
        ' ; '.join(case) + '\n' for case in FACE_CASES)
    write('mesh_faces.list', text.encode('latin-1'), check)


if __name__ == '__main__':
    main()
