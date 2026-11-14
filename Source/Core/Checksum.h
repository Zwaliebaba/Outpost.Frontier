// Checksum: the port of BBI.Core.Utility.Checksum, the hash every simulation state object folds
// itself into (GetChecksum) and that a tick's checksum is made of. Bit-exactness of the whole
// simulation is checked against this value (Design/Verification.md §1).
#pragma once

#include <cstdint>

#include "Core/Arithmetic.h"

namespace Core
{
    class Checksum
    {
    public:
        static constexpr std::int32_t MULTIPLIER = 65599;

        constexpr Checksum() noexcept = default;

        explicit constexpr Checksum(std::int32_t _sum) noexcept
            : m_sum(_sum)
        {
        }

        [[nodiscard]] constexpr std::int32_t Sum() const noexcept
        {
            return m_sum;
        }

        constexpr void Add(std::int32_t _value) noexcept
        {
            m_sum = WrapAdd(WrapMul(m_sum, MULTIPLIER), _value);
        }

        // Checksum.Combine(val1, ..., valN): the values folded in order, starting from 0.
        template <std::same_as<std::int32_t>... Values>
        [[nodiscard]] static constexpr std::int32_t Combine(Values... _values) noexcept
        {
            Checksum checksum;
            (checksum.Add(_values), ...);
            return checksum.m_sum;
        }

        [[nodiscard]] constexpr bool operator==(const Checksum&) const noexcept = default;

    private:
        std::int32_t m_sum = 0;
    };
}
