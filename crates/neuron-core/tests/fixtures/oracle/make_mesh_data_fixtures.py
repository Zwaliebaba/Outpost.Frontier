"""Writes the oracle inputs of the mesh data suites (work package P1.7).

Everything here is synthetic: small UTF archives holding the nodes the
RendComp and Deformable2 loaders read (VMeshLibrary, VMeshPart, VMeshWire,
MultiLevel, Mesh, Sphere, and the .dfm skeleton and mesh tables), built from
the layouts in crates/neuron-core/src/mesh_data/. Run it with Python 3 from
any directory; it rewrites mesh_data/ under this directory. The oracle
harness (CppSource/Oracle/oracle_mesh_data*.cpp) and the Rust test
tests/oracle_mesh_data.rs read the same files.

No input here makes the reference read or write outside its buffers: those
cases are covered by the Rust unit tests only (divergence register).
"""

import os
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, 'mesh_data')

MAGIC = 0x20465455
VERSION = 0x101
HEADER = 0x38
TIMES = (0x2C210021, 0, 0x2C216000)


class File:
    def __init__(self, name, data):
        self.name = name
        self.data = bytes(data)


class Dir:
    def __init__(self, name, children):
        self.name = name
        self.children = children


def build(children):
    """A 0x2C archive whose root directory holds `children`."""
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


def write(name, data):
    path = os.path.join(ROOT, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def write_lines(name, header, lines):
    write(name, ('\n'.join(['# ' + header] + lines) + '\n').encode())


def u16s(values):
    return struct.pack('<%dH' % len(values), *values)


def u32s(values):
    return struct.pack('<%dI' % len(values), *[v & 0xFFFFFFFF for v in values])


def f32s(values):
    return struct.pack('<%df' % len(values), *values)


def stride(fvf):
    if fvf & 0xC == 0xC:
        position = 0x1C
    elif fvf & 4:
        position = 0x10
    elif fvf & 2:
        position = 0xC
    else:
        position = 0
    size = position + (0xC if fvf & 0x10 else 0)
    size += (4 if fvf & 0x40 else 0) + (4 if fvf & 0x80 else 0)
    for t in range((fvf >> 8) & 0xF):
        size += (8, 0xC, 0x10, 4)[(fvf >> ((16 + 2 * t) & 31)) & 3]
    return size


def vmesh_data(fvf, groups, indices, vertex_count, flags=1, seed=1, trailing=b''):
    """groups: (material CRC, first vertex, last vertex, index count)."""
    out = bytearray(struct.pack('<BBBBIHHHH', flags, 0, 0, 0, 4, len(groups), len(indices), fvf,
                                vertex_count))
    for material, begin, end, count in groups:
        out += struct.pack('<IHHHH', material, begin, end, count, 0xCC)
    out += u16s(indices)
    x = seed
    for _ in range(stride(fvf) * vertex_count):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out + trailing)


def vmesh_ref(crc, vertex_start, vertex_count, index_start, index_count, group_start, group_count,
              radius=8.0, unknown=0x3C):
    out = struct.pack('<II6H', unknown, crc, vertex_start, vertex_count, index_start, index_count,
                      group_start, group_count)
    out += f32s([4.0, -4.0, 3.5, -3.5, 2.0, -2.25, 0.5, -0.25, 1.0 / 3.0, radius])
    return out


def vwire(crc, indices, bias=2, remap=7, vertices=12, trailing=b''):
    return struct.pack('<HHIHHHH', 0x10, 0, crc, bias, remap, len(indices), vertices) + u16s(indices) \
        + trailing


def part(crc, *ranges):
    return Dir(b'VMeshPart', [File(b'VMeshRef', vmesh_ref(crc, *ranges))])


