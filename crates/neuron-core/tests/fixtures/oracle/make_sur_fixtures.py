"""Writes the oracle inputs of the sur suite (work package P1.7).

Everything here is synthetic: small .sur files built from the layout in
crates/neuron-core/src/sur/mod.rs, with IVP compact surfaces built from the
layout in crates/neuron-core/src/sur/compact.rs (closed convex meshes whose
opposite edges are computed here). Run it with Python 3 from any directory;
it rewrites sur/ under this directory. The oracle harness
(CppSource/Oracle/oracle_sur.cpp) loads every file of sur/files.list through
PhyArch::LoadSurfaces and the Rust test tests/oracle_sur.rs through
neuron_core::sur::load.

Every part the reference keeps has an exts chunk or was reset by a repeated
id: without either its corners are never written, so the reference's dump
of them would not be deterministic.
"""

import os
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, 'sur')

NODE = 0x1C


def f32(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def words(*values):
    return b''.join(struct.pack('<I', v & 0xFFFFFFFF) for v in values)


# --- Meshes -------------------------------------------------------------------

TETRA = [(0, 1, 2), (0, 3, 1), (1, 3, 2), (2, 3, 0)]
# A triangular prism: points 0..2 bottom, 3..5 top.
PRISM = [(0, 2, 1), (3, 4, 5),
         (0, 1, 4), (0, 4, 3),
         (1, 2, 5), (1, 5, 4),
         (2, 0, 3), (2, 3, 5)]
OCTA = [(0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4),
        (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5)]


def points_of(count, seed, scale=1.0):
    out = []
    for i in range(count):
        x = scale * ((i * 37 + seed * 11) % 17 - 8) / 4.0
        y = scale * ((i * 53 + seed * 7) % 13 - 6) / 3.0
        z = scale * ((i * 29 + seed * 5) % 11 - 5) / 2.0
        out.append((f32(x), f32(y), f32(z), f32(0.125 * i) if seed % 2 else 0))
    return out


def opposite(triangles, t, e):
    a, b = triangles[t][e], triangles[t][(e + 1) % 3]
    for u, tri in enumerate(triangles):
        for f in range(3):
            if tri[f] == b and tri[(f + 1) % 3] == a:
                here = 16 * t + 4 * (e + 1)
                there = 16 * u + 4 * (f + 1)
                return (there - here) // 4
    raise ValueError('open mesh')


class Ledge:
    """A compact ledge. pool: None (points after the triangles), 'before' or
    'after' (in a point block before or after all ledges)."""

    def __init__(self, triangles, points, client=0, flags=0x04, unknown=0,
                 tri_bits=lambda t: 0, pierce=lambda t, e: False, pool=None):
        self.triangles = triangles
        self.points = points
        self.client = client
        self.flags = flags
        self.unknown = unknown
        self.tri_bits = tri_bits
        self.pierce = pierce
        self.pool = pool
        self.offset = None
        self.points_at = None

    def body_size(self):
        size = 16 + 16 * len(self.triangles)
        if self.pool is None:
            size += 16 * len(self.points)
        return size


class Node:
    def __init__(self, center, radius, box, ledge=None, left=None, right=None, free=0):
        self.center = center
        self.radius = radius
        self.box = box
        self.ledge = ledge
        self.left = left
        self.right = right
        self.free = free
        self.offset = None


def preorder(node, out):
    out.append(node)
    if node.left is not None:
        preorder(node.left, out)
        preorder(node.right, out)
    return out


def surface(root, deviation, mass=(0.0, 0.0, 0.0), inertia=(1.0, 1.0, 1.0), radius=2.0):
    """The blob: a 0x30 header, an optional point block, the ledges, an
    optional second point block, then the nodes in pre-order."""
    nodes = preorder(root, [])
    ledges = []
    for node in nodes:
        if node.ledge is not None and node.ledge not in ledges:
            ledges.append(node.ledge)
    at = 0x30
    for ledge in ledges:
        if ledge.pool == 'before':
            ledge.points_at = at
            at += 16 * len(ledge.points)
    for ledge in ledges:
        ledge.offset = at
        if ledge.pool is None:
            ledge.points_at = at + 16 + 16 * len(ledge.triangles)
        at += ledge.body_size()
    for ledge in ledges:
        if ledge.pool == 'after':
            ledge.points_at = at
            at += 16 * len(ledge.points)
    for node in nodes:
        node.offset = at
        at += NODE
    size = at
    blob = bytearray(size)
    blob[0:0x24] = words(*(f32(v) for v in mass), *(f32(v) for v in inertia), f32(radius),
                         (size << 8) | deviation, root.offset)
    hull_of = {}
    for node in nodes:
        if node.ledge is not None and node.left is not None:
            hull_of[id(node.ledge)] = node
    for ledge in ledges:
        hull = hull_of.get(id(ledge))
        client = hull.offset - ledge.offset if hull is not None else ledge.client
        flags = (ledge.flags & ~3) | (1 if hull is not None else 0)
        size_div_16 = ledge.body_size() // 16
        out = words(ledge.points_at - ledge.offset, client, (size_div_16 << 8) | flags)
        out += struct.pack('<hh', len(ledge.triangles), ledge.unknown)
        for t, tri in enumerate(ledge.triangles):
            info = t | (((t + 1) % len(ledge.triangles)) << 12) | (ledge.tri_bits(t) << 24)
            edge_words = []
            for e in range(3):
                opp = opposite(ledge.triangles, t, e)
                edge = tri[e] | ((opp & 0x7FFF) << 16)
                if ledge.pierce(t, e):
                    edge |= 0x80000000
                edge_words.append(edge)
            out += words(info, *edge_words)
        blob[ledge.offset:ledge.offset + len(out)] = out
        point_bytes = b''.join(words(*p) for p in ledge.points)
        blob[ledge.points_at:ledge.points_at + len(point_bytes)] = point_bytes
    for node in nodes:
        right = node.right.offset - node.offset if node.right is not None else 0
        to_ledge = node.ledge.offset - node.offset if node.ledge is not None else 0
        blob[node.offset:node.offset + NODE] = (
            words(right, to_ledge, *(f32(v) for v in node.center), f32(node.radius))
            + bytes(node.box) + bytes([node.free]))
    return bytes(blob)


def tetra(seed, **kw):
    return Ledge(TETRA, points_of(4, seed), **kw)


def leaf(seed, ledge=None, **kw):
    return Node((seed * 0.5, -seed * 0.25, 1.0), 1.5 + seed, (seed & 0xFF, (2 * seed) & 0xFF, (255 - seed) & 0xFF),
                ledge if ledge is not None else tetra(seed, client=1000 + seed), **kw)


def inner(seed, left, right, ledge=None, free=0):
    return Node((0.0, seed * 1.0, -seed * 1.0), 4.0 + seed, (100, 101, 102 + seed),
                ledge, left, right, free)


SURF_TETRA = surface(leaf(1), 0xC7, mass=(0.5, -0.25, 1.0), inertia=(0.1, 0.2, 0.3),
                     radius=1.75)

SURF_TREE = surface(
    inner(1,
          leaf(2),
          inner(2,
                leaf(3, Ledge(PRISM, points_of(6, 3), client=-5, pool='after')),
                leaf(4, tetra(4, client=0x7FFFFFFF, pool='after'))),
          ledge=Ledge(OCTA, points_of(6, 9, 3.0))),
    0x9B, mass=(1.0, 2.0, 3.0), inertia=(4.0, 5.0, 6.0), radius=7.5)

# Raw float bits that must survive: NaN payloads, -0.0, a denormal, infinity.
ODD_POINTS = [(0x7FC00001, 0x80000000, 0x00000001, 0xFFFFFFFF),
              (0x7F800000, 0xFF800000, 0x3F800000, 0x12345678),
              (0x00800000, 0x80800000, 0x40490FDB, 0x00000000),
              (0x7FBFFFFF, 0x3EAAAAAB, 0xC1200000, 0x80000001)]
SURF_ODD = surface(
    inner(3,
          leaf(5, Ledge(TETRA, ODD_POINTS, client=0x13579BDF, flags=0xFC, unknown=-2,
                        tri_bits=lambda t: (0x81 * (t + 1)) & 0xFF,
                        pierce=lambda t, e: (t + e) % 2 == 0, pool='before')),
          leaf(6, Ledge(OCTA, points_of(6, 6), client=3, unknown=0x7FFF, pool='before'),
               free=0xAB),
          free=0x5A),
    0x00)

# A terminal ledge without triangles (its point offset is never followed)
# beside a regular one.
EMPTY = Ledge([], [], client=42)
SURF_EMPTY = surface(inner(4, leaf(7, EMPTY), leaf(8)), 0xFF)
# The ledge offset is patched to a value the reference never follows.
_EMPTY = bytearray(SURF_EMPTY)
_EMPTY[EMPTY.offset:EMPTY.offset + 4] = words(0x7FFF0000)
SURF_EMPTY = bytes(_EMPTY)

# Two terminal nodes that name the same ledge.
SHARED = tetra(9, client=9)
SURF_SHARED = surface(inner(5, leaf(9, SHARED), leaf(10, SHARED)), 0x10)


def balanced(depth, seed):
    if depth == 0:
        return leaf(seed % 200, tetra(seed, client=seed, pool='after' if seed % 3 else None))
    left = balanced(depth - 1, seed * 2)
    right = balanced(depth - 1, seed * 2 + 1)
    hull = Ledge(TETRA, points_of(4, seed + 50, 5.0)) if depth % 2 == 0 else None
    return inner(depth, left, right, ledge=hull)


SURF_DEEP = surface(
    inner(9, balanced(4, 1), inner(8, leaf(11), balanced(2, 3))), 0x80)


# --- .sur files --------------------------------------------------------------

def header(tag=b'vers', version=f32(2.0)):
    return tag + words(version)


def part(part_id, *chunks):
    return words(part_id, len(chunks)) + b''.join(chunks)


def part_count(part_id, count, *chunks):
    return words(part_id, count) + b''.join(chunks)


def surf(blob, tag=b'surf'):
    return tag + words(len(blob)) + blob


def exts(lo, hi, tag=b'exts'):
    return tag + words(*(f32(v) for v in lo), *(f32(v) for v in hi))


def exts_bits(bits, tag=b'exts'):
    return tag + words(*bits)


def hpid(*ids, tag=b'hpid'):
    return tag + words(len(ids), *ids)


def fxd(tag=b'!fxd'):
    return tag


def old_format():
    """The layout of the files without "vers": an extents pair, then named
    records, each with a surf chunk."""
    out = words(*(f32(v) for v in (-1, -1, -1, 1, 1, 1)))
    out += words(4) + b'Root' + b'surf' + words(1, len(SURF_TETRA)) + SURF_TETRA
    return out


FILES = {
    'basic.sur': header() + part(0, fxd(), exts((-1, -2, -3), (1, 2, 3)), surf(SURF_TETRA)),
    'tree.sur': header() + part(0x1234, exts((0, 0, 0), (8, 8, 8)), surf(SURF_TREE),
                                hpid(0xAABBCCDD, 7)),
    'odd.sur': header() + part(77, surf(SURF_ODD),
                               exts_bits((0x7FC00000, 0x80000000, 1, 0xFF800000, 2, 3))),
    'empty_ledge.sur': header() + part(5, exts((0, 0, 0), (1, 1, 1)), surf(SURF_EMPTY)),
    'shared.sur': header() + part(6, exts((0, 0, 0), (1, 1, 1)), surf(SURF_SHARED), fxd()),
    'deep.sur': header() + part(1, exts((-9, -9, -9), (9, 9, 9)), surf(SURF_DEEP)),
    # Parts in file order 5, 1, max, 3; tags in any case; repeated chunks.
    'multi.sur': header(b'VeRs')
    + part(5, exts((0, 0, 0), (1, 1, 1)), hpid(3, 1, 3, 2), hpid(2, 4), fxd(b'!FXD'))
    + part(1, surf(SURF_TETRA, b'SURF'), exts((1, 1, 1), (2, 2, 2), b'Exts'),
           surf(SURF_SHARED), exts((5, 5, 5), (6, 6, 6)))
    + part(0xFFFFFFFF, exts((0, 0, 0), (0, 0, 0)), hpid(tag=b'HPID'),
           b'hpid' + words(0xFFFFFFFD), hpid(9))
    + part(3, exts((3, 3, 3), (4, 4, 4)), fxd(), fxd()),
    # Repeated ids reset the earlier part.
    'duplicate.sur': header()
    + part(2, fxd(), exts((-1, -1, -1), (1, 1, 1)), surf(SURF_TETRA), hpid(1, 2))
    + part(9, exts((2, 2, 2), (3, 3, 3)))
    + part(2, hpid(5))
    + part(9, exts((7, 7, 7), (8, 8, 8)), fxd())
    + part_count(6, 1, exts((1, 1, 1), (1, 1, 1)))
    + part_count(6, 0)
    + part_count(9, -5)
    + part(2, hpid(5, 6), hpid(6, 5)),
    # An unknown chunk ends the load; the parts so far stay.
    'unknown.sur': header() + part(1, exts((0, 0, 0), (1, 1, 1)), surf(SURF_TETRA))
    + part(2, exts((1, 1, 1), (2, 2, 2)), b'abcd', fxd()) + part(3, exts((0, 0, 0), (0, 0, 0))),
    'nul_tag.sur': header() + part(4, exts((0, 0, 0), (1, 1, 1)), b'su\0f'),
    'old_format.sur': old_format(),
    'version_one.sur': header(version=f32(1.0)) + part(0, exts((0, 0, 0), (1, 1, 1))),
    'version_nan.sur': header(version=0x7FC00000) + part(0, exts((0, 0, 0), (1, 1, 1))),
    'version_minus.sur': header(version=f32(-2.0)),
    'version_short.sur': b'vers' + b'\0\0',
    'tag_short.sur': b'ver',
    'empty.sur': b'',
    'header_only.sur': header(),
    'tail.sur': header() + part(8, exts((0, 0, 0), (1, 1, 1))) + b'\x01\x02',
}


def write(name, data):
    path = os.path.join(ROOT, name)
    with open(path, 'wb') as f:
        f.write(data)


def main():
    if os.path.isdir(ROOT):
        shutil.rmtree(ROOT)
    os.makedirs(ROOT)
    for name, data in FILES.items():
        write(name, data)
    listing = ['# the .sur files of the sur suite, written by make_sur_fixtures.py']
    listing += ['sur/' + name for name in FILES]
    write('files.list', ('\n'.join(listing) + '\n').encode())


if __name__ == '__main__':
    main()
