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

} // namespace

std::uint32_t SolveFormation(FormationId _formation, std::span<const ShipId> _shipIds,
                             HullClass (*_hullClassOf)(ShipId, void*), void* _context, const XMFLOAT2& _anchorMetres,
                             float _anchorFacingRadians, std::span<FormationStation> _outStations) noexcept
{
  if (_shipIds.empty() || _outStations.empty() || _hullClassOf == nullptr)
  {
    return 0;
  }
  if (_formation != FormationId::Line)
  {
    return 0; // Wedge and Claw are S10's. `ValidateOrder` refuses them first.
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

  // A Line lies across the facing, so the ships stand shoulder to shoulder
  // looking the same way rather than nose to tail. Right is facing rotated a
  // quarter turn clockwise in the sim's CCW-from-+x convention (ADR-001 §3).
  const float sinFacing = std::sin(_anchorFacingRadians);
  const float cosFacing = std::cos(_anchorFacingRadians);
  const XMFLOAT2 right{sinFacing, -cosFacing};

  // Centred on the anchor: an odd count puts one ship exactly on the point the
  // player clicked, which is what makes a single-ship move land where they
  // pointed rather than half a spacing to one side.
  const float centre = 0.5f * static_cast<float>(count - 1);

  for (std::uint32_t index = 0; index < count; ++index)
  {
    const float offset = (static_cast<float>(index) - centre) * spacing;
    _outStations[index].positionMetres = XMFLOAT2{_anchorMetres.x + right.x * offset, _anchorMetres.y + right.y * offset};
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
