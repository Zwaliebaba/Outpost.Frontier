"""Writes number_text.list, rdl_markup.list and rich_text.list, the inputs
of the P4.12, P4.13 and P4.14 text suites (Oracle.exe neuron-client,
oracle_text.cpp and oracle_rich_text.cpp, and
crates/neuron-client/tests/oracle_vectors.rs). Run from any folder:

    python make_text_fixtures.py [--check]

--check compares the lists with what the script would write and fails when
they differ.

number_text.list, one case per line:

    lua <double bits, 16 hex digits>
        MsvcrtFormatLuaNumber of the value.
    printf <count> <format, hex bytes> [i:<8 hex digits> | d:<16 hex digits>]...
        MsvcrtNumericVsnprintf into a buffer of <count> bytes, with int and
        double arguments in order. An empty format is written as "-".

rdl_markup.list, one case per line: UTF-16LE text in hex ("-" for none),
read by XMLReader::read_buffer into an empty RenderDisplayList.

rich_text.list, one case per line, tokens separated by spaces:

    style <id> <ops>        StyleCollection::set_style(id, list)
    styleattr <id> <hex>    StyleCollection::set_style(id, attributes)
    layout <width> <origin h> <origin v> <single line 0|1> <wrap 0|1>
           <attributes hex> <ops>
        LayoutRichText of the list on a recording context that starts from
        the attributes, then the laid list executed, its dimensions and the
        source's caret positions.
    lex <raw 0|1> <ops> / <ops>
        lex_compare of the two lists, and of the first with itself.

An op is a node: t:<units> (TextNode, UTF-16 units as 4 hex digits each),
p:<units> (TextPtrNode), a:<attrs>:<mask>:<default mask> (TRANode, hex),
s:<op> (AttribStackNode), x:<rel h>:<rel v>:<h bits>:<v bits> (PositionNode,
floats as 8 hex digits), n (ParagraphNode), j:<mode> (JustifyNode),
c:<left>:<top>:<right>:<bottom> (ClipNode), y:<id> (StyleNode), 0
(NullNode), i:<w bits>:<h bits> or i:- (ImageNode with an image of that size
or none), and [ <ops> ] (RDLRefNode over a list of its own). The styles
persist from line to line.
"""

import os
import struct
import sys


def bits(value):
    return "%016x" % struct.unpack("<Q", struct.pack("<d", value))[0]


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


INF = float("inf")
IND = "fff8000000000000"
QNAN = "7ff8000000000000"


def lua_cases():
    values = [
        0.0, -0.0, 1.0, -1.0, 29.875, -0.99999, 0.99999, 0.1, 9e-6, 0.0001,
        0.00001, 1e20, 1.5e-300, 123456.789, 1234567890123456.0, 1e16,
        12345678901234567.0, 65535.5, 0.9999999999999999, 0.000778, -0.00072,
        1.7976931348623157e308, 5e-324, 2.2250738585072014e-308,
        1e15 + 0.3, 1.0 / 3.0, 2.0 / 3.0, 100.0, 3e-5, 123e-7, 1e21, 1e-4 * 3,
        1125899906842624.25, 0.5, 2.5, 1e100, 9.999999999999999e22,
        f32(0.1), f32(1.0 / 3.0), f32(3.4028235e38), INF, -INF,
    ]
    lines = ["lua " + bits(v) for v in values]
    lines += ["lua " + IND, "lua " + QNAN]
    return lines


def arg(value):
    if isinstance(value, int):
        return "i:%08x" % (value & 0xFFFFFFFF)
    if isinstance(value, str):
        return "d:" + value
    return "d:" + bits(value)


def printf(fmt, *args, count=32):
    text = fmt.encode("ascii").hex() or "-"
    return " ".join(["printf", str(count), text] + [arg(a) for a in args])