def library_vms():
    entries = [
        Dir(b'data.mesh.lod0-112.vms', [File(b'VMeshData', vmesh_data(
            0x112, [(0x11111111, 0, 3, 6), (0x22222222, 4, 6, 3)], [0, 1, 2, 2, 3, 0, 0, 1, 2], 7))]),
        Dir(b'second.lod1.vms', [File(b'VMeshData', vmesh_data(
            0x112, [(0x33333333, 2, 4, 3)], [0, 1, 2, 2, 1], 5, seed=2))]),
        Dir(b'diffuse.vms', [File(b'VMeshData', vmesh_data(
            0x142, [(0, 0, 2, 3)], [2, 1, 0], 3, seed=3, trailing=b'tail'))]),
        Dir(b'DATA.MESH.LOD0-112.VMS', [File(b'VMeshData', vmesh_data(0x2, [], [], 1, seed=4))]),
        Dir(b'two_sets.vms', [File(b'VMeshData', vmesh_data(
            0x252, [(5, 0, 1, 3)], [0, 1, 0], 2, seed=5))]),
        Dir(b'old.vms', [File(b'VMeshData', vmesh_data(0x112, [], [], 0, flags=0))]),
        Dir(b'empty.vms', [File(b'VMeshData', b'')]),
        Dir(b'no_leaf.vms', [File(b'readme', b'x')]),
        Dir(b'.hidden', [File(b'VMeshData', vmesh_data(0x112, [], [], 1))]),
        File(b'loose.vms', b'not a mesh'),
        Dir(b'blend.vms', [File(b'VMeshData', vmesh_data(0x1C, [], [], 2, seed=6))]),
        Dir(b'zero_fvf.vms', [File(b'VMeshData', vmesh_data(0, [], [], 3, seed=7))]),
        Dir(b'after_zero.vms', [File(b'VMeshData', vmesh_data(0x112, [(9, 1, 1, 3)], [0, 0, 0], 2,
                                                             seed=8))]),
        Dir(b'flag3.vms', [File(b'VMeshData', vmesh_data(0x212, [(1, 0, 0, 3)], [0, 0, 0], 1,
                                                        flags=3, seed=9))]),
    ]
    return build([Dir(b'VMeshLibrary', entries), File(b'Exporter Version', b'synthetic\0')])


def ship_cmp():
    library = Dir(b'VMeshLibrary', [
        Dir(b'ship.lod0.vms', [File(b'VMeshData', vmesh_data(
            0x112, [(0xA, 0, 5, 6), (0xB, 6, 8, 3)], [0, 1, 2, 3, 4, 5, 0, 1, 2], 9, seed=11))]),
        Dir(b'ship.lod1.vms', [File(b'VMeshData', vmesh_data(
            0x112, [(0xA, 0, 2, 3)], [0, 1, 2], 3, seed=12))]),
    ])
    multi = Dir(b'MultiLevel', [
        File(b'Switch2', f32s([0.0, 120.5, 1000.0])),
        Dir(b'Level0', [part(0xC0FFEE01, 0, 9, 0, 9, 0, 2)]),
        Dir(b'Level1', [part(0xC0FFEE02, 0, 3, 0, 3, 0, 1, 2.5)]),
    ])
    wire = Dir(b'VMeshWire', [File(b'VWireData', vwire(0xC0FFEE01, [0, 1, 1, 2, 2, 0]))])
    part1 = Dir(b'Part1.3db', [part(0x1234, 1, 2, 3, 4, 5, 6),
                               Dir(b'VMeshWire', [File(b'VWireData', vwire(0x1234, [3, 4], trailing=b'\x01\x02\x03'))])])
    part2 = Dir(b'Part2.3db', [Dir(b'MultiLevel', [Dir(b'Level0', [part(0x55, 0, 1, 0, 3, 0, 1)])])])
    part3 = Dir(b'Part3.3db', [
        Dir(b'MultiLevel', [File(b'Switch2', f32s([0.0, 10.0, 20.0])),
                            Dir(b'Level0', [part(0x66, 0, 1, 0, 3, 0, 1)])]),
        part(0x67, 0, 1, 0, 3, 0, 1),
    ])
    part4 = Dir(b'Part4.3db', [Dir(b'VMeshWire', [File(b'VWireData', b'')])])
    part5 = Dir(b'Part5.3db', [Dir(b'VMeshPart', [File(b'VMeshRef', vmesh_ref(0x77, 0, 1, 0, 3, 0, 1)
                                                        + vmesh_ref(0x78, 9, 9, 9, 9, 9, 9))])])
    part6 = Dir(b'Part6.3db', [Dir(b'MultiLevel', [
        File(b'Switch2', f32s([0.0, 50.0, 60.0, 70.0])),
        Dir(b'Level0', [part(0x81, 0, 1, 0, 3, 0, 1)]),
        Dir(b'Level1', [part(0x82, 0, 1, 0, 3, 0, 1),
                        Dir(b'Sphere', [File(b'Radius', f32s([5.0])), File(b'Sides', u32s([1])),
                                        File(b'M0', b'Glow\0')])]),
        Dir(b'Level2', [Dir(b'Sphere', [File(b'Sides', u32s([0]))])]),
    ])])
    nested = Dir(b'Group', [Dir(b'Inner.3db', [part(0x99, 0, 1, 0, 3, 0, 1)]),
                            File(b'note', b'x')])
    return build([File(b'Exporter Version', b'synthetic\0'), library, multi, wire, part1, part2,
                  part3, part4, part5, part6, nested,
                  Dir(b'Cmpnd', [Dir(b'Cons', [File(b'Sphere', b'\0' * 0xD4)])])])


