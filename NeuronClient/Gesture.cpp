#include "pch.h"

#include "Gesture.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

/// The contact with this id, or null once it has lifted. Searched rather than
/// indexed because a slot is not an identity: a finger that lifts leaves the
/// ones after it to move up.
[[nodiscard]] const PointerContact* Find(const InputFrame& _frame, std::uint32_t _id) noexcept
{
  const std::uint32_t count = std::min(_frame.contactCount, MAX_POINTER_CONTACTS);
  for (std::uint32_t index = 0; index < count; ++index)
  {
    const PointerContact& contact = _frame.contacts[index];
    if (contact.phase != PointerPhase::None && contact.id == _id)
    {
      return &contact;
    }
  }
  return nullptr;
}

/// The oldest contact this gesture is not already following. `contacts` is
/// oldest-first by `Window`'s contract, so the first match is the answer.
[[nodiscard]] const PointerContact* Oldest(const InputFrame& _frame, bool _haveA, std::uint32_t _a, bool _haveB,
                                           std::uint32_t _b) noexcept
{
  const std::uint32_t count = std::min(_frame.contactCount, MAX_POINTER_CONTACTS);
  for (std::uint32_t index = 0; index < count; ++index)
  {
    const PointerContact& contact = _frame.contacts[index];
    if (contact.phase == PointerPhase::None || contact.phase == PointerPhase::Up)
    {
      continue; // A contact that is leaving is not one to start following.
    }
    if ((_haveA && contact.id == _a) || (_haveB && contact.id == _b))
    {
      continue;
    }
    return &contact;
  }
  return nullptr;
}

[[nodiscard]] float Separation(const PointerContact& _a, const PointerContact& _b) noexcept
{
  const auto dx = static_cast<float>(_a.x - _b.x);
  const auto dy = static_cast<float>(_a.y - _b.y);
  return std::sqrt(dx * dx + dy * dy);
}

} // namespace

/*
 * Ends the gesture and keeps this frame's edges.
 *
 * The distinction between this and `Cancel` is the whole of a bug that a test
 * caught on its first run: a tap *ends* the gesture, and clearing the state
 * wholesale on the way out erased the `tapped` flag the same frame had just
 * set. So ending and cancelling are two operations, and only one of them is
 * allowed to say nothing happened.
 */
void GestureRecognizer::End() noexcept
{
  m_havePrimary = false;
  m_haveSecond = false;
  m_dwellSeconds = 0.0f;
  m_pinchStartSeparation = 0.0f;
  m_state.phase = GesturePhase::None;
  m_state.dwellProgress = 0.0f;
  m_state.secondContactDown = false;
  m_state.pinchScale = 1.0f;
}

void GestureRecognizer::Cancel() noexcept
{
  End();
  m_state.tapped = false;
  m_state.tapCount = 0;
  m_state.longPressed = false;
  m_state.deltaX = 0.0f;
  m_state.deltaY = 0.0f;

  // And the run is forgotten. A gesture that was taken away from the player
  // mid-way is not a tap for the next one to pair with -- a double tap
  // straddling a cancel is two unrelated presses.
  m_tapRun = 0;
}