def printf_cases():
    return [
        # The HUD's texts, already formatted.
        printf("1234"),
        printf("75%%"),
        printf("12 M"),
        printf("3.4K"),
        printf(""),
        printf("7" * 31),
        printf("7" * 32),
        printf("7" * 40),
        # FormatFloat's formats: the VC6 fixed text.
        printf("%.2f", 0.105),
        printf("%.2f", 1.005),
        printf("%.2f", 0.125),
        printf("%.0f", 0.5),
        printf("%.0f", 1.5),
        printf("%.0f", 2.5),
        printf("%.1f", 0.05),
        printf("%.2f", -0.001),
        printf("%.2f", -0.0),
        printf("%.2f", 0.0),
        printf("%.1f", 9.96),
        printf("%.1f", 1e20),
        printf("%.1f", 1e30),
        printf("%.1f", 123456789012345678.0),
        printf("%.1f", 1125899906842624.25),
        printf("%.20f", 0.1),
        printf("%.2f", f32(0.1)),
        printf("%.9f", f32(0.1)),
        printf("%.1f", f32(12.345)),
        printf("%.0f", f32(3.4028235e38)),
        printf("%.3fK", 12.5),
        printf("%.1f%", 50.0),
        printf("%.1f%%", 50.0),
        printf("%.1fM", 0.25),
        printf("%.100f", 1.0),
        printf("%.101f", 1.0),
        printf("%.2f", 12.5, count=4),
        printf("%.2f", 12.5, count=5),
        printf("%.2f", 12.5, count=6),
        printf("%.100f" + "K" * 200, 1e300),
        # Infinity and NaN through the legacy printf.
        printf("%.2f", INF),
        printf("%.2f%%", -INF),
        printf("%.3f", IND),
        printf("%.1f", QNAN),
        printf("%.0f", INF),
        printf("%.4f", INF),
        # FormatInt and other formats: the legacy printf.
        printf("%d%c", 7, ord("K")),
        printf("%d%c", 7, 0),
        printf("%d%c", -12, ord("M")),
        printf("%.1f%d", 0.25, 3),
        printf("%.f", 2.5),
        printf("%5.1f", 3.14159),
        printf("%5.2f", 0.125),
        printf("%e", 12345.678),
        printf("%g", 1e20),
        printf("%g", 0.0001),
        printf("%.3g", 2.5e-5),
        printf("%+05d|%-4d|%x", 42, 7, 255),
    ]


def markup(text):
    return text.encode("utf-16-le").hex() or "-"


