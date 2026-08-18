#pragma once

#include <cstdint>

/*
 * PCG32.
 *
 * The only random source the simulation is allowed to use (ADR-005): seeded,
 * carried in world state, and reproducing exactly on replay. std::mt19937 would
 * also be reproducible but is 2.5 KB of state to checkpoint; the engine's rand()
 * is neither seeded per-world nor specified.
 *
 * Presentation code (jitter, idle animation) may use it freely -- nothing
 * replays the client.
 */

namespace Neuron
{

class Pcg32
{
public:
  constexpr Pcg32() noexcept = default;

  constexpr explicit Pcg32(std::uint64_t _seed, std::uint64_t _sequence = 0xda3e39cb94b95bdbull) noexcept
  {
    Seed(_seed, _sequence);
  }

  constexpr void Seed(std::uint64_t _seed, std::uint64_t _sequence = 0xda3e39cb94b95bdbull) noexcept
  {
    m_state = 0;
    m_increment = (_sequence << 1u) | 1u;
    Next();
    m_state += _seed;
    Next();
  }

  /// Uniform in [0, 2^32).
  constexpr std::uint32_t Next() noexcept
  {
    const std::uint64_t previous = m_state;
    m_state = previous * MULTIPLIER + m_increment;
    const auto xorshifted = static_cast<std::uint32_t>(((previous >> 18u) ^ previous) >> 27u);
    const auto rotation = static_cast<std::uint32_t>(previous >> 59u);
    return (xorshifted >> rotation) | (xorshifted << ((~rotation + 1u) & 31u));
  }

  /// Uniform in [0, _bound). Rejection-sampled, so the distribution is exact rather than modulo-biased.
  constexpr std::uint32_t NextBelow(std::uint32_t _bound) noexcept
  {
    if (_bound == 0)
    {
      return 0;
    }
    const std::uint32_t threshold = (~_bound + 1u) % _bound;
    for (;;)
    {
      const std::uint32_t value = Next();
      if (value >= threshold)
      {
        return value % _bound;
      }
    }
  }

  /// Uniform in [0, 1). 24 bits of mantissa: every value is exactly representable.
  [[nodiscard]] constexpr float NextUnitFloat() noexcept
  {
    return static_cast<float>(Next() >> 8u) * (1.0f / 16777216.0f);
  }

  [[nodiscard]] constexpr std::uint64_t State() const noexcept { return m_state; }
  [[nodiscard]] constexpr std::uint64_t Increment() const noexcept { return m_increment; }

private:
  static constexpr std::uint64_t MULTIPLIER = 6364136223846793005ull;

  std::uint64_t m_state = 0;
  std::uint64_t m_increment = 0xda3e39cb94b95bdbull;
};

} // namespace Neuron
