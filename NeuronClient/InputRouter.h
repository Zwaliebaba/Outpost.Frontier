#pragma once

#include "Gesture.h"
#include "InputMap.h"
#include "UiDrawList.h"
#include "UiFocus.h"

#include <cstdint>
#include <span>

/*
 * Who gets this frame's input (ADR-020 §2).
 *
 * Until T3 the whole of this was one bool -- `m_uiConsumedPress`, "the press
 * landed on chrome above the world" -- which was right for a client with one
 * surface and no focus owner. A screen that navigates needs the general form,
 * and the general form is small: **a claim over a latched frame**, not an event
 * queue.
 *
 * That distinction is the design. `InputFrame` is what the camera, the
 * selection and the puck already read; converting it into a queue of events
 * would touch every one of those consumers to buy a property that claims
 * already provide. So the frame stays what it is, and this wraps it with three
 * flags.
 *
 * **Three channels, claimed independently: pointer, wheel, keyboard.** A wheel
 * notch over the hangar's wing column must not also cost the click still
 * pending in the same frame -- they are two things the player did, and one
 * consuming the other is a click that silently disappears.
 *
 * **The order is the caller's, and it is: focused widget → active surface →
 * cross-surface layers → world.** Layers sit after the surface because the two
 * may not contend for a pixel: the toast zones and the strip's clamp are
 * already written so they never overlap a surface's controls. There is exactly
 * one declared exception -- the debug strip is draggable and can land over
 * another surface's control, so *while being dragged, and on its drag handle
 * only*, it asks ahead of the surface. Any layer needing a wider exception is a
 * defect in the zone tables rather than a routing question.
 *
 * **The keyboard order is the rule that had to be written down**, because "W"
 * must type or pan and never both. While an editable field holds focus it takes
 * the text channel and the editing keys, and no action bound to a printable key
 * survives to a later stage; F1 and Escape, bound to keys no field can consume
 * as text, still route past it. That is `ActionSurvivesTextEditing` below, and
 * it is a table rather than a habit precisely so that adding an action forces
 * somebody to answer the question for it.
 */

namespace Neuron
{

/*
 * Whether this action still reaches later stages while an editable field holds
 * focus.
 *
 * **Read the key, not the verb.** The table below is a fact about `Window`'s
 * binding table (`Window.cpp`'s `SetKey`), and the two must be read together:
 * `PanForward` is `W` *and* `↑`, so a field takes it twice over -- `W` is a
 * character and `↑` moves the caret -- and `ResetView` is `Home`, which is an
 * editing key with a camera binding rather than the other way round. Every
 * suppressed action here is suppressed on both counts, which is why the rule
 * can be per action even though printability is per key.
 *
 * What survives is what a field can make no use of: the modifiers, which
 * qualify pointer gestures and mean nothing typed on their own; the two
 * diagnostics F-keys, which ADR-020 §2 names explicitly ("F1 still works while
 * typing"); and `Back`, which is routed rather than suppressed -- the field sees
 * Escape first and claims the channel when it cancels an edit with it.
 *
 * A **binding-capture** control is not this case: it claims the whole keyboard
 * channel while armed, F-keys included, because the next key pressed is the
 * answer it is waiting for. That is `UiFocus::ClaimsWholeKeyboard`, handled in
 * the router rather than here.
 */
[[nodiscard]] constexpr bool ActionSurvivesTextEditing(InputAction _action) noexcept
{
  switch (_action)
  {
  case InputAction::Modifier:
  case InputAction::SelectAdd:
  case InputAction::QueueOrder:
  case InputAction::ToggleDiagnostics:
  case InputAction::ToggleFeedFreeze:
  case InputAction::Back:
    return true;

  case InputAction::PanLeft:
  case InputAction::PanRight:
  case InputAction::PanForward:
  case InputAction::PanBack:
  case InputAction::YawLeft:
  case InputAction::YawRight:
  case InputAction::ZoomIn:
  case InputAction::ZoomOut:
  case InputAction::ResetView:
  case InputAction::CycleParameter:
    return false;

  /*
   * `Confirm` is the field's own key and reaches nothing behind it.
   *
   * Suppressed rather than routed, which is the opposite answer from `Back` and
   * for a reason the two do not share: Escape means something to a *surface*
   * as well as to a field -- go back -- so it has to reach one when no field
   * wants it. Enter means nothing to any surface in this client, so a rule that
   * let it through would be a rule reserving a key for a future somebody.
   */
  case InputAction::Confirm:
    return false;
  }
  return false; // Unreachable for a value from the enumeration; see below.
}

/// The switch above is exhaustive over the enumeration as it stands, and an
/// action added without a case falls through to "suppressed" -- which is the
/// safe direction but a silent one. This is the noisy version: adding an action
/// fails the build here until somebody has decided what it does while a player
/// is typing.
static_assert(INPUT_ACTION_COUNT == 17,
              "ActionSurvivesTextEditing must answer for every InputAction -- add the new one to its switch");

class InputRouter
{
public:
  /*
   * Latches this frame and takes the focus owner's word for what the keyboard
   * looks like.
   *
   * Focus is read once, here, rather than consulted per query: a field that
   * lost focus part-way through a frame's routing would otherwise make the
   * first half of that frame's keys mean something different from the second
   * half.
   *
   * **An unfocused window routes nothing at all.** Every channel starts claimed,
   * so no stage sees a key edge or a click that belongs to whatever the player
   * alt-tabbed to. The caller still has work to do about it -- clearing focus
   * and cancelling an in-flight drag -- which `WindowDeactivated` is for.
   */
  void Begin(const InputFrame& _frame, const UiFocus& _focus) noexcept;

