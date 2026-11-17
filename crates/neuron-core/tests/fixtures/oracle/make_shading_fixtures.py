"""Writes the oracle inputs of the shading suites (work package P1.7).

Everything here is synthetic: TGA and DDS levels made from counters, small
UTF archives with `Texture library`, `Material library` and `MaterialAnim`
nodes, and format-map lines. Run it with Python 3 from any directory; it
rewrites shading/ under this directory. The oracle harness
(CppSource/Oracle/oracle_shading*.cpp) and the Rust test
tests/oracle_shading.rs read the same files.
"""

import os
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, 'shading')


def write(name, data):
    path = os.path.join(ROOT, *name.split('/'))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def write_lines(name, header, lines):
    write(name, ('\n'.join(['# ' + header] + lines) + '\n').encode())


def counter(n, start=0, step=1):
    return bytes((start + i * step) & 0xFF for i in range(n))


def u32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def f32(*values):
    return b''.join(struct.pack('<f', v) for v in values)


# --- UTF archives (0x2C records, depth first) --------------------------------

class File:
    def __init__(self, name, data):
        self.name, self.data = name, data


class Dir:
    def __init__(self, name, children):
        self.name, self.children = name, children


def utf(children):
    tree = bytearray(0x2C)
    strings = bytearray(b'\\\0')
    data = bytearray()

    def name_offset(name):
        off = len(strings)
        strings.extend(name.encode('latin-1') + b'\0')
        return off

    def place(items):
        if not items:
            return 0
        first = len(tree)
        tree.extend(bytes(0x2C * len(items)))
        for i, item in enumerate(items):
            at = first + i * 0x2C
            nxt = at + 0x2C if i + 1 < len(items) else 0
            flags = 0x10 if isinstance(item, Dir) else 0x80
            struct.pack_into('<III', tree, at, nxt, name_offset(item.name), flags)
            if isinstance(item, File):
                struct.pack_into('<I', tree, at + 0x10, len(data))
                struct.pack_into('<I', tree, at + 0x1C, len(item.data))
                data.extend(item.data)
        for i, item in enumerate(items):
            if isinstance(item, Dir):
                struct.pack_into('<I', tree, first + i * 0x2C + 0x10, place(item.children))
        return first

    struct.pack_into('<III', tree, 0, 0, 0, 0x10)
    struct.pack_into('<I', tree, 0x10, place(children))
    t = 0x38
    s = t + len(tree)
    d = s + len(strings)
    header = struct.pack('<14I', 0x20465455, 0x101, t, 0, 0, 0x2C, s, 0, 0, d, 0, 0, 0, 0)
    return bytes(header + tree + strings + data)


# --- TGA ------------------------------------------------------------------------

def tga(kind, depth, width, height, descriptor=0, pixels=None, cmap=None, id_bytes=b'',
        cm_first=0, cm_entry=24, extra=b''):
    cm_len = 0 if cmap is None else 256
    header = struct.pack('<BBBHHBHHhhBB', len(id_bytes), 1 if cmap is not None else 0, kind,
                         cm_first, cm_len, cm_entry if cmap is not None else 0, 0, 0,
                         width, height, depth, descriptor)
    body = id_bytes
    if cmap is not None:
        # The decoder skips (first + length) * 3 bytes whatever the entry
        # width; pad so that the pixels it reads are inside the level.
        skip = (cm_first + cm_len) * 3
        body += cmap + bytes(max(0, skip - len(cmap)))
    if pixels is None:
        bpp = {8: 1, 15: 3, 16: 2, 24: 3, 32: 4}.get(depth, 0)
        if kind & 7 == 1 and cmap is not None:
            bpp = 1
        pixels = counter(width * height * bpp, width + height, 7)
    return header + body + pixels + extra


def palette(entry=3):
    return bytes((i * 3 + j * 85) & 0xFF for i in range(256) for j in range(entry))