const GestureState& GestureRecognizer::Update(const InputFrame& _frame, const GestureTuning& _tuning, float _deltaSeconds) noexcept
{
  /*
   * The edges are this frame's and nothing else's, so they are cleared before
   * anything can set them. The phase and the positions are *not*: they describe
   * a gesture that outlives a frame, and clearing them here would make every
   * drag one frame long.
   */
  m_state.tapped = false;
  m_state.tapCount = 0;
  m_state.longPressed = false;
  m_state.deltaX = 0.0f;
  m_state.deltaY = 0.0f;

  /*
   * The gap since the last tap, aged before anything can return early.
   *
   * A double tap is two taps with *nothing* between them, and the frames
   * between are exactly the ones where there is no contact to process -- so
   * counting only on frames that reach the state machine would make the window
   * depend on how fast the player lifted.
   */
  m_sinceTapSeconds += std::max(0.0f, _deltaSeconds);

  if (!_frame.windowFocused)
  {
    // A finger the window stopped hearing about may already have lifted, and a
    // dwell that completed elsewhere would fire an order nobody gave.
    Cancel();
    return m_state;
  }

  // --- the primary contact -------------------------------------------------

  const PointerContact* primary = m_havePrimary ? Find(_frame, m_primaryId) : nullptr;
  if (m_havePrimary && primary == nullptr)
  {
    /*
     * The contact vanished without an `Up`, which a well-behaved window does
     * not do -- but a lost capture or a device reset does, and the honest
     * response is the same as a cancel: report nothing. A tap synthesised from
     * a contact that disappeared is a click the player never made.
     */
    Cancel();
    return m_state;
  }

  if (primary == nullptr)
  {
    // Nothing followed yet. Take the oldest contact that has just arrived or is
    // still down; a frame with none leaves the phase at `None`.
    primary = Oldest(_frame, false, 0, false, 0);
    if (primary == nullptr)
    {
      m_state.phase = GesturePhase::None;
      m_state.dwellProgress = 0.0f;
      m_state.secondContactDown = false;
      return m_state;
    }

    m_havePrimary = true;
    m_primaryId = primary->id;
    m_haveSecond = false;
    m_dwellSeconds = 0.0f;
    m_state.phase = GesturePhase::Pressing;
    m_state.originX = static_cast<float>(primary->x);
    m_state.originY = static_cast<float>(primary->y);
    m_state.x = m_state.originX;
    m_state.y = m_state.originY;
    m_state.dwellProgress = 0.0f;
    m_state.pinchScale = 1.0f;
    m_lastX = m_state.x;
    m_lastY = m_state.y;
  }
  else
  {
    m_state.deltaX = static_cast<float>(primary->x) - m_lastX;
    m_state.deltaY = static_cast<float>(primary->y) - m_lastY;
    m_state.x = static_cast<float>(primary->x);
    m_state.y = static_cast<float>(primary->y);
    m_lastX = m_state.x;
    m_lastY = m_state.y;
  }

  // --- the second contact --------------------------------------------------

  const PointerContact* second = m_haveSecond ? Find(_frame, m_secondId) : nullptr;
  if (m_haveSecond && second == nullptr)
  {
    /*
     * The second finger lifted. The gesture does not end with it -- the primary
     * is still down and still means whatever it meant -- but a pinch has
     * nothing left to measure, so it falls back to a drag rather than freezing
     * at its last scale.
     */
    m_haveSecond = false;
    m_pinchStartSeparation = 0.0f;
    if (m_state.phase == GesturePhase::Pinching)
    {
      m_state.phase = GesturePhase::Dragging;
      m_state.pinchScale = 1.0f;
    }
  }

  if (second == nullptr && m_state.phase != GesturePhase::None)
  {
    second = Oldest(_frame, true, m_primaryId, false, 0);
    if (second != nullptr)
    {
      m_haveSecond = true;
      m_secondId = second->id;
      m_pinchStartSeparation = Separation(*primary, *second);
    }
  }

  m_state.secondContactDown = second != nullptr;
  if (second != nullptr)
  {
    m_state.secondX = static_cast<float>(second->x);
    m_state.secondY = static_cast<float>(second->y);
  }

  // --- what it became ------------------------------------------------------

  const bool lifted = primary->phase == PointerPhase::Up;

  switch (m_state.phase)
  {
  case GesturePhase::Pressing:
  {
    /*
     * A pinch is tested before the dwell, because it is the one thing that can
     * take a press away from the order surfaces: two fingers separating is a
     * player zooming, and letting the dwell complete underneath would open a
     * puck in the middle of it.
     */
    if (second != nullptr && m_pinchStartSeparation > 0.0f)
    {
      const float separation = Separation(*primary, *second);
      if (std::fabs(separation - m_pinchStartSeparation) > _tuning.pinchSlopPixels)
      {
        m_state.phase = GesturePhase::Pinching;
        m_state.dwellProgress = 0.0f;
        m_dwellSeconds = 0.0f;
        break;
      }
    }

    const float movedX = m_state.x - m_state.originX;
    const float movedY = m_state.y - m_state.originY;
    const bool wandered = std::sqrt(movedX * movedX + movedY * movedY) > _tuning.slopPixels;

    if (lifted)
    {
      /*
       * Up before it became anything else. A press that wandered past the slop
       * and lifted in the same frame is a very short drag rather than a tap --
       * the player moved, and a tap at the lift point would act on somewhere
       * they had left.
       */
      if (!wandered)
      {
        /*
         * A double is this tap plus the last one, if the last one was recent
         * and near. Both conditions, because either alone is a coincidence: two
         * taps a second apart on one row are two decisions, and two taps at
         * opposite corners of the screen are not aimed at the same thing
         * however fast they came.
         */
        const float fromLastX = m_state.x - m_lastTapX;
        const float fromLastY = m_state.y - m_lastTapY;
        const bool pairs = m_tapRun > 0 && m_sinceTapSeconds <= _tuning.doubleTapSeconds &&
                           std::sqrt(fromLastX * fromLastX + fromLastY * fromLastY) <= _tuning.doubleTapSlopPixels;

        m_tapRun = pairs ? m_tapRun + 1u : 1u;
        m_sinceTapSeconds = 0.0f;
        m_lastTapX = m_state.x;
        m_lastTapY = m_state.y;

        m_state.tapped = true;
        m_state.tapCount = m_tapRun;
        m_state.tapX = m_state.x;
        m_state.tapY = m_state.y;
      }
      End();
      return m_state;
    }

    if (wandered)
    {
      m_state.phase = GesturePhase::Dragging;
      m_state.dwellProgress = 0.0f;
      m_dwellSeconds = 0.0f;
      break;
    }

    // The dwell, and it is reported before it completes: the print's ring fills
    // from the first millisecond so a player who did not mean this can lift.
    m_dwellSeconds += std::max(0.0f, _deltaSeconds);
    if (_tuning.longPressSeconds <= 0.0f || m_dwellSeconds >= _tuning.longPressSeconds)
    {
      m_state.phase = GesturePhase::Holding;
      m_state.dwellProgress = 1.0f;
      m_state.longPressed = true;
      break;
    }
    m_state.dwellProgress = std::min(1.0f, m_dwellSeconds / _tuning.longPressSeconds);
    break;
  }

  case GesturePhase::Dragging:
  case GesturePhase::Holding:
    // Both run until the finger lifts. What the movement *means* differs --
    // one pans the camera and the other places a puck -- and that is the
    // surface's question, not this one's.
    if (lifted)
    {
      End();
      return m_state;
    }
    break;

  case GesturePhase::Pinching:
    if (lifted)
    {
      End();
      return m_state;
    }
    break;

  case GesturePhase::None:
    break;
  }

  /*
   * The pinch's numbers, computed after the switch rather than inside its own
   * case, because the frame that *enters* `Pinching` needs them too -- and a
   * scale of 1 on the first frame of a pinch is a zoom that jumps on the
   * second. The test that caught it asserts the scale on the entry frame for
   * exactly that reason.
   */
  if (m_state.phase == GesturePhase::Pinching && second != nullptr && m_pinchStartSeparation > 0.0f)
  {
    m_state.pinchScale = Separation(*primary, *second) / m_pinchStartSeparation;
    m_state.pinchCentreX = 0.5f * (m_state.x + m_state.secondX);
    m_state.pinchCentreY = 0.5f * (m_state.y + m_state.secondY);
  }

  return m_state;
}

} // namespace Neuron
