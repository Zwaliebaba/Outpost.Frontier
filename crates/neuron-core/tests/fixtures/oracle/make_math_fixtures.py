"""Writes the oracle inputs for neuron-core::math (work package P1.4).

The files are synthetic: nothing here comes from the original game. Run it
with Python 3 from any directory; it rewrites every file in math/. The
oracle harness (CppSource/Oracle/oracle_math.cpp) and the Rust test
tests/oracle_math.rs read the same files.

Each input line is a list of 32-bit words, the f32 bit patterns written as
eight lower-case hex digits and separated by single spaces:

  math/matrix.hex     A (9 words) B (9) s (1)
  math/transform.hex  A (12 words, rotation then translation) B (12)
  math/vector.hex     X (12) v (3)
  math/quat.hex       q (4, W X Y Z) p (4) v (3) t (1)
  math/list.hex       X (12) then 1..6 vectors (3 words each)
  math/sqrt.hex       x (1)
"""

import math
import os
import random
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'math')

POS_ZERO = 0x00000000
NEG_ZERO = 0x80000000
ONE = 0x3F800000
MIN_DENORMAL = 0x00000001
MAX_DENORMAL = 0x007FFFFF
NEG_DENORMAL = 0x80000001
MIN_NORMAL = 0x00800000
MAX_FLOAT = 0x7F7FFFFF
NEG_MAX_FLOAT = 0xFF7FFFFF
INF = 0x7F800000
NEG_INF = 0xFF800000
NAN = 0x7FC00000

EDGE_WORDS = [POS_ZERO, NEG_ZERO, ONE, 0xBF800000, MIN_DENORMAL, MAX_DENORMAL, NEG_DENORMAL,
              MIN_NORMAL, MAX_FLOAT, NEG_MAX_FLOAT, INF, NEG_INF, NAN, 0x3F7FFFFF, 0x3F800001]

rng = random.Random(0x5EED_0B4D)


def bits(value):
    """The f32 bit pattern nearest to a Python float (round to nearest even)."""
    return struct.unpack('<I', struct.pack('<f', value))[0]


def val(word):
    return struct.unpack('<f', struct.pack('<I', word))[0]


def line(words):
    return ' '.join('%08x' % w for w in words)


def write(name, header, lines):
    os.makedirs(OUT, exist_ok=True)
    text = '\n'.join(['# ' + h for h in header] + lines) + '\n'
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(text.encode())


def uniform(lo=-4.0, hi=4.0):
    return bits(rng.uniform(lo, hi))


def wide():
    """A value of random sign and magnitude between 1e-6 and 1e6."""
    return bits(rng.choice((-1.0, 1.0)) * 10.0 ** rng.uniform(-6.0, 6.0))


def vec(kind='uniform'):
    if kind == 'wide':
        return [wide() for _ in range(3)]
    return [uniform(-100.0, 100.0) for _ in range(3)]


def unit_quat():
    while True:
        q = [rng.gauss(0.0, 1.0) for _ in range(4)]
        n = math.sqrt(sum(c * c for c in q))
        if n > 1e-3:
            return [c / n for c in q]


def quat_matrix(q):
    """Rotation matrix of a unit quaternion (W X Y Z), in double."""
    w, x, y, z = q
    return [
        1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
        2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
        2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y),
    ]


def axis_angle(axis, angle):
    n = math.sqrt(sum(c * c for c in axis))
    s = math.sin(angle / 2.0) / n
    return [math.cos(angle / 2.0)] + [c * s for c in axis]


def rotation(q=None):
    return [bits(v) for v in quat_matrix(q or unit_quat())]


IDENTITY = [ONE, 0, 0, 0, ONE, 0, 0, 0, ONE]


