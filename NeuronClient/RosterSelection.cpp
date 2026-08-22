#include "pch.h"

#include "RosterSelection.h"

#include <algorithm>

namespace Neuron
{

bool RosterSelection::Holds(std::uint32_t _shipId) const noexcept
{
  return std::find(m_ids.begin(), m_ids.end(), _shipId) != m_ids.end();
}

bool RosterSelection::Toggle(std::uint32_t _shipId)
{
  const auto found = std::find(m_ids.begin(), m_ids.end(), _shipId);
  if (found != m_ids.end())
  {
    m_ids.erase(found);
    return false;
  }
  m_ids.push_back(_shipId);
  return true;
}

void RosterSelection::Add(std::uint32_t _shipId)
{
  if (!Holds(_shipId))
  {
    m_ids.push_back(_shipId);
  }
}

void RosterSelection::AddAll(std::span<const std::uint32_t> _shipIds)
{
  for (const std::uint32_t shipId : _shipIds)
  {
    Add(shipId);
  }
}

void RosterSelection::Remove(std::uint32_t _shipId)
{
  const auto found = std::find(m_ids.begin(), m_ids.end(), _shipId);
  if (found != m_ids.end())
  {
    m_ids.erase(found);
  }
}

void RosterSelection::Reconcile(std::span<const std::uint32_t> _available)
{
  // `std::erase_if` rather than a rebuild, so the survivors keep the order the
  // player picked them in -- which is the order `Wave` reports.
  std::erase_if(m_ids, [_available](std::uint32_t _held) {
    return std::find(_available.begin(), _available.end(), _held) == _available.end();
  });
}

std::uint32_t RosterSelection::WaveCount(std::uint32_t _shipsPerCommand) const noexcept
{
  if (_shipsPerCommand == 0 || m_ids.empty())
  {
    return 0;
  }
  const auto held = static_cast<std::uint32_t>(m_ids.size());
  return (held + _shipsPerCommand - 1) / _shipsPerCommand;
}

std::span<const std::uint32_t> RosterSelection::Wave(std::uint32_t _wave, std::uint32_t _shipsPerCommand) const noexcept
{
  if (_wave >= WaveCount(_shipsPerCommand))
  {
    return {};
  }

  const std::size_t first = static_cast<std::size_t>(_wave) * _shipsPerCommand;
  const std::size_t count = std::min(static_cast<std::size_t>(_shipsPerCommand), m_ids.size() - first);
  return std::span<const std::uint32_t>{m_ids.data() + first, count};
}

} // namespace Neuron
