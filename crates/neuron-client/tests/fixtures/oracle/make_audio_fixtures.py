"""Writes audio/*.list, the inputs of the P4.10 oracle suites.

Oracle.exe neuron-client (CppSource/Oracle/oracle_audio.cpp) runs them
through the reference's SoundManager code; crates/neuron-client/tests/
oracle_vectors.rs through the port. Floats are f32 bit patterns in eight
hex digits, vectors three of them joined by commas, '-' means "the source
does not answer". The token layout of each file is described at the top of
the file itself. Run from any folder:

    python make_audio_fixtures.py [--check]

With --check nothing is written; the script fails when a file differs from
what it would write.
"""

import os
import random
import struct
import sys

CHECK = "--check" in sys.argv[1:]

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "audio")


def f(x):
    """A float as its f32 bit pattern."""
    return "%08x" % struct.unpack("<I", struct.pack("<f", x))[0]


def bits(b):
    return "%08x" % b


def vec(v):
    return ",".join(f(x) for x in v)


def opt(x, fmt=f):
    return "-" if x is None else fmt(x)


def write_text(name, text):
    path = os.path.join(OUT, name)
    data = text.encode("latin-1")
    if CHECK:
        with open(path, "rb") as existing:
            if existing.read() != data:
                sys.exit("audio/%s is out of date; run make_audio_fixtures.py" % name)
        return
    with open(path, "wb") as out:
        out.write(data)


def write(name, header, lines):
    write_text(name, "".join("# " + h + "\n" for h in header) + "".join(l + "\n" for l in lines))


def rnd_vec(r, scale):
    return [r.uniform(-scale, scale) for _ in range(3)]


def unit(v):
    n = sum(x * x for x in v) ** 0.5
    return [x / n for x in v]