  /*
   * What this frame's contacts meant (I1, ADR-020's 2026-08-22 amendment).
   *
   * Recognised outside and handed in, rather than tracked here: a gesture spans
   * frames and this object is rebuilt every one of them. The caller owns a
   * `GestureRecognizer`, updates it once, and passes the answer to `Begin`'s
   * companion here so that every stage reads the same one.
   *
   * **Gated by the pointer claim, exactly like the buttons are.** A long-press
   * that landed on chrome must not also reach the world, and a stage that has
   * claimed the pointer is the one stage entitled to the gesture. That is why
   * this is on the router at all rather than read straight off the recognizer:
   * the claim is the mechanism, and a second way in would route around it.
   */
  void NoteGesture(const GestureState& _gesture) noexcept { m_gesture = _gesture; }

  /*
   * This frame's gesture, or a `None` one once the pointer has been claimed.
   *
   * A later stage asks the same question and gets a different answer, which is
   * the whole mechanism -- the same sentence the button edges carry.
   */
  [[nodiscard]] const GestureState& Gesture() const noexcept;

  /// The latched frame, for everything a claim does not gate: the cursor
  /// position, the viewport size, whether the cursor is inside the window.
  [[nodiscard]] const InputFrame& Frame() const noexcept { return m_frame; }

  [[nodiscard]] float CursorX() const noexcept { return static_cast<float>(m_frame.cursorX); }
  [[nodiscard]] float CursorY() const noexcept { return static_cast<float>(m_frame.cursorY); }

  /// True when this frame arrived with the window not in the foreground. The
  /// caller's cue to clear focus and cancel any drag (ADR-020 §3).
  [[nodiscard]] bool WindowDeactivated() const noexcept { return !m_frame.windowFocused; }

  // --- the pointer channel ------------------------------------------------

  [[nodiscard]] bool PointerAvailable() const noexcept { return !m_pointerClaimed; }

  /// This frame's button edges and levels, or nothing once the pointer has been
  /// claimed. A later stage asks the same question and gets a different answer,
  /// which is the whole mechanism.
  [[nodiscard]] bool Pressed(InputButton _button) const noexcept;
  [[nodiscard]] bool Released(InputButton _button) const noexcept;
  [[nodiscard]] bool Down(InputButton _button) const noexcept;

  /// Takes the pointer for the rest of the frame. False when somebody already
  /// had it, so a stage can tell "I took this" from "I was too late".
  bool ClaimPointer() noexcept;

  /*
   * Takes the pointer if a left press landed inside `_rect`.
   *
   * The common case in one call, because writing it out at each site is how a
   * stage ends up testing the rect and forgetting the claim -- a button that
   * works and also box-selects the fleet behind it.
   *
   * **This is the mouse-only door, and after R30's conversion nothing in this
   * client uses it.** It is kept rather than deleted for the reason `PickBox`
   * was: a left press is still a real thing a desk produces, and removing the
   * only way to ask about one would turn "the chrome answers a finger" into "the
   * chrome refuses a mouse". The order puck's right-button sequence is the case
   * that would want it back (I3).
   */
  bool ClaimPointerIn(const UiRect& _rect) noexcept;

