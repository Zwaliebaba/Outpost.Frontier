#include "pch.h"

#include "SessionResume.h"

#include <algorithm>

namespace Neuron
{

void ResumeTable::Lapse(PlayerId _player, std::uint64_t _token, std::uint16_t _grid, std::uint32_t _expiresAtTick)
{
  if (_player == INVALID_PLAYER_ID)
  {
    return; // Nobody's session cannot be resumed, so there is nothing to keep.
  }
  // One row per commander: a commander has one session, and an older token
  // that still worked after a newer one was issued would be a second key to
  // the same door.
  m_lapsed.erase(std::remove_if(m_lapsed.begin(), m_lapsed.end(),
                                [_player](const LapsedSession& _row) { return _row.playerId == _player; }),
                 m_lapsed.end());
  m_lapsed.push_back(LapsedSession{_player, _token, _grid, _expiresAtTick});
}

bool ResumeTable::TryResume(PlayerId _player, std::uint64_t _token, std::uint32_t _tick, LapsedSession& _outSession)
{
  /*
   * A zero token is what a first-time client sends, and it must never match.
   * Checked before the walk rather than relying on no row holding zero,
   * because "no row happens to hold zero" is a property of another function.
   */
  if (_player == INVALID_PLAYER_ID || _token == 0)
  {
    return false;
  }

  const auto row = std::find_if(m_lapsed.begin(), m_lapsed.end(),
                                [_player, _token](const LapsedSession& _candidate)
                                { return _candidate.playerId == _player && _candidate.resumeToken == _token; });
  if (row == m_lapsed.end())
  {
    return false;
  }

  /*
   * Expiry is judged here as well as on the tick, because the two answer
   * different questions: the sweep keeps the table small, and this one decides
   * whether a particular arrival is in time. Relying on the sweep alone would
   * make "did I make it back in time" depend on whether a tick happened to have
   * run since the deadline passed.
   */
  if (_tick > row->expiresAtTick)
  {
    m_lapsed.erase(row);
    return false;
  }

  _outSession = *row;
  m_lapsed.erase(row);
  return true;
}

void ResumeTable::Expire(std::uint32_t _tick)
{
  m_lapsed.erase(std::remove_if(m_lapsed.begin(), m_lapsed.end(),
                                [_tick](const LapsedSession& _row) { return _tick > _row.expiresAtTick; }),
                 m_lapsed.end());
}

std::uint32_t ResumeDeadlineTick(std::uint32_t _tick, std::uint32_t _tickRate) noexcept
{
  /*
   * Saturating rather than wrapping. The tick is a u32 and rolls over about
   * every 2.4 days at 20 Hz (ADR-018 D5's own note), and a deadline that wrapped
   * past zero would read as "already expired" -- ending every session on the
   * shard at the moment the counter turned over.
   */
  const std::uint64_t window = static_cast<std::uint64_t>(SESSION_GRACE_SECONDS) * _tickRate;
  const std::uint64_t deadline = static_cast<std::uint64_t>(_tick) + window;
  return deadline > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(deadline);
}

} // namespace Neuron
