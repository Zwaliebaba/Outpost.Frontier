#include "pch.h"

#include "RoutePlan.h"

#include <algorithm>

namespace Neuron
{

bool FleetIsInScene(std::span<const EntityId> _fleet, std::span<const EntityId> _scene) noexcept
{
  /*
   * Every member, not any. A route order names the whole fleet, and the
   * authority refuses the whole thing for one id it cannot place -- so a client
   * that sent the leg because *most* of the fleet was visible would be sending
   * an order it had reason to expect back.
   *
   * An empty fleet is not in the scene. There is nothing to fly, and answering
   * "yes, vacuously" would let the caller send an order for nobody.
   */
  if (_fleet.empty())
  {
    return false;
  }
  for (const EntityId id : _fleet)
  {
    if (std::find(_scene.begin(), _scene.end(), id) == _scene.end())
    {
      return false;
    }
  }
  return true;
}

void RoutePlan::Set(std::span<const RouteLeg> _legs) noexcept
{
  Clear();
  if (_legs.empty() || _legs.size() > m_legs.size())
  {
    /*
     * Refused whole rather than truncated.
     *
     * A plan that silently stopped nine jumps short would strand a fleet
     * somewhere the player believes is on the way -- and they would find out by
     * looking, hours later, at a fleet that has been sitting at a gate. The
     * caller sees `None` and can say so.
     */
    return;
  }

  for (std::size_t index = 0; index < _legs.size(); ++index)
  {
    m_legs[index] = _legs[index];
  }
  m_count = static_cast<std::uint32_t>(_legs.size());
  m_hop = 0;
  m_state = RouteState::Ready;
}

void RoutePlan::Clear() noexcept
{
  m_count = 0;
  m_hop = 0;
  m_flyingSeq = 0;
  m_state = RouteState::None;
}

RouteLeg RoutePlan::Ready() const noexcept
{
  return m_state == RouteState::Ready && m_hop < m_count ? m_legs[m_hop] : RouteLeg{};
}

void RoutePlan::NoteSent(std::uint32_t _clientOrderSeq) noexcept
{
  if (m_state != RouteState::Ready)
  {
    return; // Nothing was ready, so nothing was sent that this plan owns.
  }
  m_flyingSeq = _clientOrderSeq;
  m_state = RouteState::Flying;
}

void RoutePlan::NoteFeedback(const OrderFeedback& _feedback) noexcept
{
  if (m_state != RouteState::Flying)
  {
    return;
  }

  bool over = false;
  bool seen = false;
  for (std::uint32_t index = 0; index < _feedback.orderCount; ++index)
  {
    const OrderProgress& progress = _feedback.orders[index];
    if (progress.clientOrderSeq != m_flyingSeq)
    {
      continue;
    }
    seen = true;
    over = progress.finished;
    break;
  }

  /*
   * The second way an order ends, and it is not an optimisation.
   *
   * An order that finished *between* two snapshots is in no record: the list
   * describes what the authority is still working on. What says it happened is
   * the header's high-water mark passing the sequence -- one monotonic number
   * that closes the loop for every order at once (ADR-004 §6). A plan that
   * waited for a record that had already been and gone would stall for ever,
   * one hop short of its destination, on a route whose hops are twenty seconds
   * apart and whose snapshots are fifty milliseconds.
   */
  if (!seen && _feedback.lastOrderSeqProcessed >= m_flyingSeq && m_flyingSeq != 0)
  {
    over = true;
  }

  if (!over)
  {
    return;
  }

  ++m_hop;
  m_flyingSeq = 0;
  m_state = m_hop < m_count ? RouteState::Ready : RouteState::None;
}

void RoutePlan::Halt() noexcept
{
  if (m_state == RouteState::None)
  {
    return; // A plan that already arrived did not halt; it finished.
  }
  m_state = RouteState::Halted;
}

} // namespace Neuron
