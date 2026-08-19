#pragma once

#include <DirectXMath.h>

/*
 * Where the ear is (ADR-011 §4).
 *
 * The problem this file exists for: the simulation is planar and the camera is
 * orthographic, and an ortho camera has no eye position -- it is a direction
 * and a viewing volume. "Put the listener at the camera" has no answer, and
 * getting it wrong produces the classic RTS failure, sound that pans backwards
 * or that ignores zoom entirely.
 *
 * The answer the ADR settles on: **the listener sits at the camera's focus on
 * the plane, raised by an elevation derived from the zoom.** Every consequence
 * is one we want. Panning moves the audio frame with the view. Zooming out
 * raises the listener and attenuates everything, so the whole battle recedes
 * exactly as it shrinks. Left and right map to screen left and right, because
 * the listener's basis *is* the camera's.
 *
 * Device-free, and that is the point: this is the half of the audio system with
 * something to get wrong, so it is the half that gets asserted on a runner with
 * no audio device.
 *
 * **Nothing here converts handedness.** X3DAudio is left-handed and so is this
 * tree (ADR-006 §3a), so positions and orientations go across as they stand. A
 * right-handed tree would negate `.z` on all four fields of both structures on
 * every update; that conversion does not exist here, and no code should be
 * written that reintroduces it.
 */

namespace Neuron
{

/*
 * How far the listener rises per metre of ortho half-height -- the `k` of
 * ADR-011 §4, and it is 1.
 *
 * It is a real tuning knob rather than a constant folded into the expression:
 * it sets how sharply zoom attenuates, which is the single number that decides
 * whether pulling back reads as stepping away or as everything muting.
 */
inline constexpr float LISTENER_ELEVATION_PER_ZOOM = 1.0f;

/// The listener, in render space (x east, y up, z north -- ADR-001 §3). Stored
/// as XMFLOAT3 and computed through XMVECTOR, per ADR-010.
struct AudioListenerState
{
  DirectX::XMFLOAT3 position{};
  DirectX::XMFLOAT3 front{};
  DirectX::XMFLOAT3 top{};
};

/*
 * The listener for a camera.
 *
 * `_focusMetres` is the plane point the view is centred on, `_yawRadians` the
 * camera's yaw and `_zoomHalfHeightMetres` its ortho half-height -- exactly
 * `IsoCamera::Focus()`, `YawRadians()` and `ZoomMetres()`, taken as arguments
 * rather than as a camera so this stays testable without one.
 *
 * `front` is the camera's forward flattened onto the plane, which is
 * `IsoCamera::ScreenUpOnPlane()` lifted into three dimensions: the ear faces
 * the way the screen faces, so a ship drawn to the right sounds to the right.
 * `top` is world up. The two are orthonormal by construction, which X3DAudio
 * checks to within 1e-5 and refuses.
 */
[[nodiscard]] AudioListenerState MakeAudioListener(const DirectX::XMFLOAT2& _focusMetres, float _yawRadians,
                                                   float _zoomHalfHeightMetres) noexcept;

/*
 * An emitter's render position: the plane point, lifted by the class's cosmetic
 * hover (ADR-001, ADR-011 §5).
 *
 * The same number the renderer draws the hull at, so audio and visuals never
 * disagree about where a ship is -- which is also why a frozen feed freezes the
 * audio with it.
 */
[[nodiscard]] DirectX::XMFLOAT3 MakeAudioEmitter(const DirectX::XMFLOAT2& _planeMetres, float _hoverMetres) noexcept;

/// Metres between the listener and an emitter. The voice pool's stealing policy
/// is "lowest priority, then farthest", and this is the "farthest".
[[nodiscard]] float AudioDistanceMetres(const AudioListenerState& _listener, const DirectX::XMFLOAT3& _emitter) noexcept;

} // namespace Neuron
