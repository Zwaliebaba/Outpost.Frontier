#pragma once

#include "Picking.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * What the player has selected, and the gesture that changes it (ADR-006 §11).
 *
 * Two things that look like one: a set of ids, and a small state machine that
 * turns press/move/release into either a point pick or a box pick. They live
 * together because the gesture is only meaningful as "what it does to the set",
 * and apart from the renderer because a selection is not a drawing -- the
 * overlay reads it, it does not own it (ADR-006 §7 keeps selection out of the
 * hull colour entirely).
 *
 * **The set is ids, not indices.** Ships arrive interpolated and re-sorted
 * every frame, and a despawn shifts everything after it. An index would select
 * a different ship next frame; an id selects the same ship until it dies, and
 * then selects nothing.
 *
 * **Device-free.** The gesture takes pixels and a plane mapping and returns a
 * decision, so the whole of click, shift-click and box-select is testable
 * without a window -- which is the accept criterion S8 was given.
 */

namespace Neuron
{

class Selection
{
public:
  /*
   * How far the cursor may travel and still count as a click.
   *
   * Not zero, because a mouse moves a pixel or two during a click on any hand
   * that is not resting on the desk, and a one-pixel box selects nothing. Four
   * pixels is small enough that a deliberate drag never reads as a click.
   */
  static constexpr float CLICK_SLOP_PIXELS = 4.0f;

  /// The pick radius floor, in pixels, handed to `PickPoint` by way of
  /// `IsoCamera::ScreenFloorMetres`. Eight pixels is a comfortable click target
  /// at any zoom without being so wide that two neighbours become one.
  static constexpr float PICK_FLOOR_PIXELS = 8.0f;

  /// A drag started at these pixels. `_additive` is the shift key, sampled at
  /// press: a modifier released mid-drag must not change what the drag means.
  void BeginDrag(float _cursorX, float _cursorY, bool _additive) noexcept;

  /// The cursor moved while the button is down.
  void UpdateDrag(float _cursorX, float _cursorY) noexcept;

  /*
   * The button came up. Resolves to a click or a box and applies it.
   *
   * `_mapping` and `_viewport*` convert the recorded pixels; `_entities` is the
   * frame's pick list; `_minRadiusMetres` is the screen floor. Everything the
   * decision needs arrives here rather than being remembered, because the
   * camera may have moved during the drag and the answer must use where things
   * are *now*.
   */
  void EndDrag(std::span<const SceneEntity> _entities, const PlaneMapping& _mapping, std::uint32_t _viewportWidth,
               std::uint32_t _viewportHeight, float _minRadiusMetres);

  /// Abandons a drag without changing the selection -- focus loss, or the
  /// window going away mid-gesture.
  void CancelDrag() noexcept { m_dragging = false; }

  [[nodiscard]] bool Dragging() const noexcept { return m_dragging; }

  /// True once the cursor has left the click slop. The overlay draws the
  /// rectangle only from this point, so a click never flashes a box.
  [[nodiscard]] bool DragIsBox() const noexcept;

  /// The drag rectangle in pixels, for the overlay. Meaningless unless
  /// `DragIsBox()`.
  [[nodiscard]] float DragStartX() const noexcept { return m_startX; }
  [[nodiscard]] float DragStartY() const noexcept { return m_startY; }
  [[nodiscard]] float DragCurrentX() const noexcept { return m_currentX; }
  [[nodiscard]] float DragCurrentY() const noexcept { return m_currentY; }

  [[nodiscard]] std::span<const std::uint16_t> Ids() const noexcept { return m_ids; }
  [[nodiscard]] std::size_t Count() const noexcept { return m_ids.size(); }
  [[nodiscard]] bool Empty() const noexcept { return m_ids.empty(); }
  [[nodiscard]] bool Contains(std::uint16_t _id) const noexcept;

  void Clear() noexcept { m_ids.clear(); }

  /*
   * Drops ids that are no longer in the world.
   *
   * Called every frame with the frame's pick list. A ship that died while
   * selected has to leave the selection, or the overlay draws a ring around
   * empty space and an order goes out for something that does not exist.
   */
  void Retain(std::span<const SceneEntity> _entities);

  /// Directly, for tests and for the HUD's select-all (S11).
  void Set(std::span<const std::uint16_t> _ids);
  void Add(std::uint16_t _id);
  void Toggle(std::uint16_t _id);

private:
  std::vector<std::uint16_t> m_ids;

  bool m_dragging = false;
  bool m_additive = false;
  float m_startX = 0.0f;
  float m_startY = 0.0f;
  float m_currentX = 0.0f;
  float m_currentY = 0.0f;
};

} // namespace Neuron