def tube_3db():
    points = f32s([float(i) / 4.0 for i in range(3 * 6)])
    corners = [0, 1, 2, 3, 4, 5, 1, 2, 3]
    lookup = [0, -1, 2, 5]
    frames = 3
    geometry = Dir(b'Geometry', [
        File(b'Point_indices', u32s(corners)),
        File(b'Vertex_normal_indices', u32s([0, 0, 1, 1, 2, 2, 0, 1, 2])),
        File(b'UV0_indices', u32s([0, 1, 2, 3, 4, 5, 6, 7, 8])),
        File(b'UV1_indices', u32s([0] * 9)),
        File(b'Color0_indices', u32s([1] * 9)),
        File(b'Points', points),
        File(b'Vertex_normals', f32s([0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0])),
        File(b'UV0', f32s([i / 8.0 for i in range(18)])),
        File(b'UV1', f32s([0.5, 0.25])),
        File(b'Color0', u32s([0xFF102030, 0x80FFFFFF])),
        File(b'UV0_anim_lookup', u32s(lookup)),
        File(b'UV0_frame_count', u32s([frames])),
        File(b'UV0_anim', f32s([i / 16.0 for i in range(2 * len(lookup) * frames)])),
        File(b'UV0_fps', f32s([12.0])),
        File(b'UV0_interpolate', u32s([1])),
    ])
    groups = Dir(b'Face_groups', [
        File(b'Count', u32s([3])),
        Dir(b'Group1', [
            File(b'Material_name', b'Tube_Inner_Material_With_A_Very_Long_Name_That_Is_Cut_At_63_Bytes!!'),
            File(b'Face_indices', u16s([8, 7, 6, 5, 4, 3])),
            File(b'Edge_indices', u16s([0, 1, 1, 2, 2, 0])),
            File(b'Edge_angles', f32s([0.5, 1.5, 2.5])),
        ]),
        Dir(b'Group0', [
            File(b'Material_name', b'TubeOuter\0junk'),
            File(b'Tristrip_indices', u16s([10, 11, 12, 12, 13, 14, 15, 16])),
        ]),
        Dir(b'Group2', [
            File(b'Face_indices', u16s([2, 1, 0])),
            File(b'Edge_angles', f32s([9.0])),
        ]),
        Dir(b'Group3', [File(b'Material_name', b'beyond'), File(b'Face_indices', u16s([0, 1, 2]))]),
        Dir(b'Group02', [File(b'Material_name', b'padded'), File(b'Face_indices', u16s([0, 1, 2]))]),
    ])
    return build([Dir(b'Mesh', [geometry, groups])])


def bare_tube_3db():
    """A Mesh without Geometry; one strip group kept and converted."""
    groups = Dir(b'Face_groups', [
        File(b'Count', u32s([1])),
        Dir(b'group0', [File(b'Material_name', b'x'), File(b'Tristrip_indices', u16s([1, 2, 3]))]),
    ])
    return build([Dir(b'Mesh', [groups])])