def apply_cases():
    r = random.Random(4010)
    lines = []
    fwd, up = [0.0, 0.0, 1.0], [0.0, 1.0, 0.0]
    base = dict(scale=1, front=fwd, up=up, lpos=[0.0, 0.0, 0.0], df=1.0, rf=1.0,
                master=0.0, gain=0.0, rate=22050, maxpan=10000.0, is3d=1,
                spos=[20.0, 0.0, 0.0], smin=10.0, smax=500.0, atten=0.0,
                pitch=None, pan=None, pancap=1, freqcap=0)
    cases = []
    # Hand-made: inside, at and beyond the minimum distance, on each axis.
    for p in ([5.0, 0, 0], [10.0, 0, 0], [20.0, 0, 0], [-40.0, 0, 0], [0, 0, 80.0],
              [0, 0, -80.0], [0, 30.0, 0], [3.0, 4.0, 0], [1000.0, 0, 0], [0, 0, 0]):
        cases.append(dict(base, spos=p))
    cases.append(dict(base, scale=-1, spos=[10.0, 0, 30.0]))
    cases.append(dict(base, df=2.0, rf=0.5, spos=[15.0, 2.0, -7.0]))
    cases.append(dict(base, rf=0.0, spos=[200.0, 0, 0]))
    cases.append(dict(base, smin=0.0, spos=[50.0, 0, 0]))
    cases.append(dict(base, smin=None, smax=None, spos=None, lpos=[3.0, 1.0, -2.0]))
    cases.append(dict(base, master=-12.5, gain=-3.0, atten=-6.0, spos=[25.0, 5.0, 5.0]))
    cases.append(dict(base, master=-80.0, gain=-50.0, spos=[25.0, 5.0, 5.0]))
    cases.append(dict(base, maxpan=1500.0, spos=[25.0, 5.0, 5.0]))
    cases.append(dict(base, pancap=0, spos=[25.0, 5.0, 5.0]))
    cases.append(dict(base, freqcap=1, pitch=37.5, spos=[25.0, 5.0, 5.0]))
    cases.append(dict(base, freqcap=1, pitch=None, spos=[25.0, 5.0, 5.0]))
    # 2D sources: client pan, clamps, frequency over the pitch range.
    for pan in (0, 1234, -10000, 10000, 20000, -32768, None):
        cases.append(dict(base, is3d=0, pan=pan))
    cases.append(dict(base, is3d=0, pancap=0, pan=500))
    for pitch in (0.0, 1.0, -1.0, 50.0, -50.0, 99.9, 100.0, -100.0, 150.0, -1000.0, 12.34):
        for rate in (11025, 22050, 44100):
            cases.append(dict(base, is3d=0, pan=0, freqcap=1, pitch=pitch, rate=rate))
    cases.append(dict(base, is3d=0, pan=0, freqcap=1, pitch=100.0, rate=65535))
    cases.append(dict(base, is3d=0, pan=0, freqcap=1, pitch=-100.0, rate=150))
    for g, m, a in ((0.0, 0.0, 0.0), (-0.004, 0.0, 0.0), (-0.005, -0.005, 0.0),
                    (-33.333, -33.333, -33.333), (-100.0, 0.0, 0.0), (5.0, 0.0, 0.0),
                    (-0.01, 0.0, 0.0)):
        cases.append(dict(base, is3d=0, pan=0, gain=g, master=m, atten=a))
    # Random listener poses and sources.
    for _ in range(80):
        front = unit(rnd_vec(r, 1.0))
        upv = unit(rnd_vec(r, 1.0))
        cases.append(dict(
            base, scale=r.choice((1, -1)), front=front, up=upv,
            lpos=rnd_vec(r, 1000.0), df=r.choice((1.0, 0.5, 2.0, r.uniform(0.1, 4))),
            rf=r.choice((1.0, r.uniform(0, 3))), master=r.uniform(-20, 0),
            gain=r.uniform(-20, 0), maxpan=r.choice((10000.0, 1500.0)),
            spos=None, smin=r.uniform(1, 50), smax=r.uniform(50, 2000),
            atten=r.uniform(-30, 0), freqcap=r.choice((0, 1)),
            pitch=r.choice((None, r.uniform(-120, 120))),
            rate=r.choice((11025, 22050, 44100))))
        c = cases[-1]
        c["spos"] = [c["lpos"][i] + r.uniform(-300, 300) for i in range(3)]
    for c in cases:
        lines.append(" ".join([
            str(c["scale"]), vec(c["front"]), vec(c["up"]), vec(c["lpos"]), f(c["df"]),
            f(c["rf"]), f(c["master"]), f(c["gain"]), str(c["rate"]), f(c["maxpan"]),
            str(c["is3d"]), opt(c["spos"], vec), opt(c["smin"]), opt(c["smax"]),
            f(c["atten"]), opt(c["pitch"]), opt(c["pan"], str), str(c["pancap"]),
            str(c["freqcap"])]))
    write("apply.list", [
        "MixerApplyInstance on the software path, one instance per line:",
        "scale front up listener-pos distance-factor rolloff master-gain arch-gain rate",
        "max3dpan is3d source-pos min max attenuation pitch pan pan-cap freq-cap",
    ], lines)


def pitch_cases():
    lines = []
    pitches = [0.0, 1.0, -1.0, 0.5, 25.0, -25.0, 50.0, -50.0, 75.0, 99.0, 100.0, -100.0,
               100.5, -100.5, 1000.0, -1e30, 12.345, -67.89, 1e-30, float("inf")]
    for p in pitches:
        for duration, flags in ((1000, 2), (1000, 0), (12345, 2), (1, 2), (4000000, 2)):
            lines.append("%s %d %d" % (f(p), duration, flags))
    lines.append("- 1000 2")
    lines.append("%s 1000 2" % bits(0x7FC00000))
    write("pitch.list", [
        "PitchToMultiplier and ScaledDurationMs: pitch duration-ms arch-flags",
    ], lines)


