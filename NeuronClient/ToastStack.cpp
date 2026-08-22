#include "pch.h"

#include "ToastStack.h"

#include <algorithm>
#include <utility>

namespace Neuron
{

double ToastDwellSeconds(ToastPriority _priority) noexcept
{
  switch (_priority)
  {
  case ToastPriority::Critical:
    return -1.0; // Until acted.
  case ToastPriority::Urgent:
    return 12.0;
  case ToastPriority::Routine:
    return 8.0;
  case ToastPriority::Ambient:
    return 5.0;
  case ToastPriority::WallTime:
    return 8.0;
  }
  return 8.0;
}

bool ToastSurvivesCombat(ToastPriority _priority) noexcept
{
  return _priority == ToastPriority::Critical || _priority == ToastPriority::Urgent;
}

bool ToastStack::Raise(ToastPriority _priority, std::uint32_t _sourceKey, std::string _head, std::string _detail,
                       double _nowSeconds)
{
  return RaiseWithAction(_priority, _sourceKey, std::move(_head), std::move(_detail), nullptr, 0, _nowSeconds);
}

bool ToastStack::RaiseWithAction(ToastPriority _priority, std::uint32_t _sourceKey, std::string _head,
                                 std::string _detail, const char* _actionLabel, std::uint32_t _actionKey,
                                 double _nowSeconds)
{
  /*
   * Coalescing first, and against the visible rows only.
   *
   * A queued toast has not been seen, so folding into one would hide a count
   * the player never had a chance to read -- the sheet's rule is that a
   * suppressed backlog is *visible, not silent*, and collapsing inside it works
   * against that.
   */
  for (Toast& toast : m_visible)
  {
    if (toast.priority == _priority && toast.sourceKey == _sourceKey &&
        _nowSeconds - toast.shownSeconds <= COALESCE_WINDOW_SECONDS)
    {
      ++toast.count;
      toast.detail = std::move(_detail);

      /*
       * The row's action is **not** replaced, and that is a rule about fingers
       * rather than about data: the chip may already be under one, and a button
       * that changes what it does between the reach and the press is the worst
       * kind of surprise a HUD can spring. The first event's action stands for
       * every event that folds into it.
       */

      /*
       * The dwell restarts, because the row is now reporting something that
       * just happened. A fill that kept its original expiry would vanish while
       * its own count was still climbing.
       *
       * Guarded on the dwell being real. A level with no dwell (Critical, and
       * anything else that waits) would otherwise get `now + -1` here -- an
       * expiry in the past, which erases the row on the next sweep. Before
       * actions existed no critical ever reached this branch, because one
       * always waited; an acted critical does.
       */
      if (const double dwell = ToastDwellSeconds(_priority); !toast.WaitsForAction() && dwell >= 0.0)
      {
        toast.shownSeconds = _nowSeconds;
        toast.expiresSeconds = _nowSeconds + dwell;
      }
      return false;
    }
  }

  Toast toast;
  toast.priority = _priority;
  toast.sourceKey = _sourceKey;
  toast.head = std::move(_head);
  toast.detail = std::move(_detail);
  // Borrowed, not copied: the word is the game's and a copy here would be the
  // engine holding a piece of another project's vocabulary.
  toast.actionLabel = _actionLabel;
  toast.actionKey = _actionKey;

  if (m_inCombat && !ToastSurvivesCombat(_priority))
  {
    // Queued, not dropped. Its dwell has not started: it starts when it is
    // shown, or a backlog would drain and expire in the same instant.
    if (m_queued.size() >= MAX_QUEUED)
    {
      m_queued.erase(m_queued.begin());
      ++m_dropped;
    }
    m_queued.push_back(std::move(toast));
    return true;
  }

  Admit(std::move(toast), _nowSeconds);
  return true;
}

void ToastStack::Admit(Toast _toast, double _nowSeconds)
{
  _toast.shownSeconds = _nowSeconds;

  const double dwell = ToastDwellSeconds(_toast.priority);
  _toast.expiresSeconds = dwell < 0.0 ? -1.0 : _nowSeconds + dwell;

  m_visible.push_back(std::move(_toast));
  SortAndCap();
}

void ToastStack::SortAndCap()
{
  /*
   * Loudest first, newest first within a level.
   *
   * `stable_sort` so two rows raised in the same frame keep the order they were
   * raised in -- which is what makes the stack chronological, the property the
   * sheet leans on when it deletes timestamps.
   */
  std::stable_sort(m_visible.begin(), m_visible.end(),
                   [](const Toast& _a, const Toast& _b)
                   {
                     if (_a.priority != _b.priority)
                     {
                       return _a.priority < _b.priority;
                     }
                     return _a.shownSeconds > _b.shownSeconds;
                   });

  // Criticals are centre-top on their own surface and do not consume a slot in
  // the bottom-right stack, so the cap counts what is left after them.
  const std::size_t criticals = CriticalCount();
  const std::size_t allowed = criticals + MAX_VISIBLE;
  while (m_visible.size() > allowed)
  {
    // The oldest of the quietest, which is what "drops oldest first" means once
    // the sort has already put the quietest last.
    m_visible.pop_back();
    ++m_dropped;
  }
}

bool ToastStack::Act(std::size_t _index, std::uint32_t& _outActionKey, double _nowSeconds)
{
  if (_index >= m_visible.size() || !m_visible[_index].HasAction() || m_visible[_index].Acted())
  {
    return false;
  }

  Toast& toast = m_visible[_index];
  toast.actedSeconds = _nowSeconds;

  /*
   * Acting is what gives a waiting row an expiry.
   *
   * Written here rather than left to the sweep so that every path which asks
   * "is this row waiting" gets the right answer immediately -- including the
   * coalescing above, which would otherwise fold a new event into an acted row
   * and restore it to waiting.
   */
  toast.expiresSeconds = _nowSeconds + TOAST_ACTED_COLLAPSE_SECONDS;

  _outActionKey = toast.actionKey;
  return true;
}

void ToastStack::Advance(double _nowSeconds)
{
  std::erase_if(m_visible, [_nowSeconds](const Toast& _t) { return !_t.WaitsForAction() && _nowSeconds >= _t.expiresSeconds; });

  // Then admit whatever combat was holding back, oldest first so the backlog
  // drains in the order it happened.
  while (!m_queued.empty() && (!m_inCombat || ToastSurvivesCombat(m_queued.front().priority)))
  {
    Toast toast = std::move(m_queued.front());
    m_queued.erase(m_queued.begin());
    Admit(std::move(toast), _nowSeconds);
  }
}

void ToastStack::SetCombat(bool _inCombat, double _nowSeconds)
{
  const bool cleared = m_inCombat && !_inCombat;
  m_inCombat = _inCombat;
  if (cleared)
  {
    // Drain immediately rather than on the next frame: the flag clearing is
    // what the player is waiting for, and a frame of nothing after it reads as
    // the backlog having been dropped.
    Advance(_nowSeconds);
  }
}

std::size_t ToastStack::CriticalCount() const noexcept
{
  std::size_t count = 0;
  for (const Toast& toast : m_visible)
  {
    if (toast.priority != ToastPriority::Critical)
    {
      break; // Sorted loudest first, so the criticals are the leading run.
    }
    ++count;
  }
  return count;
}

void ToastStack::Dismiss(std::size_t _index)
{
  if (_index < m_visible.size())
  {
    m_visible.erase(m_visible.begin() + static_cast<std::ptrdiff_t>(_index));
  }
}

void ToastStack::Clear() noexcept
{
  m_visible.clear();
  m_queued.clear();
}

} // namespace Neuron
