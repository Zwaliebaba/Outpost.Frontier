#include "pch.h"

#include "OrderPuck.h"

#include "Picking.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Neuron
{

void OrderPuck::Begin(float _cursorX, float _cursorY, bool _queued) noexcept
{
  m_active = true;
  m_queued = _queued;
  m_anchorX = _cursorX;
  m_anchorY = _cursorY;
  m_cursorX = _cursorX;
  m_cursorY = _cursorY;
}

void OrderPuck::Update(float _cursorX, float _cursorY) noexcept
{
  if (m_active)
  {
    m_cursorX = _cursorX;
    m_cursorY = _cursorY;
  }
}

bool OrderPuck::HasFacing() const noexcept
{
  if (!m_active)
  {
    return false;
  }

  // Euclidean, unlike the selection's box slop. That one asks whether the mouse
  // stayed still, where a diagonal twitch is no more a drag than a horizontal
  // one; this asks whether the arc is long enough for its *angle* to be
  // meaningful, and the angle's stability depends on the arc's length, not on
  // its longest axis.
  const float dx = m_cursorX - m_anchorX;
  const float dy = m_cursorY - m_anchorY;
  return dx * dx + dy * dy > FACING_SLOP_PIXELS * FACING_SLOP_PIXELS;
}

PuckSample OrderPuck::Resolve(const PlaneMapping& _mapping, std::uint32_t _viewportWidth,
                              std::uint32_t _viewportHeight) const noexcept
{
  PuckSample sample;
  sample.queued = m_queued;

  const XMFLOAT2 anchorPlane = NdcToPlane(_mapping, PixelsToNdc(m_anchorX, m_anchorY, _viewportWidth, _viewportHeight));
  sample.targetMetres = anchorPlane;

  if (!HasFacing())
  {
    return sample;
  }

  // Both ends through the same mapping, then the difference: the drag is an
  // arc on the plane, and its direction there is what the fleet will face.
  // Taking the angle in pixels instead would be right only at a yaw of zero,
  // and wrong by exactly the camera's yaw everywhere else -- a bug that looks
  // correct in every screenshot taken from the default view.
  const XMFLOAT2 cursorPlane = NdcToPlane(_mapping, PixelsToNdc(m_cursorX, m_cursorY, _viewportWidth, _viewportHeight));
  const float dx = cursorPlane.x - anchorPlane.x;
  const float dy = cursorPlane.y - anchorPlane.y;
  if (dx == 0.0f && dy == 0.0f)
  {
    // Past the pixel slop and still nowhere on the plane: a degenerate mapping,
    // which is what a zero-size viewport produces. Reported as no facing, so
    // the caller substitutes one rather than believing atan2(0, 0).
    return sample;
  }

  sample.facingRadians = std::atan2(dy, dx);
  sample.facingFromDrag = true;
  return sample;
}

bool SelectionCentre(std::span<const SceneEntity> _entities, std::span<const std::uint16_t> _ids,
                     XMFLOAT2& _outMetres) noexcept
{
  float sumX = 0.0f;
  float sumY = 0.0f;
  std::uint32_t found = 0;

  for (const std::uint16_t id : _ids)
  {
    const auto entity = std::find_if(_entities.begin(), _entities.end(), [id](const SceneEntity& _e) { return _e.id == id; });
    if (entity == _entities.end())
    {
      continue;
    }
    sumX += entity->planeMetres.x;
    sumY += entity->planeMetres.y;
    ++found;
  }

  if (found == 0)
  {
    return false;
  }

  const float inverse = 1.0f / static_cast<float>(found);
  _outMetres = XMFLOAT2{sumX * inverse, sumY * inverse};
  return true;
}

float TravelFacingRadians(std::span<const SceneEntity> _entities, std::span<const std::uint16_t> _ids,
                          const XMFLOAT2& _targetMetres, float _ifStationary) noexcept
{
  XMFLOAT2 centre{};
  if (!SelectionCentre(_entities, _ids, centre))
  {
    return _ifStationary;
  }

  const float dx = _targetMetres.x - centre.x;
  const float dy = _targetMetres.y - centre.y;
  if (dx == 0.0f && dy == 0.0f)
  {
    return _ifStationary;
  }
  return std::atan2(dy, dx);
}

OrderIntent MakeOrderIntent(const PuckSample& _sample, const OrderDefaults& _defaults, std::span<const std::uint16_t> _ids,
                            std::uint32_t _orderSeq) noexcept
{
  OrderIntent intent;
  intent.kind = _defaults.kind;
  intent.parameter = _defaults.parameter;
  intent.targetXMetres = _sample.targetMetres.x;
  intent.targetYMetres = _sample.targetMetres.y;
  intent.facingRadians = _sample.facingRadians;
  intent.queued = _sample.queued;
  intent.orderSeq = _orderSeq;
  intent.entityIds = _ids.data();
  intent.entityCount = static_cast<std::uint32_t>(_ids.size());
  return intent;
}

} // namespace Neuron
