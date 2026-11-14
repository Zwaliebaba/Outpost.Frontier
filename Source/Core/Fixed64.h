// Fixed64: the port of BBI.Core.Utility.FixedPoint.Fixed64, the simulation's number (Q32.32 in a
// 64-bit integer). Ported line by line from the decompiled C#, because the simulation is checked
// bit for bit against the original (Design/Verification.md §1); tests: Source/Tests/Fixed64Tests.cpp
// against vectors computed by the original code (Tools/Vectors).
//
// Kept from the original, deliberately: its special "infinities" (MaxValue and -MaxValue raw), its
// saturating +, its multiplication's overflow rules, its division algorithm, its lookup tables, and
// the names of its members (PascalCase functions, UPPER_CASE constants: CONTRIBUTING.md).
// Left out: the decimal conversions (unused by the simulation) and ToString.
// Preconditions the original enforced by throwing (DivideByZeroException): integer division by
// zero in FromRational, Divide and %; they are asserted here.
#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>

#include "Core/Arithmetic.h"

namespace Core
{
    class Fixed64
    {
    public:
        // --- constants (defined below the class, in the original's order) ---------------------

        static const Fixed64 EPSILON;
        static const Fixed64 INFINITESIMAL_TRESHHOLD;   // sic, as the original
        static const Fixed64 POSITIVE_INFINITY;
        static const Fixed64 NEGATIVE_INFINITY;
        static const Fixed64 MAX_VALUE;
        static const Fixed64 MIN_VALUE;
        static const Fixed64 ZERO;
        static const Fixed64 ONE_FOURTH;
        static const Fixed64 HALF;
        static const Fixed64 NEGATIVE_ONE;
        static const Fixed64 ONE;
        static const Fixed64 TWO;
        static const Fixed64 SQRT_TWO;
        static const Fixed64 ONE_OVER_SQRT_TWO;
        static const Fixed64 FOUR;
        static const Fixed64 ONE_HUNDREDTH;
        static const Fixed64 ONE_HUNDRED;
        static const Fixed64 ONE_THOUSANDTH;
        static const Fixed64 ONE_THOUSAND;
        static const Fixed64 PI;
        static const Fixed64 TWO_PI;
        static const Fixed64 HALF_PI;
        static const Fixed64 PI_OVER_FOUR;
        static const Fixed64 RADIANS_TO_DEGREES;
        static const Fixed64 DEGREES_TO_RADIANS;
        static const Fixed64 LOG2_E;
        static const Fixed64 LN2;
        static const Fixed64 MAX_EXPONENT_OF_TWO;
        static const Fixed64 MIN_EXPONENT_OF_TWO;
        static const Fixed64 SQRT_MAX_VALUE;
        static const Fixed64 LUT_INTERVAL;
        static const Fixed64 WORKING_PRECISION;
        static const Fixed64 WORKING_PRECISION_SQ;
        static const Fixed64 ONE_OVER_WORKING_PRECISION;
        static const Fixed64 QUICK_ATAN_COEFFICIENT;

        static constexpr int TRIG_LOOKUP_SIZE = 16;
        static constexpr int EXP_LOOKUP_SIZE = 56;

        static constexpr std::array<std::int64_t, EXP_LOOKUP_SIZE> EXP2_LUT = {
            4294967296LL, 4349437894LL, 4404599311LL, 4460460309LL, 4517029759LL, 4574316648LL, 4632330073LL,
            4691079249LL, 4750573506LL, 4810822296LL, 4871835186LL, 4933621867LL, 4996192154LL, 5059555983LL,
            5123723419LL, 5188704654LL, 5254510008LL, 5321149934LL, 5388635016LL, 5456975972LL, 5526183657LL,
            5596269064LL, 5667243323LL, 5739117708LL, 5811903634LL, 5885612663LL, 5960256501LL, 6035847003LL,
            6112396176LL, 6189916179LL, 6268419323LL, 6347918077LL, 6428425068LL, 6509953083LL, 6592515070LL,
            6676124144LL, 6760793584LL, 6846536838LL, 6933367523LL, 7021299433LL, 7110346532LL, 7200522964LL,
            7291843052LL, 7384321301LL, 7477972397LL, 7572811217LL, 7668852823LL, 7766112469LL, 7864605603LL,
            7964347869LL, 8065355108LL, 8167643365LL, 8271228884LL, 8376128119LL, 8482357731LL, 8589934592LL};

        static constexpr std::array<std::int64_t, EXP_LOOKUP_SIZE> LOG2_LUT = {
            0LL, 111648570LL, 221320956LL, 329085898LL, 435008615LL, 539151033LL, 641572009LL, 742327531LL,
            841470894LL, 939052879LL, 1035121905LL, 1129724170LL, 1222903791LL, 1314702922LL, 1405161872LL,
            1494319209LL, 1582211860LL, 1668875203LL, 1754343152LL, 1838648235LL, 1921821672LL, 2003893442LL,
            2084892347LL, 2164846076LL, 2243781258LL, 2321723519LL, 2398697527LL, 2474727046LL, 2549834972LL,
            2624043380LL, 2697373561LL, 2769846060LL, 2841480708LL, 2912296656LL, 2982312407LL, 3051545843LL,
            3120014252LL, 3187734358LL, 3254722341LL, 3320993860LL, 3386564081LL, 3451447689LL, 3515658917LL,
            3579211557LL, 3642118980LL, 3704394158LL, 3766049672LL, 3827097732LL, 3887550192LL, 3947418562LL,
            4006714019LL, 4065447426LL, 4123629337LL, 4181270013LL, 4238379432LL, 4294967296LL};

