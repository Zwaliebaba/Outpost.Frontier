#include "pch.h"

#include "SurfaceStack.h"

namespace Neuron
{

bool SurfaceStack::Holds(SurfaceId _surface) const noexcept
{
  for (std::uint32_t entry = 0; entry < m_depth; ++entry)
  {
    if (m_entries[entry] == _surface)
    {
      return true;
    }
  }
  return false;
}

SurfaceChange SurfaceStack::Push(SurfaceId _surface) noexcept
{
  const SurfaceId leaving = Active();
  if (_surface == leaving)
  {
    // Already here. Not an error and not a navigation -- the caller's button
    // simply named the screen the player is looking at.
    return SurfaceChange{};
  }

  for (std::uint32_t entry = 0; entry < m_depth; ++entry)
  {
    if (m_entries[entry] == _surface)
    {
      // Return to it rather than push a second copy, discarding everything
      // above. The discarded breadcrumbs need no exit: they stopped being
      // active when they were covered, and ran their exits then.
      m_depth = entry + 1;
      return SurfaceChange{leaving, _surface, true};
    }
  }

  if (m_depth == MAX_SURFACE_DEPTH)
  {
    /*
     * Full. Drop the oldest breadcrumb -- slot 1, the first step away from the
     * base -- and slide the rest down. The base survives because it is the way
     * home, and the newest step survives because refusing it would leave a
     * control that does nothing.
     */
    for (std::uint32_t entry = 1; entry + 1 < m_depth; ++entry)
    {
      m_entries[entry] = m_entries[entry + 1];
    }
    --m_depth;
  }

  m_entries[m_depth] = _surface;
  ++m_depth;
  return SurfaceChange{leaving, _surface, true};
}

SurfaceChange SurfaceStack::Back() noexcept
{
  if (m_depth <= 1)
  {
    return SurfaceChange{};
  }

  const SurfaceId leaving = Active();
  --m_depth;
  return SurfaceChange{leaving, Active(), true};
}

SurfaceChange SurfaceStack::Rebase(SurfaceId _base) noexcept
{
  const SurfaceId leaving = Active();
  const bool wasAlone = m_depth == 1 && leaving == _base;

  m_entries[0] = _base;
  m_depth = 1;

  // Re-basing onto the surface already standing alone changed nothing, and
  // reporting an entry for it would re-run entry work on a client that did not
  // move -- a reconnect landing back where it was, say.
  if (wasAlone)
  {
    return SurfaceChange{};
  }
  return SurfaceChange{leaving, _base, true};
}

} // namespace Neuron