def matrices():
    """Matrices of every kind the kernels distinguish."""
    out = [IDENTITY, [0] * 9, [NEG_ZERO] * 9]
    # Rotations, including ones whose trace is not positive (each axis
    # largest) for MatrixToQuaternion's second branch.
    for _ in range(6):
        out.append(rotation())
    for axis in ([1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 0], [0, 1, 1], [1, 0, 1], [1, 1, 1]):
        for angle in (math.pi, 0.9 * math.pi, 2.5, 0.5 * math.pi):
            out.append(rotation(axis_angle(axis, angle)))
    # Scaled and sheared (non-orthonormal) rotations.
    for scale in (0.5, 3.0, 1e3, 1e-3):
        out.append([bits(val(w) * scale) for w in rotation()])
    for _ in range(3):
        r = rotation()
        out.append([bits(val(w) * rng.uniform(0.5, 2.0)) for w in r])
    # General matrices.
    for _ in range(10):
        out.append([uniform() for _ in range(9)])
    for _ in range(4):
        out.append([wide() for _ in range(9)])
    # Singular and near-singular: a dependent third row, then that row
    # nudged by one ulp; determinants around the 1e-8 threshold.
    for _ in range(4):
        a = [rng.uniform(-2, 2) for _ in range(3)]
        b = [rng.uniform(-2, 2) for _ in range(3)]
        c = [x + y for x, y in zip(a, b)]
        rows = [bits(v) for v in a + b + c]
        out.append(rows)
        nudged = list(rows)
        nudged[8] += 1
        out.append(nudged)
    for d in (1e-3, 2.2e-3, 2.1e-3, 1e-2):
        out.append([bits(d), 0, 0, 0, bits(d), 0, 0, 0, bits(d)])
    out.append([bits(1e-8), 0, 0, 0, ONE, 0, 0, 0, ONE])
    out.append([bits(1.0000001e-8), 0, 0, 0, ONE, 0, 0, 0, ONE])
    # Large, tiny and denormal entries.
    out.append([bits(1e12), bits(3e11), 0, bits(-2e12), bits(5e11), bits(1e10), 0, bits(7e11), bits(1e12)])
    out.append([bits(1e-20), bits(3e-21), 0, bits(-2e-20), bits(5e-21), 0, 0, 0, bits(1e-20)])
    out.append([MIN_DENORMAL, MAX_DENORMAL, NEG_DENORMAL, ONE, MIN_NORMAL, 0, 0, NEG_ZERO, ONE])
    # Permutations and sign flips: zero and one lanes.
    for perm in ((1, 0, 2), (2, 1, 0), (0, 2, 1), (1, 2, 0), (2, 0, 1)):
        m = [0] * 9
        for row, col in enumerate(perm):
            m[row * 3 + col] = ONE
        out.append(m)
    out.append([0xBF800000, NEG_ZERO, 0, NEG_ZERO, ONE, NEG_ZERO, 0, 0, 0xBF800000])
    # Infinities and NaNs.
    out.append([INF, 0, 0, 0, ONE, 0, 0, 0, ONE])
    out.append([ONE, NAN, 0, 0, ONE, 0, 0, 0, ONE])
    out.append([ONE, 0, 0, 0, NAN, 0, 0, 0, ONE])
    out.append([MAX_FLOAT, MAX_FLOAT, 0, NEG_MAX_FLOAT, MAX_FLOAT, 0, 0, 0, MAX_FLOAT])
    return out


def scalar():
    return rng.choice([uniform(), wide(), ONE, POS_ZERO, NEG_ZERO, bits(0.5), INF, MIN_DENORMAL,
                       bits(1e30), bits(-2.0)])


def translation():
    return rng.choice([vec(), vec(), vec('wide'), [0, 0, 0], [NEG_ZERO, 0, NEG_ZERO],
                       [0, 0, ONE], [INF, 0, 0], [bits(1e20), bits(-1e20), 0]])


def matrix_lines():
    ms = matrices()
    lines = []
    for index, a in enumerate(ms):
        b = ms[(index * 7 + 3) % len(ms)]
        lines.append(line(a + b + [scalar()]))
    return lines


def transform_lines():
    ms = matrices()
    lines = []
    for index, a in enumerate(ms):
        b = ms[(index * 5 + 1) % len(ms)]
        lines.append(line(a + translation() + b + translation()))
    return lines


def vector_lines():
    ms = matrices()
    lines = []
    for index, m in enumerate(ms):
        v = vec('wide') if index % 3 == 0 else vec()
        lines.append(line(m + translation() + v))
    for word in EDGE_WORDS:
        lines.append(line(rotation() + vec() + [word, uniform(), uniform()]))
        lines.append(line(IDENTITY + [word, 0, 0] + [uniform(), word, uniform()]))
    return lines


