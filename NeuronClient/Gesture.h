#pragma once

#include "InputMap.h"

#include <cstdint>

/*
 * What the contacts meant (ADR-020's 2026-08-22 amendment, Plan-of-Record §1).
 *
 * The seam D15.4 refused. `InputFrame::contacts` is what is touching the
 * screen; this is the layer that turns it into the five things the design
 * actually spends -- **tap, drag, long-press, second finger, pinch** -- and it
 * is the whole of I1.
 *
 * **It decides nothing.** A tap is not a selection and a long-press is not an
 * order: `puck-and-wheel.png` §1's table turns a long-press into a puck or a
 * wheel depending on a toggle and a hit test, and neither the toggle nor the
 * hit test is an input question. So this reports gestures and the surfaces
 * above it decide, which is the same split `InputFrame` and `MapCameraInput`
 * already make one level down.
 *
 * **A latched state, not an event queue**, which is `InputRouter`'s reasoning
 * applied here: the camera, the selection and the puck all read a frame today,
 * and handing them a queue would touch every one of them to buy a property
 * claims already provide. Edges (`tapped`, `longPressed`) sit beside levels
 * (`phase`, `dwellProgress`) for the reason `InputFrame` separates them -- a
 * tap fires once and a drag runs every frame it lasts.
 *
 * **Device-free**, which is what makes I1 the first slice of the input phase
 * rather than part of a screen: nothing here knows a window exists, so the
 * whole gesture vocabulary is testable before a touch display is.
 *
 * **The mouse arrives through it and is not special-cased here.** `Window`
 * fills contact zero from the left button, so a click is a tap and a
 * click-drag is a drag with no code in this file aware of either. What a mouse
 * cannot do is put down a *second* contact, and this file does not pretend
 * otherwise: `secondContactDown` is simply false on a desk, and the surfaces
 * that need a second finger sequence it differently there (the puck as
 * anchor-then-drag, Plan-of-Record §1).
 */

namespace Neuron
{

/*
 * What the primary contact is doing. One phase at a time, and the transitions
 * are the design:
 *
 *     None ── down ──▶ Pressing ── moved past slop ──▶ Dragging ── up ──▶ None
 *                          │                                 │
 *                          ├── dwell complete ──▶ Holding ── up ──▶ None
 *                          │
 *                          ├── up ──▶ (tapped) ──▶ None
 *                          │
 *                          └── second contact separates ──▶ Pinching ── up ──▶ None
 *
 * `Dragging` and `Holding` are the fork the whole model turns on: **drag pans,
 * long-press commands**, and which one a contact became is decided by whether
 * it moved before it dwelled. That is why the slop and the dwell are one
 * decision rather than two independent thresholds -- a finger that wanders
 * during the dwell wanted the camera, and one that stays wanted the order
 * surface.
 */
enum class GesturePhase : std::uint8_t
{
  None,
  Pressing,
  Dragging,
  Holding,
  Pinching
};

/*
 * Feel, in one place, in the same spirit as `CameraTuning`: starting points to
 * be tuned against a display and a hand, not decisions.
 */
struct GestureTuning
{
  /*
   * The dwell, from `puck-and-wheel.png`'s own step 1: **LONG-PRESS 350 ms**.
   *
   * Lifted from the print rather than picked, because it is one of the few
   * interaction numbers the corpus actually states, and a number invented here
   * would quietly become the one the screens were built against.
   */
  float longPressSeconds = 0.350f;

  /*
   * How far a contact may wander and still be the same press.
   *
   * A quarter of the 48 px target floor (ADR-020 §8): a finger that has moved
   * less than a quarter of the smallest thing it could be aiming at has not
   * changed its mind, and one that has moved more is going somewhere. Deriving
   * it from the floor rather than picking a pixel count is what keeps the two
   * numbers from drifting apart when the floor is re-argued.
   */
  float slopPixels = 12.0f;

  /*
   * How much the gap between two contacts must change before it is a pinch
   * rather than two fingers resting.
   *
   * Larger than the slop, and deliberately: a pinch cancels a dwell, so a
   * threshold at the slop would make a second finger landing slightly off
   * eat the long-press the player was in the middle of.
   */
  float pinchSlopPixels = 24.0f;
};

/*
 * This frame's answer.
 *
 * Positions are client-area pixels in floats -- the same units `InputRouter`
 * hands out for the cursor, so a surface testing a rect does not convert.
 */
struct GestureState
{
  GesturePhase phase = GesturePhase::None;

