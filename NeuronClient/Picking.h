#pragma once

#include "IsoCamera.h"
#include "RenderWorld.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * Cursor to ship, in two dimensions (ADR-006 §11).
 *
 * The projection is orthographic and the world is a plane, so there is no ray
 * to intersect and no depth buffer to read back: a pixel maps to a plane point
 * through `PlaneMapping`, and picking is a distance test against a list of
 * circles. That is the whole reason ADR-006 §2 fixed the camera as ortho --
 * "picking loses its uniform-direction ray" is what a perspective camera costs.
 *
 * **Against the render world, not the simulation.** The list a pick runs over
 * is the interpolated one the frame is drawing (ADR-006 §11): the ship the
 * player clicked is the ship they saw, not the one the last snapshot placed a
 * tick and a half ago. Nothing here is sent to the server, and nothing waits
 * for it.
 *
 * **Device-free and game-free.** No D3D12, no window, and no idea what a ship
 * is -- a `SceneEntity` is an opaque id, a point and a radius, which is exactly
 * what the maths needs and nothing more (ADR-014). That is what makes the
 * accept criterion a unit test rather than a screenshot.
 */

namespace Neuron
{

/*
 * Picking runs over `SceneEntity` (RenderWorld.h) rather than a type of its
 * own. It reads three of the fields -- id, position, radius -- and ignores the
 * gauges, which is the price of the frame having one per-entity record instead
 * of three arrays to keep in lockstep.
 */

/*
 * Client-area pixels to NDC.
 *
 * Y flips: pixels count down from the top-left and NDC counts up from the
 * centre. A zero-size viewport -- which is what a window reports before its
 * first resize -- maps everything to the centre rather than dividing by it.
 *
 * Here rather than file-local in each caller because ADR-006 §11's "same plane
 * point feeds the order puck -- one code path" is a claim about this arithmetic
 * specifically: box selection, point picking and the order puck all turn a
 * cursor into a plane point, and three copies of the flip is three chances for
 * one of them to be off by a sign.
 */
[[nodiscard]] DirectX::XMFLOAT2 PixelsToNdc(float _x, float _y, std::uint32_t _viewportWidth,
                                            std::uint32_t _viewportHeight) noexcept;

/// NDC to a plane point, which is `PlaneMapping`'s own definition applied:
/// `origin + rightPerNdc * ndc.x + upPerNdc * ndc.y`.
[[nodiscard]] DirectX::XMFLOAT2 NdcToPlane(const PlaneMapping& _mapping, const DirectX::XMFLOAT2& _ndc) noexcept;

/*
 * Plane point back to NDC -- the inverse of `PlaneMapping`.
 *
 * `PlaneMappingForNdc` goes one way and this goes the other, and they are
 * asserted to round-trip. Box selection needs this direction: a drag rectangle
 * is axis-aligned in *screen* space and an arbitrary parallelogram on the
 * plane, so testing a ship against it is far easier after mapping the ship into
 * screen space than after mapping four corners onto the plane.
 *
 * Returns false when the mapping is degenerate (a zero-size viewport before the
 * first resize), rather than dividing by a zero determinant and returning
 * infinities that compare as inside every box.
 */
[[nodiscard]] bool PlaneToNdc(const PlaneMapping& _mapping, const DirectX::XMFLOAT2& _planeMetres,
                              DirectX::XMFLOAT2& _outNdc) noexcept;

/*
 * The nearest target whose circle contains the point, or `INVALID_ENTITY_ID`.
 *
 * `_minRadiusMetres` is ADR-006 §11's screen floor: at 40 km of zoom an
 * Interceptor's 12 m radius is a fraction of a pixel, and a pick that demanded
 * that accuracy would be a pick nobody can make. `IsoCamera::ScreenFloorMetres`
 * converts a pixel budget into this. It is a floor and not a replacement --
 * a Carrier stays as easy to hit as it looks.
 *
 * Nearest rather than first, because overlapping ships are the normal case in a
 * formation and "whichever happened to be earlier in the array" is not an
 * answer a player can predict. Ties go to the earlier target, which only
 * matters for two ships at exactly the same point.
 */
[[nodiscard]] std::uint16_t PickPoint(std::span<const SceneEntity> _entities, const DirectX::XMFLOAT2& _planeMetres,
                                      float _minRadiusMetres) noexcept;

/*
 * Every target inside a screen-space rectangle, appended to `_outIds`.
 *
 * The rectangle is given in NDC because that is what the drag produces once the
 * viewport is divided out, and because NDC is where the test is cheap. Corners
 * may be in any order -- a drag up-and-left is as valid as down-and-right.
 *
 * Centre-inside, not overlap: a box catches a ship whose *position* is in it.
 * Overlap would mean a box that clips the edge of a Carrier selects it, which
 * makes a careful drag around a formation pick up the capital ship parked
 * beside it.
 */
void PickBox(std::span<const SceneEntity> _entities, const PlaneMapping& _mapping, const DirectX::XMFLOAT2& _ndcCornerA,
             const DirectX::XMFLOAT2& _ndcCornerB, std::vector<std::uint16_t>& _outIds);

} // namespace Neuron
