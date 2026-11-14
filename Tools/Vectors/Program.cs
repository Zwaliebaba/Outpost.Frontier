// Writes test vectors for Source/Core by running the original's Fixed64 and Checksum (compiled from
// the decompiled sources). Output: one line per case, tab-separated:
//   op  arg...  =>  result...
// Fixed64 values are raw int64 (decimal), ints decimal, doubles and floats as hex bit patterns
// (16 and 8 digits), bools 0/1. A case whose original throws (integer division by zero) is skipped.
// Inputs come from a fixed-seed splitmix64, so the file is reproducible.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using BBI.Core.Utility;
using BBI.Core.Utility.FixedPoint;

internal static class Program
{
    private static ulong sState = 0x0D0C_2016_DEAD_BEEFul;
    private static readonly StringBuilder sOut = new StringBuilder();
    private static readonly Dictionary<string, int> sCounts = new Dictionary<string, int>();
    private const int N = 300;

    private static ulong Next()
    {
        ulong z = (sState += 0x9E3779B97F4A7C15ul);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ul;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBul;
        return z ^ (z >> 31);
    }

    private static long Range(long lo, long hi) => lo + (long)(Next() % (ulong)(hi - lo + 1));
    private static int Int(int lo, int hi) => (int)Range(lo, hi);
    private static Fixed64 Raw(long r) => Fixed64.FromRawValue(r);

    private static readonly long[] sSpecial =
    {
        0, 1, -1, 2, -2, 3, 4294967296L, -4294967296L, 2147483648L, -2147483648L, 4294967295L, 4294967297L,
        long.MaxValue, long.MinValue, long.MaxValue - 1, long.MinValue + 1, 9223372036854775806L, -9223372036854775806L,
        13493037704L, -13493037704L, 26986075409L, 6746518852L, 1L << 62, -(1L << 62), 1L << 32 << 30, 8589934592L,
        4294L, 42949673L, 429496730L, 0x7FFFFFFF00000000L, -0x7FFFFFFF00000000L, 0x0000000100000001L,
    };

    // A Fixed64 from a mix of distributions: special values, any bits, moderate values, fractions,
    // whole numbers.
    private static Fixed64 F()
    {
        switch (Next() % 6)
        {
            case 0: return Raw(sSpecial[Next() % (ulong)sSpecial.Length]);
            case 1: return Raw((long)Next());
            case 2: return Raw(Range(-(1L << 42), 1L << 42));               // |x| < 1024
            case 3: return Raw(Range(-(1L << 33), 1L << 33));               // |x| < 2
            case 4: return Fixed64.FromInt(Int(-100000, 100000));
            default: return Raw(Range(-(1L << 56), 1L << 56));              // |x| < 2^24
        }
    }

    private static Fixed64 Moderate() => (Next() % 4 == 0) ? Raw(sSpecial[Next() % (ulong)sSpecial.Length]) : Raw(Range(-(1L << 40), 1L << 40));
    private static Fixed64 Angle() => Raw(Range(-(100L << 32), 100L << 32));
    private static Fixed64 Exponent() => Raw(Range(-(40L << 32), 40L << 32));
    private static int SmallInt() => (Next() % 3 == 0) ? (int)(uint)Next() : Int(-70, 70);

    private static string S(Fixed64 f) => f.RawValue.ToString(CultureInfo.InvariantCulture);
    private static string S(int i) => i.ToString(CultureInfo.InvariantCulture);
    private static string S(long l) => l.ToString(CultureInfo.InvariantCulture);
    private static string S(double d) => BitConverter.DoubleToInt64Bits(d).ToString("x16");
    private static string S(float f) => BitConverter.ToInt32(BitConverter.GetBytes(f), 0).ToString("x8");
    private static string S(bool b) => b ? "1" : "0";

