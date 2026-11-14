// Core::Fixed64 and Core::Checksum against the original: every vector in Vectors/Fixed64.tsv was
// computed by the original's own C# (Tools/Vectors), and each must come out bit for bit the same.
#include "Test.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Checksum.h"
#include "Core/Fixed64.h"

using Core::Checksum;
using Core::Fixed64;

namespace
{
    std::filesystem::path VectorFile()
    {
        return std::filesystem::path(std::source_location::current().file_name()).parent_path() / "Vectors" / "Fixed64.tsv";
    }

    std::int64_t I64(std::string_view _text)
    {
        std::int64_t value = 0;
        std::from_chars(_text.data(), _text.data() + _text.size(), value);
        return value;
    }

    std::uint64_t U64(std::string_view _text)
    {
        std::uint64_t value = 0;
        std::from_chars(_text.data(), _text.data() + _text.size(), value);
        return value;
    }

    std::int32_t I32(std::string_view _text) { return static_cast<std::int32_t>(I64(_text)); }
    Fixed64 F(std::string_view _text) { return Fixed64::FromRawValue(I64(_text)); }

    std::uint64_t Hex(std::string_view _text)
    {
        std::uint64_t value = 0;
        std::from_chars(_text.data(), _text.data() + _text.size(), value, 16);
        return value;
    }

    double D(std::string_view _text) { return std::bit_cast<double>(Hex(_text)); }
    float Fl(std::string_view _text) { return std::bit_cast<float>(static_cast<std::uint32_t>(Hex(_text))); }

    std::string Str(Fixed64 _f) { return std::to_string(_f.RawValue()); }
    std::string Str(std::int64_t _i) { return std::to_string(_i); }
    std::string Str(std::int32_t _i) { return std::to_string(_i); }
    std::string Str(bool _b) { return _b ? "1" : "0"; }
    std::string Str(double _d) { return std::format("{:016x}", std::bit_cast<std::uint64_t>(_d)); }
    std::string Str(float _f) { return std::format("{:08x}", std::bit_cast<std::uint32_t>(_f)); }

    using Args = std::vector<std::string_view>;
    using Results = std::vector<std::string>;

    const std::map<std::string_view, Fixed64>& Constants()
    {
        static const std::map<std::string_view, Fixed64> constants = {
            {"Epsilon", Fixed64::EPSILON}, {"InfinitesimalTreshhold", Fixed64::INFINITESIMAL_TRESHHOLD},
            {"PositiveInfinity", Fixed64::POSITIVE_INFINITY}, {"NegativeInfinity", Fixed64::NEGATIVE_INFINITY},
            {"MaxValue", Fixed64::MAX_VALUE}, {"MinValue", Fixed64::MIN_VALUE}, {"Zero", Fixed64::ZERO},
            {"OneFourth", Fixed64::ONE_FOURTH}, {"Half", Fixed64::HALF}, {"NegativeOne", Fixed64::NEGATIVE_ONE},
            {"One", Fixed64::ONE}, {"Two", Fixed64::TWO}, {"SqrtTwo", Fixed64::SQRT_TWO},
            {"OneOverSqrtTwo", Fixed64::ONE_OVER_SQRT_TWO}, {"Four", Fixed64::FOUR},
            {"OneHundredth", Fixed64::ONE_HUNDREDTH}, {"OneHundred", Fixed64::ONE_HUNDRED},
            {"OneThousandth", Fixed64::ONE_THOUSANDTH}, {"OneThousand", Fixed64::ONE_THOUSAND}, {"PI", Fixed64::PI},
            {"TwoPI", Fixed64::TWO_PI}, {"HalfPI", Fixed64::HALF_PI}, {"PIOverFour", Fixed64::PI_OVER_FOUR},
            {"RadiansToDegrees", Fixed64::RADIANS_TO_DEGREES}, {"DegreesToRadians", Fixed64::DEGREES_TO_RADIANS},
            {"Log2E", Fixed64::LOG2_E}, {"LN2", Fixed64::LN2}, {"MaxExponentOfTwo", Fixed64::MAX_EXPONENT_OF_TWO},
            {"MinExponentOfTwo", Fixed64::MIN_EXPONENT_OF_TWO}, {"SqrtMaxValue", Fixed64::SQRT_MAX_VALUE},
            {"LutInterval", Fixed64::LUT_INTERVAL}, {"WorkingPrecision", Fixed64::WORKING_PRECISION},
            {"WorkingPrecisionSq", Fixed64::WORKING_PRECISION_SQ},
            {"OneOverWorkingPrecision", Fixed64::ONE_OVER_WORKING_PRECISION},
            {"kQuickAtanCoeficient", Fixed64::QUICK_ATAN_COEFFICIENT},
        };
        return constants;
    }

