#include "pch.h"

#include "Picking.h"

#include <cmath>

using namespace DirectX;

namespace Neuron
{

bool PlaneToNdc(const PlaneMapping& _mapping, const XMFLOAT2& _planeMetres, XMFLOAT2& _outNdc) noexcept
{
  // Solving `plane - origin = right * ndc.x + up * ndc.y` for ndc: a 2x2 with
  // the two axes as columns, inverted by hand because it is two lines and
  // because XMMatrixInverse would need the vector shuffles to say the same.
  const float determinant = _mapping.rightPerNdc.x * _mapping.upPerNdc.y - _mapping.rightPerNdc.y * _mapping.upPerNdc.x;
  if (!(std::fabs(determinant) > 0.0f))
  {
    // Zero, or a NaN that made the comparison false. Either way the axes do not
    // span the plane and there is no answer to give.
    _outNdc = XMFLOAT2{0.0f, 0.0f};
    return false;
  }

  const float offsetX = _planeMetres.x - _mapping.origin.x;
  const float offsetY = _planeMetres.y - _mapping.origin.y;

  _outNdc.x = (offsetX * _mapping.upPerNdc.y - offsetY * _mapping.upPerNdc.x) / determinant;
  _outNdc.y = (offsetY * _mapping.rightPerNdc.x - offsetX * _mapping.rightPerNdc.y) / determinant;
  return true;
}

std::uint32_t PickPoint(std::span<const PickTarget> _targets, const XMFLOAT2& _planeMetres, float _minRadiusMetres) noexcept
{
  std::uint32_t best = INVALID_PICK_ID;
  float bestDistanceSquared = 0.0f;

  for (const PickTarget& target : _targets)
  {
    if (target.id == INVALID_PICK_ID)
    {
      continue;
    }

    const float dx = target.planeMetres.x - _planeMetres.x;
    const float dy = target.planeMetres.y - _planeMetres.y;
    const float distanceSquared = dx * dx + dy * dy;

    // Squared throughout: the comparison is the same and the square root is
    // one per target in the hot path of every click and every hover.
    const float radius = target.pickRadiusMetres > _minRadiusMetres ? target.pickRadiusMetres : _minRadiusMetres;
    if (distanceSquared > radius * radius)
    {
      continue;
    }

    if (best == INVALID_PICK_ID || distanceSquared < bestDistanceSquared)
    {
      best = target.id;
      bestDistanceSquared = distanceSquared;
    }
  }
  return best;
}

void PickBox(std::span<const PickTarget> _targets, const PlaneMapping& _mapping, const XMFLOAT2& _ndcCornerA,
             const XMFLOAT2& _ndcCornerB, std::vector<std::uint32_t>& _outIds)
{
  const float minX = _ndcCornerA.x < _ndcCornerB.x ? _ndcCornerA.x : _ndcCornerB.x;
  const float maxX = _ndcCornerA.x < _ndcCornerB.x ? _ndcCornerB.x : _ndcCornerA.x;
  const float minY = _ndcCornerA.y < _ndcCornerB.y ? _ndcCornerA.y : _ndcCornerB.y;
  const float maxY = _ndcCornerA.y < _ndcCornerB.y ? _ndcCornerB.y : _ndcCornerA.y;

  for (const PickTarget& target : _targets)
  {
    if (target.id == INVALID_PICK_ID)
    {
      continue;
    }

    XMFLOAT2 ndc{};
    if (!PlaneToNdc(_mapping, target.planeMetres, ndc))
    {
      return; // Degenerate mapping: no target is inside anything.
    }

    if (ndc.x >= minX && ndc.x <= maxX && ndc.y >= minY && ndc.y <= maxY)
    {
      _outIds.push_back(target.id);
    }
  }
}

} // namespace Neuron
