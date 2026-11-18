"""Writes scene_script.list, the inputs of the P4.14 oracle suite.

Each line is one case: buffers in hex, separated by spaces, run one after
the other in one fresh Lua state (Oracle.exe neuron-client and
crates/neuron-client/tests/oracle_vectors.rs). A buffer is one or more
compiled Lua 3.2 chunks, assembled here instruction by instruction, in the
subset of design/facts.md F15. Run from any folder:

    python make_scene_script_fixtures.py
"""

import os
import struct

# lopcodes.h, Lua 3.2
ENDCODE, PUSHNUMBERW, PUSHNUMBER, PUSHNUMBERNEGW, PUSHNUMBERNEG = 0, 6, 7, 8, 9
PUSHCONSTANTW, PUSHCONSTANT, GETGLOBALW, GETGLOBAL = 10, 11, 14, 15
CREATEARRAYW, CREATEARRAY, SETGLOBALW, SETGLOBAL = 21, 22, 24, 25
SETLISTW, SETLIST, SETMAP, ADDOP, LONGARGW, LONGARG = 28, 29, 30, 37, 64, 65


class Chunk:
    """Main function of a chunk: code, constants and the stack depth."""

    def __init__(self):
        self.code = bytearray()
        self.consts = []
        self.depth = 0
        self.max_depth = 0

    def _push(self, n=1):
        self.depth += n
        self.max_depth = max(self.max_depth, self.depth)

    def _pop(self, n=1):
        self.depth -= n
        assert self.depth >= 0

    def const(self, value):
        key = (type(value).__name__, value)
        for i, c in enumerate(self.consts):
            if (type(c).__name__, c) == key:
                return i
        self.consts.append(value)
        return len(self.consts) - 1

    def _arg(self, op, wide_op, arg):
        if arg < 256:
            self.code += bytes([op, arg])
        else:
            assert arg < 65536
            self.code += bytes([wide_op, arg >> 8, arg & 255])

    def number(self, n):
        if n >= 0:
            self._arg(PUSHNUMBER, PUSHNUMBERW, n)
        else:
            self._arg(PUSHNUMBERNEG, PUSHNUMBERNEGW, -n)
        self._push()

    def text_number(self, text):
        """A number constant stored as `text` (bytes)."""
        self._arg(PUSHCONSTANT, PUSHCONSTANTW, self.const(("num", text)))
        self._push()

    def string(self, s):
        self._arg(PUSHCONSTANT, PUSHCONSTANTW, self.const(("str", s)))
        self._push()

    def get(self, name):
        self._arg(GETGLOBAL, GETGLOBALW, self.const(("str", name)))
        self._push()

    def set(self, name):
        self._arg(SETGLOBAL, SETGLOBALW, self.const(("str", name)))
        self._pop()

    def table(self, size):
        self._arg(CREATEARRAY, CREATEARRAYW, size)
        self._push()

    def setlist(self, block, n):
        self._arg(SETLIST, SETLISTW, block)
        self.code.append(n)
        self._pop(n)

    def setmap(self, pairs):
        self.code += bytes([SETMAP, pairs - 1])
        self._pop(2 * pairs)

    def add(self):
        self.code.append(ADDOP)
        self._pop()

    def raw(self, data, push=0):
        self.code += bytes(data)
        self._push(push)

    def bytes(self):
        code = bytes([self.max_depth, 0]) + bytes(self.code) + bytes([ENDCODE])
        out = bytearray(b"\x1bLua\x32\x00")
        out += struct.pack(">I", 0)  # line defined
        out += struct.pack(">I", 0)  # no source name
        out += struct.pack(">I", len(code)) + code
        out += struct.pack(">I", 0)  # no locals
        out += struct.pack(">I", len(self.consts))
        for kind, value in self.consts:
            if kind == "num":
                out += bytes([1, len(value)]) + value
            else:
                out += bytes([2]) + struct.pack(">I", len(value) + 1) + value + b"\x00"
        return bytes(out)