    // op -> computes the results from the arguments, exactly as Tools/Vectors/Program.cs does.
    const std::map<std::string_view, std::function<Results(const Args&)>>& Operations()
    {
        using R = Results;
        static const std::map<std::string_view, std::function<Results(const Args&)>> ops = {
            {"const", [](const Args& a) { auto it = Constants().find(a[0]); return it == Constants().end() ? R{"unknown"} : R{Str(it->second)}; }},
            {"add", [](const Args& a) { return R{Str(F(a[0]) + F(a[1]))}; }},
            {"sub", [](const Args& a) { return R{Str(F(a[0]) - F(a[1]))}; }},
            {"neg", [](const Args& a) { return R{Str(-F(a[0]))}; }},
            {"mul", [](const Args& a) { return R{Str(F(a[0]) * F(a[1]))}; }},
            {"mul_moderate", [](const Args& a) { return R{Str(F(a[0]) * F(a[1]))}; }},
            {"div", [](const Args& a) { return R{Str(F(a[0]) / F(a[1]))}; }},
            {"div_moderate", [](const Args& a) { return R{Str(F(a[0]) / F(a[1]))}; }},
            {"mod", [](const Args& a) { return R{Str(F(a[0]) % F(a[1]))}; }},
            {"shl", [](const Args& a) { return R{Str(F(a[0]) << I32(a[1]))}; }},
            {"shr", [](const Args& a) { return R{Str(F(a[0]) >> I32(a[1]))}; }},
            {"cmp", [](const Args& a) { return R{Str(static_cast<std::int32_t>(F(a[0]).CompareTo(F(a[1]))))}; }},
            {"abs", [](const Args& a) { return R{Str(Fixed64::Abs(F(a[0])))}; }},
            {"floor", [](const Args& a) { return R{Str(Fixed64::Floor(F(a[0])))}; }},
            {"ceiling", [](const Args& a) { return R{Str(Fixed64::Ceiling(F(a[0])))}; }},
            {"round", [](const Args& a) { return R{Str(Fixed64::Round(F(a[0])))}; }},
            {"frac", [](const Args& a) { return R{Str(Fixed64::Frac(F(a[0])))}; }},
            {"sign", [](const Args& a) { return R{Str(Fixed64::Sign(F(a[0])))}; }},
            {"intsign", [](const Args& a) { return R{Str(Fixed64::IntSign(F(a[0])))}; }},
            {"intvalue", [](const Args& a) { return R{Str(Fixed64::IntValue(F(a[0])))}; }},
            {"longvalue", [](const Args& a) { return R{Str(Fixed64::LongValue(F(a[0])))}; }},
            {"min", [](const Args& a) { return R{Str(Fixed64::Min(F(a[0]), F(a[1])))}; }},
            {"max", [](const Args& a) { return R{Str(Fixed64::Max(F(a[0]), F(a[1])))}; }},
            {"clamp", [](const Args& a) { return R{Str(Fixed64::Clamp(F(a[0]), F(a[1]), F(a[2])))}; }},
            {"clamp01", [](const Args& a) { return R{Str(Fixed64::Clamp01(F(a[0])))}; }},
            {"clampint", [](const Args& a) { return R{Str(Fixed64::Clamp(I32(a[0]), I32(a[1]), I32(a[2])))}; }},
            {"repeat", [](const Args& a) { return R{Str(Fixed64::Repeat(F(a[0]), F(a[1])))}; }},
            {"repeat3", [](const Args& a) { return R{Str(Fixed64::Repeat(F(a[0]), F(a[1]), F(a[2])))}; }},
            {"lerp", [](const Args& a) { return R{Str(Fixed64::Lerp(F(a[0]), F(a[1]), F(a[2])))}; }},
            {"inverselerp", [](const Args& a) { return R{Str(Fixed64::InverseLerp(F(a[0]), F(a[1]), F(a[2])))}; }},
            {"approximately", [](const Args& a) { return R{Str(Fixed64::Approximately(F(a[0]), F(a[1])))}; }},
            {"bigenoughsquared", [](const Args& a) { return R{Str(Fixed64::BigEnoughSquared(F(a[0])))}; }},
            {"isinfinity", [](const Args& a) { return R{Str(Fixed64::IsInfinity(F(a[0])))}; }},
            {"sqrt", [](const Args& a) { return R{Str(Fixed64::Sqrt(F(a[0])))}; }},
            {"exp", [](const Args& a) { return R{Str(Fixed64::Exp(F(a[0])))}; }},
            {"log", [](const Args& a) { return R{Str(Fixed64::Log(F(a[0])))}; }},
            {"exp2", [](const Args& a) { return R{Str(Fixed64::Exp2(F(a[0])))}; }},
            {"log2", [](const Args& a) { return R{Str(Fixed64::Log2(F(a[0])))}; }},
            {"pow", [](const Args& a) { return R{Str(Fixed64::Pow(F(a[0]), F(a[1])))}; }},
            {"powint", [](const Args& a) { return R{Str(Fixed64::Pow(F(a[0]), I32(a[1])))}; }},
            {"sin", [](const Args& a) { return R{Str(Fixed64::Sin(F(a[0])))}; }},
            {"cos", [](const Args& a) { return R{Str(Fixed64::Cos(F(a[0])))}; }},
            {"tan", [](const Args& a) { return R{Str(Fixed64::Tan(F(a[0])))}; }},
            {"sincos", [](const Args& a) { Fixed64 s, c; Fixed64::SinCos(F(a[0]), s, c); return R{Str(s), Str(c)}; }},
            {"atan2", [](const Args& a) { return R{Str(Fixed64::Atan2(F(a[0]), F(a[1])))}; }},
            {"pointoncircle", [](const Args& a) { Fixed64 x, y; Fixed64::PointOnCircle(F(a[0]), F(a[1]), x, y); return R{Str(x), Str(y)}; }},
            {"fromint", [](const Args& a) { return R{Str(Fixed64::FromInt(I32(a[0])))}; }},
            {"fromlong", [](const Args& a) { return R{Str(Fixed64::FromLong(I64(a[0])))}; }},
            {"fromrational", [](const Args& a) { return R{Str(Fixed64::FromRational(I32(a[0]), I32(a[1])))}; }},
            {"multiplyint", [](const Args& a) { return R{Str(Fixed64::Multiply(F(a[0]), I32(a[1])))}; }},
            {"divideint", [](const Args& a) { return R{Str(Fixed64::Divide(F(a[0]), I32(a[1])))}; }},
            {"fromscientific", [](const Args& a) { return R{Str(Fixed64::FromScientificNotation(I32(a[0]), I32(a[1])))}; }},
            {"fromdouble", [](const Args& a) { return R{Str(Fixed64::UnsafeFromDouble(D(a[0])))}; }},
            {"fromfloat", [](const Args& a) { return R{Str(Fixed64::UnsafeFromFloat(Fl(a[0])))}; }},
            {"fromfloattrunc", [](const Args& a) { return R{Str(Fixed64::UnsafeFromFloat(Fl(a[0]), Fl(a[1])))}; }},
            {"floatvalue", [](const Args& a) { return R{Str(Fixed64::UnsafeFloatValue(F(a[0])))}; }},
            {"doublevalue", [](const Args& a) { return R{Str(Fixed64::UnsafeDoubleValue(F(a[0])))}; }},
            {"checksum", [](const Args& a) { return R{Str(F(a[0]).GetChecksum())}; }},
            {"intabs", [](const Args& a) { return R{Str(Fixed64::IntAbs(I32(a[0])))}; }},
            {"factorial", [](const Args& a) { return R{Str(Fixed64::Factorial(I32(a[0])))}; }},
            {"ispoweroftwo", [](const Args& a) { return R{Str(Fixed64::IsPowerOfTwo(U64(a[0])))}; }},
            {"checksumadd", [](const Args& a) { Checksum c(I32(a[0])); c.Add(I32(a[1])); c.Add(I32(a[2])); return R{Str(c.Sum())}; }},
            {"combine2", [](const Args& a) { return R{Str(Checksum::Combine(I32(a[0]), I32(a[1])))}; }},
            {"combine5", [](const Args& a) { return R{Str(Checksum::Combine(I32(a[0]), I32(a[1]), I32(a[2]), I32(a[3]), I32(a[4])))}; }},
        };
        return ops;
    }

