#include "pch.h"

#include "EntityTransits.h"

#include <algorithm>

namespace Neuron
{

void EntityTransitList::Clear() noexcept
{
  m_known.clear();
  m_scratch.clear();
  m_transits.clear();
  m_seenFrame = false;
}

void EntityTransitList::Note(std::span<const SceneEntity> _entities, double _nowSeconds)
{
  // Retire what has finished before adding to it, so a frame that starts a
  // fleet's worth of fades is not competing for room with fades that are over.
  const auto expired = std::remove_if(m_transits.begin(), m_transits.end(), [&](const EntityTransit& _transit) {
    return _nowSeconds - _transit.startSeconds >= FADE_SECONDS;
  });
  m_transits.erase(expired, m_transits.end());

  m_scratch.clear();
  m_scratch.reserve(_entities.size());
  for (const SceneEntity& entity : _entities)
  {
    m_scratch.push_back(Known{entity.id, entity.planeMetres, entity.pickRadiusMetres});
  }
  std::sort(m_scratch.begin(), m_scratch.end(),
            [](const Known& _left, const Known& _right) { return _left.id < _right.id; });

  if (!m_seenFrame)
  {
    // The first scene of a session is not a fleet arriving; it is a fleet
    // already there. Recorded and not remarked on.
    m_known.swap(m_scratch);
    m_seenFrame = true;
    return;
  }

  const auto start = [&](const Known& _known, bool _arriving) {
    if (m_transits.size() >= MAX_ACTIVE)
    {
      // The oldest goes, not the newest: the newest is the one still happening,
      // and the oldest has the least of its second left.
      m_transits.erase(m_transits.begin());
      ++m_dropped;
    }
    EntityTransit transit;
    transit.planeMetres = _known.planeMetres;
    transit.radiusMetres = _known.radiusMetres;
    transit.startSeconds = _nowSeconds;
    transit.arriving = _arriving;
    m_transits.push_back(transit);
  };

  // Two sorted walks. An id in one list and not the other is the event; an id
  // in both is a ship that simply moved, which is not one.
  std::size_t oldIndex = 0;
  std::size_t newIndex = 0;
  while (oldIndex < m_known.size() || newIndex < m_scratch.size())
  {
    if (newIndex >= m_scratch.size() || (oldIndex < m_known.size() && m_known[oldIndex].id < m_scratch[newIndex].id))
    {
      start(m_known[oldIndex], false);
      ++oldIndex;
    }
    else if (oldIndex >= m_known.size() || m_scratch[newIndex].id < m_known[oldIndex].id)
    {
      start(m_scratch[newIndex], true);
      ++newIndex;
    }
    else
    {
      ++oldIndex;
      ++newIndex;
    }
  }

  m_known.swap(m_scratch);
}

} // namespace Neuron
