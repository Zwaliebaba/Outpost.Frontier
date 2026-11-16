"""Writes the oracle inputs of work package P1.5 under util/.

The files are synthetic: nothing here comes from the original game. Run it
with Python 3 from any directory; it rewrites every file it owns. The oracle
harness (CppSource/Oracle/oracle_util*.cpp) and the Rust test
tests/oracle_util.rs read the same files and must produce byte-identical
vectors.
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def write(name, data):
    path = os.path.join(HERE, 'util', name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def lines(name, items, header):
    write(name, ('\n'.join(['# ' + header] + items) + '\n').encode())


def f32(value):
    return '%08x' % struct.unpack('<I', struct.pack('<f', value))[0]


# --- Rect: "left top width height left2 top2 width2 height2 x y" ---------

RECTS = [
    (0, 0, 10, 10, 5, 5, 10, 10, 5, 5),
    (0, 0, 10, 10, 20, 20, 5, 5, 9, 9),
    (0, 0, 10, 10, 10, 0, 5, 5, 10, 0),
    (0, 0, 4, 4, 0, 0, 4, 4, 3, 4),
    (1, 2, 0, 5, 9, 9, 3, 0, 1, 2),
    (1, 2, -1, -1, 3, 4, -1, -1, 0, 0),
    (-5, -5, 3, 3, -4, -6, 10, 2, -3, -5),
    (-100, 50, 7, 9, -99, 52, 1, 1, -94, 58),
    (3, 3, 0, 0, 0, 0, 1, 1, 3, 3),
    (0, 0, 1, 1, 0, 0, 0, 0, 0, 0),
    (10, 20, 30, 40, 10, 20, 30, 40, 39, 59),
    (10, 20, 30, 40, 10, 20, 31, 40, 40, 60),
    (7, -3, 5, -2, 7, -3, 5, -2, 7, -3),
    (1000000, -1000000, 333, 444, 999999, -999999, 2, 1000, 1000001, -999998),
    (0, 0, 5, 5, 2, -10, 1, 30, 2, 0),
    (-7, -7, 15, 15, -7, -7, 0, 15, 0, 0),
]


def make_rects():
    lines('rect.list', [' '.join(str(v) for v in r) for r in RECTS],
          'left top width height left2 top2 width2 height2 x y')


# --- Catmull-Rom segment ------------------------------------------------
# "p0 p1 p2 p3 (x y z each, f32 bits) samples t point(x y z) start step"

SPLINES = [
    ((0, 0, 0), (1, 0, 0), (2, 1, 0), (3, 3, 1), 20, 0.25, (1.5, 0.3, 0.1), 0.0, 0.05),
    ((0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0), 20, 0.5, (1.4, 0.0, 0.0), 0.0, 0.01),
    ((-10.5, 4.25, 3), (0.1, 0.2, 0.3), (7.7, -2.2, 9.9), (12, 0, -4), 20, 0.1, (3, 0, 5), 0.0, 0.02),
    ((100, 200, 300), (110, 190, 305), (130, 170, 290), (150, 160, 280), 16, 0.9, (125, 175, 295), 0.2, 0.1),
    ((1, 1, 1), (2, 3, 5), (7, 11, 13), (17, 19, 23), 20, 1.5, (5, 8, 10), 0.0, 0.03),
    ((0.001, 0.002, 0.003), (0.01, 0.02, 0.03), (0.05, 0.01, -0.02), (0.07, -0.01, -0.05), 20, -0.5, (0.03, 0.015, 0.0), 0.0, 0.07),
    ((5000, -3000, 12.5), (5100, -2950, 13), (5180, -2930, 14.5), (5230, -2925, 17), 7, 0.333, (5150, -2940, 13.7), 0.1, 0.001),
    ((0, 0, 0), (0, 0, 0), (1, 1, 1), (1, 1, 1), 1, 0.75, (0.5, 0.5, 0.5), 0.0, 0.125),
    ((3, 1, 4), (1, 5, 9), (2, 6, 5), (3, 5, 8), 0, 0.6, (1.5, 5.5, 7), 0.0, 0.2),
    ((-1, 0, 1), (0, 1, 0), (1, 0, -1), (0, -1, 0), 20, 0.0, (0.5, 0.6, -0.4), 0.0, 0.004),
]


def make_splines():
    items = []
    for p0, p1, p2, p3, samples, t, point, start, step in SPLINES:
        floats = [f32(v) for v in p0 + p1 + p2 + p3]
        items.append(' '.join(floats + [str(samples), f32(t)] + [f32(v) for v in point] +
                              [f32(start), f32(step)]))
    lines('spline.list', items, 'p0 p1 p2 p3 samples t point start step (floats as f32 bits)')


# --- MD5 text form ------------------------------------------------------

PATTERN = bytes((i * 7 + 3) % 256 for i in range(1000))

MD5_INPUTS = [
    b'', b'a', b'abc', b'message digest', b'abcdefghijklmnopqrstuvwxyz',
    b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789',
    b'1234567890' * 8, b'Player', b'player', b'\xe9t\xe9',
    'Ünïcödé ✓ ключ'.encode('utf-8'), b'\x00', b'nul\x00inside', b'\xff' * 70,
] + [PATTERN[:n] for n in (55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000)]

MD5_TEXTS = [
    b'98500190-b04fd23c-7d3f96d6-727fe128', b'98500190-B04FD23C-7D3F96D6-727FE128',
    b'00000000-00000000-00000000-00000000', b'ffffffff-ffffffff-ffffffff-ffffffff',
    b'', b'1-2', b'1-2-3-4', b'1-2-3-4-5', b'1 -2-3-4', b' 0xA-+B--1-123456789',
    b'\t\n7-8-9-a', b'-0x10-0X20-30-40', b'11112222333344445-1-1-1', b'zz', b'-', b'12345678-',
    b'deadbeefcafe-1-2-3', b'1--2', b'+-1', b'7-\x00-8', b'0001-0002-0003-0004',
]


def make_md5():
    lines('md5.hex', [d.hex() for d in MD5_INPUTS], 'MD5 inputs, hex')
    lines('md5_parse.hex', [d.hex() for d in MD5_TEXTS], 'MD5Hash::FromString inputs, hex')


# --- Timing::Delta::calc_dt ---------------------------------------------
# "frequency start stop start_ms stop_ms started stopped"

TIMING = [
    (10000000, 0, 166667, 0, 16, 1, 1),
    (10000000, 123456789, 123456789 + 333333, 1000, 1033, 1, 1),
    (10000000, 0, 10000000, 0, 900, 1, 1),
    (10000000, 0, 10000000, 0, 950, 1, 1),
    (10000000, 0, 10000000, 0, 949, 1, 1),
    (10000000, 0, 10000000, 0, 1051, 1, 1),
    (10000000, 0, 10000000, 4294967000, 704, 1, 1),
    (3579545, 1000, 1000 + 59659, 50, 66, 1, 1),
    (1000000000, 5, 5 + 16666667, 7, 23, 1, 1),
    (2400000000, 0, 2400000000 * 3, 0, 3000, 1, 1),
    (1000, 0, 1000, 0, 900, 1, 1),
    (1000, 0, 51, 900, 951, 1, 1),
    (10000000, 500, 100, 10, 0, 1, 1),
    (10000000, 0, 166667, 0, 16, 1, 0),
    (10000000, 0, 166667, 0, 16, 0, 1),
    (10000000, -7000000, 7000000, 100, 1500, 1, 1),
    (10000000, 0, 20000000000000, 0, 2000000000, 1, 1),
    (10000000, 0, 30000000000000, 0, 3000000000, 1, 1),
    (14318180, 0, 715909, 0, 40, 1, 1),
]


def make_timing():
    lines('timing.list', [' '.join(str(v) for v in t) for t in TIMING],
          'frequency start stop start_ms stop_ms started stopped')


# --- ErrorHandler log channels ------------------------------------------

def h(text):
    return text.hex() if text else '-'


DIAG_INI = (b'[Error]\r\n'
            b'log = ini_all.log\r\n'
            b'log = ini_cat.log, 32, 2\r\n'
            b'log = $Text, 7\r\n'
            b'log = ini_float.log, 48.9, 3.5\r\n'
            b'[Other]\r\n'
            b'log = ini_never.log\r\n'
            b'[error]\r\n'
            b'log = ini_second.log, -1, 1\r\n')

DIAG_SCRIPT = [
    'ini diag_error.ini',
    'new %s -1 7 0 0' % h(b'spew.log'),
    'new %s -1 100 1 0' % h(b'upper.log'),
    'new %s 5 7 0 0' % h(b'$window'),
    'new %s 6 7 0 0' % h(b'$bogus'),
    'new - 7 7 0 0',
    'new - 8 7 0 1',
    'enabled -1', 'enabled 32', 'enabled 7', 'enabled 99',
    'dump 100001 %s' % h(b'Error line'),
    'dump 100002 %s' % h(b'Warning line\n'),
    'dump 100003 %s' % h(b'Notice'),
    'dump 100004 %s' % h(b'Trace'),
    'dump 100007 %s' % h(b'Severity seven'),
    'dump 100008 %s' % h(b'Severity eight'),
    'dump 100000 %s' % h(b'Fatal'),
    'dump 202 %s' % h(b'cat 32 warning'),
    'dump 203 %s' % h(b'cat 32 notice'),
    'dump 302 %s' % h(b'cat 48 warning'),
    'dump 100002 -',
    'dump 100002 %s' % h(b'a' * 510),
    'dump 100002 %s' % h(b'b' * 511),
    'dump 100002 %s' % h(b'c' * 1100),
    'dump 100002 %s' % h(b'with\x00nul'),
    'dump 100001 %s' % h(b'mixed Case %s %d \xe9'),
    'comment -1 %s' % h(b'comment all'),
    'comment 32 %s' % h(b'comment 32'),
    'comment 5 %s' % h(b'comment window'),
    'comment 32 %s' % h(b'x' * 600),
    'disable 32',
    'dump 202 %s' % h(b'after disable'),
    'enabled 32',
    'enable -1',
    'dump 202 %s' % h(b'after enable'),
    'disable -1',
    'dump 100001 %s' % h(b'all disabled'),
    'comment -1 %s' % h(b'all disabled comment'),
    'enable 48',
    'dump 302 %s' % h(b'only 48'),
    'enabled 48', 'enabled -1',
    'close',
    'read %s' % h(b'ini_all.log'),
    'read %s' % h(b'ini_cat.log'),
    'read %s' % h(b'ini_float.log'),
    'read %s' % h(b'ini_never.log'),
    'read %s' % h(b'ini_second.log'),
    'read %s' % h(b'spew.log'),
    'read %s' % h(b'upper.log'),
    'ini missing_error.ini',
    'new %s 16 4 0 0' % h(b'second.log'),
    'new %s 16 3 1 0' % h(b'second_text.log'),
    'dump 100002 %s' % h(b'general'),
    'dump 104 %s' % h(b'cat16 trace'),
    'dump 105 %s' % h(b'cat16 sev5'),
    'dump 102 %s' % h(b'cat16 warning'),
    'enabled 16', 'enabled -1',
    'close',
    'read %s' % h(b'second.log'),
    'read %s' % h(b'second_text.log'),
]


def make_diag():
    write('diag_error.ini', DIAG_INI)
    lines('diag.script', DIAG_SCRIPT, 'ErrorHandler script; text and names are hex, - is empty')


if __name__ == '__main__':
    make_rects()
    make_splines()
    make_md5()
    make_timing()
    make_diag()
