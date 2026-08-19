#include "pch.h"

#include "AudioListener.h"

#include <cmath>

namespace Neuron
{

using namespace DirectX;

AudioListenerState MakeAudioListener(const XMFLOAT2& _focusMetres, float _yawRadians, float _zoomHalfHeightMetres) noexcept
{
  AudioListenerState listener;

  // The focus point, lifted. A negative zoom is not a camera state this can be
  // reached from, but a listener below the plane would invert the field, so the
  // lift is floored at zero rather than trusted.
  const float elevation = std::max(0.0f, _zoomHalfHeightMetres) * LISTENER_ELEVATION_PER_ZOOM;
  listener.position = XMFLOAT3{_focusMetres.x, elevation, _focusMetres.y};

  // `IsoCamera::ScreenUpOnPlane()` lifted into three dimensions -- the view
  // direction flattened onto the plane, pointing away from the camera. Kept in
  // step with that function by construction rather than by comment: both are
  // (sin yaw, cos yaw), and a camera whose yaw convention changed would have to
  // change both, which is what the projection test in NeuronClientTests exists
  // to catch.
  const float sinYaw = std::sin(_yawRadians);
  const float cosYaw = std::cos(_yawRadians);
  listener.front = XMFLOAT3{sinYaw, 0.0f, cosYaw};
  listener.top = XMFLOAT3{0.0f, 1.0f, 0.0f};

  return listener;
}

XMFLOAT3 MakeAudioEmitter(const XMFLOAT2& _planeMetres, float _hoverMetres) noexcept
{
  // x east, y the cosmetic height, z north -- the same mapping `BuildScene`
  // uses for the instance it draws.
  return XMFLOAT3{_planeMetres.x, _hoverMetres, _planeMetres.y};
}

float AudioDistanceMetres(const AudioListenerState& _listener, const XMFLOAT3& _emitter) noexcept
{
  const XMVECTOR listener = XMLoadFloat3(&_listener.position);
  const XMVECTOR emitter = XMLoadFloat3(&_emitter);
  return XMVectorGetX(XMVector3Length(XMVectorSubtract(emitter, listener)));
}

} // namespace Neuron