def markup_cases():
    texts = [
        # The card tags.
        "<RDL><PUSH/><TEXT>Hello</TEXT><PARA/><POP/></RDL>",
        "<RDL><PUSH/><JUST loc=\"c\"/><TRA bold=\"true\"/><TEXT>Title</TEXT>"
        "<TRA bold=\"false\"/><PARA/><JUST loc=\"l\"/><TEXT>Body</TEXT><PARA/><POP/></RDL>",
        "<RDL><TEXT>a &lt;b&gt; &amp; c &quot;d&quot; &apos;e&apos; &#65;&#x42;</TEXT></RDL>",
        "<RDL><TEXT>\U0001F600é’</TEXT></RDL>",
        # TRA attributes.
        "<RDL><TRA italic=\"true\"/><TRA underline=\"false\"/><TRA bold=\"default\"/>"
        "<TRA italic=\"default\" underline=\"true\" bold=\"yes\"/></RDL>",
        "<RDL><TRA font=\"1\"/><TRA font=\"9\"/><TRA font=\"0\"/><TRA font=\"33\"/>"
        "<TRA bold=\"true\" font=\"4\"/><TRA font=\"default\"/></RDL>",
        "<RDL><TRA color=\"#FF0000\"/><TRA color=\"#123456\"/><TRA color=\"#12345\"/>"
        "<TRA color=\"#abcdef\"/><TRA color=\"#GG00zz\"/><TRA color=\"default\"/></RDL>",
        "<RDL><TRA color=\"gray\"/><TRA color=\"blue\"/><TRA color=\"green\"/><TRA color=\"aqua\"/>"
        "<TRA color=\"red\"/><TRA color=\"fuchsia\"/><TRA color=\"yellow\"/><TRA color=\"white\"/>"
        "<TRA color=\"g\"/><TRA color=\"purple\"/><TRA color=\"256\"/><TRA color=\"0\"/></RDL>",
        "<RDL><TRA mask=\"0xff\" data=\"0x1234\" def=\"0x0f\"/><TRA data=\"-1\" mask=\"255\"/>"
        "<TRA mask=\"99999999999\" data=\"0x1ffffffff\"/><TRA mask=\"0x\" data=\" 12\"/>"
        "<TRA bold=\"true\" font=\"0\" data=\"0x1ff\" mask=\"-1\"/><TRA unknown=\"1\"/></RDL>",
        # POS, STYLE, JUST.
        "<RDL><POS h=\"10\" v=\"-20\"/><POS h=\"5\" relH=\"true\" relV=\"false\"/>"
        "<POS v=\"12x\" relV=\"yes\" relH=\"\"/><POS h=\"2147483647\" v=\"-2147483648\"/></RDL>",
        "<RDL><STYLE id=\"3\"/><STYLE id=\"0x10005\"/><STYLE/><STYLE name=\"x\" id=\"65535\"/></RDL>",
        "<RDL><JUST loc=\"C\"/><JUST loc=\"r\"/><JUST loc=\"R\"/><JUST loc=\"x\"/><JUST/>"
        "<JUST loc=\"\"/></RDL>",
        # Names and values compared over their own length.
        "<R><P/><T d=\"0x10\" b=\"\"/><TE>x</TE><PO r=\"\"/><S i=\"7\"/><J l=\"r\"/><PU/><POP/></R>",
        "<RDL><PARAS/><TEXTS>x</TEXTS><TRA bolder=\"true\"/><Q/></RDL>",
        # Character data and the open TEXT node.
        "<RDL><TEXT>a<PARA/>b</TEXT>c<TEXT><TEXT>d</TEXT>e</TEXT></RDL>",
        "<RDL><TEXT><T>a</T>b</TEXT></RDL>",
        "<TEXT>root</TEXT>",
        "<RDL><TEXT/><TEXT></TEXT></RDL>",
        "<RDL>\n  <TEXT> spaced \t text </TEXT>\n</RDL>",
        "<RDL><TEXT>a\r\nb\rc</TEXT></RDL>",
        "<?xml version=\"1.0\"?><!-- c --><RDL><?pi x?><TEXT>a<!-- c -->b</TEXT></RDL>",
        "<RDL><TEXT><![CDATA[<b>&]]></TEXT></RDL>",
        "<r:RDL xmlns:r=\"urn:rdl\"><r:TEXT>a</r:TEXT><r:PARA/></r:RDL>",
        # Not well-formed: what was read before the error stays.
        "",
        "Object Unknown",
        "Trade & Group Offers",
        "<Ship Type>",
        "<Value>",
        "<PARA>",
        "<RDL><TEXT>a</TEXT></RDL>trailing",
        "<RDL><TEXT>&nbsp;</TEXT></RDL>",
        "<RDL><TRA bold=\"true\" bold=\"false\"/></RDL>",
        "<a></b>",
    ]
    return [markup(t) for t in texts]


def t(text):
    return "t:" + "".join("%04x" % u for u in units(text))


def p(text):
    return "p:" + "".join("%04x" % u for u in units(text))


def units(text):
    data = text.encode("utf-16-le")
    return [data[i] | data[i + 1] << 8 for i in range(0, len(data), 2)]


def fbits(value):
    return "%08x" % struct.unpack("<I", struct.pack("<f", value))[0]


def a(attrs, mask, default=0):
    return "a:%08x:%08x:%08x" % (attrs & 0xFFFFFFFF, mask & 0xFFFFFFFF, default & 0xFFFFFFFF)


def x(h, v, rel_h=0, rel_v=0):
    return "x:%d:%d:%s:%s" % (rel_h, rel_v, fbits(h), fbits(v))


def img(w, h):
    return "i:%s:%s" % (fbits(w), fbits(h))


def font(index, bold=False):
    """A TRANode that sets the font byte (index in bits 3..7, bold in bit 0)."""
    return a(index << 3 | int(bold), 0xFF)