    private static void Emit(string op, string[] args, string[] results)
    {
        sOut.Append(op).Append('\t').Append(string.Join("\t", args)).Append("\t=>\t").Append(string.Join("\t", results)).Append('\n');
        sCounts[op] = sCounts.TryGetValue(op, out int n) ? n + 1 : 1;
    }

    private static void Case(string op, Func<string[]> args, Func<string[]> result)
    {
        for (int i = 0; i < N; i++)
        {
            string[] a = args();
            string[] r;
            try
            {
                r = result();
            }
            catch (ArithmeticException)
            {
                continue;
            }
            Emit(op, a, r);
        }
    }

    private static double RandomDouble()
    {
        switch (Next() % 5)
        {
            case 0: return BitConverter.Int64BitsToDouble((long)Next());
            case 1: return (Next() % 2 == 0 ? 1.0 : -1.0) * (Next() % 1000000) / 1000.0;
            case 2: return (long)Next() / 1e9;
            case 3: return new[] { 0.0, -0.0, 0.5, -0.5, 1.5, 2.5, 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0, double.NaN, double.PositiveInfinity, double.NegativeInfinity, 1e300, -1e300 }[Next() % 15];
            default: return (double)(Int(-1000000, 1000000)) / 64.0;
        }
    }

    private static float RandomFloat()
    {
        switch (Next() % 4)
        {
            case 0: return BitConverter.ToSingle(BitConverter.GetBytes((uint)Next()), 0);
            case 1: return (float)RandomDouble();
            case 2: return new[] { 0f, 0.1f, 0.25f, 0.5f, 1f, 0.05f, 0.01f, 1.5f, 2.5f, -2.5f, 1e-3f }[Next() % 11];
            default: return Int(-100000, 100000) / 100f;
        }
    }

    private static int Main(string[] argv)
    {
        if (argv.Length != 1)
        {
            Console.Error.WriteLine("usage: Outpost.Vectors OUTPUT.tsv");
            return 2;
        }
        Fixed64 x = default, y = default, z = default;
        int i = 0, j = 0;
        double d = 0;
        float f = 0, g = 0;

        // Every public static Fixed64 field: the constants, including the computed ones.
        foreach (FieldInfo field in typeof(Fixed64).GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)
                                                   .Where(fi => fi.FieldType == typeof(Fixed64)).OrderBy(fi => fi.Name, StringComparer.Ordinal))
            Emit("const", new[] { field.Name }, new[] { S((Fixed64)field.GetValue(null)) });

