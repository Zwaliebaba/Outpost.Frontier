#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

/*
 * Single-producer / single-consumer queue (ADR-007).
 *
 * This is the only sanctioned way for a foreign thread to reach an owning one:
 * msquic workers and XAudio2 callbacks push, the owner drains on its own tick
 * or frame. Nothing else crosses a thread boundary.
 *
 * Fixed capacity, power of two, no allocation after construction, and no
 * blocking: a full queue drops and counts rather than stalling a callback the
 * OS expects back promptly. Acquire/release pairing is what makes the payload
 * visible to the consumer -- relaxed would publish the index before the data.
 */

namespace Neuron
{

template <typename T, std::size_t CapacityPowerOfTwo>
class RingBuffer
{
public:
  static_assert(std::is_trivially_copyable_v<T>, "RingBuffer carries plain data across threads");
  static_assert(CapacityPowerOfTwo >= 2, "capacity must leave room for at least one element");
  static_assert((CapacityPowerOfTwo & (CapacityPowerOfTwo - 1)) == 0, "capacity must be a power of two");

  /// Producer side. False means the queue was full and the item was dropped.
  [[nodiscard]] bool TryPush(const T& _item) noexcept
  {
    const std::size_t write = m_write.load(std::memory_order_relaxed);
    const std::size_t next = (write + 1) & Mask;
    if (next == m_read.load(std::memory_order_acquire))
    {
      m_dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    m_items[write] = _item;
    m_write.store(next, std::memory_order_release);
    return true;
  }

  /// Consumer side. False means the queue was empty.
  [[nodiscard]] bool TryPop(T& _outItem) noexcept
  {
    const std::size_t read = m_read.load(std::memory_order_relaxed);
    if (read == m_write.load(std::memory_order_acquire))
    {
      return false;
    }
    _outItem = m_items[read];
    m_read.store((read + 1) & Mask, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool Empty() const noexcept
  {
    return m_read.load(std::memory_order_acquire) == m_write.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t Count() const noexcept
  {
    const std::size_t write = m_write.load(std::memory_order_acquire);
    const std::size_t read = m_read.load(std::memory_order_acquire);
    return (write - read) & Mask;
  }

  [[nodiscard]] static constexpr std::size_t Capacity() noexcept { return CapacityPowerOfTwo - 1; }

  /// How many pushes were dropped because the queue was full. A rising count is a real signal.
  [[nodiscard]] std::uint64_t DroppedCount() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
  static constexpr std::size_t Mask = CapacityPowerOfTwo - 1;

  T m_items[CapacityPowerOfTwo]{};
  std::atomic<std::size_t> m_write{0};
  std::atomic<std::size_t> m_read{0};
  std::atomic<std::uint64_t> m_dropped{0};
};

} // namespace Neuron