  /// Where the primary contact is, and where it went down. Both are meaningless
  /// while `phase` is `None`, and neither is cleared: a surface reads them only
  /// on the frames the phase says to.
  float x = 0.0f;
  float y = 0.0f;
  float originX = 0.0f;
  float originY = 0.0f;

  /// This frame's movement of the primary contact. Zero on the frame it went
  /// down, which is what stops a press at a new position reading as a drag
  /// across the screen from the last one.
  float deltaX = 0.0f;
  float deltaY = 0.0f;

  /*
   * A press that lifted without becoming anything else (an edge).
   *
   * `tapX`/`tapY` are where it *lifted*, not where it landed. They differ by up
   * to the slop, and the lift is the better answer: it is the last thing the
   * player saw their finger over.
   */
  bool tapped = false;
  float tapX = 0.0f;
  float tapY = 0.0f;

  /// The dwell completed this frame (an edge). `x`/`y` are where, and the phase
  /// is `Holding` from here until the contact lifts.
  bool longPressed = false;

  /*
   * How far through the dwell, 0..1 (a level).
   *
   * Non-zero only while `Pressing`, and non-zero from the **first frame** of
   * it: the print requires the dwell be "visible from the first millisecond --
   * a ring that fills -- so a player who did not mean to long-press knows to
   * lift". A surface that only learned about the dwell when it completed could
   * not draw that.
   */
  float dwellProgress = 0.0f;

  /*
   * A second finger is down (a level), and where.
   *
   * Two things in the design spend it and neither is a pinch: the puck's second
   * finger twists arrival facing, and a *held* second finger appends the order
   * to the queue rather than replacing it. Both are the surface's to interpret;
   * this only says that a second contact exists and where it is.
   */
  bool secondContactDown = false;
  float secondX = 0.0f;
  float secondY = 0.0f;

  /*
   * The pinch, while `phase` is `Pinching`.
   *
   * `pinchScale` is the current separation over the separation when the pinch
   * began, so 1 is unchanged, above 1 is spreading and below 1 is closing. A
   * ratio rather than a pixel delta because that is what a zoom wants and what
   * survives a change of display density.
   */
  float pinchScale = 1.0f;
  float pinchCentreX = 0.0f;
  float pinchCentreY = 0.0f;
};

/*
 * Follows contacts across frames and says what they meant.
 *
 * Holds the state a single frame cannot carry -- where a press began, how long
 * it has been down, what a pinch started at -- and nothing else. One instance
 * per window; it is not re-entrant and does not need to be.
 */
class GestureRecognizer
{
public:
  /*
   * Advances by one frame and returns this frame's answer.
   *
   * `_deltaSeconds` is the frame's own elapsed time, the same argument
   * `MapCameraInput` takes and for the same reason: the dwell is measured in
   * seconds and this file may not read a clock, so the caller who already has
   * one passes it.
   *
   * **An unfocused frame cancels everything.** A finger the window stopped
   * hearing about is a finger that may already have lifted, and a dwell that
   * completed while the player was in another application would fire an order
   * they did not give.
   */
  const GestureState& Update(const InputFrame& _frame, const GestureTuning& _tuning, float _deltaSeconds) noexcept;

  /// This frame's answer again, for a second reader.
  [[nodiscard]] const GestureState& State() const noexcept { return m_state; }

  /*
   * Drops any gesture in progress without reporting it.
   *
   * For the cases where something above this decides the gesture is no longer
   * the player's to finish -- a surface opening under the finger, focus moving.
   * Deliberately not an edge: a cancelled gesture reports *nothing*, because a
   * tap or a long-press that fires as a side effect of being cancelled is the
   * bug this exists to make impossible.
   */
  void Cancel() noexcept;

private:
  /// Ends the gesture but keeps the edge that ended it -- a tap has to survive
  /// its own cleanup. See the note beside the definition.
  void End() noexcept;

  /// The contact this gesture is about, tracked by id so a frame in which two
  /// fingers moved cannot swap them.
  std::uint32_t m_primaryId = 0;
  std::uint32_t m_secondId = 0;
  bool m_havePrimary = false;
  bool m_haveSecond = false;

  float m_lastX = 0.0f;
  float m_lastY = 0.0f;
  float m_dwellSeconds = 0.0f;

  /// The separation the pinch is measured against, captured when the second
  /// contact arrived rather than when the pinch was recognised: a scale
  /// measured from the moment it crossed the threshold would jump.
  float m_pinchStartSeparation = 0.0f;

  GestureState m_state;
};

} // namespace Neuron
