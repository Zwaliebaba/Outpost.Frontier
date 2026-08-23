#include "pch.h"

#include "InputRouter.h"

namespace Neuron
{

void InputRouter::Begin(const InputFrame& _frame, const UiFocus& _focus) noexcept
{
  m_frame = _frame;

  const bool routes = _frame.windowFocused;
  m_pointerClaimed = !routes;
  m_wheelClaimed = !routes;
  m_keyboardClaimed = !routes;

  m_fieldHoldsFocus = _focus.Kind() == FocusKind::EditableField;
  m_captureHoldsFocus = _focus.ClaimsWholeKeyboard();

  // A gesture is per frame like everything else here. A caller that forgets to
  // hand one over gets `None` rather than last frame's, which is the safe
  // direction: a stale long-press is an order nobody gave.
  m_gesture = GestureState{};
}

const GestureState& InputRouter::Gesture() const noexcept
{
  return m_pointerClaimed ? m_noGesture : m_gesture;
}

bool InputRouter::Pressed(InputButton _button) const noexcept
{
  return !m_pointerClaimed && m_frame.Pressed(_button);
}

bool InputRouter::Released(InputButton _button) const noexcept
{
  return !m_pointerClaimed && m_frame.Released(_button);
}

bool InputRouter::Down(InputButton _button) const noexcept
{
  return !m_pointerClaimed && m_frame.Down(_button);
}

bool InputRouter::ClaimPointer() noexcept
{
  if (m_pointerClaimed)
  {
    return false;
  }
  m_pointerClaimed = true;
  return true;
}

bool InputRouter::ClaimPointerIn(const UiRect& _rect) noexcept
{
  if (!Pressed(InputButton::Left) || !_rect.Contains(CursorX(), CursorY()))
  {
    return false;
  }
  return ClaimPointer();
}

bool InputRouter::ClaimWheel() noexcept
{
  if (m_wheelClaimed)
  {
    return false;
  }
  m_wheelClaimed = true;
  return true;
}

float InputRouter::ClaimWheelIn(const UiRect& _rect) noexcept
{
  const float steps = WheelSteps();
  if (steps == 0.0f || !_rect.Contains(CursorX(), CursorY()))
  {
    // A rect with no notches over it does not claim: the wheel is still there
    // for whatever is underneath, and claiming on hover alone would stop the
    // camera zooming any time the cursor rested on a panel.
    return 0.0f;
  }
  (void)ClaimWheel();
  return steps;
}

bool InputRouter::Pressed(InputAction _action) const noexcept
{
  if (m_keyboardClaimed)
  {
    return false;
  }
  if (m_captureHoldsFocus || (m_fieldHoldsFocus && !ActionSurvivesTextEditing(_action)))
  {
    return false;
  }
  return m_frame.Pressed(_action);
}

bool InputRouter::Down(InputAction _action) const noexcept
{
  if (m_keyboardClaimed)
  {
    return false;
  }
  if (m_captureHoldsFocus || (m_fieldHoldsFocus && !ActionSurvivesTextEditing(_action)))
  {
    return false;
  }
  return m_frame.Down(_action);
}

bool InputRouter::ClaimKeyboard() noexcept
{
  if (m_keyboardClaimed)
  {
    return false;
  }
  m_keyboardClaimed = true;
  return true;
}

std::span<const char32_t> InputRouter::Characters() const noexcept
{
  if (!m_fieldHoldsFocus || m_keyboardClaimed)
  {
    return {};
  }
  return std::span<const char32_t>{m_frame.characters, m_frame.characterCount};
}

} // namespace Neuron
