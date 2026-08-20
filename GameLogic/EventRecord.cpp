#include "pch.h"

#include "EventRecord.h"

namespace Game
{

void EventRecord::Emit(std::uint32_t _tick, EventKind _kind, AnchorId _anchor, std::uint16_t _count)
{
  if (_count == 0)
  {
    // Nothing happened to nobody. Emitting it would put an empty line in a log
    // whose whole value is that every line is something the player can act on.
    return;
  }

  if (m_entries.size() >= CAPACITY)
  {
    /*
     * The oldest goes. An erase-from-front on a vector rather than a ring
     * buffer, and the reason is honest rather than clever: this happens once
     * per event only after 512 of them, the entries are eight bytes, and a ring
     * would cost an index-translation on every read for a saving nobody can
     * measure. Revisit if the cap ever grows by an order of magnitude.
     */
    m_entries.erase(m_entries.begin());
    ++m_dropped;
  }

  m_entries.push_back(EventEntry{_tick, _kind, _anchor, _count});
}

void EventRecord::Clear() noexcept
{
  m_entries.clear();
  m_dropped = 0;
}

} // namespace Game
