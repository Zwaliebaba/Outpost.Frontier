"""Writes the oracle inputs of work package P1.9 (net) in net/.

The inputs are synthetic. Run it with Python 3 from any directory; it
rewrites every file in net/. The oracle harness (CppSource/Oracle,
oracle_net.cpp) and the Rust test tests/oracle_net.rs read the same files
and must produce byte-identical vectors.

- net/quantise.hex  f32 bit patterns for every quantiser and fire tier
- net/expand.hex    16-bit words; the expanders read the low byte, the
                    fire word tier the whole word
- net/hull.hex      i32 hit points (bit patterns) for the hull byte
- net/doubles.hex   f64 bit patterns for the x87 double field
- net/header.hex    u32 packet IDs for the message header
- net/write.txt     writer scripts: "<limit> <op>:<hex> ..."
- net/read.txt      reader scripts: "<input hex> <op> ..."

The scripts avoid the cases where the port deliberately differs from the
C++ (see the divergence register): the wide-string helpers' strict
capacity comparison, their zero-length decode, and the unchecked copies of
RpcServerPutWideText / RpcServerGetWideText past the limit.
"""

import os
import random
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def write(name, lines, header):
    path = os.path.join(HERE, 'net', name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = '\n'.join(['# ' + line for line in header] + lines) + '\n'
    with open(path, 'wb') as f:
        f.write(text.encode())


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def f64_bits(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def neighbours(bits):
    """The f32 bit pattern and the patterns just below and above it in value."""
    out = [bits]
    magnitude = bits & 0x7fffffff
    sign = bits & 0x80000000
    if magnitude > 0:
        out.append(sign | (magnitude - 1))
    else:
        out.append(0x80000001 if sign == 0 else 0x00000001)
    if magnitude < 0x7f7fffff:
        out.append(sign | (magnitude + 1))
    return out


def quantise_inputs():
    special = [
        0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x00800000,
        0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x40000000, 0xc0000000,
        0x3f7fffff, 0x3f800001, 0xbf7fffff, 0xbf800001, 0x7f7fffff, 0xff7fffff,
        0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fc00001,
        0x7f800001, 0xff800001, 0x7fa00000, 0x7fbfffff, 0xffbfffff,
    ]
    values = []
    # Truncation boundaries of each scale.
    for k in (-129, -128, -127, -64, -1, 1, 2, 63, 64, 126, 127, 128, 129, 254, 255, 256):
        values.append(k / 127.0)
        values.append(k / 255.0)
    for k in (1, 25, 26, 51, 127, 254, 255, 256, 511):
        values.append(k / (255.0 * struct.unpack('<f', struct.pack('<f', 0.1))[0]))
    for k in (-128, -127, -1, 1, 63, 126, 127, 128):
        values.append(k * 64.0 / 127.0)
    for k in (-32768, -32767, -1, 1, 16383, 32766, 32767, 32768):
        values.append(k * 8192.0 / 32767.0)
    # Fire tier limits.
    for v in (64.0, -64.0, 8192.0, -8192.0, 32.0, 4096.0, 0.5, 100.0, -5000.0, 1e5):
        values.append(v)
    # Results beyond 32 bits and beyond 64 bits.
    for v in (1e10, -1e10, 3e9, 2.0 ** 63 / 127.0, 2.0 ** 63 / 255.0, -(2.0 ** 63) / 127.0,
              2.0 ** 63 / (127.0 / 64.0), 2.0 ** 63 * 8192.0 / 32767.0,
              2.0 ** 63 / (255.0 * 0.1), 1e20, -1e20, 1e38):
        values.append(v)
    bits = list(special)
    for v in values:
        bits.extend(neighbours(f32_bits(v)))
    rng = random.Random(1909)
    for _ in range(60):
        bits.append(f32_bits(rng.uniform(-2.0, 2.0)))
    for _ in range(40):
        bits.append(f32_bits(rng.uniform(-100.0, 100.0)))
    for _ in range(40):
        bits.append(f32_bits(rng.uniform(-10000.0, 10000.0)))
    for _ in range(40):
        bits.append(rng.getrandbits(32))
    seen = set()
    unique = []
    for b in bits:
        if b not in seen:
            seen.add(b)
            unique.append(b)
    return ['%08x' % b for b in unique]


def expand_inputs():
    words = [b | (((b * 37 + 11) & 0xff) << 8) for b in range(256)]
    words += [0x0000, 0x7fff, 0x8000, 0x8001, 0xffff, 0x0001, 0x7ffe, 0x4000, 0xc000, 0x3fff]
    return ['%04x' % w for w in words]


def hull_inputs():
    values = [0, 1, -1, 2, 127, 128, 255, 256, 257, 1000, 0x7fffffff, -0x80000000,
              16777216, 16777217, 16777219, 2147483520, -16777217, 33554435, 123456789]
    rng = random.Random(2003)
    values += [rng.randint(-0x80000000, 0x7fffffff) for _ in range(20)]
    return ['%08x' % (v & 0xffffffff) for v in values]


def double_inputs():
    values = [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0x7ff0000000000000,
              0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000, 0x7ff0000000000001,
              0xfff4000000000000, 0x7ff7ffffffffffff, 0x0000000000000001, 0x7fefffffffffffff,
              f64_bits(3.141592653589793), f64_bits(-1e300)]
    return ['%016x' % v for v in values]


def header_inputs():
    ids = [0x00010001, 0x00010002, 0x00020004, 0x0002004f, 0x00030001, 0x0003002f,
           0x00000000, 0x00ff00ff, 0x00030abc, 0x01020304, 0xffffffff, 0x00020100]
    return ['%08x' % i for i in ids]


# ---------------------------------------------------------------- scripts

WIDTH = {'u8': 1, 'u16': 2, 'u32': 4, 'f32': 4, 'f32x': 4, 'vec': 12}


def write_ops_ok(limit, ops):
    """True when the C++ and the port agree on the script (see module doc)."""
    cursor = 0
    for name, data in ops:
        if name in WIDTH:
            cursor += WIDTH[name]
            if cursor > limit:
                return True
        elif name == 'bytes':
            cursor += len(data)
            if cursor > limit:
                return True
        elif name in ('b16', 'b32'):
            cursor += 2 if name == 'b16' else 4
            if cursor > limit:
                return True
            cursor += len(data)
            if cursor > limit:
                return True
        elif name == 'wide16':
            if any(data[i:i + 2] == b'\0\0' for i in range(0, len(data), 2)):
                return False
            cursor += 2
            if cursor == limit:
                return False
            if cursor > limit:
                return True
            cursor += len(data)
            if cursor == limit:
                return False
            if cursor > limit:
                return True
        elif name == 'text16':
            cursor += 2
            if cursor > limit:
                return True
            cursor += len(data)
            if cursor > limit:
                return False
        else:
            raise ValueError(name)
    return True


def render_write(limit, ops):
    parts = [str(limit)]
    for name, data in ops:
        if name in ('wide16', 'text16'):
            units = [data[i:i + 2] for i in range(0, len(data), 2)]
            parts.append(name + ':' + ''.join('%04x' % struct.unpack('<H', u)[0] for u in units))
        elif name in ('u8', 'u16', 'u32'):
            value = int.from_bytes(data, 'little')
            parts.append('%s:%0*x' % (name, 2 * len(data), value))
        elif name in ('f32', 'f32x'):
            parts.append('%s:%08x' % (name, struct.unpack('<I', data)[0]))
        elif name == 'vec':
            parts.append('vec:' + ''.join('%08x' % v for v in struct.unpack('<3I', data)))
        else:
            parts.append(name + ':' + data.hex())
    return ' '.join(parts)


def random_write_op(rng):
    name = rng.choice(['u8', 'u16', 'u32', 'f32', 'f32x', 'vec', 'bytes', 'wide16', 'text16',
                       'b16', 'b32'])
    if name in WIDTH:
        if name in ('f32', 'f32x') and rng.random() < 0.3:
            return name, struct.pack('<I', rng.choice([0x7f800001, 0xffbfffff, 0x7fc00000]))
        return name, bytes(rng.getrandbits(8) for _ in range(WIDTH[name]))
    count = rng.randint(0, 6)
    if name in ('wide16', 'text16'):
        return name, b''.join(struct.pack('<H', rng.randint(1, 0xffff)) for _ in range(count))
    return name, bytes(rng.getrandbits(8) for _ in range(count))


def write_scripts():
    fixed = [
        (64, [('u8', b'\x01'), ('u16', b'\x02\x03'), ('u32', b'\x04\x05\x06\x07'),
              ('f32', struct.pack('<I', 0x3f800000)), ('f32x', struct.pack('<I', 0x7f800001)),
              ('vec', struct.pack('<3I', 0x3f800000, 0xc0000000, 0x00000000))]),
        (6, [('u32', b'\x04\x03\x02\x01'), ('u16', b'\x06\x05')]),
        (5, [('u32', b'\x04\x03\x02\x01'), ('u16', b'\x06\x05')]),
        (3, [('u32', b'\x04\x03\x02\x01')]),
        (0, [('bytes', b'')]),
        (0, [('u8', b'\x00')]),
        (20, [('f32x', struct.pack('<I', b)) for b in (0x7fa00000, 0xffbfffff, 0x7fc00001,
                                                       0x00000001, 0xff800000)]),
        (8, [('f32', struct.pack('<I', 0x7f800001)), ('f32', struct.pack('<I', 0xff800001))]),
        (12, [('vec', struct.pack('<3I', 0x7f800001, 0x7fc00000, 0xff800000))]),
        (11, [('vec', struct.pack('<3I', 0x7f800001, 0x7fc00000, 0xff800000))]),
        (32, [('wide16', b'A\0B\0'), ('u8', b'\xff')]),
        (1, [('wide16', b'A\0')]),
        (5, [('wide16', b'A\0B\0')]),
        (8, [('wide16', b''), ('u8', b'\x01')]),
        (16, [('wide16', struct.pack('<2H', 0xd83d, 0xde00)), ('u8', b'\x02')]),
        (16, [('text16', b'A\0B\0'), ('u8', b'\x01')]),
        (4, [('text16', b''), ('u8', b'\x01')]),
        (1, [('text16', b'')]),
        (16, [('b16', b'abc')]),
        (4, [('b16', b'abc')]),
        (1, [('b16', b'')]),
        (16, [('b32', b'\x01\x02')]),
        (5, [('b32', b'\x01\x02')]),
        (300, [('bytes', bytes(range(256)))]),
        (255, [('bytes', bytes(range(256)))]),
    ]
    scripts = []
    for limit, ops in fixed:
        assert write_ops_ok(limit, ops), (limit, ops)
        scripts.append(render_write(limit, ops))
    rng = random.Random(1917)
    while len(scripts) < len(fixed) + 60:
        ops = [random_write_op(rng) for _ in range(rng.randint(1, 8))]
        limit = rng.randint(0, 48)
        if write_ops_ok(limit, ops):
            scripts.append(render_write(limit, ops))
    return scripts


READ_WIDTH = {'u8': 1, 'u16': 2, 'u32': 4, 'f32': 4, 'f64': 8, 'vec': 12}


def read_ops_ok(data, ops):
    size = len(data)
    cursor = 0
    for op in ops:
        name, _, arg = op.partition(':')
        if name in READ_WIDTH or name == 'bytes':
            cursor += READ_WIDTH.get(name, 0) + (int(arg) if name == 'bytes' else 0)
            if cursor > size:
                return True
        elif name == 'wide16':
            cursor += 2
            if cursor == size:
                return False
            if cursor > size:
                return True
            count = struct.unpack('<H', data[cursor - 2:cursor])[0]
            if count == 0:
                return False
            units = data[cursor:cursor + 2 * count]
            cursor += 2 * count
            if cursor == size:
                return False
            if cursor > size:
                return True
            if any(units[i:i + 2] == b'\0\0' for i in range(0, len(units), 2)):
                return False
        elif name == 'text16':
            cursor += 2
            if cursor > size:
                return True
            count = struct.unpack('<H', data[cursor - 2:cursor])[0]
            cursor += 2 * count
            if cursor > size:
                return False
        elif name == 'b32':
            cursor += 4
            if cursor > size:
                return True
            count = struct.unpack('<I', data[cursor - 4:cursor])[0]
            if count > 0x10000:
                return False
            cursor += count
            if cursor > size:
                return True
        else:
            raise ValueError(name)
    return True


def read_scripts():
    fixed = [
        ('0102030405060708090a0b0c0d0e0f10', 'u8 u16 u32 f32'),
        ('010203', 'u32'),
        ('010203', 'u16 u16'),
        ('', 'u8'),
        ('', 'bytes:0'),
        ('0100000041', 'b32'),
        ('000000004142', 'b32 u16'),
        ('050000004142', 'b32'),
        ('0200410042007f', 'wide16 u8'),
        ('030041004200', 'wide16'),
        ('01', 'wide16'),
        ('02003dd800de07', 'wide16 u8'),
        ('0200410042007f', 'text16 u8'),
        ('000001', 'text16 u8'),
        ('0100000000000000f07f', 'f64 u16'),
        ('010000000000f07f', 'f64'),
        ('0100807f0000c07f000080ff', 'vec'),
        ('0100807f0000c07f0000', 'vec'),
        ('0100807f', 'f32'),
        ('aabbccdd', 'bytes:2 bytes:0 bytes:2'),
        ('aabbccdd', 'bytes:5'),
    ]
    scripts = []
    for data, ops in fixed:
        assert read_ops_ok(bytes.fromhex(data), ops.split()), (data, ops)
        scripts.append(data + ' ' + ops)
    rng = random.Random(1969)
    names = ['u8', 'u16', 'u32', 'f32', 'f64', 'vec', 'bytes', 'wide16', 'text16', 'b32']
    while len(scripts) < len(fixed) + 60:
        ops = []
        for _ in range(rng.randint(1, 6)):
            name = rng.choice(names)
            ops.append('bytes:%d' % rng.randint(0, 5) if name == 'bytes' else name)
        # Mostly small counts so counted fields often fit.
        data = bytearray()
        for _ in range(rng.randint(0, 24)):
            data.append(rng.choice([0, 0, 1, 2, 3, rng.getrandbits(8)]))
        if read_ops_ok(bytes(data), ops):
            scripts.append(bytes(data).hex() + ' ' + ' '.join(ops))
    return scripts


def main():
    write('quantise.hex', quantise_inputs(), ['f32 bit patterns'])
    write('expand.hex', expand_inputs(), ['16-bit words; byte expanders read the low byte'])
    write('hull.hex', hull_inputs(), ['i32 hit points as bit patterns; unknown archetype'])
    write('doubles.hex', double_inputs(), ['f64 bit patterns'])
    write('header.hex', header_inputs(), ['u32 packet IDs'])
    write('write.txt', write_scripts(),
          ['writer scripts: <limit> <op>:<hex> ...',
           'ops: u8 u16 u32 (values), f32 f32x (bits), vec (3 x bits), bytes b16 b32 (bytes),',
           'wide16 text16 (UTF-16 units, 4 hex digits each)'])
    write('read.txt', read_scripts(),
          ['reader scripts: <input hex> <op> ...',
           'ops: u8 u16 u32 f32 f64 vec bytes:<n> wide16 text16 b32'])


if __name__ == '__main__':
    main()