def quats():
    out = [[ONE, 0, 0, 0], [0xBF800000, 0, 0, 0], [0, ONE, 0, 0], [0, 0, ONE, 0], [0, 0, 0, ONE],
           [0, 0, 0, 0], [NEG_ZERO, NEG_ZERO, NEG_ZERO, NEG_ZERO]]
    for _ in range(20):
        out.append([bits(c) for c in unit_quat()])
    for _ in range(6):
        out.append([uniform(-3, 3) for _ in range(4)])
    out.append([bits(1e-20), bits(2e-20), 0, bits(-1e-20)])
    out.append([bits(1e18), bits(-3e18), bits(2e18), 0])
    out.append([MIN_DENORMAL, 0, MAX_DENORMAL, 0])
    out.append([NAN, 0, 0, 0])
    out.append([INF, 0, 0, 0])
    return out


def quat_lines():
    qs = quats()
    lines = []
    ts = [POS_ZERO, ONE, bits(0.5), bits(0.25), bits(-0.5), bits(2.0), NEG_ZERO]
    for index, q in enumerate(qs):
        for step, offset in enumerate((1, 9)):
            p = qs[(index * 3 + offset) % len(qs)]
            if step == 1 and index % 2 == 0:
                # The opposite hemisphere.
                p = [w ^ 0x80000000 for w in q[:1]] + [uniform(-1, 1) for _ in range(3)]
            t = ts[(index + step) % len(ts)] if index % 3 else uniform(0, 1)
            lines.append(line(q + p + vec() + [t]))
    # Same quaternion and its negation: the dot product is +-|q|^2 and the
    # negated pair interpolates to zero at t = 0.5.
    for q in qs[7:12]:
        neg = [w ^ 0x80000000 for w in q]
        lines.append(line(q + q + vec() + [bits(0.5)]))
        lines.append(line(q + neg + vec() + [bits(0.5)]))
    return lines


LANE_VALUES = {0: [POS_ZERO, NEG_ZERO], 1: [ONE], 2: None}


def lane_row(kernel):
    row = []
    for lane in (kernel // 9, kernel // 3 % 3, kernel % 3):
        choices = LANE_VALUES[lane]
        row.append(rng.choice(choices) if choices else uniform())
    return row


def list_vectors(count):
    out = []
    for i in range(count):
        if i % 4 == 3:
            v = [rng.choice(EDGE_WORDS), uniform(), rng.choice(EDGE_WORDS)]
        elif i % 4 == 2:
            v = vec('wide')
        else:
            v = vec()
        out.extend(v)
    return out


def list_lines():
    lines = []
    # Every fast-dot kernel, once with rows that save many operations (the
    # kernel path) and once with general rows (the plain loop unless the
    # row itself saves four or more).
    for kernel in range(27):
        for others in ((1, 3), (26, 26), (13, 0), (rng.randrange(27), rng.randrange(27))):
            rows = lane_row(kernel) + lane_row(others[0]) + lane_row(others[1])
            if kernel % 2:
                rows = rows[3:6] + rows[0:3] + rows[6:9]
            lines.append(line(rows + translation() + list_vectors(1 + kernel % 6)))
    for m in matrices()[:24]:
        lines.append(line(m + translation() + list_vectors(rng.randrange(1, 7))))
    return lines


def sqrt_lines():
    words = []
    # Every table slot, at a small or a large exponent.
    for index in range(0x400):
        mantissa = (index << 14) & 0x7FFFFF
        exponent = (0x7E if index % 2 else 0x40) + (index >> 9)
        words.append((exponent << 23) | mantissa | rng.randrange(0x4000))
    words.extend(EDGE_WORDS)
    words.extend([bits(4.0), bits(16.0), bits(2.0), bits(0.25), bits(1e-30), bits(1e30),
                  bits(-4.0), bits(-1e-3), 0x00400000, 0x00000002])
    for _ in range(64):
        words.append(wide() & 0x7FFFFFFF)
    return [line([w]) for w in words]


def main():
    write('matrix.hex', ['A (9 words) B (9) s (1); see make_math_fixtures.py'], matrix_lines())
    write('transform.hex', ['A (12 words) B (12); see make_math_fixtures.py'], transform_lines())
    write('vector.hex', ['X (12 words) v (3); see make_math_fixtures.py'], vector_lines())
    write('quat.hex', ['q (4 words) p (4) v (3) t (1); see make_math_fixtures.py'], quat_lines())
    write('list.hex', ['X (12 words) then vectors (3 each); see make_math_fixtures.py'], list_lines())
    write('sqrt.hex', ['x (1 word); see make_math_fixtures.py'], sqrt_lines())


if __name__ == '__main__':
    main()
