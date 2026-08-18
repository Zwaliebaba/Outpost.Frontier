#pragma once

#include <cstdint>

/*
 * Wall clock and timing (ADR-002).
 *
 * Named Clock, not Time: a header called Time.h shadows the CRT's <time.h> on a
 * case-insensitive filesystem the moment this folder is on the include path,
 * and every <ctime> symbol then vanishes from the global namespace.
 *
 * QPC throughout: it is monotonic, and its frequency is fixed at boot, so a
 * span measured here does not move when the system clock does.
 *
 * GameLogic must not include this. Simulation time is the tick count and
 * nothing else (ADR-005); a sim that reads a clock stops replaying.
 */

namespace Neuron
{

class Clock
{
public:
  /// Reads the performance counter frequency once. Safe to call more than once.
  static void Initialise() noexcept;

  /// Raw performance counter ticks.
  [[nodiscard]] static std::int64_t Counter() noexcept;

  /// Counter ticks per second.
  [[nodiscard]] static std::int64_t Frequency() noexcept;

  [[nodiscard]] static double SecondsBetween(std::int64_t _from, std::int64_t _to) noexcept;
  [[nodiscard]] static double MillisecondsBetween(std::int64_t _from, std::int64_t _to) noexcept;

  /// Seconds since Initialise().
  [[nodiscard]] static double SecondsSinceStart() noexcept;

  /// Microseconds since Initialise(), for wire timestamps that must stay integral.
  [[nodiscard]] static std::int64_t MicrosecondsSinceStart() noexcept;

private:
  static std::int64_t sm_frequency;
  static std::int64_t sm_startCounter;
};

/*
 * A waitable timer for the tick loop (ADR-002 §2).
 *
 * Sleep() drifts, because it wakes "at least" after the interval and the error
 * accumulates. This schedules against an absolute deadline instead, so a slow
 * tick is absorbed rather than added to every tick after it. High resolution
 * where the OS provides it, plain otherwise.
 */
class WaitableTimer
{
public:
  WaitableTimer() noexcept;
  ~WaitableTimer() noexcept;

  WaitableTimer(const WaitableTimer&) = delete;
  WaitableTimer& operator=(const WaitableTimer&) = delete;

  [[nodiscard]] bool Valid() const noexcept { return m_handle != nullptr; }

  /// Blocks until _deadlineCounter (a Clock::Counter() value). Returns false only if waiting failed.
  bool WaitUntil(std::int64_t _deadlineCounter) noexcept;

private:
  void* m_handle = nullptr;
};

} // namespace Neuron
