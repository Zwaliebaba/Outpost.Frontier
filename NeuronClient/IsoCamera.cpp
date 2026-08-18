#include "pch.h"

#include "IsoCamera.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

using namespace DirectX;

constexpr float ELEVATION_RADIANS = IsoCamera::ELEVATION_DEGREES * XM_PI / 180.0f;

/// sin(30 degrees) = 0.5 exactly, and the whole overlay geometry leans on it.
/// Spelled as the trig rather than as 0.5 so changing the elevation -- which
/// ADR-006 forbids, but a fork may not -- moves everything together.
const float ELEVATION_SINE = std::sin(ELEVATION_RADIANS);
const float ELEVATION_COSINE = std::cos(ELEVATION_RADIANS);

[[nodiscard]] float WrapAngle(float _radians) noexcept
{
  // XMScalarModAngle brings any angle into (-pi, pi], which keeps the stored
  // yaw from drifting to a magnitude where float32 loses angular resolution.
  return XMScalarModAngle(_radians);
}

} // namespace

void IsoCamera::SetViewport(std::uint32_t _widthPixels, std::uint32_t _heightPixels) noexcept
{
  // A zero-sized viewport is a minimised window, not a camera state: keeping
  // the last real size means the projection never divides by zero.
  if (_widthPixels != 0 && _heightPixels != 0)
  {
    m_viewportWidth = _widthPixels;
    m_viewportHeight = _heightPixels;
  }
}

void IsoCamera::SetFocus(const XMFLOAT2& _focusMetres) noexcept
{
  m_focusMetres.x = std::clamp(_focusMetres.x, -PLAY_AREA_HALF_EXTENT_METRES, PLAY_AREA_HALF_EXTENT_METRES);
  m_focusMetres.y = std::clamp(_focusMetres.y, -PLAY_AREA_HALF_EXTENT_METRES, PLAY_AREA_HALF_EXTENT_METRES);
}

void IsoCamera::SetYawRadians(float _yawRadians) noexcept
{
  m_yawRadians = WrapAngle(_yawRadians);
}

void IsoCamera::SetZoomMetres(float _halfHeightMetres) noexcept
{
  m_zoomMetres = std::clamp(_halfHeightMetres, MIN_ZOOM_METRES, MAX_ZOOM_METRES);
}

void IsoCamera::SetYawSnapDegrees(float _degrees) noexcept
{
  m_yawSnapRadians = _degrees > 0.0f ? _degrees * XM_PI / 180.0f : 0.0f;
}

void IsoCamera::Orbit(float _deltaRadians) noexcept
{
  SetYawRadians(m_yawRadians + _deltaRadians);
}

void IsoCamera::SnapYawToDetent() noexcept
{
  if (m_yawSnapRadians <= 0.0f)
  {
    return;
  }
  const float detents = std::round(m_yawRadians / m_yawSnapRadians);
  SetYawRadians(detents * m_yawSnapRadians);
}

void IsoCamera::NudgeYawDetents(std::int32_t _steps) noexcept
{
  if (m_yawSnapRadians <= 0.0f)
  {
    return;
  }

  // Round to the detent the camera is nearest *before* stepping, so a nudge
  // after a free orbit lands on a detent rather than carrying the drag's
  // leftover fraction forward for ever.
  const float detents = std::round(m_yawRadians / m_yawSnapRadians) + static_cast<float>(_steps);
  SetYawRadians(detents * m_yawSnapRadians);
}

void IsoCamera::Zoom(float _steps) noexcept
{
  // Multiplicative: one notch should feel the same at 600 m as at 30 km, and a
  // linear step cannot do that across a 80:1 range.
  SetZoomMetres(m_zoomMetres * std::pow(ZOOM_STEP_FACTOR, -_steps));
}

void IsoCamera::PanPixels(float _rightPixels, float _upPixels) noexcept
{
  const float metresPerPixel = MetresPerPixel();
  const XMFLOAT2 right = ScreenRightOnPlane();
  const XMFLOAT2 up = ScreenUpOnPlane();

  // Screen-up is foreshortened by sin(elevation) on the way to the screen, so
  // undoing it on the way back costs 1/sin(elevation) -- 2x at 30 degrees.
  const float rightMetres = _rightPixels * metresPerPixel;
  const float upMetres = _upPixels * metresPerPixel / ELEVATION_SINE;

  SetFocus(XMFLOAT2{m_focusMetres.x + right.x * rightMetres + up.x * upMetres,
                    m_focusMetres.y + right.y * rightMetres + up.y * upMetres});
}

