#include "pch.h"

#include "World.h"

#include "Eta.h"
#include "Formation.h"

#include "EntityRecord.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

/*
 * The order half of `World`: submission, ingest, the group table and its
 * lifecycle. `World.cpp` keeps the movement half -- steering, integration,
 * contact -- and the tables both halves read.
 *
 * Two translation units, one class, and the seam is the one the tick already
 * draws: everything here runs in `IngestOrders`/`GroupAdvance` (plus the
 * between-ticks `SubmitOrder`), everything there in `Steering`/`Integrate`/
 * `Separate`. The two halves meet only through `Guidance` -- a group *writes*
 * it, steering *reads* it -- which is the same one-way handoff ADR-005 §1
 * designed so that the movement model never learns that groups exist.
 */

namespace Game
{
namespace
{

/// Same clamp as `World.cpp`'s -- one line over a public constant, duplicated
/// rather than exported, so each half stays self-contained.
[[nodiscard]] float ClampToPlayArea(float _metres) noexcept
{
  return std::clamp(_metres, -World::PLAY_AREA_HALF_EXTENT_METRES, World::PLAY_AREA_HALF_EXTENT_METRES);
}

/// Brings an angle into (-pi, pi]. DirectXMath's own, so the wrap is the same
/// one the camera and the wire use -- and not an `Est` function (ADR-010 §6).
[[nodiscard]] float WrapAngle(float _radians) noexcept
{
  return XMScalarModAngle(_radians);
}

} // namespace

ValidationView World::Validation()
{
  m_validationMarks.resize(m_ids.size());
  for (std::size_t slot = 0; slot < m_ids.size(); ++slot)
  {
    m_validationMarks[slot].xCm = Neuron::MetresToCentimetres(m_positions[slot].x);
    m_validationMarks[slot].yCm = Neuron::MetresToCentimetres(m_positions[slot].y);
    m_validationMarks[slot].hullClass = static_cast<HullClass>(m_classes[slot]);
  }

  ValidationView view;
  view.shipIds = m_ids;
  view.shipMarks = m_validationMarks;
  view.queuedLegs = 0; // Per-group, and resolved in SubmitOrder where the group is known.

  /*
   * The station, if this grid has one (ADR-017 §2).
   *
   * Its position comes out of the same table every other ship's does rather
   * than being assumed to be the grid centre. It *is* the centre today -- the
   * anchor's origin is the structure's universe position -- but "the station
   * is at (0,0)" is a fact about how the bake places things, and a validator
   * that hard-coded it would be judging distance against an assumption instead
   * of against the world.
   */
  if (m_stationShip != INVALID_SHIP_ID)
  {
    std::uint32_t slot = 0;
    if (FindSlot(m_stationShip, slot))
    {
      view.stationAnchor = m_anchor;
      view.stationXCm = Neuron::MetresToCentimetres(m_positions[slot].x);
      view.stationYCm = Neuron::MetresToCentimetres(m_positions[slot].y);
    }
  }
  return view;
}

OrderVerdict World::SubmitOrder(const OrderSubmit& _order)
{
  NEURON_ASSERT_OWNER(m_owner);

  // Appending needs to know which group it would append to, and that is the
  // group the *first* named ship already belongs to. One group per order means
  // a selection spanning two groups cannot append to both, so the answer is the
  // first member's -- which is also what the client's ghost is drawn against.
  ValidationView view = Validation();
  if (_order.queueMode == QueueMode::Append && _order.shipCount > 0)
  {
    if (const OrderGroup* group = FindGroupOf(_order.shipIds[0]))
    {
      view.queuedLegs = group->legCount;
    }
  }

  const OrderVerdict verdict = ValidateOrder(view, _order);
  if (!verdict.accepted)
  {
    return verdict;
  }

  const bool append = _order.queueMode == QueueMode::Append;

  OrderGroup group;

  /*
   * An append is **not given an id here**, and that is the correction (S12b).
   *
   * It used to take the next one, and `IngestOrders` then threw it away when
   * the append landed on an existing group -- so the ack named an order that
   * appeared in no snapshot, ever, and the sequence counter skipped a number
   * per queued waypoint. Zero is already the verdict's "no order" (`World.h`),
   * and the client learns the real id from the order record the next snapshot
   * carries, which is a path `OnFeedback` already walks.
   *
   * Why the id cannot simply be resolved here: the group an append joins is
   * found at *ingest*, not at submit, and it has to be. A Replace and an Append
   * submitted between the same pair of ticks are both pending when the second
   * one is validated, so at submit the Append can only see the group the
   * Replace is about to destroy. Resolving early would append to a corpse.
   */
  group.serverOrderId = append ? 0 : m_nextOrderId;
  group.clientOrderSeq = _order.orderSeq;
  group.formation = _order.formation;
  group.state = OrderState::Underway;
  group.memberCount = _order.shipCount;
  std::copy(_order.shipIds, _order.shipIds + _order.shipCount, group.members);

  // The leg arrives quantised and is converted once, here. Everything downstream
  // works in metres, and the number it works from is the number that was
  // validated -- not a second conversion of the player's original click.
  group.legCount = 1;
  group.legIndex = 0;
  group.legs[0].anchorMetres = XMFLOAT2{ClampToPlayArea(Neuron::CentimetresToMetres(_order.target.xCm)),
                                        ClampToPlayArea(Neuron::CentimetresToMetres(_order.target.yCm))};
  group.legs[0].facingRadians = WrapAngle(Neuron::HeadingToRadians(_order.target.facingTurns16));
  group.legStartTick = m_tick;
  group.legDeadlineTick = 0; // Sized by ApplyLeg, which is where the stations exist.

  m_pending.push_back(PendingOrder{group, _order.queueMode, _order.kind, _order.anchor});
  if (!append)
  {
    ++m_nextOrderId;
  }

  OrderVerdict accepted;
  accepted.accepted = true;
  accepted.reason = OrderReason::Accepted;
  accepted.serverOrderId = group.serverOrderId;
  return accepted;
}

void World::IngestOrders()
{
  // Drained in arrival order, and within an order in the order the ships were
  // listed. Both are part of the replay contract: the same log has to produce
  // the same assignment every time (ADR-005 §5).
  for (PendingOrder& pending : m_pending)
  {
    OrderGroup& submitted = pending.group;
    m_lastOrderSeqProcessed = std::max(m_lastOrderSeqProcessed, submitted.clientOrderSeq);

    /*
     * A dock never enters the group table (ADR-017 §2). It is not a movement
     * plan with a destination -- it is the fleet leaving the world, all at
     * once, which is what "together, one moment" means. So ingest consumes it
     * into transfer records and the ships are gone by the time this tick's
     * steering runs.
     *
     * Out of their groups first, and that falls out of `TransferOut` calling
     * `Despawn`: a group holding a docked id would keep solving a station for
     * a ship that is no longer anywhere.
     */
    if (pending.kind == OrderKind::Dock)
    {
      for (std::uint16_t index = 0; index < submitted.memberCount; ++index)
      {
        TransferRequest request;
        request.kind = TransferKind::Dock;
        request.anchor = pending.anchor;
        if (TransferOut(submitted.members[index], request))
        {
          m_filed.push_back(request);
        }
      }
      continue;
    }

    const bool append = pending.queueMode == QueueMode::Append;
    if (append)
    {
      OrderGroup* existing = FindGroupOf(submitted.members[0]);
      if (existing != nullptr && existing->legCount < MAX_ORDER_LEGS)
      {
        /*
         * Appending to a group that had already **finished** (it lingers a
         * moment before retirement) makes the new leg the current leg, and a
         * current leg needs its clock: without this reset it inherits the
         * previous leg's spent deadline and times out the tick it starts --
         * the fleet "skips" a waypoint nobody saw it fly. The every-tick
         * re-solve used to paper over half of this by rewriting guidance
         * anyway; now that solving is change-driven, the zeroed deadline is
         * also what tells the solve the leg is new.
         */
        if (existing->state == OrderState::Done)
        {
          existing->legStartTick = m_tick;
          existing->legDeadlineTick = 0;
        }

        // The group keeps its id and its ships; only the plan grew.
        existing->legs[existing->legCount] = submitted.legs[0];
        ++existing->legCount;
        existing->state = OrderState::Underway;

        /*
         * And it **takes the appending order's sequence** (S12b).
         *
         * A group is reported under one `clientOrderSeq`, and the client
         * matches its ghosts by that. Keeping the original meant the appended
         * order's sequence appeared in no record ever: the high-water mark
         * passed it, `OnFeedback` read that as "decided and no longer running",
         * and the ghost the player had just created retired without a bounce or
         * a promotion -- the silent disappearance `puck-and-wheel.png` §4
         * forbids outright.
         *
         * Naming the group after the most recent order that shaped it is what
         * makes the newest ghost the one that gets promoted, and it is also the
         * ghost that knows about the whole queue.
         */
        existing->clientOrderSeq = submitted.clientOrderSeq;
        continue;
      }

      // Nothing to append to. Validation passed because there was no group and
      // therefore no full queue, so this becomes the first leg of a new one --
      // and *now* it needs an id, which submit deliberately did not give it.
      submitted.serverOrderId = m_nextOrderId;
      ++m_nextOrderId;
    }

    // Replace: the members leave whatever they were doing. A ship may belong to
    // one group at a time, or two plans would write the same guidance and the
    // last writer would win by array order.
    for (std::uint16_t index = 0; index < submitted.memberCount; ++index)
    {
      ForgetShipInGroups(submitted.members[index]);
    }

    submitted.legStartTick = m_tick;
    submitted.legDeadlineTick = 0; // As above -- sized once the stations are solved.
    m_groups.push_back(submitted);
  }
  m_pending.clear();

  /*
   * Solve the groups whose stations are stale -- and **only** those. This ran
   * unconditionally for every live group once, 20 times a second, re-deriving
   * the same stations and rewriting the same guidance almost every time; the
   * work should scale with what changed, not with what exists.
   *
   * "Stale" is decidable from fields the group already carries, both hashed:
   * a zero deadline is a leg that has not been solved yet (ingest and
   * `GroupAdvance` both zero it when a leg starts), and a `memberCount` that
   * disagrees with `solvedMemberCount` is a casualty since the last solve --
   * the survivors close the hole, which is the one reason a solved leg ever
   * needs solving again.
   */
  for (OrderGroup& group : m_groups)
  {
    if (group.state == OrderState::Done)
    {
      continue;
    }
    if (group.legDeadlineTick == 0 || group.memberCount != group.solvedMemberCount)
    {
      ApplyLeg(group);
    }
  }
}

void World::ApplyLeg(OrderGroup& _group)
{
  if (_group.legIndex >= _group.legCount)
  {
    return;
  }

  // Only the members that still exist. A group whose ships died mid-leg solves
  // for the survivors, which is what keeps a formation from leaving a hole
  // where a casualty was.
  ShipId living[MAX_SHIPS_PER_ORDER] = {};
  std::uint32_t livingCount = 0;
  for (std::uint16_t index = 0; index < _group.memberCount; ++index)
  {
    std::uint32_t slot = 0;
    if (FindSlot(_group.members[index], slot))
    {
      living[livingCount] = _group.members[index];
      ++livingCount;
    }
  }
  if (livingCount == 0)
  {
    _group.state = OrderState::Done;
    _group.doneTick = m_tick;
    return;
  }

  const OrderGroupLeg& leg = _group.legs[_group.legIndex];

  FormationStation stations[MAX_SHIPS_PER_ORDER] = {};
  const auto lookup = [](ShipId _shipId, void* _context) -> HullClass
  {
    const World& world = *static_cast<const World*>(_context);
    std::uint32_t slot = 0;
    if (!world.FindSlot(_shipId, slot))
    {
      return HullClass::Interceptor; // Unreachable: the caller filtered to living ships.
    }
    return static_cast<HullClass>(world.Classes()[slot]);
  };

  const std::uint32_t solved = SolveFormation(_group.formation, std::span<const ShipId>{living, livingCount}, lookup, this,
                                              leg.anchorMetres, leg.facingRadians, std::span<FormationStation>{stations});

  for (std::uint32_t index = 0; index < solved; ++index)
  {
    std::uint32_t slot = 0;
    if (!FindSlot(stations[index].shipId, slot))
    {
      continue;
    }
    Guidance& guidance = m_guidances[slot];
    guidance.mode = GuidanceMode::Seek;
    guidance.targetXMetres = ClampToPlayArea(stations[index].positionMetres.x);
    guidance.targetYMetres = ClampToPlayArea(stations[index].positionMetres.y);
    guidance.arrivalFacingRadians = leg.facingRadians;
  }

  // Solved for this membership. `IngestOrders` compares this against
  // `memberCount` to decide whether the stations are stale, so a casualty --
  // the one thing that changes a solved leg's answer -- re-triggers the solve
  // and the survivors close the hole.
  _group.solvedMemberCount = _group.memberCount;

  /*
   * The deadline, set from the leg's own estimate now that the stations are
   * resolved and `LegEtaSeconds` has something to measure.
   *
   * Here rather than where `legStartTick` is set, because the estimate needs
   * the guidance this function just wrote: before it, a member's remaining
   * distance is whatever the *previous* leg left behind.
   *
   * **Once per leg, and that is the whole reason for the zero check.** This
   * function runs again whenever a casualty shrinks the membership mid-leg,
   * and computing the deadline unconditionally would push it forward on every
   * such solve. A leg that could never complete would then hold its group past
   * the timeout, which is the exact failure the timeout exists to prevent.
   */
  if (_group.legDeadlineTick != 0)
  {
    return;
  }

  const float expected = LegEtaSeconds(_group);
  if (expected < 0.0f)
  {
    // Nothing in the group can move, so no estimate. It still gets a deadline,
    // because a group that could never advance must not sit in the list
    // forever holding an order id and a slot in the snapshot.
    _group.legDeadlineTick = m_tick + LEG_TIMEOUT_MAX_TICKS;
    return;
  }

  const float ticks = expected * LEG_TIMEOUT_FACTOR / TICK_SECONDS;
  const auto bounded = static_cast<std::uint32_t>(std::min(ticks, static_cast<float>(LEG_TIMEOUT_MAX_TICKS)));
  _group.legDeadlineTick = m_tick + std::min(bounded + LEG_TIMEOUT_GRACE_TICKS, LEG_TIMEOUT_MAX_TICKS);
}

float World::LegEtaSeconds(const OrderGroup& _group) const noexcept
{
  if (_group.state == OrderState::Done || _group.legIndex >= _group.legCount)
  {
    return -1.0f; // Nothing under way, so nothing to be due.
  }

  TravelLeg legs[MAX_SHIPS_PER_ORDER];
  std::uint32_t count = 0;
  for (std::uint16_t index = 0; index < _group.memberCount && count < MAX_SHIPS_PER_ORDER; ++index)
  {
    std::uint32_t slot = 0;
    if (!FindSlot(_group.members[index], slot))
    {
      continue; // Died mid-leg. The rest of the group still arrives.
    }

    // Against the ship's own **guidance target** rather than the leg's anchor:
    // that is the station `ApplyLeg` resolved for it, and the far end of a Line
    // is most of a kilometre past the anchor.
    const Guidance& guidance = m_guidances[slot];
    const float dx = guidance.targetXMetres - m_positions[slot].x;
    const float dy = guidance.targetYMetres - m_positions[slot].y;

    /*
     * Speed **along the way it is going**, not speed outright.
     *
     * A ship still swinging onto its heading is moving fast in a direction that
     * does not help, and crediting the full magnitude would promise an arrival
     * its velocity is not carrying it toward. The projection is negative while
     * it is pointing away, and the model clamps that to zero -- which is the
     * honest reading: no progress is being made.
     */
    const float distance = std::sqrt(dx * dx + dy * dy);
    float closing = 0.0f;
    if (distance > 0.0f)
    {
      closing = (m_velocities[slot].x * dx + m_velocities[slot].y * dy) / distance;
    }

    legs[count] = TravelLeg{static_cast<HullClass>(m_classes[slot]), distance, closing};
    ++count;
  }
  return GroupTravelSeconds(std::span<const TravelLeg>{legs, count});
}

void World::GroupAdvance()
{
  for (OrderGroup& group : m_groups)
  {
    if (group.state == OrderState::Done || group.legIndex >= group.legCount)
    {
      continue;
    }

    bool everyoneArrived = true;
    std::uint32_t living = 0;
    for (std::uint16_t index = 0; index < group.memberCount; ++index)
    {
      std::uint32_t slot = 0;
      if (!FindSlot(group.members[index], slot))
      {
        continue;
      }
      ++living;

      const Guidance& guidance = m_guidances[slot];
      const float dx = guidance.targetXMetres - m_positions[slot].x;
      const float dy = guidance.targetYMetres - m_positions[slot].y;
      if (dx * dx + dy * dy > ARRIVAL_TOLERANCE_METRES * ARRIVAL_TOLERANCE_METRES)
      {
        everyoneArrived = false;
        break;
      }
    }

    if (living == 0)
    {
      group.state = OrderState::Done;
      group.doneTick = m_tick;
      continue;
    }

    // Or the leg ran past its deadline. A straggler behind a station must not
    // wedge the fleet behind it forever (ADR-005 §2), and the tick index is the
    // only clock this simulation has. The deadline is the leg's own -- set from
    // what the leg was estimated to take, not from a constant that fits no
    // journey (`LEG_TIMEOUT_FACTOR`).
    const bool timedOut = m_tick >= group.legDeadlineTick;
    if (!everyoneArrived && !timedOut)
    {
      group.state = OrderState::Underway;
      continue;
    }

    ++group.legIndex;
    group.legStartTick = m_tick;
    group.legDeadlineTick = 0; // A new leg: the next ApplyLeg sizes the deadline to it.
    if (group.legIndex >= group.legCount)
    {
      group.state = OrderState::Done;
      group.doneTick = m_tick;
      continue;
    }
    group.state = OrderState::Arriving;
    ApplyLeg(group);
  }

  /*
   * Retire what finished a while ago (the linger's own comment, `World.h`, is
   * the argument for "a while" rather than "now"). This is the **only** exit
   * from the table for a group whose ships outlive it -- without it, finished
   * orders accumulated for the whole session and sixteen of them crowded every
   * live order out of the snapshot.
   *
   * `erase_if` keeps the survivors' relative order, so iteration order stays
   * a pure function of the order log and the replay contract is untouched
   * (ADR-005 §5).
   */
  std::erase_if(m_groups, [this](const OrderGroup& _group)
                { return _group.state == OrderState::Done && m_tick >= _group.doneTick + ORDER_DONE_LINGER_TICKS; });
}

OrderGroup* World::FindGroupOf(ShipId _shipId) noexcept
{
  for (OrderGroup& group : m_groups)
  {
    if (std::find(group.members, group.members + group.memberCount, _shipId) != group.members + group.memberCount)
    {
      return &group;
    }
  }
  return nullptr;
}

void World::ForgetShipInGroups(ShipId _shipId) noexcept
{
  for (OrderGroup& group : m_groups)
  {
    ShipId* found = std::find(group.members, group.members + group.memberCount, _shipId);
    if (found == group.members + group.memberCount)
    {
      continue;
    }
    // Shift down rather than swap-and-pop: member order is the order the client
    // listed them, and a group that reordered itself would reorder the ghost.
    std::copy(found + 1, group.members + group.memberCount, found);
    --group.memberCount;
    group.members[group.memberCount] = INVALID_SHIP_ID;
  }

  // And the emptied husks go with it, which is what the contract on this
  // function always said: an order cannot outlive its last member. Both
  // callers used to repeat this line themselves.
  std::erase_if(m_groups, [](const OrderGroup& _group) { return _group.memberCount == 0; });
}

} // namespace Game
