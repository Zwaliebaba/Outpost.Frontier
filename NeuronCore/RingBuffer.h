#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

/*
 * Bounded lock-free queues (ADR-007): RingBuffer (SPSC) and MpscRingBuffer.
 *
 * These are the only sanctioned way for a foreign thread to reach an owning
 * one: msquic workers and XAudio2 callbacks push, the owner drains on its own
 * tick or frame. Nothing else crosses a thread boundary.
 *
 * Use the SPSC one wherever exactly one thread pushes -- a telemetry lane, an
 * audio callback. Reach for MpscRingBuffer only when several threads genuinely
 * push to the same queue (ADR-003's transport completions), because it pays a
 * compare-exchange per operation that the SPSC one does not.
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
    const std::size_t next = (write + 1) & MASK;
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
    m_read.store((read + 1) & MASK, std::memory_order_release);
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
    return (write - read) & MASK;
  }

  [[nodiscard]] static constexpr std::size_t Capacity() noexcept { return CapacityPowerOfTwo - 1; }

  /// How many pushes were dropped because the queue was full. A rising count is a real signal.
  [[nodiscard]] std::uint64_t DroppedCount() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
  static constexpr std::size_t MASK = CapacityPowerOfTwo - 1;

  T m_items[CapacityPowerOfTwo]{};
  std::atomic<std::size_t> m_write{0};
  std::atomic<std::size_t> m_read{0};
  std::atomic<std::uint64_t> m_dropped{0};
};

/*
 * Multi-producer / single-consumer, same bounded no-allocation contract.
 *
 * Dmitry Vyukov's bounded queue: every slot carries a sequence number, and a
 * producer claims a slot only when that number says the slot's previous
 * occupant has been consumed. The sequence is what makes it safe -- a plain
 * index-and-CAS scheme lets a producer overwrite a slot a slow consumer has not
 * finished reading, and the bug looks like memory corruption rather than a race.
 *
 * The algorithm is actually multi-consumer too; that is not exposed, because
 * ADR-007's rule is that exactly one thread owns the draining and a queue that
 * permits two invites someone to break it.
 */
template <typename T, std::size_t CapacityPowerOfTwo>
class MpscRingBuffer
{
public:
  static_assert(std::is_trivially_copyable_v<T>, "MpscRingBuffer carries plain data across threads");
  static_assert(CapacityPowerOfTwo >= 2, "capacity must leave room for at least one element");
  static_assert((CapacityPowerOfTwo & (CapacityPowerOfTwo - 1)) == 0, "capacity must be a power of two");

  MpscRingBuffer() noexcept
  {
    // Slot i starts "ready for the producer at position i".
    for (std::size_t i = 0; i < CapacityPowerOfTwo; ++i)
    {
      m_slots[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MpscRingBuffer(const MpscRingBuffer&) = delete;
  MpscRingBuffer& operator=(const MpscRingBuffer&) = delete;

  /// Any thread. False means the queue was full and the item was dropped.
  [[nodiscard]] bool TryPush(const T& _item) noexcept
  {
    std::size_t position = m_write.load(std::memory_order_relaxed);
    Slot* slot = nullptr;
    for (;;)
    {
      slot = &m_slots[position & MASK];
      const std::size_t sequence = slot->sequence.load(std::memory_order_acquire);
      const auto distance = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position);
      if (distance == 0)
      {
        if (m_write.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
        {
          break; // The slot is ours.
        }
      }
      else if (distance < 0)
      {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false; // The consumer has not caught up: full.
      }
      else
      {
        position = m_write.load(std::memory_order_relaxed); // Lost the race; re-read and retry.
      }
    }

    slot->item = _item;
    slot->sequence.store(position + 1, std::memory_order_release);
    return true;
  }

  /// The owning thread only. False means the queue was empty.
  [[nodiscard]] bool TryPop(T& _outItem) noexcept
  {
    std::size_t position = m_read.load(std::memory_order_relaxed);
    Slot* slot = nullptr;
    for (;;)
    {
      slot = &m_slots[position & MASK];
      const std::size_t sequence = slot->sequence.load(std::memory_order_acquire);
      const auto distance = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position + 1);
      if (distance == 0)
      {
        if (m_read.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
        {
          break;
        }
      }
      else if (distance < 0)
      {
        return false; // Nothing published at this position yet.
      }
      else
      {
        position = m_read.load(std::memory_order_relaxed);
      }
    }

    _outItem = slot->item;
    slot->sequence.store(position + MASK + 1, std::memory_order_release); // Free for the next lap.
    return true;
  }

  [[nodiscard]] bool Empty() const noexcept
  {
    const std::size_t position = m_read.load(std::memory_order_acquire);
    const std::size_t sequence = m_slots[position & MASK].sequence.load(std::memory_order_acquire);
    return static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position + 1) < 0;
  }

  /// Approximate while producers are running -- the two indices are read at
  /// different moments. Exact once they have stopped.
  [[nodiscard]] std::size_t Count() const noexcept
  {
    const std::size_t write = m_write.load(std::memory_order_acquire);
    const std::size_t read = m_read.load(std::memory_order_acquire);
    return write > read ? write - read : 0;
  }

  [[nodiscard]] static constexpr std::size_t Capacity() noexcept { return CapacityPowerOfTwo; }

  [[nodiscard]] std::uint64_t DroppedCount() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

private:
  static constexpr std::size_t MASK = CapacityPowerOfTwo - 1;

  struct Slot
  {
    std::atomic<std::size_t> sequence{0};
    T item{};
  };

  Slot m_slots[CapacityPowerOfTwo];
  std::atomic<std::size_t> m_write{0};
  std::atomic<std::size_t> m_read{0};
  std::atomic<std::uint64_t> m_dropped{0};
};

} // namespace Neuron
