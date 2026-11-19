"""Writes the oracle inputs of work package P4.1: account.list and perf_options.list with the
PerfOptions.ini files under perf/.

The inputs are synthetic: nothing here comes from the original game. Run it with Python 3 from
any directory; it rewrites every file it owns, or with --check reports a file that differs. The
oracle harness (CppSource/Oracle/oracle_account.cpp, oracle_perf_options.cpp, Oracle.exe
game-app) and the Rust test tests/oracle_vectors.rs read the same files and must produce
byte-identical vectors.
"""

import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = "--check" in sys.argv[1:]


def write(name, data):
    path = os.path.join(HERE, name)
    if CHECK:
        try:
            with open(path, "rb") as existing:
                same = existing.read() == data
        except OSError:
            same = False
        if not same:
            sys.exit("%s is out of date; run make_game_app_fixtures.py" % name)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as out:
        out.write(data)


# --- CPlayerAccount (account.list) -------------------------------------------


def md5_text(data):
    """MD5Hash::AsString: the four state words, each a little-endian u32 of the digest."""
    words = struct.unpack("<4I", hashlib.md5(data).digest())
    return "-".join("%08x" % w for w in words)


def key_of(raw):
    """GenerateTextKey's layout: the text, '-', the last four characters of its MD5 text."""
    return raw + "-" + md5_text(raw.encode())[-4:]


def h(data):
    return data.hex() if data else "."


def product_id(seed, length):
    return bytes((seed * 7 + i * 13) & 0xFF for i in range(length))


def account_lines():
    uuid = bytes(range(0x10, 0x20)).hex()
    lines = [
        "seed nokey %s %s %s 1234 -" % (h(product_id(1, 0xA4)), uuid, h(b"pilot")),
        "seed none %s %s %s 0 %s" % (h(product_id(2, 0xA4)), uuid, h(b"Administrator"), h(b"SERVER")),
        "seed nokey nokey %s - 4294967295 -" % uuid,
        "seed none none %s - 0 -" % bytes(16).hex(),
        "seed %s %s %s %s 7 -" % (h(product_id(3, 16)), h(product_id(4, 0xA4)), uuid, h(b"pilot")),
        # A first value over the buffer leaves its size for the second query.
        "seed %s %s %s %s 7 -" % (h(product_id(5, 0x401)), h(product_id(6, 0xA4)), uuid, h(b"pilot")),
        "seed none %s %s %s 7 -" % (h(product_id(7, 0x400)), uuid, h(b"pilot")),
        "seed none %s %s %s 7 -" % (h(product_id(8, 0x401)), uuid, h(b"pilot")),
        "seed %s nokey %s %s 7 %s" % (h(b""), uuid, h(b"Jos\xe9"), h(b"Name with spaces")),
        "seed nokey %s %s %s 99 %s" % (h(product_id(9, 0xA4)), uuid, h(b""), h(b"")),
    ]
    raw = md5_text(b"outpost frontier")
    good = key_of(raw)
    bad_check = good[:-1] + ("0" if good[-1] != "0" else "1")
    lines += [
        "setkey %s" % h(good.encode()),
        "setkey %s" % h(good.upper().encode()),
        "setkey %s" % h(bad_check.encode()),
        "setkey %s" % h((good + "x").encode()),
        "setkey %s" % h(good[:0x24].encode()),
        "setkey %s" % h(good[:0x23].encode()),
        "setkey %s" % h(good[:10].encode()),
        "setkey %s" % h(key_of("not an md5 text but 35 bytes long!!").encode()),
        "setkey %s" % h((raw + "+" + md5_text(raw.encode())[-4:]).encode()),
        "getkey %s" % h(good.encode()),
        "getkey %s" % h((good + "x").encode()),
        "getkey %s" % h((good + "xy").encode()),
        "getkey %s" % h(good[:0x24].encode()),
        "getkey %s" % h(bad_check.encode()),
        "getkey nokey",
    ]
    name = md5_text(good.encode())
    sig = md5_text(name.encode())
    server = md5_text(b"server one")
    lines += [
        "load %s %s %s" % (h(name.encode()), h(sig.encode()), h(server.encode())),
        "load %s %s %s" % (h(name.encode()), h(md5_text(b"other").encode()), h(server.encode())),
        "load %s %s %s" % (h(name.upper().encode()), h(sig.encode()), h(server.encode())),
        "load %s - %s" % (h(name.encode()), h(server.encode())),
        "load - - %s" % h(server.encode()),
        "load %s %s %s" % (h(b"12-34"), h(md5_text(b"00000012-00000034-98badcfe-10325476").encode()),
                           h(b"garbage")),
        "load %s %s %s" % (h(b"  0x1a-+2-ffffffffff-0"), h(sig.encode()), h(b"12-34")),
        "load %s %s ." % (h(name.encode()), h(sig.encode())),
    ]
    return lines


def make_account():
    header = [
        "# P4.1 account oracle inputs (CPlayerAccount, common_playeraccount.cpp), written by",
        "# make_game_app_fixtures.py; the formats are in CppSource/Oracle/oracle_account.cpp.",
        "# Hex fields use . for no bytes.",
    ]
    write("account.list", ("\n".join(header + account_lines()) + "\n").encode())


# --- PerfOptions.ini (perf_options.list, perf/) ------------------------------

NAMES = """mouse_speed gamma_ramp DitherControl MipFilterControlCallback TurnSensitivity
generic_effect_perf Nebula SunEffect Asteroids space_dust_detail star_sphere
AtmosphereMaterialConfig HUDAnimConfig level_of_detail use_environment_maps
high_detail_textures sound_max_dist_scale sound_level_of_detail USE_3D_SOUNDS audio_music
audio_ambient audio_sfx audio_interface audio_voice rtc_performance general_graphics
advanced_graphics default_graphics skip_transition_scripts enable_tooltips enable_rollover
cockpit USE_DYNAMIC_COMM_HEAD DIFFICULTY_SCALE autohide_maneuver_bar max_submitted_sounds""".split()
assert len(NAMES) == 36