def tga_list():
    blobs = [
        tga(2, 32, 4, 2),
        tga(2, 32, 4, 2, 0x20),
        tga(2, 32, 4, 2, 0x10),
        tga(2, 32, 4, 2, 0x30),
        tga(2, 32, 3, 3, 0x08),
        tga(2, 24, 3, 2),
        tga(2, 24, 3, 2, 0x20, id_bytes=b'ident'),
        tga(2, 16, 5, 1),
        tga(2, 16, 2, 2, 0x31),
        tga(1, 8, 4, 4, cmap=palette()),
        tga(1, 8, 4, 4, 0x20, cmap=palette()),
        tga(1, 8, 4, 2, 0x10, cmap=palette()),
        tga(1, 8, 2, 2, 0x30, cmap=palette(), cm_first=2),
        tga(1, 8, 2, 2, cmap=palette(4), cm_entry=32),
        tga(1, 8, 2, 2, cmap=palette(2), cm_entry=16),
        tga(3, 8, 2, 2),
        tga(10, 32, 2, 2),
        tga(0, 24, 2, 2),
        tga(2, 12, 2, 2),
        tga(4, 24, 2, 2),
        tga(2, 32, 0, 3),
        tga(2, 32, 2, 2, extra=b'TRUEVISION-XFILE.\0'),
    ]
    write_lines('tga.list', 'TGA levels, hex', [b.hex() for b in blobs])


# --- DDS ------------------------------------------------------------------------

DDPF_FOURCC, DDPF_RGB, DDPF_ALPHAPIXELS = 0x4, 0x40, 0x1


