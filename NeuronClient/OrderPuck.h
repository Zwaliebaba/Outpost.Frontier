#pragma once

#include "EntityRecord.h" // EntityId, INVALID_ENTITY_ID (ADR-022 §8a).

#include "IsoCamera.h"
#include "OrderIntent.h"
#include "RenderWorld.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>

/*
 * The gesture that becomes an order (`puck-and-wheel.png` §2, ADR-006 §11).
 *
 * A right-button drag, and it carries the two decisions the print says the
 * player makes: *where* the fleet goes and *which way it faces when it gets
 * there*. Press fixes the destination; the drag away from it fixes the arrival
 * facing; release commits.
 *
 * **The print draws a touch gesture and this is a mouse.** There, the puck
 * follows the finger and a *second* finger twists the footprint -- two pointers
 * for two degrees of freedom, and the sheet is explicit that the second one is
 * only free because planar movement bought it ("now a screen point maps to
 * exactly one world position, so the second finger is free"). A mouse has one
 * pointer, so the two decisions have to be sequenced instead of concurrent, and
 * anchor-then-drag is the sequence that keeps both: the same press that names
 * the place starts the arc that names the heading. The alternative -- drag the
 * puck and take the facing from somewhere else -- would have thrown the degree
 * of freedom away rather than rebound it.
 *
 * **Below the slop there is still a facing.** A quick right-click is the common
 * case and it says nothing about heading, so `Resolve` reports that it has none
 * and the caller substitutes one -- `TravelFacingRadians`, the way the group is
 * already pointing at the target. A fleet that arrives facing the way it
 * travelled is what a player who did not think about facing expects to see.
 *
 * **Device-free and game-free.** Pixels, a `PlaneMapping` and a list of
 * `SceneEntity` in, plane metres and radians out. It does not know what a Move
 * is -- `OrderIntent::kind` is filled from what the game answered (ADR-014) --
 * and it does not send anything. Nothing here reaches the network; that is
 * `ClientApp`'s job once the gesture has resolved and the game has agreed.
 */

namespace Neuron
{

/// One resolved gesture, in the units `OrderIntent` wants.
struct PuckSample
{
  DirectX::XMFLOAT2 targetMetres{};
  float facingRadians = 0.0f;

  /// Whether the facing came from the drag or from the fallback. The overlay
  /// draws the arc only in the first case, so a click never flashes one.
  bool facingFromDrag = false;

  /// Append to the queue rather than replace it -- the print's queue chip,
  /// sampled at press so a modifier released mid-gesture cannot change what the
  /// gesture already meant.
  bool queued = false;
};

class OrderPuck
{
public:
  /*
   * How far the cursor must travel before the drag means a heading.
   *
   * Larger than `Selection::CLICK_SLOP_PIXELS`, and for a different reason. The
   * selection slop asks "did the mouse stay still"; this asks "is this arc long
   * enough to be aiming at something". A four-pixel arc has an angle dominated
   * by the last pixel the mouse happened to move, so honouring it would make a
   * fleet arrive facing a direction the player did not choose -- worse than
   * ignoring it, because they will see the result and think they chose it.
   */
  static constexpr float FACING_SLOP_PIXELS = 12.0f;

  /// The button went down. `_queued` is the queue modifier at that instant.
  void Begin(float _cursorX, float _cursorY, bool _queued) noexcept;

  /// The cursor moved while the button is down. Ignored when idle, so a move
  /// that arrives after a cancel cannot restart the gesture.
  void Update(float _cursorX, float _cursorY) noexcept;

  /// Abandons the gesture without producing anything -- focus loss, escape, or
  /// a selection that emptied while the button was held.
  void Cancel() noexcept { m_active = false; }

  [[nodiscard]] bool Active() const noexcept { return m_active; }

  /// True once the drag has left the slop, which is when it carries a facing.
  [[nodiscard]] bool HasFacing() const noexcept;

  [[nodiscard]] bool Queued() const noexcept { return m_queued; }

  /// The anchor and the cursor, in pixels, for the arc the overlay draws.
  /// Meaningless unless `Active()`.
  [[nodiscard]] float AnchorPixelsX() const noexcept { return m_anchorX; }
  [[nodiscard]] float AnchorPixelsY() const noexcept { return m_anchorY; }
  [[nodiscard]] float CursorPixelsX() const noexcept { return m_cursorX; }
  [[nodiscard]] float CursorPixelsY() const noexcept { return m_cursorY; }

  /*
   * Resolves the gesture into plane space.
   *
   * The mapping and viewport arrive here rather than being remembered from the
   * press, because the camera may have panned or orbited during the drag and
   * the answer has to be where the anchor is *now*. That is the same rule
   * `Selection::EndDrag` follows and for the same reason.
   *
   * The facing is measured on the plane, not on the screen: a drag toward the
   * top of the window means "face away from the camera", and at a yaw of ninety
   * degrees that is a different world heading than it was at zero.
   *
   * A gesture below the slop reports `facingFromDrag == false` and a facing of
   * zero, which is not a heading -- it is the absence of one. The caller fills
   * it in, because the obvious default (`TravelFacingRadians`) needs the target
   * this call is what produces.
   */
  [[nodiscard]] PuckSample Resolve(const PlaneMapping& _mapping, std::uint32_t _viewportWidth,
                                   std::uint32_t _viewportHeight) const noexcept;

private:
  bool m_active = false;
  bool m_queued = false;
  float m_anchorX = 0.0f;
  float m_anchorY = 0.0f;
  float m_cursorX = 0.0f;
  float m_cursorY = 0.0f;
};

/*
 * Where a selection is, as one point: the average of the ids present in
 * `_entities`.
 *
 * Ids that are not there are skipped rather than counted at the origin -- a
 * selection is pruned every frame, and one that slipped through would drag the
 * centre halfway across the map. Returns false when none of them are present,
 * leaving the output untouched, because a fleet of nothing has no centre.
 *
 * Two callers, both about the same fleet: the fallback facing, and where a
 * refused ghost bounces back to.
 */
[[nodiscard]] bool SelectionCentre(std::span<const SceneEntity> _entities, std::span<const EntityId> _ids,
                                   DirectX::XMFLOAT2& _outMetres) noexcept;

/*
 * The heading a group would arrive on if nobody chose one: from where it is now
 * toward where it is going.
 *
 * Returns `_ifStationary` when the group is already at the target or none of
 * the selection is present, because `atan2(0, 0)` is a direction nobody asked
 * for.
 */
[[nodiscard]] float TravelFacingRadians(std::span<const SceneEntity> _entities, std::span<const EntityId> _ids,
                                        const DirectX::XMFLOAT2& _targetMetres, float _ifStationary) noexcept;

/*
 * Fills an intent from a resolved gesture, the game's defaults and a selection.
 *
 * A function rather than a constructor on `OrderIntent`, because that type is
 * NeuronCore's neutral seam struct and this is the client's opinion about how
 * to fill it. The ids are borrowed: `OrderIntent::entityIds` is a pointer into
 * the caller's selection, which must outlive the call the intent is passed to.
 */
[[nodiscard]] OrderIntent MakeOrderIntent(const PuckSample& _sample, const OrderDefaults& _defaults,
                                          std::span<const EntityId> _ids, std::uint32_t _orderSeq) noexcept;

} // namespace Neuron
