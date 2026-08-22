#include "pch.h"

#include "Picking.h"

#include <cmath>

using namespace DirectX;

namespace Neuron
{

XMFLOAT2 PixelsToNdc(float _x, float _y, std::uint32_t _viewportWidth, std::uint32_t _viewportHeight) noexcept
{
  if (_viewportWidth == 0 || _viewportHeight == 0)
  {
    return XMFLOAT2{0.0f, 0.0f};
  }
  return XMFLOAT2{(_x / static_cast<float>(_viewportWidth)) * 2.0f - 1.0f,
                  1.0f - (_y / static_cast<float>(_viewportHeight)) * 2.0f};
}

XMFLOAT2 NdcToPixels(const XMFLOAT2& _ndc, std::uint32_t _viewportWidth, std::uint32_t _viewportHeight) noexcept
{
  return XMFLOAT2{(_ndc.x + 1.0f) * 0.5f * static_cast<float>(_viewportWidth),
                  (1.0f - _ndc.y) * 0.5f * static_cast<float>(_viewportHeight)};
}

XMFLOAT2 NdcToPlane(const PlaneMapping& _mapping, const XMFLOAT2& _ndc) noexcept
{
  return XMFLOAT2{_mapping.origin.x + _mapping.rightPerNdc.x * _ndc.x + _mapping.upPerNdc.x * _ndc.y,
                  _mapping.origin.y + _mapping.rightPerNdc.y * _ndc.x + _mapping.upPerNdc.y * _ndc.y};
}

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

EntityId PickPoint(std::span<const SceneEntity> _entities, const XMFLOAT2& _planeMetres, float _minRadiusMetres) noexcept
{
  EntityId best = INVALID_ENTITY_ID;
  float bestDistanceSquared = 0.0f;

  for (const SceneEntity& entity : _entities)
  {
    if (entity.id == INVALID_ENTITY_ID)
    {
      continue;
    }

    const float dx = entity.planeMetres.x - _planeMetres.x;
    const float dy = entity.planeMetres.y - _planeMetres.y;
    const float distanceSquared = dx * dx + dy * dy;

    // Squared throughout: the comparison is the same and the square root is
    // one per target in the hot path of every click and every hover.
    const float radius = entity.pickRadiusMetres > _minRadiusMetres ? entity.pickRadiusMetres : _minRadiusMetres;
    if (distanceSquared > radius * radius)
    {
      continue;
    }

    if (best == INVALID_ENTITY_ID || distanceSquared < bestDistanceSquared)
    {
      best = entity.id;
      bestDistanceSquared = distanceSquared;
    }
  }
  return best;
}

void PickBox(std::span<const SceneEntity> _entities, const PlaneMapping& _mapping, const XMFLOAT2& _ndcCornerA,
             const XMFLOAT2& _ndcCornerB, std::vector<EntityId>& _outIds)
{
  const float minX = _ndcCornerA.x < _ndcCornerB.x ? _ndcCornerA.x : _ndcCornerB.x;
  const float maxX = _ndcCornerA.x < _ndcCornerB.x ? _ndcCornerB.x : _ndcCornerA.x;
  const float minY = _ndcCornerA.y < _ndcCornerB.y ? _ndcCornerA.y : _ndcCornerB.y;
  const float maxY = _ndcCornerA.y < _ndcCornerB.y ? _ndcCornerB.y : _ndcCornerA.y;

  for (const SceneEntity& entity : _entities)
  {
    if (entity.id == INVALID_ENTITY_ID)
    {
      continue;
    }

    XMFLOAT2 ndc{};
    if (!PlaneToNdc(_mapping, entity.planeMetres, ndc))
    {
      return; // Degenerate mapping: nothing is inside anything.
    }

    if (ndc.x >= minX && ndc.x <= maxX && ndc.y >= minY && ndc.y <= maxY)
    {
      _outIds.push_back(entity.id);
    }
  }
}

} // namespace Neuron
