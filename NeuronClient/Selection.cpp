#include "pch.h"

#include "Selection.h"

#include <algorithm>

using namespace DirectX;

namespace Neuron
{

bool Selection::Contains(EntityId _id) const noexcept
{
  return std::find(m_ids.begin(), m_ids.end(), _id) != m_ids.end();
}

void Selection::Set(std::span<const EntityId> _ids)
{
  m_ids.assign(_ids.begin(), _ids.end());
}

void Selection::Add(EntityId _id)
{
  if (_id != INVALID_ENTITY_ID && !Contains(_id))
  {
    m_ids.push_back(_id);
  }
}

void Selection::Toggle(EntityId _id)
{
  if (_id == INVALID_ENTITY_ID)
  {
    return;
  }
  const auto found = std::find(m_ids.begin(), m_ids.end(), _id);
  if (found == m_ids.end())
  {
    m_ids.push_back(_id);
  }
  else
  {
    m_ids.erase(found);
  }
}

void Selection::Retain(std::span<const SceneEntity> _entities)
{
  std::erase_if(m_ids,
                [_entities](EntityId _id)
                {
                  return std::find_if(_entities.begin(), _entities.end(),
                                      [_id](const SceneEntity& _target) { return _target.id == _id; }) == _entities.end();
                });
}

void Selection::Tap(float _tapX, float _tapY, bool _additive, std::span<const SceneEntity> _entities,
                    const PlaneMapping& _mapping, std::uint32_t _viewportWidth, std::uint32_t _viewportHeight,
                    float _minRadiusMetres)
{
  // The plane point under the tap, then the nearest ship to it -- through the
  // same two functions the order puck uses, which is ADR-006 §11's one code
  // path rather than two that agree today.
  const XMFLOAT2 planePoint = NdcToPlane(_mapping, PixelsToNdc(_tapX, _tapY, _viewportWidth, _viewportHeight));
  const EntityId hit = PickPoint(_entities, planePoint, _minRadiusMetres);

  if (_additive)
  {
    // The desk's shift accelerator toggles, so the same tap both adds a ship
    // and takes it back out. Additive on empty space does nothing rather than
    // clearing -- the modifier means "adjust this selection", and clearing is
    // the opposite.
    Toggle(hit);
    return;
  }

  // A plain tap replaces, including with nothing: tapping empty space is how a
  // player deselects, and it has to work even when they meant to hit a ship.
  m_ids.clear();
  Add(hit);
}

} // namespace Neuron