def values(seed):
    return ["%.2f" % (((i * 37 + seed * 11) % 101) / 100) for i in range(36)]


def body(version="5", skip="FALSE", count="36", opts=None, display=True, eol="\r\n"):
    """A file laid out as SavePerfOptions writes it."""
    opts = opts if opts is not None else list(zip(NAMES, values(1)))
    lines = ["[PerfVersion]", "version=%s" % version, "SkipMachineWarnings=%s" % skip, "",
             "[PerfCount]", "count=%s" % count, "", "[PerfOptions]"]
    lines += ["%s = %s" % (n, v) for n, v in opts]
    if display:
        lines += ["", "[Display]", "size= 1280, 960", "color_bpp= 32", "depth_bpp= 24"]
    return eol.join(lines) + eol


def ini(name, text):
    write(os.path.join("perf", name), text.encode("latin-1"))


def make_perf():
    ini("valid.ini", body())
    ini("valid_lf.ini", body(eol="\n", skip="TRUE"))
    ini("old_version.ini", body(version="4"))
    negative = list(zip(NAMES, values(2)))
    negative[5] = (NAMES[5], "-0.25")
    ini("negative.ini", body(opts=negative))
    ini("count_mismatch.ini", body(count="35"))
    ini("count_fraction.ini", body(count="36.9"))
    ini("unknown.ini", body(opts=list(zip(NAMES, values(3))) + [("not_an_option", "0.5"), ("Mouse_Speed", "0.75")]))
    ini("partial.ini", body(count="3", opts=[("audio_music", "0.25"), ("audio_sfx", "0.5"), ("cockpit", "1")]))
    ini("no_version.ini", "[PerfCount]\r\ncount=1\r\n\r\n[PerfOptions]\r\naudio_music = 0.5\r\n\r\n"
        "[Display]\r\nsize= 800, 600\r\n")
    ini("options_first.ini", "[PerfOptions]\r\naudio_music = 0.5\r\n[PerfVersion]\r\nversion=5\r\n"
        "[PerfCount]\r\ncount=1\r\n[Display]\r\nsize= 640, 480\r\ncolor_bpp= 16\r\n")
    ini("display_only.ini", "[Display]\nsize= 1920, 1080\ncolor_bpp= 32\ndepth_bpp= 16\n")
    ini("skip.ini", body(version="5.5", skip="TRUE", display=False))
    ini("two_versions.ini", "[PerfVersion]\nversion=5\n[PerfVersion]\nversion=1\nSkipMachineWarnings=TRUE\n"
        "[PerfCount]\ncount=1\n[PerfOptions]\ncockpit = 1\n")
    ini("version_edges.ini", body(version="4.99999999", display=False))
    ini("version_tiny.ini", body(version="5.000000001", display=False))
    ini("odd_values.ini", body(count="6", opts=[("audio_music", "1e1"), ("audio_sfx", ".5"), ("cockpit", "abc"),
                                                ("audio_voice", "0x10"), ("Nebula", "-0"), ("SunEffect", "0.125")]))
    late = list(zip(NAMES, values(4)))
    late.append(("audio_music", "-1"))
    ini("negative_last.ini", body(count="37", opts=late))
    ini("empty.ini", "")
    ini("dup.ini", body(count="4", opts=[("audio_music", "0.25"), ("audio_music", "0.75"), ("cockpit", "1"),
                                         ("cockpit", "0")]))
    ini("display_odd.ini", "[PerfVersion]\nversion=5\n[Display]\nsize= 1024\ncolor_bpp= abc\n"
        "size= 300, 200, 100\ndepth_bpp= -7\n")
    cases = [
        "valid.ini d 1280 960 32 24",
        "valid.ini - 1024 768 32 24",
        "valid_lf.ini d 800 600 16 16",
        "old_version.ini d 1024 768 32 24",
        "negative.ini d 1024 768 32 24",
        "count_mismatch.ini d 1024 768 32 24",
        "count_fraction.ini d 1024 768 32 24",
        "unknown.ini d 1024 768 32 24",
        "partial.ini d 1024 768 32 24",
        "no_version.ini d 1024 768 32 24",
        "options_first.ini d 1024 768 32 24",
        "display_only.ini d 1920 1080 32 24",
        "skip.ini d 1024 768 32 24",
        "two_versions.ini d 1024 768 32 24",
        "version_edges.ini d 1024 768 32 24",
        "version_tiny.ini d 1024 768 32 24",
        "odd_values.ini d 1024 768 32 24",
        "negative_last.ini d 1024 768 32 24",
        "missing.ini d 1024 768 32 24",
        "empty.ini d 1024 768 32 24",
        "dup.ini d 1024 768 32 24",
        "display_odd.ini d 1024 768 32 24",
        "valid.ini,partial.ini d 1280 960 32 24",
        "valid.ini,old_version.ini,skip.ini d 1280 960 32 24",
        "partial.ini,partial.ini d 0 0 -1 -1",
    ]
    header = [
        "# P4.1 PerfOptions.ini oracle inputs (LoadPerfOptions, SavePerfOptions), written by",
        "# make_game_app_fixtures.py; the format is in CppSource/Oracle/oracle_perf_options.cpp: files",
        "# (comma-separated, under perf/), d or - (a display block or none) and the saved display's",
        "# width, height, colour and depth.",
    ]
    write("perf_options.list", ("\n".join(header + cases) + "\n").encode())


if __name__ == "__main__":
    make_account()
    make_perf()
