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

void OwnerThread::Claim() noexcept
{
  m_owner = ThisThread();
}

void OwnerThread::Release() noexcept
{
  m_owner = 0;
}

bool OwnerThread::OwnedByThisThread() const noexcept
{
  // Unclaimed is owned by whoever asks. An object that has not been given an
  // owner yet is being set up, and asserting during construction would make the
  // rule fire on the one path that cannot violate it.
  return m_owner == 0 || m_owner == ThisThread();
}

} // namespace Neuron

#endif
