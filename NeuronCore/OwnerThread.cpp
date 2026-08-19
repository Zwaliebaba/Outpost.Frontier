#include "pch.h"

#include "OwnerThread.h"

#ifdef _DEBUG

namespace Neuron
{
namespace
{

/// The calling thread's id, as a plain number. `GetCurrentThreadId` rather than
/// `std::this_thread::get_id` because the value has to be storable and
/// comparable without a header GameLogic must not see transitively.
[[nodiscard]] std::uint32_t ThisThread() noexcept
{
  return static_cast<std::uint32_t>(::GetCurrentThreadId());
}

} // namespace

bool OwnerThread::Enter() noexcept
{
  const std::uint32_t thisThread = ThisThread();
  if (m_owner == 0)
  {
    // Unowned: this thread takes it. No interlocked exchange, deliberately --
    // two threads racing to adopt the same object is exactly the bug being
    // looked for, and making the adoption atomic would hide the losing thread
    // instead of tripping it. The window is one store wide and the check that
    // follows is what actually reports.
    m_owner = thisThread;
    return true;
  }
  return m_owner == thisThread;
}

void OwnerThread::Release() noexcept
{
  m_owner = 0;
}

} // namespace Neuron

#endif