        static constexpr std::array<std::int64_t, TRIG_LOOKUP_SIZE> SIN_LUT = {
            0LL, 448946331LL, 892973912LL, 1327217884LL, 1746920580LL, 2147483647LL, 2524518435LL, 2873894071LL,
            3191782721LL, 3474701532LL, 3719550786LL, 3923647863LL, 4084756634LL, 4201111955LL, 4271439015LL,
            4294967296LL};

        // --- construction and conversion ---------------------------------------------------------

        constexpr Fixed64() noexcept = default;

        [[nodiscard]] constexpr std::int64_t RawValue() const noexcept { return m_raw; }

        [[nodiscard]] static constexpr Fixed64 FromRawValue(std::int64_t _raw) noexcept { return Fixed64(_raw); }
        [[nodiscard]] static constexpr Fixed64 FromInt(std::int32_t _value) noexcept { return Fixed64(WrapMul<std::int64_t>(_value, ONE_RAW)); }
        [[nodiscard]] static constexpr Fixed64 FromLong(std::int64_t _value) noexcept { return Fixed64(WrapMul(_value, ONE_RAW)); }

        [[nodiscard]] static constexpr Fixed64 FromRational(std::int32_t _numerator, std::int32_t _denominator) noexcept
        {
            assert(_denominator != 0);
            return Fixed64(WrapMul<std::int64_t>(_numerator, ONE_RAW) / _denominator);
        }

        [[nodiscard]] static constexpr Fixed64 Multiply(Fixed64 _number, std::int32_t _multiplicator) noexcept
        {
            return Fixed64(WrapMul<std::int64_t>(_number.m_raw, _multiplicator));
        }

        [[nodiscard]] static constexpr Fixed64 Divide(Fixed64 _number, std::int32_t _divisor) noexcept
        {
            assert(_divisor != 0);
            // long.MinValue / -1 overflows in C# too (it throws); it cannot occur with an int divisor
            // except -1, which C# would turn into an OverflowException. Wrap it like negation.
            if (_divisor == -1)
                return Fixed64(WrapNeg(_number.m_raw));
            return Fixed64(_number.m_raw / _divisor);
        }

        [[nodiscard]] static constexpr Fixed64 FromScientificNotation(std::int32_t _coefficient, std::int32_t _exponent) noexcept;
        [[nodiscard]] static constexpr Fixed64 Tenth(std::int32_t _value) noexcept { return FromRational(_value, 10); }
        [[nodiscard]] static constexpr Fixed64 Hundredth(std::int32_t _value) noexcept { return FromRational(_value, 100); }
        [[nodiscard]] static constexpr Fixed64 Thousandth(std::int32_t _value) noexcept { return FromRational(_value, 1000); }

        [[nodiscard]] static constexpr std::int64_t LongValue(Fixed64 _number) noexcept { return _number.m_raw / ONE_RAW; }
        [[nodiscard]] static constexpr std::int32_t IntValue(Fixed64 _number) noexcept { return static_cast<std::int32_t>(LongValue(_number)); }

        [[nodiscard]] static constexpr Fixed64 Frac(Fixed64 _number) noexcept
        {
            return Fixed64(_number.m_raw >= 0 ? (_number.m_raw & LOW_MASK) : WrapNeg(WrapNeg(_number.m_raw) & LOW_MASK));
        }

        // Authored data enters the simulation through these, once, at load (Design/Inventory.md §2).
        [[nodiscard]] static constexpr Fixed64 UnsafeFromDouble(double _value) noexcept
        {
            std::int64_t raw = DoubleToInt64(_value * 4294967296.0);
            if (_value > 2147483647.0)
                raw = std::numeric_limits<std::int64_t>::max();
            return Fixed64(raw);
        }

        [[nodiscard]] static constexpr Fixed64 UnsafeFromFloat(float _value) noexcept { return UnsafeFromDouble(_value); }
        [[nodiscard]] static constexpr Fixed64 FromConstFloat(float _value) noexcept { return UnsafeFromDouble(_value); }

        [[nodiscard]] static Fixed64 UnsafeFromFloat(float _value, float _truncateToFraction) noexcept
        {
            if (_truncateToFraction >= 1.0f)
                return UnsafeFromFloat(_value);
            const double whole = RoundHalfEven(static_cast<double>(_value));
            const double fraction = static_cast<double>(_value) - whole;
            const std::int32_t steps = DoubleToInt32(RoundHalfEven(fraction / static_cast<double>(_truncateToFraction)));
            return UnsafeFromDouble(whole + static_cast<double>(_truncateToFraction * static_cast<float>(steps)));
        }

        [[nodiscard]] static constexpr float UnsafeFloatValue(Fixed64 _number) noexcept
        {
            return static_cast<float>(_number.m_raw) / 4294967296.0f;
        }

        [[nodiscard]] static constexpr double UnsafeDoubleValue(Fixed64 _number) noexcept
        {
            return static_cast<double>(_number.m_raw) / 4294967296.0;
        }

        // --- comparison and hashing ----------------------------------------------------------------

        [[nodiscard]] constexpr int CompareTo(Fixed64 _value) const noexcept;

        [[nodiscard]] friend constexpr bool operator==(Fixed64 _x, Fixed64 _y) noexcept { return _x.m_raw == _y.m_raw; }
        [[nodiscard]] friend constexpr bool operator<(Fixed64 _x, Fixed64 _y) noexcept { return _x.m_raw < _y.m_raw; }
        [[nodiscard]] friend constexpr bool operator>(Fixed64 _x, Fixed64 _y) noexcept { return _x.m_raw > _y.m_raw; }
        [[nodiscard]] friend constexpr bool operator<=(Fixed64 _x, Fixed64 _y) noexcept { return _x.m_raw <= _y.m_raw; }
        [[nodiscard]] friend constexpr bool operator>=(Fixed64 _x, Fixed64 _y) noexcept { return _x.m_raw >= _y.m_raw; }

