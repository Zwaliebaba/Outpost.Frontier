#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

/*
 * FNV-1a.
 *
 * Chosen for what it has to do here rather than for speed: schema and content
 * hashes (ADR-004, ADR-009, ADR-012) and the world hash the replay-determinism
 * suite compares (ADR-005). All three need a hash that is stable across runs
 * and trivially reimplementable -- std::hash is neither, being permitted to
 * differ between builds and platforms.
 *
 * This is not a cryptographic hash and nothing here should treat it as one.
 */

namespace Neuron
{

inline constexpr std::uint64_t FNV_OFFSET_BASIS_64 = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t FNV_PRIME_64 = 0x00000100000001b3ull;

/// Hashes bytes, continuing from an existing value so a hash can be accumulated.
[[nodiscard]] constexpr std::uint64_t HashBytes(const void* _data, std::size_t _byteCount,
                                                std::uint64_t _seed = FNV_OFFSET_BASIS_64) noexcept
{
  const auto* bytes = static_cast<const unsigned char*>(_data);
  std::uint64_t hash = _seed;
  for (std::size_t i = 0; i < _byteCount; ++i)
  {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= FNV_PRIME_64;
  }
  return hash;
}

/// Hashes text. constexpr so a schema string can be hashed at compile time.
[[nodiscard]] constexpr std::uint64_t HashText(std::string_view _text, std::uint64_t _seed = FNV_OFFSET_BASIS_64) noexcept
{
  std::uint64_t hash = _seed;
  for (const char c : _text)
  {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= FNV_PRIME_64;
  }
  return hash;
}

/// Folds a value into a running hash. For world hashes, feed fields in a fixed order.
template <typename T> [[nodiscard]] std::uint64_t HashValue(const T& _value, std::uint64_t _seed = FNV_OFFSET_BASIS_64) noexcept
{
  static_assert(std::is_trivially_copyable_v<T>, "HashValue needs a trivially copyable value");
  return HashBytes(&_value, sizeof(T), _seed);
}

} // namespace Neuron
