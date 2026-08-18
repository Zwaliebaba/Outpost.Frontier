#include "pch.h"

#include "InputMap.h"

#include "IsoCamera.h"

namespace Neuron
{
namespace
{

/// Screen-edge push, as ADR-006 §4 lists alongside drag. Only while the window
/// has focus and the cursor is over it: an edge pan that runs because the mouse
/// happens to rest at the top of another monitor is a bug report.
[[nodiscard]] float EdgePush(std::int32_t _position, std::uint32_t _extent, std::int32_t _margin) noexcept
{
  if (_extent == 0)
  {
    return 0.0f;
  }
  if (_position <= _margin)
  {
    return -1.0f;
  }
  if (_position >= static_cast<std::int32_t>(_extent) - _margin - 1)
  {
    return 1.0f;
  }
  return 0.0f;
}

} // namespace

CameraIntent MapCameraInput(const InputFrame& _input, const CameraTuning& _tuning, float _deltaSeconds) noexcept
{
  CameraIntent intent;

  intent.zoomSteps = _input.wheelSteps;
  if (_input.Pressed(InputAction::ZoomIn))
  {
    intent.zoomSteps += 1.0f;
  }
  if (_input.Pressed(InputAction::ZoomOut))
  {
    intent.zoomSteps -= 1.0f;
  }

  if (_input.Pressed(InputAction::YawLeft))
  {
    --intent.yawDetentSteps;
  }
  if (_input.Pressed(InputAction::YawRight))
  {
    ++intent.yawDetentSteps;
  }

  intent.resetView = _input.Pressed(InputAction::ResetView);

  const bool orbiting = _input.Down(InputButton::Middle) && _input.Down(InputAction::Modifier);

  if (orbiting)
  {
    // Horizontal drag only. Elevation is fixed at 30 degrees (ADR-006 §4), so
    // there is nothing for a vertical drag to mean, and inventing one would
    // break the 2:1 ring geometry the overlay pass depends on.
    intent.orbitRadians = static_cast<float>(_input.cursorDeltaX) * _tuning.orbitRadiansPerPixel;
  }
  else if (_input.Down(InputButton::Middle))
  {
    // Grab-and-drag: the plane follows the cursor, so the focus moves against
    // it. Getting this sign wrong is the classic "the map fights me" feel.
    intent.panRightPixels -= static_cast<float>(_input.cursorDeltaX);
    intent.panUpPixels += static_cast<float>(_input.cursorDeltaY);
  }

  // Releasing an orbit drops onto the nearest detent -- the snap is what makes
  // 45 degrees a rule rather than a suggestion.
  if (_input.Released(InputButton::Middle) && _input.Down(InputAction::Modifier))
  {
    intent.snapYaw = true;
  }

  const float keyboardPan = _tuning.keyboardPanPixelsPerSecond * _deltaSeconds;
  if (_input.Down(InputAction::PanLeft))
  {
    intent.panRightPixels -= keyboardPan;
  }
  if (_input.Down(InputAction::PanRight))
  {
    intent.panRightPixels += keyboardPan;
  }
  if (_input.Down(InputAction::PanForward))
  {
    intent.panUpPixels += keyboardPan;
  }
  if (_input.Down(InputAction::PanBack))
  {
    intent.panUpPixels -= keyboardPan;
  }

  if (_tuning.edgePanEnabled && _input.windowFocused && _input.cursorInsideWindow && !_input.Down(InputButton::Middle))
  {
    const float edgePan = _tuning.edgePanPixelsPerSecond * _deltaSeconds;
    intent.panRightPixels += EdgePush(_input.cursorX, _input.viewportWidth, _tuning.edgePanMarginPixels) * edgePan;
    // Screen y grows downward, so the top edge pans forward.
    intent.panUpPixels -= EdgePush(_input.cursorY, _input.viewportHeight, _tuning.edgePanMarginPixels) * edgePan;
  }

  return intent;
}

void ApplyCameraIntent(IsoCamera& _camera, const CameraIntent& _intent) noexcept
{
  if (_intent.resetView)
  {
    _camera.SetFocus(DirectX::XMFLOAT2{0.0f, 0.0f});
    _camera.SetYawRadians(0.0f);
  }

  if (_intent.orbitRadians != 0.0f)
  {
    _camera.Orbit(_intent.orbitRadians);
  }
  if (_intent.snapYaw)
  {
    _camera.SnapYawToDetent();
  }
  if (_intent.yawDetentSteps != 0)
  {
    _camera.NudgeYawDetents(_intent.yawDetentSteps);
  }
  if (_intent.zoomSteps != 0.0f)
  {
    _camera.Zoom(_intent.zoomSteps);
  }

  // Last, and on purpose: pan is expressed in screen axes, and orbit and zoom
  // are what decide where those axes point and how far a pixel reaches.
  if (_intent.panRightPixels != 0.0f || _intent.panUpPixels != 0.0f)
  {
    _camera.PanPixels(_intent.panRightPixels, _intent.panUpPixels);
  }
}

} // namespace Neuron