def start_cases():
    r = random.Random(4011)
    lines = []
    base = dict(now=1000, start=900, last=0, started=0, freq=0, looping=0, pitch=None,
                rate=22050, block=4, frames=22050, ls=0, le=22050, playing=0, never=0)
    cases = [dict(base)]
    cases.append(dict(base, start=1000))
    cases.append(dict(base, start=0))  # 1000 ms in: the end
    cases.append(dict(base, start=1))
    cases.append(dict(base, started=1, last=900))  # same tick: no restart
    cases.append(dict(base, started=1, last=800))  # restarted, still playing
    cases.append(dict(base, started=1, last=800, playing=0))
    cases.append(dict(base, started=1, last=800, playing=1))
    cases.append(dict(base, freq=1, pitch=None))
    cases.append(dict(base, freq=1, pitch=100.0))
    cases.append(dict(base, freq=1, pitch=-100.0))
    cases.append(dict(base, freq=1, pitch=33.3))
    cases.append(dict(base, looping=1, start=0, now=5000))
    cases.append(dict(base, looping=1, start=0, now=5000, ls=1000, le=2000))
    cases.append(dict(base, looping=1, start=0, now=5000, ls=1000, le=30000, frames=40000))
    cases.append(dict(base, never=1))
    cases.append(dict(base, never=1, started=1, last=900))
    cases.append(dict(base, now=10, start=20))  # wraps: far in the future
    cases.append(dict(base, rate=44100, block=2, frames=100000, start=0, now=2000))
    for _ in range(40):
        frames = r.randint(1, 200000)
        ls = r.randint(0, frames)
        cases.append(dict(
            base, now=r.randint(0, 100000), start=r.randint(0, 100000),
            last=r.choice((0, 5)), started=r.randint(0, 1), freq=r.randint(0, 1),
            looping=r.randint(0, 1), pitch=r.choice((None, r.uniform(-100, 100))),
            rate=r.choice((11025, 22050, 44100)), block=r.choice((1, 2, 4)), frames=frames,
            ls=ls, le=r.randint(ls + 1, frames + 1), playing=r.randint(0, 1)))
    for c in cases:
        lines.append(" ".join(str(x) for x in [
            c["now"], c["start"], c["last"], c["started"], c["freq"], c["looping"]]) + " " +
            opt(c["pitch"]) + " " + " ".join(str(x) for x in [
                c["rate"], c["block"], c["frames"], c["ls"], c["le"], c["playing"],
                c["never"]]))
    write("start.list", [
        "MixerSetStartOffset: now start-tick last-start-tick started freq-control looping",
        "pitch rate block-align frames loop-start loop-end buffer-playing never-set-start-pos",
    ], lines)


def listener_cases():
    r = random.Random(4012)
    lines = []
    fixed = [
        ([0, 0, 1], [0, 1, 0]), ([0, 0, -1], [0, 1, 0]), ([1, 0, 0], [0, 0, 1]),
        ([0.6, 0.0, 0.8], [0, 1, 0]), ([0.1, 0.2, 0.3], [-0.3, 0.2, 0.1]),
    ]
    for scale in (1, -1):
        for rh in (0, 1):
            for front, up in fixed:
                lines.append("v %d %d %s %s %s %s" % (
                    scale, rh, vec(front), vec(up), vec([1.5, -2.5, 3.25]),
                    vec([0.0, -10.0, 7.0])))
    for _ in range(30):
        m = [r.uniform(-1, 1) for _ in range(9)]
        lines.append("m %d %d %s %s %s" % (
            r.choice((1, -1)), r.randint(0, 1), ",".join(f(x) for x in m),
            vec(rnd_vec(r, 5000)), vec(rnd_vec(r, 100))))
    for _ in range(30):
        lines.append("v %d %d %s %s %s %s" % (
            r.choice((1, -1)), r.randint(0, 1), vec(rnd_vec(r, 3)), vec(rnd_vec(r, 3)),
            vec(rnd_vec(r, 5000)), vec(rnd_vec(r, 100))))
    write("listener.list", [
        "The listener: v scale right-handed front up position velocity",
        "or m scale right-handed matrix(9) position velocity",
    ], lines)