        [[nodiscard]] constexpr std::int32_t GetHashCode() const noexcept { return HashInt64(m_raw); }
        [[nodiscard]] constexpr std::int32_t GetChecksum() const noexcept { return HashInt64(m_raw); }

        // --- arithmetic --------------------------------------------------------------------------

        [[nodiscard]] friend constexpr Fixed64 operator+(Fixed64 _x, Fixed64 _y) noexcept;
        [[nodiscard]] friend constexpr Fixed64 operator-(Fixed64 _x) noexcept { return Fixed64(WrapNeg(_x.m_raw)); }
        [[nodiscard]] friend constexpr Fixed64 operator-(Fixed64 _x, Fixed64 _y) noexcept { return _x + -_y; }
        [[nodiscard]] friend constexpr Fixed64 operator*(Fixed64 _x, Fixed64 _y) noexcept;
        [[nodiscard]] friend constexpr Fixed64 operator/(Fixed64 _x, Fixed64 _y) noexcept;

        [[nodiscard]] friend constexpr Fixed64 operator%(Fixed64 _x, Fixed64 _y) noexcept
        {
            assert(_y.m_raw != 0);
            if (_x.m_raw == std::numeric_limits<std::int64_t>::min() && _y.m_raw == -1)
                return Fixed64(0);
            return Fixed64(_x.m_raw % _y.m_raw);
        }

        [[nodiscard]] friend constexpr Fixed64 operator<<(Fixed64 _x, int _count) noexcept { return Fixed64(ShiftLeft(_x.m_raw, _count)); }
        [[nodiscard]] friend constexpr Fixed64 operator>>(Fixed64 _x, int _count) noexcept { return Fixed64(ShiftRight(_x.m_raw, _count)); }

        constexpr Fixed64& operator+=(Fixed64 _y) noexcept { return *this = *this + _y; }
        constexpr Fixed64& operator-=(Fixed64 _y) noexcept { return *this = *this - _y; }
        constexpr Fixed64& operator*=(Fixed64 _y) noexcept { return *this = *this * _y; }
        constexpr Fixed64& operator/=(Fixed64 _y) noexcept { return *this = *this / _y; }
        constexpr Fixed64& operator%=(Fixed64 _y) noexcept { return *this = *this % _y; }

        // --- functions (static, as the original) -------------------------------------------------

        [[nodiscard]] static constexpr Fixed64 Sign(Fixed64 _value) noexcept { return FromInt(IntSign(_value)); }

        [[nodiscard]] static constexpr std::int32_t IntSign(Fixed64 _value) noexcept
        {
            const std::int64_t raw = _value.m_raw;
            const std::int64_t negativeBit = static_cast<std::int64_t>(static_cast<std::uint64_t>(WrapNeg(raw)) >> 63);
            return static_cast<std::int32_t>((raw >> 63) | negativeBit);
        }

        [[nodiscard]] static constexpr std::int32_t IntAbs(std::int32_t _value) noexcept
        {
            const std::int32_t mask = _value >> 31;
            return WrapAdd(_value, mask) ^ mask;
        }

        [[nodiscard]] static constexpr Fixed64 Abs(Fixed64 _value) noexcept;
        [[nodiscard]] static constexpr Fixed64 Floor(Fixed64 _value) noexcept { return Fixed64(_value.m_raw & HIGH_MASK); }
        [[nodiscard]] static constexpr Fixed64 Ceiling(Fixed64 _value) noexcept;
        [[nodiscard]] static constexpr Fixed64 Round(Fixed64 _value) noexcept;
        [[nodiscard]] static constexpr Fixed64 Min(Fixed64 _x, Fixed64 _y) noexcept { return _x.m_raw >= _y.m_raw ? _y : _x; }
        [[nodiscard]] static constexpr Fixed64 Max(Fixed64 _x, Fixed64 _y) noexcept { return _x.m_raw <= _y.m_raw ? _y : _x; }

        [[nodiscard]] static constexpr Fixed64 Clamp(Fixed64 _f, Fixed64 _min, Fixed64 _max) noexcept
        {
            if (_f.m_raw > _min.m_raw)
                return _f.m_raw < _max.m_raw ? _f : _max;
            return _min;
        }

        [[nodiscard]] static constexpr std::int32_t Clamp(std::int32_t _i, std::int32_t _min, std::int32_t _max) noexcept
        {
            if (_i > _min)
                return _i < _max ? _i : _max;
            return _min;
        }

        [[nodiscard]] static constexpr Fixed64 Clamp01(Fixed64 _f) noexcept;
        [[nodiscard]] static constexpr Fixed64 Repeat(Fixed64 _f, Fixed64 _length) noexcept;
        [[nodiscard]] static constexpr Fixed64 Repeat(Fixed64 _f, Fixed64 _min, Fixed64 _max) noexcept;
        [[nodiscard]] static constexpr bool BigEnough(Fixed64 _value) noexcept;
        [[nodiscard]] static constexpr bool Approximately(Fixed64 _value, Fixed64 _other) noexcept { return !BigEnough(_value - _other); }
        [[nodiscard]] static constexpr bool BigEnoughSquared(Fixed64 _valueSquared) noexcept;
        [[nodiscard]] static constexpr bool IsInfinity(Fixed64 _value) noexcept { return IsPositiveInfinity(_value) || IsNegativeInfinity(_value); }
        [[nodiscard]] static constexpr bool IsNegativeInfinity(Fixed64 _value) noexcept { return _value.m_raw <= -MAX_RAW; }
        [[nodiscard]] static constexpr bool IsPositiveInfinity(Fixed64 _value) noexcept { return _value.m_raw >= std::numeric_limits<std::int64_t>::max(); }
        [[nodiscard]] static constexpr Fixed64 Lerp(Fixed64 _from, Fixed64 _to, Fixed64 _t) noexcept;
        [[nodiscard]] static constexpr Fixed64 InverseLerp(Fixed64 _from, Fixed64 _to, Fixed64 _value) noexcept;

