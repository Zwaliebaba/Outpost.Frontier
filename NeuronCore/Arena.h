#pragma once

#include "Debug.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

/*
 * Bump allocator for per-frame and per-tick scratch.
 *
 * Allocation is a pointer add; the whole arena is released with Reset() at a
 * known point (end of tick, end of frame), so nothing is freed individually and
 * nothing leaks. Destructors are never run, which is why only trivially
 * destructible types are allowed -- anything owning a resource belongs
 * somewhere with a lifetime.
 *
 * Exhaustion returns nullptr and counts. It does not grow: a scratch arena that
 * silently reallocates hides the fact that a frame's working set moved.
 */

namespace Neuron
{

class Arena
{
public:
  Arena() noexcept = default;

  explicit Arena(std::size_t _capacityBytes)
    : m_memory(std::make_unique<std::uint8_t[]>(_capacityBytes))
    , m_capacityBytes(_capacityBytes)
  {
  }

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;

  [[nodiscard]] void* Allocate(std::size_t _byteCount, std::size_t _alignment = alignof(std::max_align_t)) noexcept
  {
    NEURON_ASSERT(_alignment != 0 && (_alignment & (_alignment - 1)) == 0);

    const std::size_t aligned = (m_used + _alignment - 1) & ~(_alignment - 1);
    if (aligned + _byteCount > m_capacityBytes)
    {
      ++m_failedAllocations;
      return nullptr;
    }
    void* result = m_memory.get() + aligned;
    m_used = aligned + _byteCount;
    if (m_used > m_highWaterMark)
    {
      m_highWaterMark = m_used; // What to size the arena by, rather than guessing.
    }
    return result;
  }

  template <typename T>
  [[nodiscard]] T* AllocateArray(std::size_t _count) noexcept
  {
    static_assert(std::is_trivially_destructible_v<T>, "an arena never runs destructors");
    if (_count == 0)
    {
      return nullptr;
    }
    if (_count > SIZE_MAX / sizeof(T))
    {
      ++m_failedAllocations;
      return nullptr;
    }
    return static_cast<T*>(Allocate(_count * sizeof(T), alignof(T)));
  }

  /// Releases everything at once. The memory stays; only the offset moves.
  void Reset() noexcept { m_used = 0; }

  [[nodiscard]] std::size_t UsedBytes() const noexcept { return m_used; }
  [[nodiscard]] std::size_t CapacityBytes() const noexcept { return m_capacityBytes; }
  [[nodiscard]] std::size_t HighWaterMarkBytes() const noexcept { return m_highWaterMark; }
  [[nodiscard]] std::uint32_t FailedAllocationCount() const noexcept { return m_failedAllocations; }

private:
  std::unique_ptr<std::uint8_t[]> m_memory;
  std::size_t m_capacityBytes = 0;
  std::size_t m_used = 0;
  std::size_t m_highWaterMark = 0;
  std::uint32_t m_failedAllocations = 0;
};

} // namespace Neuron