def list_items(c, items, emit):
    """A positional constructor, flushed every 64 items like the compiler."""
    c.table(len(items))
    for i in range(0, len(items), 64):
        block = items[i : i + 64]
        for item in block:
            emit(c, item)
        c.setlist(i // 64, len(block))


def scene():
    """A script in the shape of the shipped ones."""
    c = Chunk()
    c.text_number(b"24.937")
    c.set(b"duration")
    # entities = { { entity_name = "Scene_1", type = SCENE, spatialprops = { pos = { 0, 1.5, -2 } } },
    #              { entity_name = "Mk", type = MARKER, userprops = { a = "x", b = "y" } } }
    c.table(2)
    c.table(3)
    c.string(b"entity_name"); c.string(b"Scene_1")
    c.string(b"type"); c.get(b"SCENE")
    c.string(b"spatialprops")
    c.table(1)
    c.string(b"pos")
    c.table(3); c.number(0); c.text_number(b"1.5"); c.number(-2); c.setlist(0, 3)
    c.setmap(1)
    c.setmap(3)
    c.table(3)
    c.string(b"entity_name"); c.string(b"Mk")
    c.string(b"type"); c.get(b"MARKER")
    c.string(b"userprops")
    c.table(2); c.string(b"a"); c.string(b"x"); c.string(b"b"); c.string(b"y"); c.setmap(2)
    c.setmap(3)
    c.setlist(0, 2)
    c.set(b"entities")
    # events = { { 0.5, START_SOUND, "Mk" }, { 1, SET_CAMERA, "Scene_1" } }
    c.table(2)
    c.table(3); c.text_number(b"0.5"); c.get(b"START_SOUND"); c.string(b"Mk"); c.setlist(0, 3)
    c.table(3); c.number(1); c.get(b"SET_CAMERA"); c.string(b"Scene_1"); c.setlist(0, 3)
    c.setlist(0, 2)
    c.set(b"events")
    return c


def numbers():
    """Stored number texts as the reference reads them, and the integer pushes."""
    c = Chunk()
    texts = [b"0.1", b"9e-006", b"1e+020", b"0.9999900000000001", b"-2.5", b"5 ", b"1.",
             b".25", b"123456789012345678901234567890", b"0.0007780000000000001",
             b"-0", b"1e-300"]
    # No text whose power of ten leaves the double range (1e-310, 0e400): the
    # reference may keep Expten's result on the x87 stack with its wider
    # exponent, which the 53-bit model of decisions.md T11 does not cover.
    list_items(c, texts, lambda c, t: c.text_number(t))
    c.set(b"texts")
    ints = [0, 1, 255, 256, 65535, -1, -255, -256, -65535]
    list_items(c, ints, lambda c, n: c.number(n))
    c.set(b"ints")
    return c


def long_list():
    """130 items: three SETLIST flushes, the last at block 2."""
    c = Chunk()
    list_items(c, list(range(130)), lambda c, n: c.number(n * 3))
    c.set(b"long")
    return c


def records():
    """Records: 40 string keys (two SETMAPs), then a table grown from size 0."""
    c = Chunk()
    c.table(40)
    for start in (0, 32):
        n = min(32, 40 - start)
        for i in range(start, start + n):
            c.string(b"k%02d" % i)
            c.number(i)
        c.setmap(n)
    c.set(b"rec")
    # Numeric keys through SETMAP into a table presized 0: collisions and rehashes.
    c.table(0)
    keys = [1, 6, 11, 16, 2, 7, 12, 30, 5, 10, 15, 20, 25, 100, 3, 47, 94, 141]
    for k in keys:
        c.number(k)
        c.number(k * 10)
    c.setmap(len(keys))
    c.set(b"grown")
    # One SETMAP per key: each insertion may rehash on its own.
    c.table(0)
    for k in [23, 46, 69, 92, 115, 0, -1, -23]:
        c.number(k)
        c.number(1)
        c.setmap(1)
    c.set(b"probes")
    return c


def mixed():
    """A list with fields, nil values, a duplicated key, shared tables."""
    c = Chunk()
    c.table(2)
    c.number(7); c.setlist(0, 1)
    c.table(0)
    c.setlist(0, 0)  # an empty flush
    c.set(b"nested_empty")
    c.set(b"unused")  # pops the outer list
    c.table(4)
    c.string(b"x"); c.number(1); c.string(b"name"); c.string(b"m"); c.setmap(2)
    c.number(10); c.get(b"NOT_A_GLOBAL"); c.number(30); c.setlist(0, 3)
    c.string(b"x"); c.number(2); c.number(1); c.string(b"one"); c.setmap(2)
    c.set(b"mixed")
    c.table(1); c.number(1); c.setlist(0, 1)
    c.set(b"shared")
    c.table(3); c.get(b"shared"); c.get(b"shared"); c.get(b"mixed"); c.setlist(0, 3)
    c.set(b"refs")
    c.table(1); c.string(b"1"); c.string(b"string one"); c.number(1); c.string(b"number one"); c.setmap(2)
    c.set(b"string_keys")
    return c


def predefined():
    """Values the reference VM predefines and globals set to nil."""
    c = Chunk()
    c.table(6)
    c.get(b"PI"); c.get(b"_VERSION"); c.get(b"foreach"); c.get(b"_INPUT"); c.get(b"HARDPOINT"); c.get(b"POSITION")
    c.setlist(0, 6)
    c.set(b"pre")
    c.get(b"NOT_A_GLOBAL")
    c.set(b"HIDDEN")
    c.get(b"Y_AXIS")
    c.set(b"copy")
    return c


def arithmetic():
    """ADDOP on numbers and on strings that convert."""
    c = Chunk()
    c.number(2); c.text_number(b"0.25"); c.add(); c.set(b"sum")
    c.string(b" 12"); c.number(1); c.add(); c.set(b"from_string")
    c.number(1); c.string(b"-.5"); c.add(); c.set(b"string_right")
    c.string(b"1e2"); c.string(b"+3"); c.add(); c.set(b"both_strings")
    c.number(-3); c.text_number(b"3"); c.add(); c.set(b"zero")
    return c


def bad_add():
    """An ADDOP on a string that does not convert, after a global was set."""
    c = Chunk()
    c.number(5); c.set(b"before")
    c.string(b"abc"); c.number(1); c.add(); c.set(b"after")
    return c


def nil_key():
    """A table store whose key is nil."""
    c = Chunk()
    c.number(1); c.set(b"before")
    c.table(1); c.get(b"NOT_A_GLOBAL"); c.number(1); c.setmap(1); c.set(b"after")
    return c


def longarg():
    """LONGARG before PUSHNUMBERW gives 65536 + n; before SETMAP it is dropped."""
    c = Chunk()
    c.raw([LONGARG, 1, PUSHNUMBERW, 0, 2], push=1)
    c.set(b"big")
    c.table(1)
    c.number(4); c.number(5)
    c.raw([LONGARG, 7])
    c.setmap(1)
    c.set(b"after_longarg")
    return c


def simple(name, value):
    c = Chunk()
    c.number(value)
    c.set(name)
    return c


def hexs(*buffers):
    return " ".join(b.hex() for b in buffers)


def main():
    events_only = Chunk()
    events_only.table(1); events_only.table(0); events_only.setlist(0, 1); events_only.set(b"events")
    entities_only = Chunk()
    entities_only.table(1); entities_only.table(0); entities_only.setlist(0, 1); entities_only.set(b"entities")
    two = simple(b"first", 1).bytes() + simple(b"second", 2).bytes()
    garbage = simple(b"kept", 3).bytes() + b"\x00"
    truncated = simple(b"cut", 4).bytes()[:-3]
    cases = [
        hexs(scene().bytes()),
        hexs(numbers().bytes()),
        hexs(long_list().bytes()),
        hexs(records().bytes()),
        hexs(mixed().bytes()),
        hexs(predefined().bytes()),
        hexs(arithmetic().bytes()),
        hexs(bad_add().bytes()),
        hexs(nil_key().bytes()),
        hexs(longarg().bytes()),
        # Two chunks in one buffer; a chunk followed by a byte that is not one;
        # a truncated chunk.
        hexs(two),
        hexs(garbage),
        hexs(truncated),
        # One state across buffers: the second script sees the first one's events.
        hexs(events_only.bytes(), entities_only.bytes()),
        # The same script twice: the first run's tables are released.
        hexs(scene().bytes(), scene().bytes()),
    ]
    header = (
        "# P4.14 scene-script oracle inputs, written by make_scene_script_fixtures.py.\n"
        "# One case per line: buffers in hex, space-separated, run in one fresh Lua state.\n"
    )
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scene_script.list")
    with open(out, "w", newline="\n") as f:
        f.write(header)
        for case in cases:
            f.write(case + "\n")


if __name__ == "__main__":
    main()