def planet_sph():
    sphere = Dir(b'Sphere', [
        File(b'Radius', f32s([3000.0])),
        File(b'Sides', u32s([4])),
        File(b'M3', b'Planet_Cap'),
        File(b'M0', b'planet_side1\0garbage'),
        File(b'M1', b'PLANET_SIDE2_WITH_A_NAME_LONGER_THAN_SIXTY_THREE_BYTES_IS_CUT_THERE'),
        File(b'm7', b'past_the_sides'),
        File(b'M02', b'not_m2'),
    ])
    return build([sphere])


def bare_sph():
    return build([Dir(b'Sphere', [File(b'Radius', f32s([1.0])[:3]), File(b'M0', b'unused')])])


def plain_3db():
    return build([File(b'Exporter Version', b'x\0'), Dir(b'Hardpoints', [Dir(b'Fixed', [])])])


def bone(name, values, skeleton=False):
    leaf = File(b'Bone to root', f32s(values))
    if skeleton:
        return Dir(name, [Dir(b'DFMSkeleton', [leaf])])
    return Dir(name, [leaf, File(b'Lod Bits', b'\xff')])


def identity(scale, x, y, z):
    return [scale, 0.0, 0.0, 0.0, scale, 0.0, 0.0, 0.0, scale, x, y, z]


def dfm_mesh(name, points, groups, bone_ids, counts, weights, uv_deform=None, uv_rows=0,
             material_count=None):
    """groups: (node name, material name or None, ('strip'|'face', indices))."""
    face_children = [File(b'Count', u32s([material_count if material_count is not None else len(groups)]))]
    for node, material, (kind, indices) in groups:
        children = []
        if material is not None:
            children.append(File(b'Material_name', material))
        if kind == 'strip':
            children.append(File(b'Tristrip_indices', u16s(indices)))
        elif kind == 'face':
            children.append(File(b'Face_indices', u16s(indices)))
        face_children.append(Dir(node, children))
    n = len(points) // 3
    corners = [i % n for i in range(n + 3)]
    geometry = [
        File(b'Point_indices', u32s(corners)),
        File(b'Points', f32s(points)),
        File(b'Vertex_normals', f32s([((i * 7) % 5) / 5.0 for i in range(3 * n)])),
        File(b'UV0_indices', u32s(list(range(len(corners))))),
        File(b'UV0', f32s([i / 32.0 for i in range(2 * len(corners))])),
        File(b'Point_bone_first', u32s([0] * n)),
        File(b'Point_bone_count', u32s(counts)),
        File(b'Bone_id_chain', u32s(bone_ids)),
        File(b'Bone_weight_chain', f32s(weights)),
    ]
    if uv_rows:
        columns = [b'UV_bone_id', b'UV_vertex_count', b'UV_plane_distance', b'Bone_X_to_U_scale',
                   b'Bone_Y_to_V_scale', b'Min_du', b'Max_du', b'Min_dv', b'Max_dv']
        for c, column in enumerate(columns):
            if c < 2:
                geometry.append(File(column, u32s([c * 10 + r for r in range(uv_rows)])))
            else:
                geometry.append(File(column, f32s([c + r / 4.0 for r in range(uv_rows)])))
    return Dir(name, [Dir(b'Face_groups', face_children), Dir(b'Geometry', geometry)])