        [[nodiscard]] static constexpr Fixed64 Sqrt(Fixed64 _value) noexcept;
        [[nodiscard]] static constexpr Fixed64 Exp(Fixed64 _value) noexcept;
        [[nodiscard]] static constexpr Fixed64 Log(Fixed64 _value) noexcept;
        [[nodiscard]] static constexpr Fixed64 Pow(Fixed64 _value, Fixed64 _exponent) noexcept;
        [[nodiscard]] static constexpr Fixed64 Pow(Fixed64 _value, std::int32_t _exponent) noexcept;
        [[nodiscard]] static constexpr bool IsPowerOfTwo(std::uint64_t _x) noexcept { return _x != 0 && (_x & (_x - 1)) == 0; }
        [[nodiscard]] static constexpr Fixed64 Exp2(Fixed64 _exponent) noexcept;
        [[nodiscard]] static constexpr Fixed64 Log2(Fixed64 _argument) noexcept;
        [[nodiscard]] static constexpr Fixed64 Sin(Fixed64 _x) noexcept;
        [[nodiscard]] static constexpr Fixed64 Cos(Fixed64 _radians) noexcept;
        [[nodiscard]] static constexpr Fixed64 Tan(Fixed64 _x) noexcept;
        [[nodiscard]] static constexpr Fixed64 Atan2(Fixed64 _y, Fixed64 _x) noexcept;
        static constexpr void SinCos(Fixed64 _radians, Fixed64& _sin, Fixed64& _cos) noexcept;
        static constexpr void PointOnCircle(Fixed64 _radians, Fixed64 _radius, Fixed64& _x, Fixed64& _y) noexcept;

        [[nodiscard]] static constexpr std::int32_t Factorial(std::int32_t _value) noexcept
        {
            std::int32_t result = 1;
            for (std::int32_t i = 2; i <= _value; ++i)
                result = WrapMul(result, i);
            return result;
        }

    private:
        static constexpr std::int64_t ONE_RAW = 4294967296LL;
        static constexpr std::int64_t MAX_RAW = 9223372036854775807LL;
        static constexpr std::int64_t LOW_MASK = 0xFFFFFFFFLL;
        static constexpr std::int64_t HIGH_MASK = -4294967296LL;

        explicit constexpr Fixed64(std::int64_t _raw) noexcept
            : m_raw(_raw)
        {
        }

        // The original overlays the raw value with its halves (mRawLower: uint at offset 0,
        // mRawUpper: int at offset 4).
        [[nodiscard]] constexpr std::uint32_t RawLower() const noexcept { return static_cast<std::uint32_t>(m_raw); }
        [[nodiscard]] constexpr std::int32_t RawUpper() const noexcept { return static_cast<std::int32_t>(m_raw >> 32); }

        [[nodiscard]] static constexpr std::int64_t AddOverflowHelper(std::int64_t _x, std::int64_t _y, bool& _overflow) noexcept
        {
            const std::int64_t sum = WrapAdd(_x, _y);
            _overflow |= ((_x ^ _y ^ sum) & std::numeric_limits<std::int64_t>::min()) != 0;
            return sum;
        }

        [[nodiscard]] static constexpr Fixed64 QuickAtan(Fixed64 _tan) noexcept;

        std::int64_t m_raw = 0;
    };

    // --- constants, in the original's order (the later ones are computed from the earlier) --------

    inline constexpr Fixed64 Fixed64::EPSILON = Fixed64::FromRawValue(2);
    inline constexpr Fixed64 Fixed64::INFINITESIMAL_TRESHHOLD = Fixed64::FromRawValue(1);
    inline constexpr Fixed64 Fixed64::POSITIVE_INFINITY = Fixed64::FromRawValue(9223372036854775807LL);
    inline constexpr Fixed64 Fixed64::NEGATIVE_INFINITY = Fixed64::FromRawValue(-9223372036854775807LL);
    inline constexpr Fixed64 Fixed64::MAX_VALUE = Fixed64::FromRawValue(9223372036854775806LL);
    inline constexpr Fixed64 Fixed64::MIN_VALUE = Fixed64::FromRawValue(-9223372036854775806LL);
    inline constexpr Fixed64 Fixed64::ZERO = Fixed64::FromRawValue(0);
    inline constexpr Fixed64 Fixed64::ONE_FOURTH = Fixed64::FromRational(1, 4);
    inline constexpr Fixed64 Fixed64::HALF = Fixed64::FromRational(1, 2);
    inline constexpr Fixed64 Fixed64::NEGATIVE_ONE = Fixed64::FromInt(-1);
    inline constexpr Fixed64 Fixed64::ONE = Fixed64::FromRawValue(4294967296LL);
    inline constexpr Fixed64 Fixed64::TWO = Fixed64::FromInt(2);

    // --- operations (definitions follow the declarations' order in the original) ----------------