def color(rgb):
    return a(rgb << 8, 0xFFFFFF00)


def layout(width, ops, origin=(0, 0), single=0, wrap=1, attrs=0xFFFFFF00):
    return " ".join(["layout", str(width), str(origin[0]), str(origin[1]), str(single), str(wrap),
                     "%08x" % attrs] + ops)


def words(text):
    return [t(text)]


PARA = "n"
LOREM = "The quick brown fox jumps over the lazy dog while Mike watches."


def rich_cases():
    lines = [
        # Styles used below (and one replaced later).
        "style 7 j:1 " + a(0xE0C38708, 0xFFFFFFFF),
        "style 8 " + a(0x52EAF510, 0xFFFFFFF8) + " " + t("[") + " s:0",
        "styleattr 9 1dbf3b01",
        "style 10 y:9 " + t("nested") + " [ " + t("ref") + " ]",
    ]
    lines += [
        # Fits on one line; empty list; empty text.
        layout(200, words("Hello")),
        layout(200, []),
        layout(200, [t(""), "0", t("x")]),
        # Word wrap, no wrap, narrow and zero widths.
        layout(60, words(LOREM)),
        layout(60, words(LOREM), wrap=0),
        layout(23, words(LOREM)),
        layout(1, words("ab cd")),
        layout(0, words("ab cd")),
        layout(-5, words("ab cd")),
        layout(60, words("Supercalifragilisticexpialidocious and more")),
        layout(60, words("short " + "W" * 12 + " tail")),
        layout(40, [t("one "), t("two "), t("three "), t("four "), t("five")]),
        layout(40, [t("one"), t(" two"), t(" three"), t("four"), t("five")], wrap=0),
        # Whitespace kinds.
        layout(30, words("a\tb\tc d  e   f\ng\rh\x0bi\x0cj")),
        layout(25, words("trailing spaces    ")),
        layout(25, words("   leading")),
        # Single-line mode.
        layout(40, words(LOREM), single=1),
        layout(40, words("fits"), single=1),
        layout(2, words("abc"), single=1),
        layout(1, words("abc"), single=1),
        layout(30, [t("ab"), img(40.0, 9.0), t("cd")], single=1),
        layout(30, [font(2), t("big "), font(0), t("small text here")], single=1, wrap=0),
        # Paragraphs, empty lines, justification.
        layout(80, [t("first"), PARA, PARA, t("third"), PARA]),
        layout(80, ["j:1", t("centred line"), PARA, "j:2", t("right"), PARA, "j:0", t("left"),
                    PARA, "j:3", t("other")]),
        layout(50, ["j:1", t(LOREM)], origin=(7, 11)),
        layout(50, ["j:2", t(LOREM)], origin=(-3, 2)),
        layout(50, [t("a"), "j:2", t("b"), PARA, t("c")]),
        # Font changes across nodes: line ascents and heights.
        layout(120, [t("small "), font(3), t("Big "), font(1, True), t("bold "), font(0),
                     t("small again and wrapping onto lines")]),
        layout(70, [font(5), t("x"), PARA, font(0), PARA, font(7), PARA, t("y")]),
        layout(100, [color(0x1234AB), t("red "), a(0, 0x01, 0x01), t("def ")]),
        # The attribute stack and default masks.
        layout(100, ["s:0", font(2), color(0xFF), t("pushed "), "s:1", t("popped "),
                     a(0x12345678, 0xFFFFFFFF, 0xFFFFFF00), t("default colour "), "s:2",
                     "s:1", "s:7", t("end")], attrs=0x80808008),
        layout(100, ["s:0", "s:0", font(4), "s:0", t("deep"), "s:2", t("cleared")]),
        # Positions: absolute and relative, negative.
        layout(100, [x(10.0, 20.0), t("abs"), x(5.5, -3.25, 1, 1), t("rel"), x(-4.0, 0.0, 1, 0),
                     t("back")]),
        layout(30, [t("aa"), x(25.0, 0.0, 1, 0), t("bb"), x(-30.0, 0.0, 1, 0), t("cc")]),
        layout(100, [x(1.0e10, -1.0e10), t("far")]),
        # Clips.
        layout(100, ["c:0:0:50:20", t("clipped"), "c:0:0:0:0", t("free"), "c:5:5:5:9",
                     "c:30:2:10:40", t("x")]),
        # Styles: known, unknown, nested, replaced.
        layout(100, ["y:7", t("styled text that wraps over the width"), "y:8", t("in eight"),
                     "s:1", "y:99", t("unknown")]),
        layout(100, ["y:10", t(" after")]),
        # References, nested and empty.
        layout(60, ["[", t("inner text "), "[", font(1), t("deeper words here"), "]", "]",
                    t("outer"), "[", "]", t("end")]),
        # Images: fitting, deferred, too wide, none.
        layout(60, [t("pic "), img(12.0, 12.0), t(" after")]),
        layout(60, [t("long words before "), img(30.0, 20.0), t("x")]),
        layout(20, [img(30.0, 7.5), t("y")]),
        layout(20, [img(30.0, 7.5), t("y")], wrap=0),
        layout(60, ["i:-", t("none"), img(8.5, 3.75)]),
        # Text pointers.
        layout(40, [p("pointer text that wraps"), p(""), t("x")]),
        # Everything at once.
        layout(90, ["s:0", "j:1", font(2), color(0x40FF40), t("Title"), PARA, "s:1", "j:0",
                    t(LOREM + " "), font(1, True), t("Bold words. "), font(0), img(10.0, 10.0),
                    t(" " + LOREM), PARA, "y:7", t("Styled ending."), PARA],
               origin=(4, 4)),
    ]
    lines += [
        # lex_compare: equal texts give -1, prefixes, case, runs split differently.
        "lex 0 " + t("abc") + " / " + t("abc"),
        "lex 0 " + t("abc") + " / " + t("ABD"),
        "lex 1 " + t("abc") + " / " + t("ABC"),
        "lex 0 " + t("ab") + " / " + t("abc"),
        "lex 0 " + t("abc") + " / " + t("ab"),
        "lex 0 " + t("ab") + " " + t("cd") + " / " + t("abcd"),
        "lex 0 " + t("abcd") + " / " + t("ab") + " " + t("cd"),
        "lex 0 / " + t("x"),
        "lex 0 " + t("x") + " /",
        "lex 0 /",
        "lex 0 " + font(1) + " " + t("") + " " + p("z") + " / " + PARA + " " + t("Z"),
        # An empty text pointer only on the left: on the right (the list compared with
        # itself included) the reference loops for ever (divergence D298).
        "lex 1 " + t("q") + " " + p("") + " / " + t("q"),
        "lex 0 " + t("Zebra") + " / " + t("apple"),
    ]
    return lines