def range_cases():
    r = random.Random(4013)
    lines = []
    base = dict(now=1000, looping=0, is3d=1, flags=0x11, duration=500, pitch=None,
                start=600, scale=1, lpos=[0.0, 0.0, 0.0], spos=[3.0, 4.0, 0.0], smax=10.0,
                arch=1)
    cases = [dict(base)]
    cases += [dict(base, smax=5.0), dict(base, smax=4.9), dict(base, smax=None),
              dict(base, flags=0x10, smax=1.0), dict(base, is3d=0, smax=1.0),
              dict(base, arch=0), dict(base, arch=0, looping=1), dict(base, looping=1),
              dict(base, start=500), dict(base, start=499), dict(base, start=2000),
              dict(base, flags=0x13, pitch=100.0, start=750),
              dict(base, flags=0x13, pitch=100.0, start=749),
              dict(base, flags=0x13, pitch=-100.0, start=0),
              dict(base, flags=0x11, pitch=-100.0, start=499),
              dict(base, flags=0x13, pitch=0.0, start=499),
              dict(base, now=5, start=0xFFFFFFF0, duration=10),
              dict(base, scale=-1, lpos=[0.0, 0.0, 5.0], spos=[0.0, 0.0, -5.0], smax=0.5),
              dict(base, scale=-1, lpos=[0.0, 0.0, 5.0], spos=[0.0, 0.0, 5.0], smax=9.0),
              dict(base, smax=float("nan")), dict(base, spos=[1e20, 0, 0], smax=1e19)]
    for _ in range(40):
        cases.append(dict(
            base, now=r.randint(0, 5000), looping=r.randint(0, 1), is3d=r.randint(0, 1),
            flags=r.choice((0x11, 0x10, 0x13, 0x12, 0x09)), duration=r.randint(0, 3000),
            pitch=r.choice((None, r.uniform(-100, 100))), start=r.randint(0, 5000),
            scale=r.choice((1, -1)), lpos=rnd_vec(r, 100), spos=rnd_vec(r, 100),
            smax=r.choice((None, r.uniform(1, 200)))))
    for c in cases:
        lines.append(" ".join([
            str(c["now"]), str(c["looping"]), str(c["is3d"]), "%x" % c["flags"],
            str(c["duration"]), opt(c["pitch"]), str(c["start"]), str(c["scale"]),
            vec(c["lpos"]), vec(c["spos"]), opt(c["smax"]), str(c["arch"])]))
    write("range.list", [
        "MixerAllowCreate and OneShotAlreadyExpired: now looping is3d arch-flags(hex)",
        "duration-ms pitch start-tick scale listener-pos source-pos max has-archetype",
    ], lines)


def ini_cases():
    values = [
        "", "0", "1", "24", "-1", "+7", "007", "2806013121", "4294967296", "99999999999",
        "true", "TRUE", "yes", "Yes", "false", "no", "NO", "nope", "0x1F", "0X1f", "x",
        "15x", "1x0", "-0x10", "0xFFFFFFFFF", "24 ; comment", "  12", "12abc", "abc",
        "3.75", "-2.5", "1e3", "1e", "1e+", ".5", "5.", ".", "+-1", "-.25e1x", "0x10",
        "inf", "INFINITY", "-Infinity", "nan", "nan(123)", "1e400", "1e-50", "1e-40",
        "3.4028235e38", "3.4028236e38", "0.1", "0.3", "123456789", "16777217",
        "0.000000000000000000000000000000000000000000001", "1.49999999999999999999",
        "2.5dB", " 0x1.8p1", " -0x1p-149", "tRuE", "yes2", "Light", "1.0 dB",
    ]
    lines = [v.encode().hex() for v in values]
    write("ini.list", [
        "ParseIniUnsigned and ParseIniFloat: the value's bytes in hex",
    ], lines)


def eax_cases():
    lines = []
    for env in (0, 1, 25, 26, -1, 1000):
        lines.append("%d %s %s %s %s" % (env, f(0.5), f(1.49), f(1.0), f(-3.0)))
    f01 = struct.unpack("<I", struct.pack("<f", 0.1))[0]
    for vol, dec, damp, gain in (
            (-0.1, 0.05, -0.5, 1.0), (1.5, 150.0, 2.5, -150.0), (0.0, 0.1, 0.0, 0.0),
            (1.0, 100.0, 2.0, -100.0), (float("inf"), float("-inf"), float("inf"), float("-inf"))):
        lines.append("0 %s %s %s %s" % (f(vol), f(dec), f(damp), f(gain)))
    lines.append("0 %s %s %s %s" % (f(0.5), bits(f01 - 1), f(1.0), f(-0.0)))
    lines.append("0 %s %s %s %s" % (f(0.5), bits(f01), f(1.0), bits(0x7FC00000)))
    lines.append("3 %s %s %s %s" % (bits(0x7FC00000), bits(0x7FC00000), bits(0x7FC00000),
                                    f(-99.99)))
    write("eax.list", [
        "SetEaxPreview and FSoundArchetype::SetGain: environment volume decay damping gain",
    ], lines)