    constexpr int Fixed64::CompareTo(Fixed64 _value) const noexcept
    {
        if (IsInfinity(*this))
        {
            if (IsInfinity(_value))
            {
                if (IsPositiveInfinity(*this))
                    return IsPositiveInfinity(_value) ? 0 : 1;
                return IsPositiveInfinity(_value) ? -1 : 0;
            }
            return IsPositiveInfinity(*this) ? 1 : -1;
        }
        return m_raw < _value.m_raw ? -1 : (m_raw > _value.m_raw ? 1 : 0);
    }

    constexpr Fixed64 Fixed64::Abs(Fixed64 _value) noexcept
    {
        if (IsInfinity(_value))
            return POSITIVE_INFINITY;
        const std::int64_t mask = _value.m_raw >> 63;
        return Fixed64(WrapAdd(_value.m_raw, mask) ^ mask);
    }

    constexpr Fixed64 Fixed64::Ceiling(Fixed64 _value) noexcept
    {
        if ((_value.m_raw & LOW_MASK) == 0)
            return _value;
        return Floor(_value) + ONE;
    }

    constexpr Fixed64 Fixed64::Round(Fixed64 _value) noexcept
    {
        const std::int64_t fraction = _value.m_raw & LOW_MASK;
        const Fixed64 floor = Floor(_value);
        if (fraction < 2147483648LL)
            return floor;
        if (fraction > 2147483648LL)
            return floor + ONE;
        if ((floor.m_raw & 0x100000000LL) != 0)
            return floor + ONE;
        return floor;
    }

    constexpr Fixed64 Fixed64::Clamp01(Fixed64 _f) noexcept { return Clamp(_f, ZERO, ONE); }

    constexpr Fixed64 Fixed64::Repeat(Fixed64 _f, Fixed64 _length) noexcept
    {
        Fixed64 result = _f - Floor(_f / _length) * _length;
        if (result < ZERO)
            result += _length;
        return result;
    }

    constexpr Fixed64 Fixed64::Repeat(Fixed64 _f, Fixed64 _min, Fixed64 _max) noexcept
    {
        if (_f < _min)
            return _max - (_min - _f - (_max - _min) * Floor((_min - _f) / (_max - _min)));
        return _min + (_f - _min - (_max - _min) * Floor((_f - _min) / (_max - _min)));
    }

    constexpr Fixed64 Fixed64::Lerp(Fixed64 _from, Fixed64 _to, Fixed64 _t) noexcept
    {
        return _from + (_to - _from) * Clamp01(_t);
    }

    constexpr Fixed64 Fixed64::InverseLerp(Fixed64 _from, Fixed64 _to, Fixed64 _value) noexcept
    {
        if (Approximately(_from, _to))
            return ZERO;
        const Fixed64 range = _to - _from;
        const Fixed64 offset = _value - _from;
        return Clamp01(offset / range);
    }

    constexpr Fixed64 operator+(Fixed64 _x, Fixed64 _y) noexcept
    {
        if (Fixed64::IsInfinity(_x) || Fixed64::IsInfinity(_y))
        {
            if (!Fixed64::IsInfinity(_y))
                return _x;
            if (!Fixed64::IsInfinity(_x))
                return _y;
            return _x;
        }
        const std::int64_t a = _x.m_raw;
        const std::int64_t b = _y.m_raw;
        std::int64_t sum = WrapAdd(a, b);
        if ((~(a ^ b) & (a ^ sum) & std::numeric_limits<std::int64_t>::min()) != 0)
            sum = a > 0 ? std::numeric_limits<std::int64_t>::max() : std::numeric_limits<std::int64_t>::min();
        return Fixed64(sum);
    }

    constexpr Fixed64 operator*(Fixed64 _x, Fixed64 _y) noexcept
    {
        const std::int64_t num = _x.m_raw;
        const std::int64_t num2 = _y.m_raw;
        const std::uint64_t num3 = static_cast<std::uint64_t>(num & Fixed64::LOW_MASK);
        const std::int64_t num4 = num >> 32;
        const std::uint64_t num5 = static_cast<std::uint64_t>(num2 & Fixed64::LOW_MASK);
        const std::int64_t num6 = num2 >> 32;
        const std::uint64_t num7 = num3 * num5;
        const std::int64_t num8 = WrapMul(static_cast<std::int64_t>(num3), num6);
        const std::int64_t num9 = WrapMul(num4, static_cast<std::int64_t>(num5));
        const std::int64_t num10 = WrapMul(num4, num6);
        const std::uint64_t x2 = num7 >> 32;
        const std::int64_t y4 = ShiftLeft(num10, 32);
        bool overflow = false;
        std::int64_t x3 = Fixed64::AddOverflowHelper(static_cast<std::int64_t>(x2), num8, overflow);
        x3 = Fixed64::AddOverflowHelper(x3, num9, overflow);
        x3 = Fixed64::AddOverflowHelper(x3, y4, overflow);
        const bool sameSign = ((num ^ num2) & std::numeric_limits<std::int64_t>::min()) == 0;
        if (sameSign)
        {
            if (x3 < 0 || (overflow && num > 0))
                return Fixed64::POSITIVE_INFINITY;
        }
        else if (x3 > 0)
        {
            return Fixed64::NEGATIVE_INFINITY;
        }
        const std::int64_t num11 = num10 >> 32;
        if (num11 != 0 && num11 != -1)
            return sameSign ? Fixed64::POSITIVE_INFINITY : Fixed64::NEGATIVE_INFINITY;
        if (!sameSign)
        {
            std::int64_t larger;
            std::int64_t smaller;
            if (num > num2)
            {
                larger = num;
                smaller = num2;
            }
            else
            {
                larger = num2;
                smaller = num;
            }
            if (x3 > smaller && smaller < -4294967296LL && larger > 4294967296LL)
                return Fixed64::NEGATIVE_INFINITY;
        }
        return Fixed64(x3);
    }

