"""Writes number_text.list and rdl_markup.list, the inputs of the P4.12,
P4.13 and P4.14 text suites (Oracle.exe neuron-client, oracle_text.cpp,
and crates/neuron-client/tests/oracle_vectors.rs). Run from any folder:

    python make_text_fixtures.py

number_text.list, one case per line:

    lua <double bits, 16 hex digits>
        MsvcrtFormatLuaNumber of the value.
    printf <count> <format, hex bytes> [i:<8 hex digits> | d:<16 hex digits>]...
        MsvcrtNumericVsnprintf into a buffer of <count> bytes, with int and
        double arguments in order. An empty format is written as "-".

rdl_markup.list, one case per line: UTF-16LE text in hex ("-" for none),
read by XMLReader::read_buffer into an empty RenderDisplayList.
"""

import os
import struct


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


def write(name, header, lines):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    with open(path, "w", newline="\n") as out:
        out.write(header)
        for line in lines:
            out.write(line + "\n")


def main():
    write(
        "number_text.list",
        "# P4.13/P4.14 number-text oracle inputs, written by make_text_fixtures.py.\n"
        "# lua <double bits> | printf <count> <format hex or -> [i:<int> | d:<double bits>]...\n",
        lua_cases() + printf_cases(),
    )
    write(
        "rdl_markup.list",
        "# P4.12 RDLX markup oracle inputs, written by make_text_fixtures.py.\n"
        "# One case per line: UTF-16LE text in hex, - for empty.\n",
        markup_cases(),
    )


if __name__ == "__main__":
    main()
