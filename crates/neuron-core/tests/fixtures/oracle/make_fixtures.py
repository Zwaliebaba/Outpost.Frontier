"""Writes the oracle inputs in this directory.

The files are synthetic: nothing here comes from the original game. Run it
with Python 3 from any directory; it rewrites every input file. The oracle
harness (CppSource/Oracle) and the Rust test tests/oracle_vectors.rs read
the same files and must produce byte-identical vectors.
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


# Device names Windows refuses as file names, with or without an extension.
RESERVED = {'con', 'prn', 'aux', 'nul'} | {'com%d' % i for i in range(1, 10)} | {'lpt%d' % i for i in range(1, 10)}


def write(name, data):
    for part in name.split('/'):
        if part.split('.')[0].lower() in RESERVED:
            raise SystemExit('reserved Windows file name: ' + name)
    path = os.path.join(HERE, *name.split('/'))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def hex_lines(name, items, header):
    lines = ['# ' + header] + [item.hex() for item in items]
    write(name, ('\n'.join(lines) + '\n').encode())


NAMES = [
    b'', b'a', b'A', b'Li01', b'li01', b'LI01_01_Base', b'rh_battleship', b'Sector01',
    b'li_elite', b'[]=;', b'0123456789', b'name with spaces', b'\xe9t\xe9', b'\xc9T\xc9',
    b'\x80\xff\x7f', b'_', b'Zz', b'cut\x00here', b'x' * 254, b'x' * 255, b'x' * 256,
    b'y' * 126, b'y' * 127, b'y' * 128, b'@include', b'Spaceflight',
]

STRICMP = [
    (b'', b''), (b'abc', b'ABC'), (b'abc', b'abd'), (b'_', b'A'), (b'a', b'_'),
    (b'ab', b'abc'), (b'\xe9', b'z'), (b'\xc9', b'\xe9'), (b'[', b'a'), (b'Z', b'['),
    (b'@', b'`'), (b'ab\x00x', b'AB'),
]

NUMBERS = [
    b'', b'0', b'12', b'-12', b'+7', b' \t 42', b'\x0b\x0c13', b'2806013121', b'4294967296',
    b'-2806013121', b'99999999999', b'1.5', b'-1.5', b'0.1', b'0.2', b'0.3',
    b'3.14159265358979323846', b'1e10', b'1E-5', b'1d3', b'6.02214076e23', b'1e308',
    b'1.8e308', b'4.9e-324', b'2.2250738585072011e-308', b'2e-308', b'1.7976931348623157e308',
    b'123456789012345678901234567890', b'0.000000000000000000000000000001',
    b'9007199254740993', b'9007199254740995', b'0.30000000000000004', b'0.1e-2',
    b'1.000000000000000000000000001', b'17976931348623157e292', b'2.47032822920623272e-324',
    b'9999999999999999999999999', b'0x1F', b'0X1f', b'0x1.8', b'-0x10', b'0x', b'101b',
    b'1021b', b'12d', b'Ah', b'1Ah', b'Fh', b'ff', b"'A", b"-'A", b"'\xe9", b'000', b'007',
    b'0.5.5', b'1e', b'e5', b'.5', b'-.5', b'1,5', b'abc', b'\xc1\xc2', b'1e5e', b'1..2',
    b'1+2', b"'a'", b'-1.5e+3', b'1.5e-3x', b'00x10', b'0b101', b'12.5d', b'1b.1',
]

FIELDS = [
    b'a,b,c', b'  x , y ', b',,', b'one', b'', b'\t1 ,\r2', b'a\xa0,\x89b', b'  ,  ,  9',
    b'0x10, 2.5 ,yes', b'z' * 1100 + b',w',
]

WIDE = [
    [], [0x31, 0x32], [0x20, 0x2D, 0x37], [0x3000, 0x2D, 0xFF11, 0xFF12], [0x0663, 0x0030],
    [0x00A0, 0x2B, 0x0669], [0x0E50, 0x0E59, 0x41], [0x32, 0x38, 0x30, 0x36, 0x30, 0x31, 0x33,
    0x31, 0x32, 0x31], [0x2D, 0x2D, 0x31], [0x1810, 0x1819, 0x1820],
]


def f64(value):
    return struct.pack('>d', value)


def f32_widened(value):
    return f64(struct.unpack('<f', struct.pack('<f', value))[0])


DOUBLES = [
    f64(0.0), f64(-0.0), f64(1.0), f64(0.5), f64(1.5), f64(2.5), f64(-2.5), f64(3.5),
    f64(0.1), f64(1e-5), f64(1.25e-5), f64(123456.0), f64(1234565.0), f64(1234575.0),
    f64(999999.5), f64(1e100), f64(1e-100), f64(2147483648.0), f64(2147483647.5),
    f64(2147483647.4), f64(-2147483648.5), f64(-2147483649.0), f64(4294967297.0),
    f64(9223372036854775808.0), f64(-9223372036854775808.0), f64(18446744073709551616.0),
    f64(1e30), f64(float('inf')), f64(float('-inf')), bytes.fromhex('7ff8000000000000'),
    bytes.fromhex('fff8000000000000'), bytes.fromhex('0000000000000001'),
    bytes.fromhex('7fefffffffffffff'), f64(0.3), f64(2 / 3), f64(1 / 3), f64(123.456),
    f32_widened(1.1), f32_widened(0.1), f32_widened(1e10), f32_widened(3e-5), f64(1e-4),
    f64(9.5e-5), f64(0.000123456), bytes.fromhex('3e60000000000000'), f64(-1.9),
    f64(5e-324 * 3), f64(100.0), f64(1e15), f64(1e16), f64(1e17), f64(0.00001),
]

SEEDS = [1, 0, 7, 12345, 0xFFFFFFFF, 0x80000000]

ENGINE = {
    'engine/basic.ini': (
        b'; engine configuration\r\n'
        b'[Zeta]\r\n'
        b'z = 1\r\n'
        b'[  spaced   name ]\r\n'
        b'key   one   =   two   words   ; comment\r\n'
        b'a=b=c\n'
        b'bracket = [x] y\n'
        b'bare key\n'
        b'empty =\n'
        b'\tTabbed\t=\tvalue\t\n'
        b'[alpha]\n'
        b'x = first\n'
        b'X = second\n'
        b'[_under]\n'
        b'u = 1\n'
        b'[Alpha]\n'
        b'x = duplicate section\n'
        b'[zeta]\n'
        b'@includex = not an include\n'
        b'[a] trailing = on the header line\n'
        b'[unclosed\n'
        b'still = in a\n'
    ),
    'engine/include.ini': (
        b'[main]\n'
        b'before = 1\n'
        b'@include sub.ini\n'
        b'after = 2\n'
        b'@INCLUDE missing.ini\n'
        b'last = 3\n'
    ),
    'engine/sub.ini': b'from_sub = yes\n[subsection]\ninner = 1\n',
    'engine/no_name.ini': b'[s]\na = 1\n@include\nb = 2\n',
    'engine/key_first.ini': b'a = 1\n[s]\n',
    'engine/stray.ini': b'[s]\na = 1\n]\nb = 2\n',
    'engine/stray_equals.ini': b'[s]\n= 1\n',
    'engine/long_ok.ini': b'[s]\nk = ' + b'v' * 0x3FD + b'\n',
    'engine/long_value.ini': b'[s]\nk = ' + b'v' * 0x3FE + b'\n',
    'engine/long_name.ini': b'[' + b'n' * 0x3FE + b']\n',
    'engine/eof.ini': b'[s]\na = 1\xffb = 2\n',
    'engine/comment_ff.ini': b'[s]\na = 1 ; note \xff more\nb = 2\n',
    'engine/nul_byte.ini': b'[s]\nab\x00cd = v\x00w x\nnext = 1\n',
    'engine/high.ini': b'[\xe9t\xe9]\n\xc9 = \x80\x81\n',
    'engine/no_newline.ini': b'[s]\nlast = 1',
    'engine/empty.ini': b'',
}

TEXT_INI = {
    'ini/basic.ini': (
        b'; leading comment\r\n'
        b'\r\n'
        b'  [Ship]  ; trailing\r\n'
        b'nickname = li_elite ; comment\r\n'
        b'\tids_name\t=\t  237033  \r\n'
        b'flag\r\n'
        b'empty =\r\n'
        b'list = 1, 2.5 , 0x10,101b, Ah, -\'A, yes, TRUE, 0 ,false\n\r'
        b'hex = 0x1F, 1021b, 12d\r'
        b'mixed = a,,b,\r\n'
        b'; just a comment\n'
        b'[;Disabled]\n'
        b'hidden = 1\n'
        b'[ Spaced Name ]\n'
        b'v = 2806013121, -2806013121, 4294967296\n'
        b'nul = ab\x00cd, ef\n'
        b'high = \xe9\xe8, \x80\n'
        b'w = 1 2 3 ; comment, with comma\n'
        b'[Ship]\n'
        b'nickname = second\n'
        b'[]\n'
        b'k=v\n'
        b'[last]\n'
        b'long = ' + b'L' * 1100 + b'\n'
        b'tail = 1'
    ),
    'ini/crlf_only.ini': b'[a]\r\rx=1\r\n\r\ny=2\n\n\rz=3',
    'ini/empty.ini': b'',
}


def main():
    hex_lines('names.hex', NAMES, 'Names for the name CRC and ID functions, one hex line each.')
    write('stricmp.hex', ('# Comparison pairs: left:right in hex.\n' +
                          ''.join(a.hex() + ':' + b.hex() + '\n' for a, b in STRICMP)).encode())
    hex_lines('numbers.hex', NUMBERS, 'Number texts for atoi, atof, ParseNumber, is_number.')
    hex_lines('fields.hex', FIELDS, 'Comma lists for StringIndex and ValueNum.')
    write('wide.hex', ('# UTF-16 texts for _wtoi: four hex digits per code unit.\n' +
                       ''.join(''.join('%04x' % u for u in w) + '\n' for w in WIDE)).encode())
    hex_lines('doubles.hex', DOUBLES, 'Doubles as big-endian bit patterns for %g and conversions.')
    write('rand.hex', ('# Seeds for srand.\n' + ''.join('%08x\n' % s for s in SEEDS)).encode())
    for name, data in ENGINE.items():
        write(name, data)
    write('engine.list', ('# Engine INI files, parsed with the profile parser.\n' +
                          ''.join(n + '\n' for n in ENGINE if n != 'engine/sub.ini')).encode())
    for name, data in TEXT_INI.items():
        write(name, data)
    write('reader.list', ('# Files read with INI_Reader, from the file and from memory.\n' +
                          ''.join(n + '\n' for n in TEXT_INI)).encode())


main()
