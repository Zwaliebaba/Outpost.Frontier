#pragma once

#include "EntityRecord.h" // EntityId, INVALID_ENTITY_ID (ADR-022 §8a).

#include "Picking.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * What the player has selected, and the gesture that changes it (ADR-006 §11).
 *
 * A set of ids, and what a tap does to it.
 *
 * **It used to be two things**: the set, and a press/move/release state machine
 * that resolved to a point pick or a box pick. I2 took the state machine away
 * -- `Gesture.h` owns when a tap happened, and once drag is the camera a box
 * has no gesture left to be (Plan-of-Record §1, rules 1 and 5). What is left is
 * the set and one function, which is what this file was always mostly about.
 *
 * Apart from the renderer because a selection is not a drawing -- the overlay
 * reads it, it does not own it (ADR-006 §7 keeps selection out of the hull
 * colour entirely).
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
  /// The pick radius floor, in pixels, handed to `PickPoint` by way of
  /// `IsoCamera::ScreenFloorMetres`. Eight pixels is a comfortable target at
  /// any zoom without being so wide that two neighbours become one.
  static constexpr float PICK_FLOOR_PIXELS = 8.0f;

  /*
   * A tap landed here: take what is under it (I2, Plan-of-Record §1's rule 1).
   *
   * **Tap selects, drag pans** -- and this is the whole of the first half. It
   * replaced a press/move/release state machine that resolved to a click *or a
   * box*, because once drag is the camera a box has no gesture left to be
   * (rule 5). What it kept is the box's *pick*: `PickBox` is still in
   * `Picking.h`, reserved for the two-finger drag rule 5 sets aside, because
   * "we never drew one" is not the same as "it is wrong".
   *
   * Everything the decision needs arrives here rather than being remembered.
   * The gesture layer owns when a tap happened and where; this owns what it
   * means, and it has no state between taps for a camera move to invalidate.
   *
   * `_additive` is the desk's shift accelerator, and the design may not require
   * it (ADR-020's amendment: no interaction may need a key). The touch path for
   * adding to a selection is rule 4's long-press on a roster row, which is not
   * this function's.
   */
  void Tap(float _tapX, float _tapY, bool _additive, std::span<const SceneEntity> _entities, const PlaneMapping& _mapping,
           std::uint32_t _viewportWidth, std::uint32_t _viewportHeight, float _minRadiusMetres);

  [[nodiscard]] std::span<const EntityId> Ids() const noexcept { return m_ids; }
  [[nodiscard]] std::size_t Count() const noexcept { return m_ids.size(); }
  [[nodiscard]] bool Empty() const noexcept { return m_ids.empty(); }
  [[nodiscard]] bool Contains(EntityId _id) const noexcept;

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
  void Set(std::span<const EntityId> _ids);
  void Add(EntityId _id);
  void Toggle(EntityId _id);

private:
  std::vector<EntityId> m_ids;
};

} // namespace Neuron