    constexpr Fixed64 operator/(Fixed64 _x, Fixed64 _y) noexcept
    {
        if (Fixed64::Abs(_y) < Fixed64::EPSILON)
        {
            if (Fixed64::Abs(_x) >= Fixed64::EPSILON)
                return _x >= Fixed64::ZERO ? Fixed64::POSITIVE_INFINITY : Fixed64::NEGATIVE_INFINITY;
            if (_y == Fixed64::ZERO)
                return Fixed64(std::numeric_limits<std::int64_t>::min());
            while (Fixed64::Abs(_y) < Fixed64::EPSILON)
            {
                _x *= Fixed64::TWO;
                _y *= Fixed64::TWO;
            }
        }
        const std::int64_t num = _x.m_raw;
        const std::int64_t num2 = _y.m_raw;
        std::uint64_t remainder = num >= 0 ? static_cast<std::uint64_t>(num) : static_cast<std::uint64_t>(WrapNeg(num));
        std::uint64_t divider = num2 >= 0 ? static_cast<std::uint64_t>(num2) : static_cast<std::uint64_t>(WrapNeg(num2));
        std::uint64_t quotient = 0;
        int bitPos = 33;
        while ((divider & 0xF) == 0 && bitPos >= 4)
        {
            divider >>= 4;
            bitPos -= 4;
        }
        while (remainder != 0 && bitPos >= 0)
        {
            int shift = std::countl_zero(remainder);
            if (shift > bitPos)
                shift = bitPos;
            remainder <<= shift;
            bitPos -= shift;
            const std::uint64_t div = remainder / divider;
            remainder %= divider;
            quotient += div << bitPos;
            if ((div & ~(~std::uint64_t{0} >> bitPos)) != 0)
                return ((num ^ num2) & std::numeric_limits<std::int64_t>::min()) != 0 ? Fixed64::NEGATIVE_INFINITY
                                                                                      : Fixed64::POSITIVE_INFINITY;
            remainder <<= 1;
            --bitPos;
        }
        ++quotient;
        std::int64_t result = static_cast<std::int64_t>(quotient >> 1);
        if (((num ^ num2) & std::numeric_limits<std::int64_t>::min()) != 0)
            result = WrapNeg(result);
        return Fixed64(result);
    }

    constexpr Fixed64 Fixed64::Sqrt(Fixed64 _value) noexcept
    {
        const std::int64_t raw = _value.m_raw;
        if (raw < 0)
            return ZERO;
        std::uint64_t num = static_cast<std::uint64_t>(raw);
        std::uint64_t result = 0;
        std::uint64_t bit = 4611686018427387904ULL;
        while (bit > num)
            bit >>= 2;
        for (int i = 0; i < 2; ++i)
        {
            while (bit != 0)
            {
                if (num >= result + bit)
                {
                    num -= result + bit;
                    result = (result >> 1) + bit;
                }
                else
                {
                    result >>= 1;
                }
                bit >>= 2;
            }
            if (i == 0)
            {
                if (num > 0xFFFFFFFFULL)
                {
                    num -= result;
                    num = (num << 32) - 2147483648ULL;
                    result = (result << 32) + 2147483648ULL;
                }
                else
                {
                    num <<= 32;
                    result <<= 32;
                }
                bit = 1073741824ULL;
            }
        }
        if (num > result)
            ++result;
        return Fixed64(static_cast<std::int64_t>(result));
    }

    constexpr Fixed64 Fixed64::Pow(Fixed64 _value, std::int32_t _exponent) noexcept
    {
        Fixed64 result = ONE;
        if (_exponent < 0)
        {
            if (_value == ZERO)
                return POSITIVE_INFINITY;
            if (_exponent == std::numeric_limits<std::int32_t>::min())
            {
                ++_exponent;
                result /= _value;
            }
            _exponent = -_exponent;
            _value = ONE / _value;
        }
        else if (_value == ZERO)
        {
            return ZERO;
        }
        while (_exponent > 0)
        {
            if ((_exponent & 1) == 1)
            {
                result *= _value;
                if (_exponent == 1)
                    break;
            }
            _value *= _value;
            _exponent >>= 1;
        }
        return result;
    }

    constexpr Fixed64 Fixed64::FromScientificNotation(std::int32_t _coefficient, std::int32_t _exponent) noexcept
    {
        return FromInt(_coefficient) * Pow(FromInt(10), _exponent);
    }

