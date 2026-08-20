#include "pch.h"

#include "ApproachChain.h"

#include <algorithm>

namespace Neuron
{

bool ApproachChain::Begin(const ContextAction& _action, std::span<const std::uint16_t> _ships) noexcept
{
  if (!_action.available || _ships.empty() || _ships.size() > MAX_SHIPS)
  {
    // Refused rather than truncated: an approach that dropped members would
    // arrive as an order about a different fleet than the one pointed at.
    return false;
  }

  m_kind = _action.kind;
  m_anchor = _action.anchor;
  m_label = _action.label;
  m_approachSeq = 0;
  m_shipCount = static_cast<std::uint32_t>(_ships.size());
  std::copy(_ships.begin(), _ships.end(), m_ships);
  m_active = true;
  return true;
}

void ApproachChain::NoteOrderRefused(std::uint32_t _orderSeq) noexcept
{
  // Zero is "no leg sent yet", and it is also what a malformed payload acks
  // with, so it must never match: a chain whose Move has not left cannot be
  // the order that just bounced.
  if (m_active && m_approachSeq != 0 && _orderSeq == m_approachSeq)
  {
    m_active = false;
  }
}

void ApproachChain::NoteWorld(std::span<const std::uint16_t> _liveIds) noexcept
{
  if (!m_active)
  {
    return;
  }
  for (std::uint32_t index = 0; index < m_shipCount; ++index)
  {
    const std::uint16_t ship = m_ships[index];
    if (std::find(_liveIds.begin(), _liveIds.end(), ship) == _liveIds.end())
    {
      m_active = false;
      return;
    }
  }
}

} // namespace Neuron