def tick_cases():
    """Scripted MixerTick sessions. Commands, one per line:

    case max-channels master-gain distance-factor rolloff scale max3dpan
    listener front up position
    arch flags(hex) channels bytes-per-sample rate block-align frames loop-start loop-end gain
    src key is3d looping arch-index(-1: none)
    set key field value   (active, start, atten, pitch, pan, pos, min, max, looping)
    tick now attached-keys stopped-keys    (comma lists, '-' for none)

    A new source is active, started at tick 0, attenuation 0, with no pitch,
    pan 0, at the origin, and answers no distances.
    """
    L = []
    o, fz, uy = vec([0, 0, 0]), vec([0, 0, 1]), vec([0, 1, 0])

    def case(maxch, master=0.0, df=1.0, rf=1.0, scale=1, maxpan=10000.0):
        L.append("case %d %s %s %s %d %s" % (maxch, f(master), f(df), f(rf), scale, f(maxpan)))
        L.append("listener %s %s %s" % (fz, uy, o))

    def arch(flags, rate=22050, block=2, frames=22050, ls=0, le=None, gain=0.0, ch=1):
        L.append("arch %x %d %d %d %d %d %d %d %s" % (
            flags, ch, block // ch, rate, block, frames, ls, frames if le is None else le,
            f(gain)))

    def src(key, is3d, looping, archi):
        L.append("src %d %d %d %d" % (key, is3d, looping, archi))

    def set_(key, field, value):
        L.append("set %d %s %s" % (key, field, value))

    def tick(now, attached, stopped=()):
        a = ",".join(str(k) for k in attached) or "-"
        s = ",".join(str(k) for k in stopped) or "-"
        L.append("tick %d %s %s" % (now, a, s))

    # 1. The cap: four looping 2D sources, two channels, changing ranks.
    case(2)
    arch(0x11)
    for k in range(4):
        src(k, 0, 1, 0)
    set_(1, "active", 0)
    set_(2, "pan", -2500)
    tick(100, [0, 1, 2, 3])
    tick(200, [3, 2, 0])
    set_(1, "active", 1)
    tick(300, [1, 0, 2, 3])
    tick(400, [])
    tick(500, [0, 0, 1])
    # 2. One-shots: expiry, future starts, restarts, finished voices.
    case(8, master=-3.0)
    arch(0x13, gain=-6.0)
    arch(0x11, rate=44100, block=4, ch=2, frames=4410)
    src(0, 0, 0, 0)
    src(1, 0, 0, 1)
    src(2, 0, 1, 1)
    src(3, 0, 0, -1)
    set_(0, "pitch", f(-50.0))
    set_(1, "start", 50)
    set_(2, "start", 2000)
    tick(60, [0, 1, 2, 3])
    tick(140, [0, 1, 2, 3], [1])
    set_(1, "start", 145)
    tick(150, [0, 1, 2, 3], [1])
    tick(1600, [0, 1, 2, 3])
    tick(2100, [0, 1, 2, 3])
    set_(0, "start", 2050)
    set_(0, "pitch", f(30.0))
    set_(0, "atten", f(-20.5))
    tick(2100, [0, 1, 2, 3], [1])
    # 3. 3D sources on pan buffers: attenuation, pan, range at creation.
    case(4, df=1.0, rf=1.5, scale=-1, maxpan=1500.0)
    arch(0x11)
    arch(0x13, rate=11025)
    src(0, 1, 1, 0)
    src(1, 1, 1, 1)
    src(2, 1, 1, 0)
    set_(0, "pos", vec([30.0, 0.0, 40.0]))
    set_(0, "min", f(5.0))
    set_(1, "pos", vec([-100.0, 10.0, 0.0]))
    set_(1, "max", f(50.0))
    set_(1, "pitch", f(12.0))
    set_(2, "pos", vec([0.0, 0.0, -3.0]))
    tick(10, [0, 1, 2])
    set_(1, "max", f(200.0))
    set_(0, "pos", vec([-30.0, 0.0, 40.0]))
    tick(20, [0, 1, 2])
    set_(1, "pos", vec([500.0, 10.0, 0.0]))
    set_(2, "atten", f(-4.0))
    tick(30, [2, 1, 0])
    # 4. A 3D archetype: a voice beyond its maximum distance is stopped.
    case(4)
    arch(0x09)
    src(0, 1, 1, 0)
    src(1, 1, 1, 0)
    set_(0, "pos", vec([0.0, 0.0, 100.0]))
    set_(0, "max", f(300.0))
    set_(1, "pos", vec([0.0, 0.0, 400.0]))
    set_(1, "max", f(300.0))
    tick(0, [0, 1])
    set_(0, "pos", vec([0.0, 0.0, 300.0]))
    set_(1, "pos", vec([0.0, 0.0, 299.0]))
    tick(10, [0, 1])
    tick(20, [0, 1])
    # 5. Loops: wrap of the start offset inside a loop range.
    case(2)
    arch(0x11, frames=10000, ls=2000, le=6000)
    src(0, 0, 1, 0)
    set_(0, "start", 0)
    tick(1000, [0])
    tick(1010, [0])
    set_(0, "start", 500)
    tick(1500, [0], [0])
    write("tick.list", [line for line in tick_cases.__doc__.strip().splitlines()], L)