    inline constexpr Fixed64 Fixed64::SQRT_TWO = Fixed64::Sqrt(Fixed64::TWO);
    inline constexpr Fixed64 Fixed64::ONE_OVER_SQRT_TWO = Fixed64::Sqrt(Fixed64::HALF);
    inline constexpr Fixed64 Fixed64::FOUR = Fixed64::FromInt(4);
    inline constexpr Fixed64 Fixed64::ONE_HUNDREDTH = Fixed64::Hundredth(1);
    inline constexpr Fixed64 Fixed64::ONE_HUNDRED = Fixed64::FromInt(100);
    inline constexpr Fixed64 Fixed64::ONE_THOUSANDTH = Fixed64::Thousandth(1);
    inline constexpr Fixed64 Fixed64::ONE_THOUSAND = Fixed64::FromInt(1000);
    inline constexpr Fixed64 Fixed64::PI = Fixed64::FromRawValue(13493037704LL);
    inline constexpr Fixed64 Fixed64::TWO_PI = Fixed64::FromRawValue(26986075409LL);
    inline constexpr Fixed64 Fixed64::HALF_PI = Fixed64::FromRawValue(6746518852LL);
    inline constexpr Fixed64 Fixed64::PI_OVER_FOUR = Fixed64::HALF_PI * Fixed64::HALF;
    inline constexpr Fixed64 Fixed64::RADIANS_TO_DEGREES = Fixed64::FromRawValue(246083499207LL);
    inline constexpr Fixed64 Fixed64::DEGREES_TO_RADIANS = Fixed64::FromRawValue(74961320LL);
    inline constexpr Fixed64 Fixed64::LOG2_E = Fixed64::FromRawValue(6196328018LL);
    inline constexpr Fixed64 Fixed64::LN2 = Fixed64::FromRawValue(2977044471LL);
    inline constexpr Fixed64 Fixed64::MAX_EXPONENT_OF_TWO = Fixed64::FromInt(31);
    inline constexpr Fixed64 Fixed64::MIN_EXPONENT_OF_TWO = Fixed64::FromInt(-32);
    inline constexpr Fixed64 Fixed64::SQRT_MAX_VALUE = Fixed64::Sqrt(Fixed64::MAX_VALUE);
    inline constexpr Fixed64 Fixed64::LUT_INTERVAL = Fixed64::FromInt(15) / Fixed64::HALF_PI;
    inline constexpr Fixed64 Fixed64::WORKING_PRECISION = Fixed64::FromScientificNotation(1, -3);
    inline constexpr Fixed64 Fixed64::WORKING_PRECISION_SQ = Fixed64::WORKING_PRECISION * Fixed64::WORKING_PRECISION;
    inline constexpr Fixed64 Fixed64::ONE_OVER_WORKING_PRECISION = Fixed64::ONE / Fixed64::WORKING_PRECISION;
    inline constexpr Fixed64 Fixed64::QUICK_ATAN_COEFFICIENT = Fixed64::FromScientificNotation(28, -2);

    constexpr bool Fixed64::BigEnough(Fixed64 _value) noexcept { return Abs(_value) >= WORKING_PRECISION; }
    constexpr bool Fixed64::BigEnoughSquared(Fixed64 _valueSquared) noexcept { return Abs(_valueSquared) >= WORKING_PRECISION_SQ; }

    constexpr Fixed64 Fixed64::Exp2(Fixed64 _exponent) noexcept
    {
        if (_exponent == ZERO)
            return ONE;
        if (_exponent >= MAX_EXPONENT_OF_TWO)
            return POSITIVE_INFINITY;
        if (_exponent < MIN_EXPONENT_OF_TWO)
            return ZERO;
        const bool negative = _exponent < ZERO;
        const std::int32_t whole = IntValue(_exponent);
        if (negative)
            _exponent.m_raw = WrapMul<std::int64_t>(_exponent.m_raw, -1);
        _exponent.m_raw &= LOW_MASK;   // mRawUpper = 0
        const std::int32_t index = (Multiply(_exponent, 55) + HALF).RawUpper();
        Fixed64 result(EXP2_LUT[static_cast<std::size_t>(index)]);
        Fixed64 delta = _exponent - FromRational(index, 55);
        delta *= LN2;
        result *= ONE + delta + (delta >> 1) * delta;
        result.m_raw = ShiftLeft(result.m_raw, whole < 0 ? -whole : whole);
        if (result < ZERO)
            result = POSITIVE_INFINITY;
        if (negative)
            result = ONE / result;
        return result;
    }

    constexpr Fixed64 Fixed64::Log2(Fixed64 _argument) noexcept
    {
        if (_argument <= ZERO)
            return NEGATIVE_INFINITY;
        const bool belowOne = _argument < ONE;
        if (belowOne)
        {
            if (_argument < EPSILON)
                return FromInt(-32);
            _argument = ONE / _argument;
        }
        std::int32_t shifts = 0;
        while (_argument > TWO)
        {
            _argument.m_raw >>= 1;
            ++shifts;
        }
        const std::int32_t index = (Multiply(_argument - ONE, 55) + HALF).RawUpper();
        Fixed64 result(LOG2_LUT[static_cast<std::size_t>(index)]);
        const Fixed64 base = ONE + FromRational(index, 55);
        const Fixed64 delta = _argument - base;
        result += delta / (LN2 * base) + (delta >> 1) * delta / (LN2 * base * (LN2 * base));
        result += FromInt(shifts);
        if (belowOne)
            result = -result;
        return result;
    }

    constexpr Fixed64 Fixed64::Exp(Fixed64 _value) noexcept { return Exp2(LOG2_E * _value); }
    constexpr Fixed64 Fixed64::Log(Fixed64 _value) noexcept { return LN2 * Log2(_value); }

    constexpr Fixed64 Fixed64::Pow(Fixed64 _value, Fixed64 _exponent) noexcept
    {
        if (_exponent == ZERO)
            return ONE;
        if (_exponent.RawLower() == 0)
            return Pow(_value, IntValue(_exponent));
        if (_value <= ZERO)
        {
            if (_value == ZERO)
                return _exponent < ZERO ? POSITIVE_INFINITY : ZERO;
            return NEGATIVE_INFINITY;
        }
        const Fixed64 fraction = _exponent - FromInt(IntValue(_exponent));
        return Exp2(Log2(_value) * fraction) * Pow(_value, IntValue(_exponent));
    }

