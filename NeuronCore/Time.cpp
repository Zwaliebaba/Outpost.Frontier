#include "pch.h"

#include "Time.h"

namespace Neuron
{

std::int64_t Time::sm_frequency = 0;
std::int64_t Time::sm_startCounter = 0;

void Time::Initialise() noexcept
{
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  sm_frequency = frequency.QuadPart;

  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  sm_startCounter = counter.QuadPart;
}

std::int64_t Time::Counter() noexcept
{
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

std::int64_t Time::Frequency() noexcept
{
  if (sm_frequency == 0)
  {
    Initialise();
  }
  return sm_frequency;
}

double Time::SecondsBetween(std::int64_t _from, std::int64_t _to) noexcept
{
  return static_cast<double>(_to - _from) / static_cast<double>(Frequency());
}

double Time::MillisecondsBetween(std::int64_t _from, std::int64_t _to) noexcept
{
  return SecondsBetween(_from, _to) * 1000.0;
}

double Time::SecondsSinceStart() noexcept
{
  if (sm_frequency == 0)
  {
    Initialise();
  }
  return SecondsBetween(sm_startCounter, Counter());
}

std::int64_t Time::MicrosecondsSinceStart() noexcept
{
  if (sm_frequency == 0)
  {
    Initialise();
  }
  const std::int64_t elapsed = Counter() - sm_startCounter;
  // Scale before dividing, splitting the value so a long uptime cannot overflow.
  const std::int64_t frequency = Frequency();
  const std::int64_t seconds = elapsed / frequency;
  const std::int64_t remainder = elapsed % frequency;
  return seconds * 1000000 + (remainder * 1000000) / frequency;
}

WaitableTimer::WaitableTimer() noexcept
{
  // High resolution where available (Windows 10 1803+); the flag is rejected on
  // older systems, so fall back rather than run without a timer.
  m_handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (m_handle == nullptr)
  {
    m_handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
}

WaitableTimer::~WaitableTimer() noexcept
{
  if (m_handle != nullptr)
  {
    CloseHandle(m_handle);
    m_handle = nullptr;
  }
}

bool WaitableTimer::WaitUntil(std::int64_t _deadlineCounter) noexcept
{
  const std::int64_t now = Time::Counter();
  if (now >= _deadlineCounter)
  {
    return true; // Already late: the caller decides whether to catch up or drop the debt.
  }

  if (m_handle == nullptr)
  {
    const double milliseconds = Time::MillisecondsBetween(now, _deadlineCounter);
    Sleep(static_cast<DWORD>(milliseconds > 0.0 ? milliseconds : 0.0));
    return true;
  }

  // Negative means relative, in 100 ns units.
  const double seconds = Time::SecondsBetween(now, _deadlineCounter);
  LARGE_INTEGER due{};
  due.QuadPart = -static_cast<LONGLONG>(seconds * 10000000.0);
  if (due.QuadPart >= 0)
  {
    return true;
  }

  if (SetWaitableTimer(m_handle, &due, 0, nullptr, nullptr, FALSE) == FALSE)
  {
    return false;
  }
  return WaitForSingleObject(m_handle, INFINITE) == WAIT_OBJECT_0;
}

} // namespace Neuron