def reverb_cases():
    """reverb.list: groups separated by ';', each "env vol decay damp" (env decimal, the others
f32 bits). The first group is [AudioProperties] as StartupDevice stores it; each later group is
one scene-script SetReverb."""
    def g(env, vol, decay, damp):
        return "%d %s %s %s" % (env, f(vol), f(decay), f(damp))
    nan = struct.unpack("<f", struct.pack("<I", 0x7FC00000))[0]
    lines = [
        # The shipped engine INI: the defaults.
        g(0, 0.0, 0.1, 1.0),
        # Out-of-range starting values: kept without a device, clamped with one.
        g(30, 1.5, 0.05, 2.5) + ";" + g(5, 0.5, 1.49, 0.5) + ";" + g(-1, -0.25, 150.0, -1.0),
        g(25, 1.0, 100.0, 2.0) + ";" + g(26, 0.0, 0.1, 0.0) + ";" + g(12, 0.75, 3.92, 0.83),
        g(1, nan, nan, nan) + ";" + g(2, 0.3, nan, 1.25),
        g(10, 0.2, 7.5, 0.1) + ";" + g(10, 0.2, 7.5, 0.1) + ";" + g(0, 1.0, 0.1, 1.0),
    ]
    write("reverb.list", [line for line in reverb_cases.__doc__.strip().splitlines()], lines)


def wav(channels, bits_, rate, frames):
    """The name the harness makes a wav of."""
    return "audio\\c%db%dr%df%d.wav" % (channels, bits_, rate, frames)


def nick(name):
    return name.encode("latin-1").hex()


ARCH_A = """; P4.10 client wiring: [Sound] archetypes for audio_arch.jsonl and audio_client.jsonl,
; written by make_audio_fixtures.py.
[Sound]
nickname = ui_click
type = interface
file = %(w2205)s
attenuation = -8
is_2d = true

[Sound]
nickname = sfx_loop
type = normal
file = %(w22050)s
range = 40, 600
attenuation = -3.9

[Sound]
nickname = sfx_bend
file = %(w11025)s
pitch_bendable = true
is_2d = true
range = 25

[Sound]
nickname = sfx_loud
file = %(w22050)s
attenuation = 12
persistent = space

[Sound]
nickname = sfx_quiet
file = %(w44100s)s
attenuation = -120
persistent = gameflow
range = 10, 5

[Sound]
nickname = amb_wind
type = ambience
file = %(w22050)s
range = -3, 100
persistent = nowhere
streamer = false
crv_pitch = 0

[Sound]
nickname = music_theme
type = music
file = %(w44100s)s
is_2d = true
volume = 3

[Sound]
nickname = voice_line
type = voice
file = %(w2205)s
is_2d = 1

[Sound]
nickname = sfx_missing
file = audio\\nothing_here.wav

[Sound]
nickname = sfx_nofile

[Sound]
type = interface
file = %(w2205)s

[Sound]
nickname = sfx_lod
file = %(w22050)s
lod1 = sfx_loop
lod2 = NONE

[Sound]
nickname = sfx_lod2
file = %(w2205)s
lod2 = ui_click

[Sound]
nickname = sfx_lod_bad
file = %(w2205)s
lod1 = no_such_sound

[Sound]
nickname = UI_Click
type = interface
file = %(w2205)s
attenuation = -6
is_2d = true
""" % dict(w2205=wav(1, 16, 22050, 2205), w22050=wav(1, 16, 22050, 22050),
          w11025=wav(1, 16, 11025, 11025), w44100s=wav(2, 16, 44100, 4410))