    std::vector<std::string_view> Split(std::string_view _line)
    {
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (true)
        {
            const std::size_t tab = _line.find('\t', start);
            parts.push_back(_line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
            if (tab == std::string_view::npos)
                return parts;
            start = tab + 1;
        }
    }
}

TEST(Fixed64MatchesTheOriginal)
{
    std::ifstream file(VectorFile());
    CHECK(file.good());
    std::map<std::string, int> checked;
    std::map<std::string, int> failed;
    std::string line;
    int unknown = 0;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();   // a checkout with CRLF line endings
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string_view> parts = Split(line);
        const auto arrow = std::find(parts.begin(), parts.end(), std::string_view("=>"));
        if (parts.empty() || arrow == parts.end())
            continue;
        const std::string op(parts[0]);
        // "edge_round" is "round" over the generator's exhaustive edge cases.
        const std::string_view base = parts[0].starts_with("edge_") ? parts[0].substr(5) : parts[0];
        const auto it = Operations().find(base);
        if (it == Operations().end())
        {
            ++unknown;
            continue;
        }
        const Args args(parts.begin() + 1, arrow);
        const Results expected(arrow + 1, parts.end());
        const Results actual = it->second(args);
        ++checked[op];
        if (actual != expected)
        {
            if (++failed[op] <= 3)
            {
                std::string in;
                for (std::string_view a : args)
                    in += std::string(a) + " ";
                std::string want;
                for (const std::string& e : expected)
                    want += e + " ";
                std::string got;
                for (const std::string& r : actual)
                    got += r + " ";
                std::println("    {}({}): original {}, C++ {}", op, in, want, got);
            }
        }
    }
    int total = 0;
    for (const auto& [op, count] : checked)
        total += count;
    int failures = 0;
    for (const auto& [op, count] : failed)
    {
        failures += count;
        std::println("    {}: {} of {} differ", op, count, checked[op]);
    }
    std::println("    {} vectors in {} operations checked, {} differ", total, checked.size(), failures);
    CHECK(unknown == 0);
    CHECK(total > 18000);
    CHECK(failures == 0);
}