  /*
   * Takes the pointer if this frame's **tap** lifted inside `_rect` (R30).
   *
   * `ClaimPointerIn`'s replacement everywhere a control is actuated, and the
   * difference is the whole of R30: a press edge is a mouse's word for "the
   * button went down", and a finger has no button. A tap is what both a finger
   * and a mouse produce through the same seam -- `Window` fills contact zero
   * from the left button, so a click *is* a tap and no site below needs to know
   * which one it got.
   *
   * **The lift point, not the cursor.** `GestureState::tapX/tapY` are where the
   * contact came up, which for a mouse is where the cursor is and for a finger
   * is the last place the player saw it. Testing the cursor instead would work
   * on a desk and put a finger's answer up to a slop's distance away from where
   * it was aimed.
   *
   * Three consequences worth stating, because all three are behaviour changes
   * rather than refactors. A control now acts on **release** rather than on
   * press, so a player who presses a button and slides off it before lifting is
   * no longer committed -- which is the touch idiom and is strictly the kinder
   * of the two. A **drag that begins on a control does nothing at all**, where a
   * press used to fire immediately: that is the same rule the camera pan already
   * carries (a gesture may only begin where it is allowed to), pointing at the
   * chrome instead of at the world.
   *
   * And a **hold past the dwell is not a tap**, so a slow press on a button that
   * has no long-press meaning actuates nothing. That is the recogniser's
   * vocabulary rather than this method's opinion -- `Pressing` becomes `Holding`
   * at 350 ms and never becomes a tap -- and it is deliberate: the roster row
   * and the hangar's wing headers both give a hold its own meaning, so a hold
   * that also actuated the control under it would make those two ambiguous. The
   * station surface has behaved this way since T3's remainder; this is the rest
   * of the chrome joining it rather than a new rule.
   */
  bool ClaimTapIn(const UiRect& _rect) noexcept;

  // --- the wheel channel --------------------------------------------------

  [[nodiscard]] bool WheelAvailable() const noexcept { return !m_wheelClaimed; }

  /// Notches this frame, positive away from the user, or zero once claimed.
  [[nodiscard]] float WheelSteps() const noexcept { return m_wheelClaimed ? 0.0f : m_frame.wheelSteps; }

  bool ClaimWheel() noexcept;

  /*
   * Takes the wheel if the cursor is inside `_rect`, returning the notches
   * taken -- zero when it was not, or when there were none.
   *
   * A list under the cursor claims the wheel before the camera's zoom sees it
   * (ADR-020 §4), and it does so by *position* rather than by a press: a wheel
   * has no press to land, and hovering is the whole gesture.
   */
  [[nodiscard]] float ClaimWheelIn(const UiRect& _rect) noexcept;

  // --- the keyboard channel -----------------------------------------------

  [[nodiscard]] bool KeyboardAvailable() const noexcept { return !m_keyboardClaimed; }

  /// This frame's action edges and levels, filtered by the focus owner and by
  /// any claim. An action a focused field swallows reports false here, which is
  /// what makes "W types or pans, never both" true by construction.
  [[nodiscard]] bool Pressed(InputAction _action) const noexcept;
  [[nodiscard]] bool Down(InputAction _action) const noexcept;

  /*
   * Takes the rest of the frame's keyboard.
   *
   * The narrow case it exists for: a focused field consuming Escape to cancel
   * its edit, so the same press does not also take the surface back a screen.
   * Coarse on purpose -- for the one frame a player pressed Escape, F1 waiting
   * a frame is not a defect worth a second set of flags.
   */
  bool ClaimKeyboard() noexcept;

  /*
   * What was typed this frame, for the focus owner to read.
   *
   * Empty unless an **editable field** holds focus, which makes "at most one
   * focus owner" structural rather than a convention somebody has to keep: a
   * widget that is not the owner cannot read a character even by asking.
   */
  [[nodiscard]] std::span<const char32_t> Characters() const noexcept;

private:
  InputFrame m_frame;

  /// This frame's gesture, and the answer handed out once the pointer is
  /// claimed. A member rather than a constant so `Gesture()` can return a
  /// reference either way.
  GestureState m_gesture;
  GestureState m_noGesture;

  bool m_pointerClaimed = true;
  bool m_wheelClaimed = true;
  bool m_keyboardClaimed = true;

  /// What the focus owner does to the keyboard, resolved once in `Begin`.
  bool m_fieldHoldsFocus = false;
  bool m_captureHoldsFocus = false;
};

} // namespace Neuron
