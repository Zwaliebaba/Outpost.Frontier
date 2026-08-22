#include "pch.h"

#include "UiScrollState.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{

std::uint32_t VisibleRowCount(float _zoneHeight, float _rowHeight) noexcept
{
  if (!(_rowHeight > 0.0f) || !(_zoneHeight > 0.0f))
  {
    // A zone dragged to nothing, or a row height nobody set. Zero rows is the
    // honest answer and every caller below already handles an empty span; the
    // alternative is a division that is either a crash or an infinity.
    return 0;
  }
  return static_cast<std::uint32_t>(_zoneHeight / _rowHeight);
}

void UiScrollState::SetExtent(std::uint32_t _contentRows, std::uint32_t _visibleRows) noexcept
{
  m_contentRows = _contentRows;
  m_visibleRows = _visibleRows;
  Clamp();
}

std::uint32_t UiScrollState::MaxOffset() const noexcept
{
  return m_contentRows > m_visibleRows ? m_contentRows - m_visibleRows : 0;
}

std::uint32_t UiScrollState::EndVisibleRow() const noexcept
{
  return std::min(m_contentRows, m_offsetRows + m_visibleRows);
}

bool UiScrollState::RowVisible(std::uint32_t _row) const noexcept
{
  return _row >= FirstVisibleRow() && _row < EndVisibleRow();
}

void UiScrollState::ScrollBy(std::int32_t _rows) noexcept
{
  if (_rows < 0)
  {
    const auto up = static_cast<std::uint32_t>(-static_cast<std::int64_t>(_rows));
    m_offsetRows = up >= m_offsetRows ? 0 : m_offsetRows - up;
  }
  else
  {
    // Saturating rather than wrapping: `MaxOffset` is about to clamp it, and a
    // wrapped offset would clamp to the *bottom* of a list the player was
    // scrolling towards the top of.
    const auto down = static_cast<std::uint64_t>(m_offsetRows) + static_cast<std::uint64_t>(_rows);
    m_offsetRows = down > MaxOffset() ? MaxOffset() : static_cast<std::uint32_t>(down);
  }
  Clamp();
}

void UiScrollState::ScrollByWheel(float _wheelSteps) noexcept
{
  if (!(_wheelSteps != 0.0f) || !std::isfinite(_wheelSteps))
  {
    return;
  }

  // Away from the user is towards the top of the content, so the sign flips
  // once, here, rather than at every call site.
  m_wheelRemainderRows -= _wheelSteps * WHEEL_ROWS_PER_NOTCH;

  /*
   * Against the end it is pushing towards, and so nothing can come of any of
   * this travel: drop it rather than bank it.
   *
   * Tested *before* the whole rows come out, which is the half easily missed --
   * a fifth of a notch never reaches a row on its own, so a wheel spun hard
   * against the bottom would otherwise accumulate a row's worth of invisible
   * travel that the player has to spend again before the list comes back.
   */
  if ((m_wheelRemainderRows < 0.0f && m_offsetRows == 0) ||
      (m_wheelRemainderRows > 0.0f && m_offsetRows >= MaxOffset()))
  {
    m_wheelRemainderRows = 0.0f;
    return;
  }

  const float whole = std::trunc(m_wheelRemainderRows);
  if (whole == 0.0f)
  {
    return;
  }
  m_wheelRemainderRows -= whole;
  ScrollBy(static_cast<std::int32_t>(whole));
}

void UiScrollState::Reset() noexcept
{
  m_offsetRows = 0;
  m_wheelRemainderRows = 0.0f;
}

void UiScrollState::Clamp() noexcept
{
  m_offsetRows = std::min(m_offsetRows, MaxOffset());
}

} // namespace Neuron