float IsoCamera::AspectRatio() const noexcept
{
  return static_cast<float>(m_viewportWidth) / static_cast<float>(m_viewportHeight);
}

float IsoCamera::MetresPerPixel() const noexcept
{
  return (2.0f * m_zoomMetres) / static_cast<float>(m_viewportHeight);
}

XMFLOAT3 IsoCamera::EyePosition() const noexcept
{
  // Yaw 0 puts the eye due south of the focus looking north, which is the
  // orientation the prints are drawn in.
  const float sinYaw = std::sin(m_yawRadians);
  const float cosYaw = std::cos(m_yawRadians);

  return XMFLOAT3{m_focusMetres.x - sinYaw * ELEVATION_COSINE * EYE_DISTANCE_METRES, ELEVATION_SINE * EYE_DISTANCE_METRES,
                  m_focusMetres.y - cosYaw * ELEVATION_COSINE * EYE_DISTANCE_METRES};
}

XMFLOAT2 IsoCamera::ScreenRightOnPlane() const noexcept
{
  // right = normalize(up x forward). At yaw 0 the camera faces north and this
  // is due east, as a player expects.
  return XMFLOAT2{std::cos(m_yawRadians), -std::sin(m_yawRadians)};
}

XMFLOAT2 IsoCamera::ScreenUpOnPlane() const noexcept
{
  // The view direction flattened onto the plane: away from the camera.
  return XMFLOAT2{std::sin(m_yawRadians), std::cos(m_yawRadians)};
}

/*
 * The LH entry points, not the RH ones (ADR-006 §3a).
 *
 * ADR-001 fixes render space as world = (sim.x, cosmetic height, sim.y) with +Y
 * up, which puts east on +X, up on +Y and north on +Z -- a left-handed basis.
 * Handing that to XMMatrixLookAtRH mirrors the view: with the camera south of
 * the focus looking north, east lands on the *left* of the screen. The RH
 * variants are right for a world where +Z is south; this is not that world.
 *
 * The corpus meshes then come out clockwise-in-NDC where they face the camera,
 * which is D3D12's front-face convention with FrontCounterClockwise = FALSE, so
 * the opaque pass takes the default rasteriser state.
 */
XMFLOAT4X4 IsoCamera::ViewMatrix() const noexcept
{
  const XMFLOAT3 eye = EyePosition();
  const XMVECTOR eyePosition = XMVectorSet(eye.x, eye.y, eye.z, 1.0f);
  const XMVECTOR focus = XMVectorSet(m_focusMetres.x, 0.0f, m_focusMetres.y, 1.0f);
  const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

  XMFLOAT4X4 view;
  XMStoreFloat4x4(&view, XMMatrixLookAtLH(eyePosition, focus, up));
  return view;
}

XMFLOAT4X4 IsoCamera::ProjectionMatrix() const noexcept
{
  const float halfHeight = m_zoomMetres;
  const float halfWidth = halfHeight * AspectRatio();

  // OffCenter rather than the symmetric overload: the HUD will eventually want
  // the world centred in the space its panels leave, and that is one asymmetric
  // pair of bounds rather than a second camera (ADR-006 §3a).
  //
  // Near at 1 m and far at twice the eye distance: an orthographic depth range
  // is linear, so 120 km across a D32 buffer is uniform millimetre precision
  // rather than the crowding a perspective projection would have here.
  XMFLOAT4X4 projection;
  XMStoreFloat4x4(&projection, XMMatrixOrthographicOffCenterLH(-halfWidth, halfWidth, -halfHeight, halfHeight, 1.0f,
                                                               2.0f * EYE_DISTANCE_METRES));
  return projection;
}

XMFLOAT4X4 IsoCamera::ViewProjectionMatrix() const noexcept
{
  const XMFLOAT4X4 view = ViewMatrix();
  const XMFLOAT4X4 projection = ProjectionMatrix();

  XMFLOAT4X4 result;
  XMStoreFloat4x4(&result, XMMatrixMultiply(XMLoadFloat4x4(&view), XMLoadFloat4x4(&projection)));
  return result;
}

} // namespace Neuron
