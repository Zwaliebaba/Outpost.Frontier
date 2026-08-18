#include "pch.h"

#include "Selection.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Neuron
{

bool Selection::Contains(std::uint16_t _id) const noexcept
{
  return std::find(m_ids.begin(), m_ids.end(), _id) != m_ids.end();
}

void Selection::Set(std::span<const std::uint16_t> _ids)
{
  m_ids.assign(_ids.begin(), _ids.end());
}

void Selection::Add(std::uint16_t _id)
{
  if (_id != INVALID_ENTITY_ID && !Contains(_id))
  {
    m_ids.push_back(_id);
  }
}

void Selection::Toggle(std::uint16_t _id)
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
                [_entities](std::uint16_t _id)
                {
                  return std::find_if(_entities.begin(), _entities.end(),
                                      [_id](const SceneEntity& _target) { return _target.id == _id; }) == _entities.end();
                });
}

void Selection::BeginDrag(float _cursorX, float _cursorY, bool _additive) noexcept
{
  m_dragging = true;
  m_additive = _additive;
  m_startX = _cursorX;
  m_startY = _cursorY;
  m_currentX = _cursorX;
  m_currentY = _cursorY;
}

void Selection::UpdateDrag(float _cursorX, float _cursorY) noexcept
{
  if (m_dragging)
  {
    m_currentX = _cursorX;
    m_currentY = _cursorY;
  }
}

bool Selection::DragIsBox() const noexcept
{
  if (!m_dragging)
  {
    return false;
  }
  // Chebyshev rather than Euclidean: the slop is about "did the mouse stay
  // still", and a diagonal twitch is no more a drag than a horizontal one.
  return std::fabs(m_currentX - m_startX) > CLICK_SLOP_PIXELS || std::fabs(m_currentY - m_startY) > CLICK_SLOP_PIXELS;
}

void Selection::EndDrag(std::span<const SceneEntity> _entities, const PlaneMapping& _mapping, std::uint32_t _viewportWidth,
                        std::uint32_t _viewportHeight, float _minRadiusMetres)
{
  if (!m_dragging)
  {
    return;
  }
  const bool isBox = DragIsBox();
  const bool additive = m_additive;
  m_dragging = false;

  if (isBox)
  {
    const XMFLOAT2 cornerA = PixelsToNdc(m_startX, m_startY, _viewportWidth, _viewportHeight);
    const XMFLOAT2 cornerB = PixelsToNdc(m_currentX, m_currentY, _viewportWidth, _viewportHeight);

    std::vector<std::uint16_t> inside;
    PickBox(_entities, _mapping, cornerA, cornerB, inside);

    if (!additive)
    {
      m_ids.clear();
    }
    for (const std::uint16_t id : inside)
    {
      Add(id); // Add, not assign: a shift-box merges rather than replaces.
    }
    return;
  }

  // A click. The plane point under the cursor, then the nearest ship to it --
  // through the same two functions the order puck uses, which is ADR-006 §11's
  // one code path rather than two that agree today.
  const XMFLOAT2 planePoint = NdcToPlane(_mapping, PixelsToNdc(m_currentX, m_currentY, _viewportWidth, _viewportHeight));
  const std::uint16_t hit = PickPoint(_entities, planePoint, _minRadiusMetres);

  if (additive)
  {
    // Shift-click toggles, so the same click both adds a ship and takes it back
    // out. A shift-click on empty space does nothing rather than clearing --
    // the modifier means "adjust this selection", and clearing is the opposite.
    Toggle(hit);
    return;
  }

  // A plain click replaces, including with nothing: clicking empty space is how
  // a player deselects, and it has to work even when they meant to hit a ship.
  m_ids.clear();
  Add(hit);
}

} // namespace Neuron