def body_dfm():
    bones = [
        bone(b'Root.3db', identity(1.0, 0.0, 0.0, 0.0)),
        bone(b'Spine.3db', identity(1.0, 0.0, 1.0, 0.0)),
        bone(b'Head.3db', identity(0.5, 0.0, 2.0, 0.25)),
        Dir(b'Short.3db', [File(b'Bone to root', b'\0' * 0x2F)]),
        bone(b'Arm.3db', identity(2.0, 1.0, 0.0, 0.0), skeleton=True),
    ]
    points = [float(i) for i in range(3 * 6)]
    mesh0 = dfm_mesh(
        b'Mesh0', points,
        [(b'Group0', b'skin_body\0', ('strip', [3, 4, 5, 5, 6, 7, 3])),
         (b'Group1', b'Eyes\0', ('face', [0, 1, 2, 2, 1, 0])),
         (b'Group5', None, ('face', [0, 1, 2]))],
        bone_ids=[0, 1, 0, 2, 3, 1, 2], counts=[1, 2, 1, 0, 2, 1],
        weights=[1.0, 0.5, 0.5, 1.0, 0.25, 0.75, 1.0], uv_rows=2)
    mesh1 = dfm_mesh(
        b'Mesh1', points[:9],
        [(b'Group0', b'skin_body\0', ('face', [2, 0, 1]))],
        bone_ids=[0, 0, 1], counts=[1, 1, 1], weights=[1.0, 1.0, 1.0], material_count=4)
    last_bone = dfm_mesh(
        b'Mesh2', points[:6],
        [(b'Group0', b'skin\0', ('face', [0, 1, 1]))],
        bone_ids=[3, 0], counts=[1, 1], weights=[1.0, 1.0])
    no_groups = Dir(b'Mesh3', [Dir(b'Geometry', [])])
    multi = Dir(b'MultiLevel', [File(b'Fractions', f32s([1.0, 0.5, 0.25])), mesh0, mesh1, last_bone,
                                no_groups])
    return build(bones + [Dir(b'Skeleton', [File(b'Name', b'body\0')]), multi])


def flat_dfm():
    mesh = dfm_mesh(b'Mesh0', [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0],
                    [(b'Group0', b'm\0', ('face', [1, 2, 0]))],
                    bone_ids=[0, 0, 0], counts=[1, 1, 1], weights=[1.0, 1.0, 1.0])
    return build([bone(b'Only.3db', identity(1.0, 0.0, 0.0, 0.0)), mesh,
                  File(b'Mesh_note', b'a file')])


def lean_dfm():
    """MultiLevel without Fractions, and nine fractions in another file."""
    mesh = dfm_mesh(b'Mesh0', [0.0] * 9, [(b'Group0', b'a\0', ('face', [0, 1, 2]))],
                    bone_ids=[], counts=[0, 0, 0], weights=[])
    return build([Dir(b'MultiLevel', [mesh])])


def many_fractions_dfm():
    mesh = dfm_mesh(b'Mesh0', [0.0] * 9, [(b'Group0', b'a\0', ('face', [0, 1, 2]))],
                    bone_ids=[], counts=[0, 0, 0], weights=[])
    return build([Dir(b'MultiLevel', [File(b'Fractions', f32s([1.0 - i / 10.0 for i in range(9)])
                                           + b'\x01'), mesh])])


FVFS = [0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE, 0x12, 0x1C, 0x42, 0x82, 0xC2, 0x102, 0x112, 0x142,
        0x152, 0x1C4, 0x212, 0x252, 0x312, 0x342, 0x3C2, 0x412, 0x812, 0xF12, 0x10112, 0x30212,
        0xC0000312, 0x80000F02, 0xFFFFFFFF, 0x1000]


def main():
    if os.path.isdir(ROOT):
        shutil.rmtree(ROOT)
    meshes = {
        'library.vms': library_vms(),
        'ship.cmp': ship_cmp(),
        'tube.3db': tube_3db(),
        'bare_tube.3db': bare_tube_3db(),
        'planet.sph': planet_sph(),
        'bare.sph': bare_sph(),
        'plain.3db': plain_3db(),
    }
    dfms = {
        'body.dfm': body_dfm(),
        'flat.dfm': flat_dfm(),
        'lean.dfm': lean_dfm(),
        'many_fractions.dfm': many_fractions_dfm(),
    }
    for name, data in list(meshes.items()) + list(dfms.items()):
        write(name, data)
    write_lines('mesh.list', 'archives for mesh_files.jsonl',
                ['mesh_data/' + n for n in meshes] + ['mesh_data/missing.3db'])
    write_lines('dfm.list', 'archives for dfm_files.jsonl',
                ['mesh_data/' + n for n in dfms] + ['mesh_data/missing.dfm'])
    write_lines('fvf.list', 'FVF codes for mesh_fvf.jsonl, hex', ['%x' % f for f in FVFS])


if __name__ == '__main__':
    main()
