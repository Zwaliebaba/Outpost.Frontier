#include "pch.h"

#include "OrderGhost.h"

#include <algorithm>

using namespace DirectX;

namespace Neuron
{

OrderGhost* OrderGhostList::Find(std::uint32_t _clientOrderSeq) noexcept
{
  const auto found = std::find_if(m_ghosts.begin(), m_ghosts.end(),
                                  [_clientOrderSeq](const OrderGhost& _g) { return _g.clientOrderSeq == _clientOrderSeq; });
  return found == m_ghosts.end() ? nullptr : &*found;
}

bool OrderGhostList::Add(const OrderIntent& _intent, const OrderPreview& _preview, const XMFLOAT2& _originMetres,
                         double _nowSeconds)
{
  if (_intent.orderSeq == 0)
  {
    // Zero is the pre-check's sequence, which means "not sent". A ghost keyed
    // on it could never be matched by an ack, so refusing it here is better
    // than tracking one that can only time out.
    return false;
  }
  if (Find(_intent.orderSeq) != nullptr || m_ghosts.size() >= MAX_GHOSTS)
  {
    return false;
  }

  OrderGhost ghost;
  ghost.clientOrderSeq = _intent.orderSeq;
  ghost.state = GhostState::Pending;
  ghost.targetMetres = XMFLOAT2{_intent.targetXMetres, _intent.targetYMetres};
  ghost.originMetres = _originMetres;
  ghost.preview = _preview;
  ghost.stateSinceSeconds = _nowSeconds;

  ghost.queued = _intent.queued;
  ghost.firstEntityId = _intent.entityCount > 0 ? _intent.entityIds[0] : INVALID_ENTITY_ID;

  // Its own target is its first leg. A queued order that is later accepted
  // hands this leg to the chain it joins and stops existing on its own.
  ghost.legs[0] = GhostLeg{ghost.targetMetres, _preview.etaSeconds};
  ghost.queuedLegCount = 1;

  m_ghosts.push_back(ghost);
  return true;
}

OrderGhost* OrderGhostList::FindChain(const OrderGhost& _appending) noexcept
{
  if (_appending.firstEntityId == INVALID_ENTITY_ID)
  {
    return nullptr;
  }
  for (OrderGhost& ghost : m_ghosts)
  {
    // Not itself, not one that is on its way out, and sharing the ship the
    // authority resolves the append by.
    if (&ghost == &_appending || ghost.state == GhostState::Rejected)
    {
      continue;
    }
    if (ghost.firstEntityId == _appending.firstEntityId && ghost.queuedLegCount < MAX_GHOST_LEGS)
    {
      return &ghost;
    }
  }
  return nullptr;
}

void OrderGhostList::Refuse(std::uint32_t _clientOrderSeq, std::uint16_t _reasonCode, bool _local, double _nowSeconds)
{
  OrderGhost* ghost = Find(_clientOrderSeq);
  if (ghost == nullptr || ghost->state == GhostState::Rejected)
  {
    // Already bouncing. A second refusal must not restart the animation, or a
    // duplicated ack would hold the ghost on screen indefinitely.
    return;
  }
  ghost->state = GhostState::Rejected;
  ghost->reasonCode = _reasonCode;
  ghost->local = _local;
  ghost->stateSinceSeconds = _nowSeconds;
}

void OrderGhostList::Forget(std::uint32_t _clientOrderSeq) noexcept
{
  std::erase_if(m_ghosts, [_clientOrderSeq](const OrderGhost& _g) { return _g.clientOrderSeq == _clientOrderSeq; });
}

void OrderGhostList::OnVerdict(const OrderVerdict& _verdict, double _nowSeconds)
{
  if (!_verdict.accepted)
  {
    Refuse(_verdict.orderSeq, _verdict.reasonCode, false, _nowSeconds);
    return;
  }

  OrderGhost* ghost = Find(_verdict.orderSeq);
  if (ghost == nullptr || ghost->state == GhostState::Rejected)
  {
    // An acceptance for a ghost that already bounced would be the two halves of
    // the loop disagreeing. The bounce is what the player saw, so it stands.
    return;
  }
  /*
   * An accepted *append* joins the chain it was queued behind (S12c).
   *
   * On acceptance rather than on send, which is the whole reason a queued
   * waypoint is its own ghost first: a fifth leg is refused with `QueueFull`,
   * and a merge at send time would have had the refusal bounce the four legs
   * the player already had. Merging here means a refused append bounces alone
   * and the chain never notices -- which is what `Refuse` already does, with no
   * special case for queues in it at all.
   *
   * The chain then **takes this order's sequence**, exactly as
   * `World::IngestOrders` makes the group take it. Both sides name the plan
   * after the most recent order that shaped it, so the record the next snapshot
   * carries matches the ghost that is left.
   */
  if (ghost->queued)
  {
    if (OrderGhost* chain = FindChain(*ghost); chain != nullptr)
    {
      chain->legs[chain->queuedLegCount] = ghost->legs[0];
      ++chain->queuedLegCount;
      chain->clientOrderSeq = ghost->clientOrderSeq;
      chain->serverOrderId = _verdict.serverOrderId != 0 ? _verdict.serverOrderId : chain->serverOrderId;
      chain->targetMetres = ghost->targetMetres;
      chain->preview = ghost->preview;
      chain->state = GhostState::UnderWay;
      chain->stateSinceSeconds = _nowSeconds;

      const std::uint32_t merged = ghost->clientOrderSeq;
      std::erase_if(m_ghosts, [merged](const OrderGhost& _ghost) { return _ghost.clientOrderSeq == merged &&
                                                                          _ghost.queued; });
      return;
    }
    // Nothing to join -- the chain finished, or this is the first order for
    // these ships. It stands on its own, which is what the authority does too.
  }

  ghost->serverOrderId = _verdict.serverOrderId;
  if (ghost->state != GhostState::UnderWay)
  {
    ghost->state = GhostState::UnderWay;
    ghost->stateSinceSeconds = _nowSeconds;
  }
}

void OrderGhostList::OnFeedback(const OrderFeedback& _feedback, double _nowSeconds)
{
  // Which ghosts this feedback settled: decided by the authority and no longer
  // running. Recorded rather than erased in place, because erasing while the
  // loop is walking the vector is the one way to get this wrong.
  bool settled[MAX_GHOSTS] = {};

  for (std::size_t index = 0; index < m_ghosts.size(); ++index)
  {
    OrderGhost& ghost = m_ghosts[index];
    if (ghost.state == GhostState::Rejected)
    {
      continue; // Bouncing. The authority's opinion arrived too late to matter.
    }

    const OrderProgress* progress = nullptr;
    for (std::uint32_t order = 0; order < _feedback.orderCount; ++order)
    {
      if (_feedback.orders[order].clientOrderSeq == ghost.clientOrderSeq)
      {
        progress = &_feedback.orders[order];
        break;
      }
    }

    if (progress != nullptr)
    {
      ghost.serverOrderId = progress->serverOrderId;
      ghost.legIndex = progress->legIndex;
      ghost.legCount = progress->legCount;
      ghost.memberCount = progress->memberCount;
      ghost.authorityEtaSeconds = progress->etaSeconds;
      if (ghost.state != GhostState::UnderWay)
      {
        ghost.state = GhostState::UnderWay;
        ghost.stateSinceSeconds = _nowSeconds;
      }
      continue;
    }

    if (ghost.clientOrderSeq > _feedback.lastOrderSeqProcessed)
    {
      continue; // Still in flight as far as the authority is concerned.
    }

    // Decided, and not running. A ghost that was already promoted has simply
    // finished -- there is nothing left to promise, and the ships themselves
    // are now the record of it, so it goes at once.
    if (ghost.state == GhostState::UnderWay)
    {
      settled[index] = true;
      continue;
    }

    // Still pending, which is the ambiguous case: this is either a refusal
    // whose ack has not arrived or an order that began and ended between two
    // snapshots. Waiting the grace out costs half a second of a ghost that is
    // over; not waiting costs a refusal its bounce, and the print is explicit
    // that a refusal without one is indistinguishable from a dropped packet.
    if (ghost.answerDueSeconds < 0.0)
    {
      ghost.answerDueSeconds = _nowSeconds + SETTLE_GRACE_SECONDS;
    }
  }

  std::size_t kept = 0;
  for (std::size_t index = 0; index < m_ghosts.size(); ++index)
  {
    if (!settled[index])
    {
      m_ghosts[kept] = m_ghosts[index];
      ++kept;
    }
  }
  m_ghosts.resize(kept);
}

void OrderGhostList::Advance(double _nowSeconds)
{
  // Counted before the erase, because a ghost that timed out and one whose
  // bounce finished are both removals and only the first is a failure.
  for (const OrderGhost& ghost : m_ghosts)
  {
    if (ghost.state == GhostState::Pending && _nowSeconds - ghost.stateSinceSeconds >= PENDING_TIMEOUT_SECONDS)
    {
      ++m_timedOut;
    }
  }

  std::erase_if(m_ghosts,
                [_nowSeconds](const OrderGhost& _g)
                {
                  if (_g.state == GhostState::Rejected)
                  {
                    return _nowSeconds - _g.stateSinceSeconds >= BOUNCE_SECONDS;
                  }
                  if (_g.state != GhostState::Pending)
                  {
                    return false;
                  }
                  // The grace the snapshot opened, or the link giving up.
                  return (_g.answerDueSeconds >= 0.0 && _nowSeconds >= _g.answerDueSeconds) ||
                         _nowSeconds - _g.stateSinceSeconds >= PENDING_TIMEOUT_SECONDS;
                });
}

float OrderGhostList::BounceFraction(const OrderGhost& _ghost, double _nowSeconds) noexcept
{
  if (_ghost.state != GhostState::Rejected)
  {
    return 0.0f;
  }
  const double elapsed = _nowSeconds - _ghost.stateSinceSeconds;
  return static_cast<float>(std::clamp(elapsed / BOUNCE_SECONDS, 0.0, 1.0));
}

void OrderGhostList::Clear() noexcept
{
  m_ghosts.clear();
}

} // namespace Neuron
