#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

/*
 * The read side of ByteWriter (ADR-004).
 *
 * This is the code that touches bytes an attacker chose, so it is written to be
 * dull: every read is bounds-checked, a short buffer sets the failure flag and
 * returns a zero value rather than throwing, and the caller checks Ok() once at
 * the end. No exceptions, no asserts on input -- a malformed datagram is a
 * dropped packet and a counter, not a crash.
 */

namespace Neuron
{

class ByteReader
{
public:
  ByteReader() noexcept = default;

  explicit ByteReader(std::span<const std::uint8_t> _buffer) noexcept
    : m_buffer(_buffer)
  {
  }

  [[nodiscard]] std::uint8_t ReadUInt8() noexcept { return ReadScalar<std::uint8_t>(); }
  [[nodiscard]] std::int8_t ReadInt8() noexcept { return ReadScalar<std::int8_t>(); }
  [[nodiscard]] std::uint16_t ReadUInt16() noexcept { return ReadScalar<std::uint16_t>(); }
  [[nodiscard]] std::int16_t ReadInt16() noexcept { return ReadScalar<std::int16_t>(); }
  [[nodiscard]] std::uint32_t ReadUInt32() noexcept { return ReadScalar<std::uint32_t>(); }
  [[nodiscard]] std::int32_t ReadInt32() noexcept { return ReadScalar<std::int32_t>(); }
  [[nodiscard]] std::uint64_t ReadUInt64() noexcept { return ReadScalar<std::uint64_t>(); }
  [[nodiscard]] std::int64_t ReadInt64() noexcept { return ReadScalar<std::int64_t>(); }
  [[nodiscard]] float ReadFloat() noexcept { return ReadScalar<float>(); }

  [[nodiscard]] bool ReadBool() noexcept { return ReadUInt8() != 0; }

  [[nodiscard]] bool ReadBytes(void* _destination, std::size_t _byteCount) noexcept
  {
    if (m_underflowed || _byteCount > BytesRemaining())
    {
      m_underflowed = true;
      return false;
    }
    if (_byteCount != 0)
    {
      std::memcpy(_destination, m_buffer.data() + m_position, _byteCount);
      m_position += _byteCount;
    }
    return true;
  }

  /// Text as a view into the source buffer -- valid only while that buffer lives.
  [[nodiscard]] std::string_view ReadText() noexcept
  {
    const std::uint16_t length = ReadUInt16();
    if (m_underflowed || length > BytesRemaining())
    {
      m_underflowed = true;
      return {};
    }
    const auto* begin = reinterpret_cast<const char*>(m_buffer.data() + m_position);
    m_position += length;
    return std::string_view{begin, length};
  }

  [[nodiscard]] std::size_t BytesRead() const noexcept { return m_position; }
  [[nodiscard]] std::size_t BytesRemaining() const noexcept { return m_buffer.size() - m_position; }
  [[nodiscard]] bool Underflowed() const noexcept { return m_underflowed; }

  /// True when every read succeeded. Check once, after reading a whole message.
  [[nodiscard]] bool Ok() const noexcept { return !m_underflowed; }

  /// True when the message was read exactly -- no missing bytes and no trailing ones.
  [[nodiscard]] bool FullyConsumed() const noexcept { return !m_underflowed && BytesRemaining() == 0; }

  /*
   * Everything not yet read, as bytes.
   *
   * For payloads this reader frames but must not interpret: the engine reads a
   * type word off a datagram and hands the rest to the game without looking
   * inside (ADR-014 §5). Empty after an underflow, because a reader that has
   * already run off the end has nothing trustworthy left to give.
   */
  [[nodiscard]] std::span<const std::uint8_t> Remaining() const noexcept
  {
    return m_underflowed ? std::span<const std::uint8_t>{} : m_buffer.subspan(m_position);
  }

private:
  template <typename T>
  [[nodiscard]] T ReadScalar() noexcept
  {
    static_assert(std::is_trivially_copyable_v<T>, "ReadScalar needs a trivially copyable value");
    static_assert(std::endian::native == std::endian::little, "the wire format is little-endian");

    T value{};
    if (m_underflowed || sizeof(T) > BytesRemaining())
    {
      m_underflowed = true;
      return T{};
    }
    std::memcpy(&value, m_buffer.data() + m_position, sizeof(T));
    m_position += sizeof(T);
    return value;
  }

  std::span<const std::uint8_t> m_buffer{};
  std::size_t m_position = 0;
  bool m_underflowed = false;
};

} // namespace Neuron