        Case("add", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(x + y) });
        Case("sub", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(x - y) });
        Case("neg", () => new[] { S(x = F()) }, () => new[] { S(-x) });
        Case("mul", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(x * y) });
        Case("mul_moderate", () => new[] { S(x = Moderate()), S(y = Moderate()) }, () => new[] { S(x * y) });
        Case("div", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(x / y) });
        Case("div_moderate", () => new[] { S(x = Moderate()), S(y = Moderate()) }, () => new[] { S(x / y) });
        Case("mod", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(x % y) });
        Case("shl", () => new[] { S(x = F()), S(i = Int(-70, 70)) }, () => new[] { S(x << i) });
        Case("shr", () => new[] { S(x = F()), S(i = Int(-70, 70)) }, () => new[] { S(x >> i) });
        Case("cmp", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(x.CompareTo(y)) });
        Case("abs", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Abs(x)) });
        Case("floor", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Floor(x)) });
        Case("ceiling", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Ceiling(x)) });
        Case("round", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Round(x)) });
        Case("frac", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Frac(x)) });
        Case("sign", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Sign(x)) });
        Case("intsign", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.IntSign(x)) });
        Case("intvalue", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.IntValue(x)) });
        Case("longvalue", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.LongValue(x)) });
        Case("min", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(Fixed64.Min(x, y)) });
        Case("max", () => new[] { S(x = F()), S(y = F()) }, () => new[] { S(Fixed64.Max(x, y)) });
        Case("clamp", () => new[] { S(x = F()), S(y = F()), S(z = F()) }, () => new[] { S(Fixed64.Clamp(x, y, z)) });
        Case("clamp01", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Clamp01(x)) });
        for (int k = 0; k < N; k++)
        {
            int v = (Next() % 2 == 0) ? (int)(uint)Next() : Int(-2000, 2000), lo = Int(-1000, 0), hi = Int(0, 1000);
            Emit("clampint", new[] { S(v), S(lo), S(hi) }, new[] { S(Fixed64.Clamp(v, lo, hi)) });
        }
        Case("repeat", () => new[] { S(x = Moderate()), S(y = Moderate()) }, () => new[] { S(Fixed64.Repeat(x, y)) });
        Case("repeat3", () => new[] { S(x = Moderate()), S(y = Moderate()), S(z = Moderate()) }, () => new[] { S(Fixed64.Repeat(x, y, z)) });
        Case("lerp", () => new[] { S(x = Moderate()), S(y = Moderate()), S(z = Raw(Range(-(1L << 33), 1L << 33))) }, () => new[] { S(Fixed64.Lerp(x, y, z)) });
        Case("inverselerp", () => new[] { S(x = Moderate()), S(y = Moderate()), S(z = Moderate()) }, () => new[] { S(Fixed64.InverseLerp(x, y, z)) });
        Case("approximately", () => new[] { S(x = Moderate()), S(y = (Next() % 2 == 0) ? x + Raw(Range(-8000000, 8000000)) : Moderate()) }, () => new[] { S(Fixed64.Approximately(x, y)) });
        Case("bigenoughsquared", () => new[] { S(x = Raw(Range(-100000, 100000))) }, () => new[] { S(Fixed64.BigEnoughSquared(x)) });
        Case("isinfinity", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.IsInfinity(x)) });
        Case("sqrt", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Sqrt(x)) });
        Case("exp", () => new[] { S(x = Raw(Range(-(30L << 32), 30L << 32))) }, () => new[] { S(Fixed64.Exp(x)) });
        Case("log", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Log(x)) });
        Case("exp2", () => new[] { S(x = Exponent()) }, () => new[] { S(Fixed64.Exp2(x)) });
        Case("log2", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.Log2(x)) });
        Case("pow", () => new[] { S(x = Raw(Range(-(1L << 36), 1L << 36))), S(y = Raw(Range(-(8L << 32), 8L << 32))) }, () => new[] { S(Fixed64.Pow(x, y)) });
        Case("powint", () => new[] { S(x = Raw(Range(-(1L << 36), 1L << 36))), S(i = SmallInt()) }, () => new[] { S(Fixed64.Pow(x, i)) });
        Case("sin", () => new[] { S(x = Angle()) }, () => new[] { S(Fixed64.Sin(x)) });
        Case("cos", () => new[] { S(x = Angle()) }, () => new[] { S(Fixed64.Cos(x)) });
        Case("tan", () => new[] { S(x = Angle()) }, () => new[] { S(Fixed64.Tan(x)) });
        Case("sincos", () => new[] { S(x = (Next() % 5 == 0) ? F() : Angle()) }, () => { Fixed64.SinCos(x, out Fixed64 s, out Fixed64 c); return new[] { S(s), S(c) }; });
        Case("atan2", () => new[] { S(x = (Next() % 5 == 0) ? F() : Moderate()), S(y = (Next() % 5 == 0) ? F() : Moderate()) }, () => new[] { S(Fixed64.Atan2(x, y)) });
        Case("pointoncircle", () => new[] { S(x = Angle()), S(y = Moderate()) }, () => { Fixed64.PointOnCircle(x, y, out Fixed64 px, out Fixed64 py); return new[] { S(px), S(py) }; });
        Case("fromint", () => new[] { S(i = (int)(uint)Next()) }, () => new[] { S(Fixed64.FromInt(i)) });
        for (int k = 0; k < N; k++)
        {
            long l = (Next() % 3 == 0) ? (long)Next() : Range(-(1L << 40), 1L << 40);
            Emit("fromlong", new[] { S(l) }, new[] { S(Fixed64.FromLong(l)) });
        }
        Case("fromrational", () => new[] { S(i = (int)(uint)Next() % 100000), S(j = Int(-1000, 1000)) }, () => new[] { S(Fixed64.FromRational(i, j)) });
        Case("multiplyint", () => new[] { S(x = F()), S(i = (Next() % 2 == 0) ? (int)(uint)Next() : Int(-100, 100)) }, () => new[] { S(Fixed64.Multiply(x, i)) });
        Case("divideint", () => new[] { S(x = F()), S(i = (Next() % 2 == 0) ? (int)(uint)Next() : Int(-100, 100)) }, () => new[] { S(Fixed64.Divide(x, i)) });
        Case("fromscientific", () => new[] { S(i = Int(-100000, 100000)), S(j = Int(-9, 4)) }, () => new[] { S(Fixed64.FromScientificNotation(i, j)) });
        Case("fromdouble", () => new[] { S(d = RandomDouble()) }, () => new[] { S(Fixed64.UnsafeFromDouble(d)) });
        Case("fromfloat", () => new[] { S(f = RandomFloat()) }, () => new[] { S(Fixed64.UnsafeFromFloat(f)) });
        Case("fromfloattrunc", () => new[] { S(f = RandomFloat()), S(g = new[] { 0.5f, 0.25f, 0.1f, 0.05f, 0.01f, 0.001f, 1f, 2f, 0.3f }[Next() % 9]) }, () => new[] { S(Fixed64.UnsafeFromFloat(f, g)) });
        Case("floatvalue", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.UnsafeFloatValue(x)) });
        Case("doublevalue", () => new[] { S(x = F()) }, () => new[] { S(Fixed64.UnsafeDoubleValue(x)) });
        Case("checksum", () => new[] { S(x = F()) }, () => new[] { S(x.GetChecksum()) });
        Case("intabs", () => new[] { S(i = (Next() % 4 == 0) ? int.MinValue : (int)(uint)Next()) }, () => new[] { S(Fixed64.IntAbs(i)) });
        Case("factorial", () => new[] { S(i = Int(-2, 40)) }, () => new[] { S(Fixed64.Factorial(i)) });
        for (int k = 0; k < N; k++)
        {
            ulong v = (Next() % 2 == 0) ? 1ul << Int(0, 63) : Next();
            Emit("ispoweroftwo", new[] { v.ToString(CultureInfo.InvariantCulture) }, new[] { S(Fixed64.IsPowerOfTwo(v)) });
        }
        for (int k = 0; k < N; k++)
        {
            int start = (int)(uint)Next(), a = (int)(uint)Next(), b = (int)(uint)Next();
            var c = new Checksum(start);
            c.Add(a);
            c.Add(b);
            Emit("checksumadd", new[] { S(start), S(a), S(b) }, new[] { S(c.Sum) });
        }
        for (int k = 0; k < N; k++)
        {
            int a = (int)(uint)Next(), b = (int)(uint)Next(), c = (int)(uint)Next(), e = (int)(uint)Next(), h = (int)(uint)Next();
            Emit("combine2", new[] { S(a), S(b) }, new[] { S(Checksum.Combine(a, b)) });
            Emit("combine5", new[] { S(a), S(b), S(c), S(e), S(h) }, new[] { S(Checksum.Combine(a, b, c, e, h)) });
        }

        // Edge cases, exhaustively rather than by chance: every special value, whole numbers and
        // exact halves around them (Round's halves-to-even, Floor, Ceiling, Frac), and every pair
        // of special values for the binary operations (their overflow and saturation branches).
        var edges = new List<long>(sSpecial);
        for (long k = -6; k <= 6; k++)
            foreach (long d0 in new[] { 0L, 1L, -1L, 2147483648L, -2147483648L, 2147483647L, 2147483649L })
                edges.Add(k * 4294967296L + d0);
        var unary = new (string, Func<Fixed64, string[]>)[]
        {
            ("edge_round", v => new[] { S(Fixed64.Round(v)) }), ("edge_floor", v => new[] { S(Fixed64.Floor(v)) }),
            ("edge_ceiling", v => new[] { S(Fixed64.Ceiling(v)) }), ("edge_frac", v => new[] { S(Fixed64.Frac(v)) }),
            ("edge_abs", v => new[] { S(Fixed64.Abs(v)) }), ("edge_neg", v => new[] { S(-v) }),
            ("edge_sign", v => new[] { S(Fixed64.Sign(v)) }), ("edge_intsign", v => new[] { S(Fixed64.IntSign(v)) }),
            ("edge_intvalue", v => new[] { S(Fixed64.IntValue(v)) }), ("edge_longvalue", v => new[] { S(Fixed64.LongValue(v)) }),
            ("edge_sqrt", v => new[] { S(Fixed64.Sqrt(v)) }), ("edge_log2", v => new[] { S(Fixed64.Log2(v)) }),
            ("edge_exp2", v => new[] { S(Fixed64.Exp2(v)) }), ("edge_sin", v => new[] { S(Fixed64.Sin(v)) }),
            ("edge_cos", v => new[] { S(Fixed64.Cos(v)) }), ("edge_checksum", v => new[] { S(v.GetChecksum()) }),
            ("edge_isinfinity", v => new[] { S(Fixed64.IsInfinity(v)) }), ("edge_floatvalue", v => new[] { S(Fixed64.UnsafeFloatValue(v)) }),
        };
        foreach (var (op, fn) in unary)
            foreach (long e in edges)
            {
                string[] r;
                try { r = fn(Raw(e)); } catch (ArithmeticException) { continue; }
                Emit(op, new[] { S(e) }, r);
            }
        var binary = new (string, Func<Fixed64, Fixed64, string[]>)[]
        {
            ("edge_add", (a, b) => new[] { S(a + b) }), ("edge_sub", (a, b) => new[] { S(a - b) }),
            ("edge_mul", (a, b) => new[] { S(a * b) }), ("edge_div", (a, b) => new[] { S(a / b) }),
            ("edge_mod", (a, b) => new[] { S(a % b) }), ("edge_cmp", (a, b) => new[] { S(a.CompareTo(b)) }),
            ("edge_atan2", (a, b) => new[] { S(Fixed64.Atan2(a, b)) }),
        };
        foreach (var (op, fn) in binary)
            foreach (long a in sSpecial)
                foreach (long b in sSpecial)
                {
                    string[] r;
                    try { r = fn(Raw(a), Raw(b)); } catch (ArithmeticException) { continue; }
                    Emit(op, new[] { S(a), S(b) }, r);
                }
        foreach (double e in new[] { 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49999999999999994, 2147483647.5, -2147483648.5, 1e-10, -1e-10, double.Epsilon })
            Emit("edge_fromdouble", new[] { S(e) }, new[] { S(Fixed64.UnsafeFromDouble(e)) });
        foreach (float e in new[] { 0.25f, 0.75f, 1.25f, -0.25f, 2.5f, -2.5f, 0.125f, 0.375f })
            foreach (float t in new[] { 0.5f, 0.25f, 0.1f })
                Emit("edge_fromfloattrunc", new[] { S(e), S(t) }, new[] { S(Fixed64.UnsafeFromFloat(e, t)) });

        File.WriteAllText(argv[0], "# Outpost.Frontier test vectors: the original's Fixed64 and Checksum (Tools/Vectors)\n" + sOut, new UTF8Encoding(false));
        Console.WriteLine($"{sCounts.Values.Sum()} vectors, {sCounts.Count} operations -> {argv[0]}");
        return 0;
    }
}
