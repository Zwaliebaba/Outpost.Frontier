#pragma once

#include <cstdint>

/*
 * One scrolling list, for every list this client has (ADR-020 §4).
 *
 * The roster's `∨ 8/8` footer, the map's route panel, the hangar's wing columns
 * and the settings body are one primitive and not four: an offset in **rows**, a
 * content count, a visible count, and a clamp. `HudRoster.h` deferred scrolling
 * as "a surface rather than a bigger number" -- it is neither; it is this.
 *
 * **Rows, never pixels.** A row outside the visible span emits nothing, and a
 * row straddling the edge is not emitted either. The alternative is a clip
 * rectangle on `UiInstance`, whose 48-byte stride ADR-006 §10a and a static
 * assert deliberately pin, or a scissor change mid-pass with a sort to group by
 * it -- both bought for a half-row the prints never draw. The pass stays one
 * upload and one draw, and this file stays arithmetic.
 *
 * **Retained per list, session lifetime** (ADR-020 §5.4): a scroll offset
 * survives navigating away and back, and is cleared by the session ending
 * rather than by leaving the screen. What clamps it is the content arriving --
 * `SetExtent` every frame, from data the client holds now.
 */

namespace Neuron
{

/*
 * How many rows one wheel notch moves.
 *
 * Three, which is what Win32 reports as the system default for a notch
 * (`SPI_GETWHEELSCROLLLINES`) and what every list on the desktop this client
 * shares a screen with already does. Reading the real setting is a later
 * kindness; disagreeing with it by an invented number is a worse start.
 */
inline constexpr float WHEEL_ROWS_PER_NOTCH = 3.0f;

/*
 * How many whole rows of `_rowHeight` fit in `_zoneHeight`.
 *
 * Truncating rather than rounding is the whole-row rule at its source: a list
 * that reported the half-row it has room for would then have a row to draw and
 * nowhere to draw it.
 */
[[nodiscard]] std::uint32_t VisibleRowCount(float _zoneHeight, float _rowHeight) noexcept;

class UiScrollState
{
public:
  /*
   * Tells the list what it is showing, and re-clamps.
   *
   * Called every frame from live data, which is what makes the clamp a
   * *correction* rather than a check: a hangar the player scrolled to the
   * bottom of, whose ships then undocked from another surface, has an offset
   * past the end of its own content until this runs. It is the same hazard
   * ADR-017 §6a.2 answers for the composer, in the one place a list can answer
   * it for itself.
   */
  void SetExtent(std::uint32_t _contentRows, std::uint32_t _visibleRows) noexcept;

  /*
   * Moves by whole rows. Negative is towards the top of the content.
   *
   * Clamped at both ends and never wrapped: a list that jumped from its last
   * row to its first would lose the player's place in exactly the moment they
   * were looking for it.
   */
  void ScrollBy(std::int32_t _rows) noexcept;

  /*
   * Moves by wheel notches, positive away from the user -- which scrolls
   * *towards the top*, the direction every other list on the machine goes.
   *
   * **The fraction is kept**, and that is not a nicety. `InputFrame::wheelSteps`
   * is deliberately fractional, because rounding makes a high-resolution wheel
   * feel notched twice; a list that truncated each frame's contribution on its
   * own would then never move at all under one, since a fifth of a notch is
   * less than a row every time. So the remainder accumulates here and whole
   * rows come out of it.
   *
   * The remainder is dropped on reaching either end, so a wheel spun hard
   * against the bottom does not have to be spun back through the same distance
   * before the list responds.
   */
  void ScrollByWheel(float _wheelSteps) noexcept;

  /// Back to the top. The entry action for a list whose content changed
  /// wholesale -- a different station's hangar is not the same list scrolled.
  void Reset() noexcept;

  [[nodiscard]] std::uint32_t Offset() const noexcept { return m_offsetRows; }
  [[nodiscard]] std::uint32_t ContentRows() const noexcept { return m_contentRows; }
  [[nodiscard]] std::uint32_t VisibleRows() const noexcept { return m_visibleRows; }

  /// The largest offset that still fills the visible span, or zero when the
  /// content fits. Scrolling past this shows air below the last row, which is a
  /// list that has lost its footing rather than one that has reached its end.
  [[nodiscard]] std::uint32_t MaxOffset() const noexcept;

  /// The half-open span of rows to draw: `[First, End)`. Empty when there is
  /// nothing to show or nowhere to show it.
  [[nodiscard]] std::uint32_t FirstVisibleRow() const noexcept { return m_offsetRows; }
  [[nodiscard]] std::uint32_t EndVisibleRow() const noexcept;

  /// Whether this row is drawn at all -- the whole-row cull, asked per row.
  [[nodiscard]] bool RowVisible(std::uint32_t _row) const noexcept;

  /// Where this row sits in the visible span, for a caller stepping down a
  /// zone. Meaningless unless `RowVisible`, which is why they are two calls.
  [[nodiscard]] std::uint32_t RowSlot(std::uint32_t _row) const noexcept { return _row - m_offsetRows; }

  /// What the affordance says: whether there is content off either end. The
  /// print draws a chevron and a count, and these are what decide the chevron.
  [[nodiscard]] bool CanScrollUp() const noexcept { return m_offsetRows > 0; }
  [[nodiscard]] bool CanScrollDown() const noexcept { return m_offsetRows < MaxOffset(); }

private:
  void Clamp() noexcept;

  std::uint32_t m_offsetRows = 0;
  std::uint32_t m_contentRows = 0;
  std::uint32_t m_visibleRows = 0;

  /// Sub-row wheel travel, signed, in rows. See `ScrollByWheel`.
  float m_wheelRemainderRows = 0.0f;
};

} // namespace Neuron
