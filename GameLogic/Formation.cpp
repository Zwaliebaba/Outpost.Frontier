#include "pch.h"

#include "Formation.h"

#include "ShipClass.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Game
{
namespace
{

/// The widest spacing any member asks for. A Line with a Carrier in it is a
/// Carrier's Line, because the spacing that keeps the Carrier clear is the one
/// that keeps everything clear.
[[nodiscard]] float LargestSpacing(std::span<const ShipId> _shipIds, HullClass (*_hullClassOf)(ShipId, void*),
                                   void* _context) noexcept
{
  float spacing = 0.0f;
  for (const ShipId shipId : _shipIds)
  {
    spacing = std::max(spacing, ShipClass(_hullClassOf(shipId, _context)).formationSpacingMetres);
  }
  return spacing;
}

/// One station, in the anchor's own axes: how far right of it and how far
/// ahead of it. Split out from the trigonometry so a shape is arithmetic on two
/// numbers rather than a second copy of the basis.
struct Offsets
{
  float right = 0.0f;
  float forward = 0.0f;
};

/*
 * How wide a Claw opens, in radians. 120 degrees is a crescent that reads as
 * one at a glance and still leaves the arms pointing more forward than sideways
 * -- past about 180 the ends turn back on each other and the shape stops being
 * legible at tactical zoom, and below about 60 it is a Line with a bend in it.
 */
constexpr float CLAW_SWEEP_RADIANS = 2.0943951f;

/// A Wedge's arms lie at 45 degrees, so a step back and a step out are the same
/// number and the diagonal between consecutive ships is one spacing exactly.
constexpr float WEDGE_DIAGONAL = 0.70710678f;

/*
 * Where the `_index`-th of `_count` ships stands, for each shape.
 *
 * **Every formation puts something recognisable exactly on the anchor**, which
 * is the rule that makes the puck honest: the player pointed at a place and
 * something is there. A Line centres its rank on it, a Wedge puts its tip on
 * it, and a Claw puts the middle of its arc on it. A single-ship order lands on
 * the point in all three.
 *
 * **Adjacent stations are exactly one spacing apart in all three.** ADR-005 §2
 * leans on that -- there is no inter-ship avoidance in the MVP because
 * "stations don't overlap by construction" -- so it is a property of the
 * geometry rather than of the numbers chosen, and `FormationTests` asserts it
 * over every count and every class mix.
 */
[[nodiscard]] Offsets StationOffsets(FormationId _formation, std::uint32_t _index, std::uint32_t _count, float _spacing) noexcept
{
  switch (_formation)
  {
  case FormationId::Wedge:
  {
    // An arrowhead: the first ship at the tip, the rest falling back in two
    // arms that alternate so the shape stays balanced at every count. An even
    // total leaves one arm a ship longer, which is what an arrowhead looks
    // like rather than a defect.
    if (_index == 0)
    {
      return Offsets{0.0f, 0.0f};
    }
    const auto rank = static_cast<float>((_index + 1u) / 2u);
    const float side = (_index % 2u) == 1u ? -1.0f : 1.0f;
    const float step = rank * _spacing * WEDGE_DIAGONAL;
    return Offsets{side * step, -step};
  }

  case FormationId::Claw:
  {
    // A crescent cupping the way the fleet will face: the middle of the arc on
    // the anchor, the arms reaching forward past it.
    if (_count < 2)
    {
      return Offsets{0.0f, 0.0f};
    }

    // The radius comes from the *chord* rather than the arc length, and that is
    // the whole reason the arms do not crowd: solving for arc length puts
    // adjacent ships a chord apart, which is shorter, and at low counts the
    // shortfall is a fifth of the spacing.
    const float step = CLAW_SWEEP_RADIANS / static_cast<float>(_count - 1u);
    const float radius = _spacing / (2.0f * std::sin(step * 0.5f));
    const float angle = -CLAW_SWEEP_RADIANS * 0.5f + static_cast<float>(_index) * step;

    // Measured from the arc's midpoint, which sits `radius` behind the circle's
    // centre. `1 - cos` rather than `cos` so the anchor is the origin.
    return Offsets{radius * std::sin(angle), radius * (1.0f - std::cos(angle))};
  }

  case FormationId::Line:
  default:
  {
    // Ships abreast across the facing, shoulder to shoulder looking the same
    // way rather than nose to tail. Centred, so an odd count puts one ship
    // exactly on the point the player clicked.
    const float centre = 0.5f * static_cast<float>(_count - 1u);
    return Offsets{(static_cast<float>(_index) - centre) * _spacing, 0.0f};
  }
  }
}

} // namespace

std::uint32_t SolveFormation(FormationId _formation, std::span<const ShipId> _shipIds,
                             HullClass (*_hullClassOf)(ShipId, void*), void* _context, const XMFLOAT2& _anchorMetres,
                             float _anchorFacingRadians, std::span<FormationStation> _outStations) noexcept
{
  if (_shipIds.empty() || _outStations.empty() || _hullClassOf == nullptr)
  {
    return 0;
  }
  const auto count = static_cast<std::uint32_t>(std::min(_shipIds.size(), _outStations.size()));

  // Sorted by id, not by array order. Two callers with the same ships in a
  // different order have to get the same answer or the client's preview and the
  // server's solve disagree by one station -- and they *do* order differently,
  // because one is a selection and the other is a dense table.
  for (std::uint32_t index = 0; index < count; ++index)
  {
    _outStations[index].shipId = _shipIds[index];
  }
  std::sort(_outStations.begin(), _outStations.begin() + count,
            [](const FormationStation& _a, const FormationStation& _b) { return _a.shipId < _b.shipId; });

  const float spacing = LargestSpacing(_shipIds, _hullClassOf, _context);

  // Forward is the way the fleet will be looking when it arrives; right is that
  // rotated a quarter turn clockwise, in the sim's CCW-from-+x convention
  // (ADR-001 §3). Every formation below is written in these two axes, so none
  // of them contains a trigonometric term of its own.
  const float sinFacing = std::sin(_anchorFacingRadians);
  const float cosFacing = std::cos(_anchorFacingRadians);
  const XMFLOAT2 forward{cosFacing, sinFacing};
  const XMFLOAT2 right{sinFacing, -cosFacing};

  for (std::uint32_t index = 0; index < count; ++index)
  {
    const Offsets offsets = StationOffsets(_formation, index, count, spacing);
    _outStations[index].positionMetres = XMFLOAT2{
        _anchorMetres.x + right.x * offsets.right + forward.x * offsets.forward,
        _anchorMetres.y + right.y * offsets.right + forward.y * offsets.forward};
  }
  return count;
}

float FormationExtentMetres(std::span<const FormationStation> _stations, const XMFLOAT2& _anchorMetres) noexcept
{
  float furthest = 0.0f;
  for (const FormationStation& station : _stations)
  {
    const float dx = station.positionMetres.x - _anchorMetres.x;
    const float dy = station.positionMetres.y - _anchorMetres.y;
    furthest = std::max(furthest, dx * dx + dy * dy);
  }
  return std::sqrt(furthest);
}

} // namespace Game
