#pragma once

#include <cstdint>

/*
 * Input, and what the camera makes of it.
 *
 * Two halves, deliberately separated:
 *
 *  - `InputFrame` is what happened this frame, already reduced to logical
 *    buttons and actions. Window fills it from Win32 messages, because that is
 *    where the virtual-key codes belong.
 *  - `MapCameraInput` turns that into a `CameraIntent`. It is a pure function
 *    of the frame, the tuning and the elapsed time, so the binding scheme can
 *    be tested without a window and without a device.
 *
 * The bindings are chosen to survive the slices that follow. Left-drag becomes
 * box-select (S8) and right-drag becomes the order puck (S9), so neither is
 * spent here: the camera takes the middle button, the wheel, the screen edge
 * and the keyboard, and Alt is what turns a middle-drag from a pan into an
 * orbit. Picking a binding now that S9 has to take back would be a worse kind
 * of cheap.
 */

namespace Neuron
{

class IsoCamera;

enum class InputButton : std::uint8_t
{
  Left,
  Right,
  Middle
};

inline constexpr std::uint32_t INPUT_BUTTON_COUNT = 3;

/*
 * Logical actions, not keys. Window owns the virtual-key table; everything
 * downstream of it speaks in intent, which is what makes rebinding a change in
 * one file rather than a search across the client.
 */
enum class InputAction : std::uint8_t
{
  PanLeft,
  PanRight,
  PanForward,
  PanBack,
  YawLeft,
  YawRight,
  ZoomIn,
  ZoomOut,
  ResetView,
  Modifier,  // Alt: turns a middle-drag into an orbit.
  SelectAdd  // Shift: a click adjusts the selection instead of replacing it.
};

inline constexpr std::uint32_t INPUT_ACTION_COUNT = 11;

/// One frame of input, already reduced to logical state. Edges (`pressed`,
/// `released`) are separate from levels (`down`) because a detent nudge must
/// fire once per press and a pan must run every frame the key is held.
struct InputFrame
{
  std::int32_t cursorX = 0; // Client-area pixels, origin top-left.
  std::int32_t cursorY = 0;
  std::int32_t cursorDeltaX = 0;
  std::int32_t cursorDeltaY = 0;

  /// Wheel notches this frame, positive away from the user. Fractional on
  /// high-resolution wheels, and kept fractional -- rounding makes a smooth
  /// wheel feel notched twice.
  float wheelSteps = 0.0f;

  bool buttonDown[INPUT_BUTTON_COUNT] = {};
  bool buttonPressed[INPUT_BUTTON_COUNT] = {};
  bool buttonReleased[INPUT_BUTTON_COUNT] = {};

  bool actionDown[INPUT_ACTION_COUNT] = {};
  bool actionPressed[INPUT_ACTION_COUNT] = {};

  std::uint32_t viewportWidth = 0;
  std::uint32_t viewportHeight = 0;
  bool windowFocused = false;
  bool cursorInsideWindow = false;

  [[nodiscard]] bool Down(InputButton _button) const noexcept { return buttonDown[static_cast<std::uint32_t>(_button)]; }
  [[nodiscard]] bool Pressed(InputButton _button) const noexcept { return buttonPressed[static_cast<std::uint32_t>(_button)]; }
  [[nodiscard]] bool Released(InputButton _button) const noexcept { return buttonReleased[static_cast<std::uint32_t>(_button)]; }
  [[nodiscard]] bool Down(InputAction _action) const noexcept { return actionDown[static_cast<std::uint32_t>(_action)]; }
  [[nodiscard]] bool Pressed(InputAction _action) const noexcept { return actionPressed[static_cast<std::uint32_t>(_action)]; }
};

/// Feel, in one place. Values are starting points to be tuned against the
/// prints, not decisions -- which is why they are here and not spread through
/// the mapping code.
struct CameraTuning
{
  float orbitRadiansPerPixel = 0.006f;      // A screen width is a little over half a turn.
  float keyboardPanPixelsPerSecond = 900.0f;
  float edgePanPixelsPerSecond = 700.0f;
  std::int32_t edgePanMarginPixels = 8;
  bool edgePanEnabled = true;
};

/// What the camera should do about it. Deltas, not state: the camera owns the
/// state, and an intent that carried absolutes would be a second copy of it.
struct CameraIntent
{
  float orbitRadians = 0.0f;
  float zoomSteps = 0.0f;
  float panRightPixels = 0.0f;
  float panUpPixels = 0.0f;
  std::int32_t yawDetentSteps = 0;
  bool snapYaw = false;
  bool resetView = false;
};

[[nodiscard]] CameraIntent MapCameraInput(const InputFrame& _input, const CameraTuning& _tuning, float _deltaSeconds) noexcept;

/// Applies an intent in the order the player perceives it: orbit and zoom
/// change what "right" and "up" mean, so panning last is the difference between
/// a drag that tracks the cursor and one that lags a frame behind it.
void ApplyCameraIntent(IsoCamera& _camera, const CameraIntent& _intent) noexcept;

} // namespace Neuron
