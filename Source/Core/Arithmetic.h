// C#'s arithmetic, in C++ that never relies on undefined behaviour (Design/Architecture.md §4).
//
// The simulation was written in C# with unchecked integers: + - * and negation wrap on overflow,
// a shift count is masked to the operand's width (x << 64 is x << 0), and on x64 Mono a float
// that does not fit an integer converts to the integer's minimum (cvttsd2si's "indefinite"
// value). In C++ each of those is undefined, so ported code does them through these functions.
#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Core
{
    // Wrapping two's-complement arithmetic, as C# unchecked.
    template <std::signed_integral T>
    [[nodiscard]] constexpr T WrapAdd(T _a, T _b) noexcept
    {
        using U = std::make_unsigned_t<T>;
        return static_cast<T>(static_cast<U>(static_cast<U>(_a) + static_cast<U>(_b)));
    }

    template <std::signed_integral T>
    [[nodiscard]] constexpr T WrapSub(T _a, T _b) noexcept
    {
        using U = std::make_unsigned_t<T>;
        return static_cast<T>(static_cast<U>(static_cast<U>(_a) - static_cast<U>(_b)));
    }

    template <std::signed_integral T>
    [[nodiscard]] constexpr T WrapMul(T _a, T _b) noexcept
    {
        using U = std::make_unsigned_t<T>;
        return static_cast<T>(static_cast<U>(static_cast<U>(_a) * static_cast<U>(_b)));
    }

    template <std::signed_integral T>
    [[nodiscard]] constexpr T WrapNeg(T _a) noexcept
    {
        using U = std::make_unsigned_t<T>;
        return static_cast<T>(static_cast<U>(U{0} - static_cast<U>(_a)));
    }

    // Shifts with C#'s masked count: 6 bits for 64-bit operands, 5 for 32-bit ones. Right shifts
    // of signed values are arithmetic (C++20 guarantees it, as C#).
    template <std::integral T>
    [[nodiscard]] constexpr T ShiftLeft(T _value, int _count) noexcept
    {
        using U = std::make_unsigned_t<T>;
        constexpr int MASK = static_cast<int>(sizeof(T) * 8) - 1;
        return static_cast<T>(static_cast<U>(static_cast<U>(_value) << (_count & MASK)));
    }

    template <std::integral T>
    [[nodiscard]] constexpr T ShiftRight(T _value, int _count) noexcept
    {
        constexpr int MASK = static_cast<int>(sizeof(T) * 8) - 1;
        return static_cast<T>(_value >> (_count & MASK));
    }

    // (long)d and (int)d in C# on x64: truncation toward zero; NaN and values out of range give
    // the type's minimum, as cvttsd2si does.
    [[nodiscard]] constexpr std::int64_t DoubleToInt64(double _value) noexcept
    {
        if (!(_value >= -9223372036854775808.0 && _value < 9223372036854775808.0))
            return std::numeric_limits<std::int64_t>::min();
        return static_cast<std::int64_t>(_value);
    }

    [[nodiscard]] constexpr std::int32_t DoubleToInt32(double _value) noexcept
    {
        if (!(_value > -2147483649.0 && _value < 2147483648.0))
            return std::numeric_limits<std::int32_t>::min();
        return static_cast<std::int32_t>(_value);
    }

    // Math.Round(double): to the nearest integer, halves to even. std::nearbyint in the default
    // rounding mode, which the game never changes.
    [[nodiscard]] inline double RoundHalfEven(double _value) noexcept
    {
        return std::nearbyint(_value);
    }

    // GetHashCode of the .NET (and Mono) value types, which checksums combine.
    [[nodiscard]] constexpr std::int32_t HashInt64(std::int64_t _value) noexcept
    {
        return static_cast<std::int32_t>(_value) ^ static_cast<std::int32_t>(_value >> 32);
    }

    [[nodiscard]] constexpr std::int32_t HashBool(bool _value) noexcept
    {
        return _value ? 1 : 0;
    }
}