    constexpr Fixed64 Fixed64::Sin(Fixed64 _x) noexcept
    {
        _x %= TWO_PI;
        bool negative = _x < ZERO;
        _x = Abs(_x);
        if (_x > PI)
        {
            _x = TWO_PI - _x;
            negative = !negative;
        }
        if (_x > HALF_PI)
            _x = PI - _x;
        const std::int32_t index = IntValue(Multiply(_x / HALF_PI, 15) + HALF);
        const std::int32_t mirror = 15 - index;
        const Fixed64 start = HALF_PI * FromRational(index, 15);
        Fixed64 sin(SIN_LUT[static_cast<std::size_t>(index)]);
        const Fixed64 cos(SIN_LUT[static_cast<std::size_t>(mirror)]);
        const Fixed64 delta = _x - start;
        sin += delta * (cos - (delta >> 1) * sin);
        if (negative)
            sin = -sin;
        return sin;
    }

    constexpr Fixed64 Fixed64::Cos(Fixed64 _radians) noexcept
    {
        _radians = Abs(_radians);
        _radians %= TWO_PI;
        if (_radians > PI)
            _radians = TWO_PI - _radians;
        const bool negative = _radians > HALF_PI;
        if (negative)
            _radians = PI - _radians;
        const std::int32_t index = IntValue(Multiply(_radians / HALF_PI, 15) + HALF);
        const std::int32_t mirror = 15 - index;
        const Fixed64 start = HALF_PI * FromRational(index, 15);
        const Fixed64 sin(SIN_LUT[static_cast<std::size_t>(index)]);
        Fixed64 cos(SIN_LUT[static_cast<std::size_t>(mirror)]);
        const Fixed64 delta = _radians - start;
        cos -= delta * (sin + (delta >> 1) * cos);
        if (negative)
            cos = -cos;
        return cos;
    }

    constexpr void Fixed64::SinCos(Fixed64 _radians, Fixed64& _sin, Fixed64& _cos) noexcept
    {
        _radians %= TWO_PI;
        bool negativeSin = _radians < ZERO;
        _radians = Abs(_radians);
        if (_radians > PI)
        {
            _radians = TWO_PI - _radians;
            negativeSin = !negativeSin;
        }
        const bool negativeCos = _radians > HALF_PI;
        if (negativeCos)
            _radians = PI - _radians;
        const std::int32_t index = IntValue(Multiply(_radians / HALF_PI, 15) + HALF);
        const std::int32_t mirror = 15 - index;
        const Fixed64 start = HALF_PI * FromRational(index, 15);
        const Fixed64 sinStart(SIN_LUT[static_cast<std::size_t>(index)]);
        const Fixed64 cosStart(SIN_LUT[static_cast<std::size_t>(mirror)]);
        const Fixed64 delta = _radians - start;
        _sin = sinStart + delta * (cosStart - (delta >> 1) * sinStart);
        _cos = cosStart - delta * (sinStart + (delta >> 1) * cosStart);
        if (negativeSin)
            _sin = -_sin;
        if (negativeCos)
            _cos = -_cos;
    }

    constexpr Fixed64 Fixed64::Tan(Fixed64 _x) noexcept
    {
        Fixed64 sin;
        Fixed64 cos;
        SinCos(_x, sin, cos);
        return sin / cos;
    }

    constexpr Fixed64 Fixed64::QuickAtan(Fixed64 _tan) noexcept
    {
        if (Abs(_tan) < ONE)
            return _tan / (ONE + QUICK_ATAN_COEFFICIENT * _tan * _tan);
        if (IsInfinity(_tan))
            return HALF_PI;
        return HALF_PI - ONE / (_tan + QUICK_ATAN_COEFFICIENT / _tan);
    }

    constexpr Fixed64 Fixed64::Atan2(Fixed64 _y, Fixed64 _x) noexcept
    {
        if (_y == ZERO)
            return _x >= ZERO ? ZERO : PI;
        if (_x == ZERO)
            return _y >= ZERO ? HALF_PI : -HALF_PI;
        while (Abs(_x) <= EPSILON)
        {
            if (Abs(_y) >= POSITIVE_INFINITY)
                return Sign(_y) * HALF_PI;
            _x *= TWO;
            _y *= TWO;
        }
        const Fixed64 ratio = Abs(_y) / Abs(_x);
        Fixed64 angle = QuickAtan(ratio);
        const Fixed64 tan = Tan(angle);
        if (!IsInfinity(ratio) && !IsInfinity(tan) && ratio != tan)
        {
            const Fixed64 cot = ONE / tan;
            angle += cot / (cot + tan) * (ratio - tan);
        }
        const bool positiveY = _y.m_raw >= 0;
        const bool positiveX = _x.m_raw >= 0;
        if (!positiveY)
            angle = positiveX ? -angle : -PI + angle;
        else if (!positiveX)
            angle = PI - angle;
        return angle;
    }

    constexpr void Fixed64::PointOnCircle(Fixed64 _radians, Fixed64 _radius, Fixed64& _x, Fixed64& _y) noexcept
    {
        Fixed64 sin;
        Fixed64 cos;
        SinCos(_radians, sin, cos);
        _x = _radius * cos;
        _y = _radius * sin;
    }

    // The constants the original computes at class initialisation, checked at compile time
    // against the values it computed (Source/Tests/Vectors/Fixed64.tsv, "const" lines).
    static_assert(Fixed64::SQRT_TWO.RawValue() == 6074001000LL);
    static_assert(Fixed64::HALF.RawValue() == 2147483648LL);
    static_assert(Fixed64::LN2.RawValue() == 2977044471LL);
}