def write(name, header, lines, check):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    text = header + "".join(line + "\n" for line in lines)
    if check:
        with open(path, "rb") as existing:
            if existing.read() != text.encode("ascii"):
                sys.exit("%s is out of date; run make_text_fixtures.py" % name)
        return
    with open(path, "w", newline="\n") as out:
        out.write(text)


def main():
    check = "--check" in sys.argv[1:]
    write(
        "number_text.list",
        "# P4.13/P4.14 number-text oracle inputs, written by make_text_fixtures.py.\n"
        "# lua <double bits> | printf <count> <format hex or -> [i:<int> | d:<double bits>]...\n",
        lua_cases() + printf_cases(),
        check,
    )
    write(
        "rdl_markup.list",
        "# P4.12 RDLX markup oracle inputs, written by make_text_fixtures.py.\n"
        "# One case per line: UTF-16LE text in hex, - for empty.\n",
        markup_cases(),
        check,
    )
    write(
        "rich_text.list",
        "# P4.12 rich-text layout oracle inputs, written by make_text_fixtures.py.\n"
        "# style <id> <ops> | styleattr <id> <hex> | layout <w> <oh> <ov> <single> <wrap> <attrs> <ops>\n"
        "# | lex <raw> <ops> / <ops>; the ops are described in the script.\n",
        rich_cases(),
        check,
    )


if __name__ == "__main__":
    main()