def dds(width, height, mips, fourcc=None, bits=0, masks=(0, 0, 0, 0), alpha=False,
        cube=False, pitch_flag=None, mip_flag=True, data=None):
    block = 8 if fourcc == b'DXT1' else 16
    levels = []
    w, h = width, height
    for _ in range(max(mips, 1)):
        if fourcc:
            levels.append(max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block)
        else:
            levels.append(max(w, 1) * max(h, 1) * (bits // 8))
        w, h = max(w // 2, 1), max(h // 2, 1)
    if pitch_flag is None:
        pitch_flag = fourcc is None
    flags = 0x1007 | (0x20000 if mip_flag else 0) | (0x8 if pitch_flag else 0x80000)
    pitch = width * (bits // 8) if pitch_flag else levels[0]
    pf_flags = (DDPF_FOURCC if fourcc else DDPF_RGB) | (DDPF_ALPHAPIXELS if alpha else 0)
    caps = 0x1000 | (0x400008 if mips > 1 else 0) | (0x8 if cube else 0)
    caps2 = 0xFE00 if cube else 0
    header = struct.pack('<4s7I44x', b'DDS ', 124, flags, height, width, pitch, 0, mips)
    header += struct.pack('<2I4s5I', 32, pf_flags, fourcc or b'\0\0\0\0', bits, *masks)
    header += struct.pack('<4I4x', caps, caps2, 0, 0)
    assert len(header) == 0x80
    faces = 6 if cube else 1
    if data is None:
        data = counter(sum(levels) * faces, width * 3 + height, 11)
    return header + data


def dds_list():
    blobs = [
        dds(16, 16, 5, b'DXT1'),
        dds(32, 8, 4, b'DXT1'),
        dds(8, 32, 3, b'DXT1'),
        dds(16, 16, 3, b'DXT3'),
        dds(8, 8, 4, b'DXT5'),
        dds(4, 4, 1, b'DXT2'),
        dds(4, 4, 1, b'DXT4'),
        dds(8, 8, 2, bits=16, masks=(0xF800, 0x7E0, 0x1F, 0)),
        dds(4, 4, 1, bits=16, masks=(0x7C00, 0x3E0, 0x1F, 0x8000), alpha=True),
        dds(4, 4, 1, bits=16, masks=(0x7C00, 0x3E0, 0x1F, 0)),
        dds(4, 4, 1, bits=16, masks=(0xF00, 0xF0, 0xF, 0xF000), alpha=True),
        dds(4, 2, 1, bits=24, masks=(0xFF0000, 0xFF00, 0xFF, 0)),
        dds(4, 4, 2, bits=32, masks=(0xFF0000, 0xFF00, 0xFF, 0xFF000000), alpha=True),
        dds(4, 4, 1, bits=32, masks=(0xFF0000, 0xFF00, 0xFF, 0)),
        dds(4, 4, 1, b'DXT1', cube=True),
        tga(2, 32, 2, 2),
    ]
    write_lines('dds.list', 'image files for D3DXGetImageInfoFromFileInMemory, hex', [b.hex() for b in blobs])


# --- format maps --------------------------------------------------------------

def format_maps_list():
    steps = [
        'defaults',
        '8:8:8:8:*:0 = DAOP',
        'empty',
        '16 : 5 : 6 : 5 : * : * = DAOT',
        '  32:8:8:8:8:* =DAA8 ; comment',
        '*:*:*:*:1:* = DAA1',
        '*:*:*:*:*:*=DAAA',
        'clear',
        'CLE = DAOP',
        '8:8 ; = DAOP',
        '8',
        '8 =',
        '24:8:8:8 = DAOTX',
        '-5:8:8:8:0:0 = DAAL',
        'x:y = DAA4',
        '16:5:5:5:1:1 = 1234',
        '=DAOP',
    ]
    lines = []
    for step in steps:
        if step in ('defaults', 'empty'):
            lines.append(step)
        else:
            lines.append('line ' + step.encode('latin-1').hex())
    write_lines('format_maps.list', 'defaults | empty | line <hex>, applied in order', lines)


# --- texture libraries --------------------------------------------------------

def frames(*records):
    out = b''
    for child, uv in records:
        out += u32(child) + b''.join(u32(x) for x in uv)
    return out


ONE = 0x3F800000


def texture_library():
    tga8 = tga(2, 32, 8, 8)
    tga4 = tga(2, 32, 4, 4)
    tga2 = tga(2, 32, 2, 2)
    lib = [
        Dir('dxt1_sq', [File('MIPS', dds(16, 16, 5, b'DXT1'))]),
        Dir('dxt1_wide', [File('MIPS', dds(32, 8, 4, b'DXT1'))]),
        Dir('dxt1_tall', [File('MIPS', dds(8, 32, 3, b'DXT1'))]),
        Dir('dxt1_deep', [File('MIPS', dds(4, 16, 4, b'DXT1'))]),
        Dir('dxt3', [File('MIPS', dds(16, 16, 3, b'DXT3'))]),
        Dir('dxt5', [File('MIPS', dds(8, 8, 4, b'DXT5'))]),
        Dir('rgb565', [File('MIPS', dds(8, 8, 2, bits=16, masks=(0xF800, 0x7E0, 0x1F, 0)))]),
        Dir('argb1555', [File('MIPS', dds(4, 4, 1, bits=16, masks=(0x7C00, 0x3E0, 0x1F, 0x8000),
                                         alpha=True))]),
        Dir('rgb888', [File('MIPS', dds(4, 2, 1, bits=24, masks=(0xFF0000, 0xFF00, 0xFF, 0)))]),
        Dir('argb8888', [File('MIPS', dds(4, 4, 2, bits=32, masks=(0xFF0000, 0xFF00, 0xFF, 0xFF000000),
                                         alpha=True))]),
        Dir('nopitch', [File('MIPS', dds(4, 4, 1, bits=16, masks=(0xF800, 0x7E0, 0x1F, 0),
                                        pitch_flag=False))]),
        Dir('both', [File('MIP0', tga4), File('MIPS', dds(4, 4, 1, b'DXT1'))]),
        Dir('mips_tga', [File('MIPS', tga4)]),
        Dir('tga32', [File('MIP2', tga2), File('MIP0', tga8), File('MIP1', tga4)]),
        Dir('tga_pal', [File('MIP0', tga(1, 8, 4, 4, 0x20, cmap=palette())),
                        File('MIP1', tga(1, 8, 2, 2, cmap=palette()))]),
        Dir('tga_pal16', [File('MIP0', tga(1, 8, 2, 2, cmap=palette(2), cm_entry=16))]),
        Dir('tga16', [File('mip0', tga(2, 16, 4, 4)), File('Mip1', tga(2, 16, 2, 2))]),
        Dir('tga24', [File('MIP0', tga(2, 24, 4, 2, 0x10))]),
        Dir('tga_gap', [File('MIP0', tga8), File('MIP2', tga2)]),
        Dir('tga_grey', [File('MIP0', tga(3, 8, 2, 2))]),
        Dir('tga_unsup', [File('MIP0', tga(2, 12, 2, 2)), File('MIP1', tga2)]),
        Dir('tga_mixed', [File('MIP0', tga(2, 24, 4, 4)), File('MIP1', tga(1, 8, 2, 2, cmap=palette()))]),
        Dir('tga_dup', [File('MIP0', tga4), File('mip0', tga2)]),
        Dir('tga_long', [File('MIP0', tga2), File('MIP10', tga2)]),
        Dir('tga_stale', [File('MIP1', tga(2, 32, 1, 1))]),
        Dir('tga_dir', [Dir('MIP0', [])]),
        Dir('cube', [File('CUBE', dds(4, 4, 1, b'DXT1', cube=True))]),
        Dir('cube_flat', [File('CUBE', dds(4, 4, 1, b'DXT1'))]),
        Dir('anim', [File('Texture count', u32(2)), File('FPS', f32(12.5)), File('Frame count', u32(3)),
                     File('Frame rects', frames((0, (0, 0, ONE, ONE)), (1, (0x3F000000, 0, ONE, ONE)),
                                                (5, (0x7F800001, 0xFF800000, 0, 0x80000000))))]),
        Dir('anim_0', [File('MIP0', tga4)]),
        Dir('anim_1', [File('MIPS', dds(4, 4, 1, b'DXT1'))]),
        Dir('anim_bad', [File('Texture count', u32(2)), File('Frame count', u32(1)),
                         File('Frame rects', frames((0, (0, 0, ONE, ONE))))]),
        Dir('anim_bad_0', [File('MIPS', dds(4, 4, 1, b'DXT5'))]),
        Dir('anim_short', [File('Texture count', u32(1)), File('Frame count', u32(2)),
                           File('Frame rects', frames((0, (0, 0, ONE, ONE))))]),
        Dir('anim_short_0', [File('MIP0', tga2)]),
        Dir('anim_nofps', [File('Texture count', u32(0)), File('Frame count', u32(1)),
                           File('Frame rects', frames((0, (0, 0, ONE, ONE))) + b'xyz')]),
        Dir('anim_empty', [File('Texture count', u32(0)), File('Frame count', u32(0)),
                           File('Frame rects', b'')]),
        Dir('anim_nested', [File('Texture count', u32(1)), File('Frame count', u32(2)),
                            File('Frame rects', frames((0, (0, 0, ONE, ONE)), (0, (0, 0, ONE, ONE))))]),
        Dir('anim_nested_0', [File('Texture count', u32(1)), File('FPS', f32(5.0)), File('Frame count', u32(2)),
                              File('Frame rects', frames((0, (0, 0, ONE, ONE)), (0, (0, 0, ONE, ONE))))]),
        Dir('anim_nested_0_0', [File('MIP0', tga2)]),
        Dir('dup', [File('MIPS', dds(4, 4, 1, b'DXT1'))]),
        Dir('DUP', [File('MIPS', dds(8, 8, 1, b'DXT1'))]),
        Dir('.hidden', [File('MIPS', dds(4, 4, 1, b'DXT1'))]),
        Dir('none', [File('junk', b'123')]),
        File('loose', b'not a node'),
    ]
    write('texlib/basic.utf', utf([Dir('Texture library', lib), File('Exporter Version', b'x\0')]))
    write('texlib/lower.utf', utf([Dir('texture library', [Dir('t', [File('MIPS', dds(4, 4, 1, b'DXT3'))])])]))
    write('texlib/notex.utf', utf([Dir('Material library', [])]))
    write_lines('texlib.list', 'archive lod_load square pow2 lod_min square_only', [
        'shading/texlib/basic.utf 1 0 0 0 0',
        'shading/texlib/basic.utf 0 0 0 0 0',
        'shading/texlib/basic.utf 1 0 0 4 0',
        'shading/texlib/basic.utf 1 2 1 0 0',
        'shading/texlib/basic.utf 1 1 2 8 1',
        'shading/texlib/lower.utf 1 0 0 0 0',
        'shading/texlib/notex.utf 1 0 0 0 0',
        'shading/texlib/missing.utf 1 0 0 0 0',
    ])


# --- material libraries -------------------------------------------------------

# Leaf names and value types of each schema family (Material.cpp).
COLOUR, FLOAT, WORD, NAME, WRAP, ADDR = 'c', 'f', 'w', 'n', 'wrap', 'addr'
SINGLE = [('Ac', COLOUR), ('Dc', COLOUR), ('Ec', COLOUR), ('Sc', COLOUR), ('Sp', FLOAT), ('Oc', FLOAT),
          ('Dt_name', NAME), ('Dt_flags', WRAP)]
DETAIL = [('Ac', COLOUR), ('Dc', COLOUR), ('Dt_name', NAME), ('Dt_flags', WRAP), ('flip u', WORD),
          ('flip v', WORD), ('Dm_name', NAME), ('Dm_flags', WRAP), ('Dm0_name', NAME), ('Dm0_flags', WRAP),
          ('Dm1_name', NAME), ('Dm1_flags', WRAP), ('TileRate', FLOAT), ('TileRate0', FLOAT),
          ('TileRate1', FLOAT)]
FAMILIES = {
    'single': SINGLE,
    'dcdtet': [('Dc', COLOUR), ('Dt_name', NAME), ('Dt_flags', WRAP), ('Et_name', NAME), ('Et_flags', WRAP)],
    'ecet': [('Ec', COLOUR), ('Et_name', NAME), ('Et_flags', WRAP)],
    'atmo': [('Ac', COLOUR), ('Dc', COLOUR), ('Dt_name', NAME), ('Dt_flags', WRAP), ('Fade', FLOAT),
             ('Alpha', FLOAT), ('Scale', FLOAT)],
    'tex': [('Dt_name', NAME), ('Dt_flags', WRAP)],
    'detail': DETAIL,
    'bt': SINGLE + [('Bt_name', NAME), ('Bt_flags', WRAP)],
    'null': [],
}
KINDS = [
    ('DcDt', 'single'), ('DcDtEc', 'single'), ('DcDtOcOt', 'single'), ('DcDtEcOcOt', 'single'),
    ('DcDtTwo', 'single'), ('DcDtEcTwo', 'single'), ('DcDtOcOtTwo', 'single'), ('DcDtEcOcOtTwo', 'single'),
    ('DcDtEt', 'dcdtet'), ('EcEt', 'ecet'), ('EcEtTwo', 'ecet'), ('AtmosphereMaterial', 'atmo'),
    ('Nebula', 'tex'), ('NebulaTwo', 'tex'), ('NullMaterial', 'null'), ('LightMap', 'tex'),
    ('DetailMapMaterial', 'detail'), ('DetailMap2Dm1Msk2PassMaterial', 'detail'),
    ('IllumDetailMapMaterial', 'detail'), ('Masked2DetailMapMaterial', 'detail'),
    ('BtDetailMapMaterial', 'bt'), ('BtDetailMapTwoMaterial', 'bt'),
]


def leaf(kind, index, seed):
    if kind == COLOUR:
        return f32(0.25 * (seed % 5), 0.5 + seed, 1.0 / (seed + 2))
    if kind == FLOAT:
        return f32(0.125 * (seed % 9))
    if kind == WORD:
        return u32([1, 0, 0xFFFFFFFF, 7, 0x80000000][seed % 5])
    if kind == NAME:
        return ('tex_%d_%d.tga' % (index, seed)).encode() + b'\0'
    return u32([0x29, 0xFF, 0x3C, 0x10, 0][seed % 5])


def material_library():
    mats = []
    for n, (name, family) in enumerate(KINDS):
        leaves = [File('Type', name.encode() + b'\0')]
        for i, (leaf_name, kind) in enumerate(FAMILIES[family]):
            leaves.append(File(leaf_name, leaf(kind, i, n + i)))
        mats.append(Dir('full_' + name.lower(), leaves))
        mats.append(Dir('init_' + name.lower(), [File('Type', name.encode())]))
    odd = [
        Dir('odd_short', [File('Type', b'DcDt\0'), File('Dc', f32(0.5, 0.5)), File('Oc', b'\0\0'),
                          File('Sp', f32(2.0) + b'more'), File('Ac', f32(0.1, 0.2, 0.3, 0.4))]),
        Dir('odd_names', [File('Type', b'DcDtEt\0'), File('Dt_name', b'name_with_nul\0trail'),
                          File('Et_name', b''), File('Et_flags', b'\x99')]),
        Dir('odd_nan', [File('Type', b'AtmosphereMaterial'), File('Fade', u32(0x7F800001)),
                        File('Alpha', u32(0xFFBFFFFF)), File('Scale', u32(0x00000001)),
                        File('Dc', u32(0x7F800001) * 3)]),
        Dir('odd_opaque', [File('Type', b'DcDtOcOt\0'), File('Oc', u32(0x7FC00000))]),
        Dir('odd_flips', [File('Type', b'Masked2DetailMapMaterial\0'), File('flip u', u32(0x7FFFFFFF)),
                          File('flip v', u32(0xFFFFFFFE)), File('Dm1_flags', u32(0xFFFFFFFF)),
                          File('TileRate1', f32(-3.0))]),
        Dir('ODD_FLIPS', [File('Type', b'DcDt\0')]),
        Dir('odd_unknown', [File('Type', b'DcDtBtOcOtTwo\0'), File('Dc', f32(1, 1, 1))]),
        Dir('odd_case', [File('Type', b'dcdt\0')]),
        Dir('odd_notype', [File('Dc', f32(1, 1, 1))]),
        Dir('odd_emptytype', [File('Type', b'')]),
        Dir('odd_longtype', [File('Type', b'D' * 0x110)]),
        Dir('odd_nultype', [File('Type', b'DcDt\0Ec')]),
        Dir('odd_unknown', [File('Type', b'DcDt\0')]),
        Dir('.dot', [File('Type', b'DcDt\0')]),
        File('loose', b'DcDt\0'),
    ]
    anims = [
        Dir('spin', [File('MACount', u32(3)), File('MAFlags', u32(0x11)),
                     File('MADeltas', f32(*[0.5 * i for i in range(15)])),
                     File('MAKeys', f32(*[0.25 * i for i in range(8)]))]),
        Dir('single', [File('MACount', u32(1)), File('MADeltas', f32(1, 2, 3, 4, 5)),
                       File('MAKeys', f32(9, 9, 9, 9))]),
        Dir('short', [File('MACount', u32(4)), File('MAFlags', b'\1\2'), File('MADeltas', f32(1, 2, 3)),
                      File('MAKeys', f32(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16))]),
        Dir('nodeltas', [File('MACount', u32(2)), File('MAKeys', f32(1, 2, 3, 4))]),
        Dir('zero', [File('MACount', u32(0)), File('MAFlags', u32(3)), File('MADeltas', f32(1, 2, 3, 4, 5))]),
        Dir('empty', []),
        Dir('SPIN', [File('MACount', u32(2)), File('MADeltas', f32(*range(10)))]),
        File('loose', u32(1)),
    ]
    write('material/mats.utf', utf([Dir('Material library', [File('Material count', u32(len(mats)))] + mats
                                        + odd),
                                    Dir('MaterialAnim', anims)]))
    write('material/anim_only.utf', utf([Dir('materialanim', [Dir('a', [File('MACount', u32(1))])])]))
    write('material/neither.utf', utf([Dir('Texture library', [])]))
    write_lines('material.list', 'archives', [
        'shading/material/mats.utf',
        'shading/material/anim_only.utf',
        'shading/material/neither.utf',
        'shading/material/missing.utf',
    ])


def main():
    if os.path.isdir(ROOT):
        shutil.rmtree(ROOT)
    tga_list()
    dds_list()
    format_maps_list()
    texture_library()
    material_library()


if __name__ == '__main__':
    main()
