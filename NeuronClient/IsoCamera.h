#pragma once

#include <DirectXMath.h>

#include <cstdint>

/*
 * The tactical camera (ADR-006 §3-§4).
 *
 * Orthographic, elevation fixed at 30 degrees, orbiting a focus point on the
 * plane. None of those three is a preference:
 *
 *  - orthographic, because a planar sim gains nothing from perspective
 *    parallax and loses a uniform picking ray, a single-number zoom, and
 *    constant icon scale across the view;
 *  - 30 degrees, because the corpus draws selection rings as 2:1 ellipses on
 *    the plane and an orthographic ground circle projects at sin(elevation),
 *    which is exactly 0.5 at 30 degrees;
 *  - a focus point rather than a free camera, because every camera motion the
 *    game offers -- orbit, zoom, pan -- is defined against the plane.
 *
 * All of this is client-only state (ADR-006 §4). Nothing here is replicated,
 * and nothing here is allowed to reach the simulation.
 *
 * Free of D3D12 and C++/WinRT so the maths is testable without a device, like
 * ClearColour.h and ObjMesh.h. DirectXMath is used natively, matrices are
 * stored as XMFLOAT4X4, and no XMVECTOR is ever a member (ADR-010).
 */

namespace Neuron
{

/*
 * How a normalised device coordinate lands on the plane.
 *
 * `plane = origin + rightPerNdc * ndc.x + upPerNdc * ndc.y`, exactly -- the
 * projection is orthographic, so the mapping is affine and three float2s carry
 * all of it. A pass that has to know where on the plane a pixel is takes this
 * rather than an inverse matrix and a ray/plane intersection.
 *
 * It is the inverse of `ViewProjectionMatrix()` restricted to y = 0, which is
 * not a claim to take on trust: `NeuronClientTests` projects plane points
 * through the real matrix and maps the resulting NDC back through this. That
 * guard exists because ADR-006 §3a's handedness defect was invisible to review
 * and visible only to a round trip through the real matrices.
 */
struct PlaneMapping
{
  DirectX::XMFLOAT2 origin{};      // The plane point at the centre of the view.
  DirectX::XMFLOAT2 rightPerNdc{}; // Plane metres per unit of NDC x.
  DirectX::XMFLOAT2 upPerNdc{};    // Plane metres per unit of NDC y.
};

class IsoCamera
{
public:
  /// Normative, not tunable: the 2:1 plane ellipse the overlay pass draws is
  /// this number (ADR-006 §4).
  static constexpr float ELEVATION_DEGREES = 30.0f;

  /// Orthographic half-height, the tactical tier's whole zoom range.
  static constexpr float MIN_ZOOM_METRES = 500.0f;
  static constexpr float MAX_ZOOM_METRES = 40000.0f;

  /// One 40 km x 40 km grid per session (ADR-001 §3), so the focus stays
  /// inside +/-20 km and the player cannot pan into empty float space.
  static constexpr float PLAY_AREA_HALF_EXTENT_METRES = 20000.0f;

  /// How far back the eye sits along the view direction. Irrelevant to an
  /// orthographic projection's shape and entirely relevant to its clip planes:
  /// it has to clear the far corner of the grid.
  static constexpr float EYE_DISTANCE_METRES = 60000.0f;

  /// Multiplier per zoom notch. 1.2 gives roughly 24 notches across the range,
  /// which is a wheel gesture rather than a journey.
  static constexpr float ZOOM_STEP_FACTOR = 1.2f;

  void SetViewport(std::uint32_t _widthPixels, std::uint32_t _heightPixels) noexcept;

  /// The plane point the camera looks at, in sim space (x east, y north, metres).
  void SetFocus(const DirectX::XMFLOAT2& _focusMetres) noexcept;
  void SetYawRadians(float _yawRadians) noexcept;
  void SetZoomMetres(float _halfHeightMetres) noexcept;

  /// The detent spacing, from config. Zero or negative disables snapping.
  void SetYawSnapDegrees(float _degrees) noexcept;

  /// Free orbit -- what a drag does while the button is held.
  void Orbit(float _deltaRadians) noexcept;

  /// Snap to the nearest detent -- what releasing the drag does.
  void SnapYawToDetent() noexcept;

  /// Step whole detents, for the keyboard. Snaps first if the yaw is between
  /// two of them, so the first press always lands on a detent.
  void NudgeYawDetents(std::int32_t _steps) noexcept;

  /// Positive zooms in. Fractional steps are honoured: a high-resolution wheel
  /// reports them and rounding would make it feel notched twice.
  void Zoom(float _steps) noexcept;

  /// Moves the focus by a screen displacement, in pixels. Screen-vertical costs
  /// more plane distance than screen-horizontal -- by 1/sin(elevation), which is
  /// the same factor that makes ground circles 2:1 ellipses.
  void PanPixels(float _rightPixels, float _upPixels) noexcept;

  [[nodiscard]] const DirectX::XMFLOAT2& Focus() const noexcept { return m_focusMetres; }
  [[nodiscard]] float YawRadians() const noexcept { return m_yawRadians; }
  [[nodiscard]] float ZoomMetres() const noexcept { return m_zoomMetres; }
  [[nodiscard]] float AspectRatio() const noexcept;

  /// World metres per screen pixel. The overlay pass's screen-floor clamps are
  /// written against this (ADR-006 §8), and so is panning.
  [[nodiscard]] float MetresPerPixel() const noexcept;

  /// Render space (ADR-001 §3): world = (sim.x, cosmetic height, sim.y), +Y up.
  [[nodiscard]] DirectX::XMFLOAT3 EyePosition() const noexcept;

  /// Unit vectors on the plane, in sim space. Screen-right and screen-up as the
  /// player experiences them, which is what pan and box-select are defined in.
  [[nodiscard]] DirectX::XMFLOAT2 ScreenRightOnPlane() const noexcept;
  [[nodiscard]] DirectX::XMFLOAT2 ScreenUpOnPlane() const noexcept;

  /// Where the view's NDC square lands on the plane. The Nebula pass anchors
  /// its field with this so the haze belongs to the world rather than to the
  /// monitor, and the S8 overlay will want the same mapping.
  [[nodiscard]] PlaneMapping PlaneMappingForNdc() const noexcept;

  /// Row-major, as DirectXMath stores them. The upload transposes for HLSL.
  [[nodiscard]] DirectX::XMFLOAT4X4 ViewMatrix() const noexcept;
  [[nodiscard]] DirectX::XMFLOAT4X4 ProjectionMatrix() const noexcept;
  [[nodiscard]] DirectX::XMFLOAT4X4 ViewProjectionMatrix() const noexcept;

private:
  DirectX::XMFLOAT2 m_focusMetres = {0.0f, 0.0f};
  float m_yawRadians = 0.0f;
  float m_zoomMetres = 8000.0f; // The config default, so a camera with no
                                // configuration still frames a fleet.
  float m_yawSnapRadians = 0.0f;
  std::uint32_t m_viewportWidth = 1600;
  std::uint32_t m_viewportHeight = 900;
};

} // namespace Neuron