ARCH_B = """; P4.10 client wiring: redefinitions for audio_arch.jsonl, written by make_audio_fixtures.py.
[Sound]
nickname = sfx_loop
type = normal
file = %(w22050)s
range = 80
attenuation = -1

[Sound]
nickname = ui_click
type = interface
file = %(w4410)s
is_2d = true

[Sound]
nickname = sfx_extra
file = %(w4410)s
type = interface
""" % dict(w22050=wav(1, 16, 22050, 22050), w4410=wav(1, 8, 22050, 4410))


def arch_cases():
    """arch.list: one command per line (the format is in oracle_audio_client.cpp)."""
    write_text("arch_a.ini", ARCH_A.replace("\n", "\r\n"))
    write_text("arch_b.ini", ARCH_B.replace("\n", "\r\n"))
    L = []

    def case(use3d=1.0, scale=1.0, maxsub=32, maxch=8):
        L.append("case %s %s %d %d" % (f(use3d), f(scale), maxsub, maxch))

    case()
    L.append("ini 0 audio/arch_a.ini")
    for name in ("ui_click", "sfx_loop", "sfx_bend", "sfx_loud", "sfx_quiet", "amb_wind",
                 "music_theme", "voice_line", "sfx_missing", "sfx_nofile", "nobody"):
        L.append("acquire " + nick(name))
    L.append("acquire " + nick("ui_click"))
    L.append("release " + nick("ui_click"))
    L.append("release " + nick("sfx_loop"))
    L.append("release " + nick("sfx_loop"))
    L.append("acquire " + nick("sfx_loop"))
    L.append("ini 0 audio/arch_b.ini")
    L.append("acquire " + nick("sfx_extra"))
    L.append("ini 1 audio/arch_b.ini")
    L.append("release " + nick("ui_click"))
    L.append("acquire " + nick("sfx_lod"))
    L.append("lod 1")
    L.append("acquire " + nick("sfx_lod"))
    L.append("acquire " + nick("sfx_lod_bad"))
    L.append("lod 2")
    L.append("acquire " + nick("sfx_lod2"))
    L.append("release " + nick("sfx_lod"))
    L.append("lod 0")
    L.append("release " + nick("sfx_lod2"))
    L.append("preload 1")
    L.append("preload 2")
    L.append("unload 1")
    L.append("unload 3")
    # Without 3D sounds every archetype is made with pan control.
    case(use3d=0.5, scale=2.0)
    L.append("ini 0 audio/arch_a.ini")
    for name in ("ui_click", "sfx_loop", "sfx_bend", "amb_wind"):
        L.append("acquire " + nick(name))
    write("arch.list", [line for line in arch_cases.__doc__.strip().splitlines()], L)


