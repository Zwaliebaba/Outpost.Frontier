#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

/*
 * Little-endian, bounds-checked serialization (ADR-004).
 *
 * Every write is checked and a write that would overrun sets the failure flag
 * rather than truncating or throwing: callers serialize a whole message, then
 * ask once whether it fit. Overrunning silently is how a wire format starts
 * disagreeing with itself.
 *
 * Little-endian is stated rather than assumed, so a big-endian port would fail
 * loudly here instead of subtly on the wire.
 */

namespace Neuron
{

class ByteWriter
{
public:
  ByteWriter() noexcept = default;

  explicit ByteWriter(std::span<std::uint8_t> _buffer) noexcept
    : m_buffer(_buffer)
  {
  }

  void WriteUInt8(std::uint8_t _value) noexcept { WriteRaw(&_value, sizeof(_value)); }
  void WriteInt8(std::int8_t _value) noexcept { WriteRaw(&_value, sizeof(_value)); }
  void WriteUInt16(std::uint16_t _value) noexcept { WriteScalar(_value); }
  void WriteInt16(std::int16_t _value) noexcept { WriteScalar(_value); }
  void WriteUInt32(std::uint32_t _value) noexcept { WriteScalar(_value); }
  void WriteInt32(std::int32_t _value) noexcept { WriteScalar(_value); }
  void WriteUInt64(std::uint64_t _value) noexcept { WriteScalar(_value); }
  void WriteInt64(std::int64_t _value) noexcept { WriteScalar(_value); }
  void WriteFloat(float _value) noexcept { WriteScalar(_value); }

  void WriteBool(bool _value) noexcept { WriteUInt8(_value ? std::uint8_t{1} : std::uint8_t{0}); }

  void WriteBytes(const void* _data, std::size_t _byteCount) noexcept { WriteRaw(_data, _byteCount); }

  /// Length-prefixed text, capped at 65535 bytes because the prefix is 16 bits.
  void WriteText(std::string_view _text) noexcept
  {
    if (_text.size() > 0xffffu)
    {
      m_overflowed = true;
      return;
    }
    WriteUInt16(static_cast<std::uint16_t>(_text.size()));
    WriteRaw(_text.data(), _text.size());
  }

  [[nodiscard]] std::size_t BytesWritten() const noexcept { return m_position; }
  [[nodiscard]] std::size_t BytesRemaining() const noexcept { return m_buffer.size() - m_position; }
  [[nodiscard]] bool Overflowed() const noexcept { return m_overflowed; }
  [[nodiscard]] bool Ok() const noexcept { return !m_overflowed; }

  /// What was actually written -- empty if anything overflowed, so a bad buffer cannot be sent by accident.
  [[nodiscard]] std::span<const std::uint8_t> Written() const noexcept
  {
    return m_overflowed ? std::span<const std::uint8_t>{} : m_buffer.subspan(0, m_position);
  }

  void Reset() noexcept
  {
    m_position = 0;
    m_overflowed = false;
  }

private:
  template <typename T>
  void WriteScalar(T _value) noexcept
  {
    static_assert(std::is_trivially_copyable_v<T>, "WriteScalar needs a trivially copyable value");
    static_assert(sizeof(T) <= 8, "WriteScalar is for scalars");
    // x64 is little-endian, so the object representation is already the wire order.
    static_assert(std::endian::native == std::endian::little, "the wire format is little-endian");
    WriteRaw(&_value, sizeof(T));
  }

  void WriteRaw(const void* _data, std::size_t _byteCount) noexcept
  {
    if (m_overflowed)
    {
      return;
    }
    if (_byteCount > BytesRemaining())
    {
      m_overflowed = true;
      return;
    }
    if (_byteCount != 0)
    {
      std::memcpy(m_buffer.data() + m_position, _data, _byteCount);
      m_position += _byteCount;
    }
  }

  std::span<std::uint8_t> m_buffer{};
  std::size_t m_position = 0;
  bool m_overflowed = false;
};

} // namespace Neuron
