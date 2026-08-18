#pragma once

#include "IsoCamera.h"

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
 * is -- a `PickTarget` is an opaque id, a point and a radius, which is exactly
 * what the maths needs and nothing more (ADR-014). That is what makes the
 * accept criterion a unit test rather than a screenshot.
 */

namespace Neuron
{

/// No target. `0` is a legitimate ship id, so the sentinel cannot be zero.
inline constexpr std::uint32_t INVALID_PICK_ID = 0xffffffffu;

/*
 * One pickable thing, as the picker sees it.
 *
 * The id is opaque: the engine never interprets it, and hands back whatever the
 * game put in (`Game::ShipId` today). The radius is the class's, in metres, so
 * a Battleship is easier to hit than an Interceptor because it is bigger --
 * which is the behaviour a player expects and the reason the radius is not a
 * constant.
 */
struct PickTarget
{
  std::uint32_t id = INVALID_PICK_ID;
  DirectX::XMFLOAT2 planeMetres{};
  float pickRadiusMetres = 0.0f;
};

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
 * The nearest target whose circle contains the point, or `INVALID_PICK_ID`.
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
[[nodiscard]] std::uint32_t PickPoint(std::span<const PickTarget> _targets, const DirectX::XMFLOAT2& _planeMetres,
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
void PickBox(std::span<const PickTarget> _targets, const PlaneMapping& _mapping, const DirectX::XMFLOAT2& _ndcCornerA,
             const DirectX::XMFLOAT2& _ndcCornerB, std::vector<std::uint32_t>& _outIds);

} // namespace Neuron