def client_cases():
    """client.list: one command per line (the format is in oracle_audio_client.cpp)."""
    L = []
    r = random.Random(4016)

    def case(use3d=1.0, scale=1.0, maxsub=32, maxch=8):
        L.append("case %s %s %d %d" % (f(use3d), f(scale), maxsub, maxch))
        L.append("ini 0 audio/arch_a.ini")

    def play(name, flags=None, start=0, pos=(0.0, 0.0, 0.0), atten=0.0, handle=1):
        if flags is None:
            L.append("play %s - 0 - 0 %d" % (nick(name), handle))
        else:
            L.append("play %s %x %d %s %s %d" % (nick(name), flags, start, vec(pos), f(atten), handle))

    def src(i, op, value=None):
        L.append("src %d %s%s" % (i, op, "" if value is None else " " + value))

    def tick(now, dt=1.0 / 30.0):
        L.append("tick %d %s" % (now, f(dt)))

    # 1. Flat sources: ranking ties, the silence floor, categories, ducking, the audio mode,
    # one-shots retired when they end.
    case(maxch=2)
    play("ui_click", 0, start=0)
    play("sfx_loop", 1, start=0)
    play("music_theme", 1, start=50)
    play("voice_line", 0, atten=-40.0)
    play("sfx_quiet", 1)
    play("nobody", 1)
    tick(1000)
    tick(1033)
    src(0, "atten", f(-100.0))
    src(1, "pan", f(-3000.0))
    L.append("vol 0 %s" % f(0.5))
    L.append("vol 2 %s" % f(0.001))
    tick(1066)
    L.append("duck push")
    tick(1100, 0.05)
    tick(1150, 0.05)
    tick(1200, 0.5)
    L.append("duck pop")
    tick(1250, 0.25)
    L.append("mode 1")
    play("ui_click", 0)
    tick(1300)
    L.append("mode 2")
    src(1, "type", "4")
    tick(1350)
    L.append("mode 0")
    tick(1500)
    src(1, "release")
    src(2, "stop")
    tick(1600)
    play("sfx_loop", None)
    tick(1700)
    tick(30000)
    # 2. Pitch: a pitch-bendable flat archetype, frequency and pitched lengths.
    case(maxch=4)
    play("sfx_bend", 0, start=0)
    play("sfx_bend", 1, start=0)
    src(0, "pitch", f(-50.0))
    src(1, "pitch", f(120.0))
    tick(10)
    tick(500)
    src(1, "pitch", f(0.0))
    tick(600)
    tick(1400)
    tick(2600)
    src(1, "offset", "100")
    tick(2700)
    # 3. Positioned sources: listener, distance scores, the maximum range, the perf scale,
    # pan buffers in 3D, 3D buffers on the software path.
    case(scale=2.0, maxch=3)
    L.append("listener %s %s %s" % (vec([0.0, 0.0, 1.0]), vec([0.0, 1.0, 0.0]), vec([10.0, 0.0, -5.0])))
    play("sfx_loop", 3, pos=(30.0, 0.0, 40.0))
    play("sfx_loop", 3, pos=(10.0, 0.0, 500.0))
    play("sfx_bend", 3, pos=(-20.0, 5.0, 0.0))
    play("ui_click", 2, pos=(60.0, 0.0, -5.0))
    play("amb_wind", 3, pos=(0.0, 0.0, 0.0))
    play("sfx_loop", 3, pos=(10.0, 0.0, 1300.0))
    play("sfx_loop", 1)
    tick(100)
    src(0, "pos", vec([10.0, 0.0, 15.0]))
    src(1, "pos", vec([10.0, 0.0, 2000.0]))
    src(2, "atten", f(-30.0))
    tick(150)
    L.append("listener %s %s %s" % (vec([1.0, 0.0, 0.0]), vec([0.0, 1.0, 0.0]), vec([0.0, 0.0, 0.0])))
    tick(200)
    tick(250)
    # 4. Many positioned sources: the sort over more than 16, the voice budget and the cap.
    case(maxsub=3, maxch=4)
    L.append("listener %s %s %s" % (vec([0.0, 0.0, 1.0]), vec([0.0, 1.0, 0.0]), vec([0.0, 0.0, 0.0])))
    for i in range(21):
        p = [r.uniform(-400.0, 400.0), r.uniform(-50.0, 50.0), r.uniform(-400.0, 400.0)]
        if i in (4, 9):
            p = [100.0, 0.0, 0.0]
        play("sfx_loop", 3, start=r.randint(0, 200), pos=p, atten=r.choice((0.0, -2.0, -6.5)))
    tick(300)
    for i in (3, 7, 11):
        src(i, "pos", vec([1.0 + i, 0.0, 0.0]))
    src(5, "stop")
    tick(400)
    tick(500)
    # 5. Without 3D sounds: positioned sources on pan buffers.
    case(use3d=0.5, maxch=4)
    L.append("listener %s %s %s" % (vec([0.0, 0.0, 1.0]), vec([0.0, 1.0, 0.0]), vec([0.0, 0.0, 0.0])))
    play("sfx_loop", 3, pos=(30.0, 0.0, 40.0))
    play("amb_wind", 3, pos=(-50.0, 0.0, 10.0), start=40)
    tick(100)
    tick(140)
    write("client.list", [line for line in client_cases.__doc__.strip().splitlines()], L)


def main():
    os.makedirs(OUT, exist_ok=True)
    apply_cases()
    pitch_cases()
    start_cases()
    listener_cases()
    range_cases()
    ini_cases()
    eax_cases()
    tick_cases()
    reverb_cases()
    arch_cases()
    client_cases()


if __name__ == "__main__":
    main()
