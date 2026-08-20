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

/// `SolveFormation`'s class lookup, over a world's own tables. A linear scan
/// rather than the slot table, because the berth scan asks about the ships in
/// an *order* and those are ids, not slots.
struct WorldClassLookup
{
  const World* world = nullptr;

  [[nodiscard]] static HullClass Of(ShipId _shipId, void* _context) noexcept
  {
    const auto* lookup = static_cast<const WorldClassLookup*>(_context);
    const std::span<const ShipId> ids = lookup->world->Ids();
    for (std::size_t slot = 0; slot < ids.size(); ++slot)
    {
      if (ids[slot] == _shipId)
      {
        return static_cast<HullClass>(lookup->world->Classes()[slot]);
      }
    }
    return HullClass::Interceptor;
  }
};

} // namespace

bool World::ShipIsUnderOrders(ShipId _shipId) const noexcept
{
  const auto carries = [_shipId](const OrderGroup& _group) noexcept
  {
    if (_group.state == OrderState::Done || _group.legIndex >= _group.legCount)
    {
      return false; // A finished group is lingering to be seen, not going anywhere.
    }
    for (std::uint16_t index = 0; index < _group.memberCount; ++index)
    {
      if (_group.members[index] == _shipId)
      {
        return true;
      }
    }
    return false;
  };

  for (const OrderGroup& group : m_groups)
  {
    if (carries(group))
    {
      return true;
    }
  }
  // Pending too, for the reason the intention clause reads them: two orders
  // accepted in one batch are both true before either is ingested.
  for (const PendingOrder& pending : m_pending)
  {
    if (carries(pending.group))
    {
      return true;
    }
  }
  return false;
}

/*
 * Is a solved placement free? (ADR-026 §2, ADR-017 §4.)
 *
 * One predicate, two callers: the parking ring asks it about a berth and a Move
 * asks it about a slid destination. It was `FindBerth`'s inner loops until
 * ADR-026 gave the second caller a reason to exist, and it is shared rather
 * than copied because a second clearance loop is a second thing to keep in step
 * with ADR-015's factor -- and one of the two would eventually lose.
 *
 * Free means both of:
 *
 * - the solved stations clear **every hull on the grid** by the avoidance
 *   model's own `AVOID_CLEARANCE_FACTOR`, ships in `_exclude` aside -- a fleet
 *   must not count itself as the reason it cannot go somewhere; and
 * - the placement lands inside **no other group's final-leg anchor**, reading
 *   the pending queue as well as the live groups.
 *
 * That second clause is the one that does the work nobody expects. It is what
 * makes two fleets sent to the same point on the same tick pick different
 * placements **with nothing reserved and nothing stored**, and it has to read
 * pending orders because both fleets are accepted before either is ingested --
 * the suite found exactly that when the parking ring shipped without it.
 *
 * The class lookup is a callback rather than the world's own tables because
 * `FindBerth` is asked about ships that have only just arrived, and passing the
 * caller's lookup keeps that path bit-identical to what it was.
 */
bool World::IsPlacementFree(std::span<const FormationStation> _stations, HullClass (*_hullClassOf)(ShipId, void*), void* _context,
                            std::span<const ShipId> _exclude, const XMFLOAT2& _candidateMetres, float _boundingMetres,
                            std::uint32_t _ignoreServerOrderId, bool _forMovePlacement) const noexcept
{
  for (const FormationStation& station : _stations)
  {
    const float solvedRadius = ShipClass(_hullClassOf(station.shipId, _context)).collisionRadiusMetres;
    for (std::size_t slot = 0; slot < m_ids.size(); ++slot)
    {
      if (std::find(_exclude.begin(), _exclude.end(), m_ids[slot]) != _exclude.end())
      {
        continue;
      }
      const float clearance =
        AVOID_CLEARANCE_FACTOR * (solvedRadius + ShipClass(static_cast<HullClass>(m_classes[slot])).collisionRadiusMetres);
      const float dx = station.positionMetres.x - m_positions[slot].x;
      const float dy = station.positionMetres.y - m_positions[slot].y;
      if (dx * dx + dy * dy < clearance * clearance)
      {
        /*
         * **A ship that is going somewhere is traffic, not an obstruction**
         * (ADR-026 §2). Only hulls that will still be there can block a
         * destination; a mover is ADR-015's problem, and ADR-015 solves it --
         * the fleet flies through space the other has vacated by the time it
         * arrives. Without this, two fleets ordered to swap places both slide,
         * each because the other is standing on its destination *now*, which
         * is the one case the avoidance model was written to make work.
         *
         * Asked only on a hit, and that is the whole reason it can afford to
         * be a scan: a placement in open space never reaches this line.
         *
         * `FindBerth` passes false and keeps every hull blocking. A berth is
         * chosen for a fleet that is *arriving*, and one already occupied is
         * occupied whether or not its tenant has plans.
         */
        if (_forMovePlacement && ShipIsUnderOrders(m_ids[slot]))
        {
          continue;
        }
        return false;
      }
    }
  }

  const auto conflicts = [&_candidateMetres, _boundingMetres, _ignoreServerOrderId,
                          _forMovePlacement](const OrderGroup& _group) noexcept
  {
    if (_group.legCount == 0)
    {
      return false;
    }
    /*
     * **An intention is a placement, not a request** (ADR-026 2).
     *
     * A group whose leg has never been applied has no deadline stamped, and
     * its anchor is still the raw point somebody asked for -- it has not been
     * through this function yet and may not survive it. Treating that as an
     * intention makes two fleets sent to one point *both* slide off it, each
     * deferring to a claim the other had not actually staked.
     *
     * Ingest places groups in sequence, so by the time the second is placed
     * the first is real, and the asymmetry falls out with nothing stored.
     *
     * `FindBerth` does not take this branch and must not: its parking orders
     * are still pending when the next fleet scans, and reading them is what
     * stops two same-tick undocks picking one berth.
     */
    if (_forMovePlacement && _group.legDeadlineTick == 0)
    {
      return false;
    }
    /*
     * A group is never its own obstruction, and forgetting that is not a
     * subtle bug: an order being placed is already in the table with its
     * asked point as its final-leg anchor, so without this clause *every*
     * destination conflicts with itself and every order in an empty sky
     * slides by a ring. The suite said so immediately, in about twenty
     * voices.
     *
     * Zero ignores nothing, which is what `FindBerth` wants: a fleet being
     * undocked has no group yet.
     */
    if (_ignoreServerOrderId != 0 && _group.serverOrderId == _ignoreServerOrderId)
    {
      return false;
    }
    const XMFLOAT2& intent = _group.legs[_group.legCount - 1].anchorMetres;
    const float dx = intent.x - _candidateMetres.x;
    const float dy = intent.y - _candidateMetres.y;
    return dx * dx + dy * dy < _boundingMetres * _boundingMetres;
  };

  for (const OrderGroup& group : m_groups)
  {
    if (conflicts(group))
    {
      return false;
    }
  }
  /*
   * The pending queue, for `FindBerth` alone. A Move placement skips it for
   * the reason above: a pending order is a request that has not been placed,
   * and it will be placed -- against this group's now-real anchor -- on the
   * very next ingest.
   */
  if (!_forMovePlacement)
  {
    for (const PendingOrder& pending : m_pending)
    {
      if (conflicts(pending.group))
      {
        return false;
      }
    }
  }
  return true;
}

bool World::FindBerth(std::span<const ShipId> _ships, FormationId _formation,
                      const XMFLOAT2& _stationCentreMetres, const XMFLOAT2& _undockPointMetres,
                      XMFLOAT2& _outBerthMetres, float& _outFacingRadians) const
{
  if (_ships.empty() || _ships.size() > MAX_SHIPS_PER_ORDER)
  {
    return false;
  }

  // The bearing the fleet is already pointing along. Candidates fan out from
  // here rather than from an arbitrary zero, so the nearest free berth is
  // usually the first one tried and the fleet rarely crosses its own doorway.
  const float baseBearing =
    std::atan2(_undockPointMetres.y - _stationCentreMetres.y, _undockPointMetres.x - _stationCentreMetres.x);

  WorldClassLookup lookup{this};
  FormationStation stations[MAX_SHIPS_PER_ORDER];

  // One largest-class spacing, the unit the dock radius pads by too.
  float largestSpacing = 0.0f;
  for (const ShipId shipId : _ships)
  {
    largestSpacing = std::max(largestSpacing, ShipClass(WorldClassLookup::Of(shipId, &lookup)).formationSpacingMetres);
  }

  constexpr float BEARING_STEP = DirectX::XM_2PI / static_cast<float>(PARKING_BEARINGS);

  for (const float ring : PARKING_RING_METRES)
  {
    for (std::uint32_t step = 0; step < PARKING_BEARINGS; ++step)
    {
      /*
       * 0, +1, -1, +2, -2, ... -- outward from the undock bearing, alternating.
       * Written as arithmetic on the step index rather than as a table, because
       * a table is a thing that can disagree with the sentence describing it.
       */
      const std::uint32_t stepsOut = (step + 1) / 2;             // 0, 1, 1, 2, 2, 3, 3 ...
      const float side = (step % 2 == 0) ? 1.0f : -1.0f;          // ... and which way each is.
      const float bearing = baseBearing + static_cast<float>(stepsOut) * side * BEARING_STEP;

      const XMFLOAT2 candidate{_stationCentreMetres.x + std::cos(bearing) * ring,
                               _stationCentreMetres.y + std::sin(bearing) * ring};

      // Facing the outward radial, so parked fleets face away from the station
      // rather than at it -- a fleet pointing inward reads as one about to dock.
      const float facing = bearing;

      const std::uint32_t placed = SolveFormation(_formation, _ships, &WorldClassLookup::Of, &lookup, candidate, facing,
                                                  std::span<FormationStation>{stations});
      if (placed == 0)
      {
        continue;
      }

      const float bounding =
        FormationExtentMetres(std::span<const FormationStation>{stations, placed}, candidate) + largestSpacing;
      if (!IsPlacementFree(std::span<const FormationStation>{stations, placed}, &WorldClassLookup::Of, &lookup, _ships, candidate,
                           bounding, 0, false))
      {
        continue;
      }

      _outBerthMetres = candidate;
      _outFacingRadians = facing;
      return true;
    }
  }

  return false;
}

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
  view.reachableAnchors = m_reachable;
  view.queuedLegs = 0; // Per-group, and resolved in SubmitOrder where the group is known.

  /*
   * The field on this grid, and what a hold has room for (ADR-024 §4a).
   *
   * The *anchor* rather than the field, because what `Mine` is judged on is
   * whether there is one here -- the clusters and the pools are simulation
   * state and the client's half of the validator has no copy of them.
   *
   * The three ore volumes are copied out of the economy rather than reached for
   * through it, for the reason `reachableAnchors` is a span of ids: the shared
   * validator sees the intersection of what both machines know, and content is
   * exactly the sort of thing a mismatched pair would read differently.
   */
  if (m_site.Exists())
  {
    view.siteAnchor = m_anchor;
  }
  if (m_economy != nullptr)
  {
    m_validationOreRoom.resize(m_ids.size());
    for (std::size_t slot = 0; slot < m_ids.size(); ++slot)
    {
      m_validationOreRoom[slot] = OreHoldFreeLitres(static_cast<std::uint32_t>(slot));
    }
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      view.oreUnitLitres[ore] = m_economy->ores[ore].unitVolumeLitres;
    }
    view.oreHoldFreeLitres = m_validationOreRoom;
  }

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

  /*
   * And the gate, if this grid is one (ADR-016 §5, U4).
   *
   * Read out of the ship table for the same reason the station is, and gated on
   * the *structure* being there rather than only on the anchor being known: a
   * jump is judged against where the gate stands, so a view that named a
   * destination without a position to judge against would let a fleet jump from
   * anywhere. Both halves of the pair are set together or neither is.
   */
  if (m_jumpAnchor != INVALID_ID && m_gateShip != INVALID_SHIP_ID)
  {
    std::uint32_t slot = 0;
    if (FindSlot(m_gateShip, slot))
    {
      view.jumpAnchor = m_jumpAnchor;
      view.gateXCm = Neuron::MetresToCentimetres(m_positions[slot].x);
      view.gateYCm = Neuron::MetresToCentimetres(m_positions[slot].y);
    }
  }
  return view;
}

OrderVerdict World::SubmitSystemOrder(const OrderSubmit& _order)
{
  return Submit(_order, true);
}

OrderVerdict World::SubmitOrder(const OrderSubmit& _order)
{
  return Submit(_order, false);
}

OrderVerdict World::Submit(const OrderSubmit& _order, bool _systemIssued)
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
  group.systemIssued = _systemIssued;
  group.kind = _order.kind;
  group.anchor = _order.anchor;
  group.oreFilter = _order.oreFilter;
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
      TransferRequest request;
      request.kind = TransferKind::Dock;
      request.anchor = pending.anchor;
      for (std::uint16_t index = 0; index < submitted.memberCount; ++index)
      {
        TransferMember member;
        if (TransferOut(submitted.members[index], member) && !request.AddMember(member))
        {
          break; // The order cap and the crossing cap are the same number.
        }
      }
      if (request.memberCount > 0)
      {
        m_filed.push_back(request);
      }
      continue;
    }

    /*
     * A player's own command ends undock protection (ADR-017 §5).
     *
     * On *ingest* rather than on submit, so the rule is the same one tick that
     * everything else in the order pipeline happens on. The parking order does
     * not count -- that is exactly what `systemIssued` is for, and without the
     * distinction a fleet would lose its fifteen seconds to the order that
     * parks it.
     */
    if (!submitted.systemIssued)
    {
      for (std::uint16_t index = 0; index < submitted.memberCount; ++index)
      {
        std::uint32_t slot = 0;
        if (FindSlot(submitted.members[index], slot))
        {
          m_protectedUntil[slot] = 0;
        }
      }
    }

    /*
     * A Mine is given its cluster and a leg to it (ADR-024 §4b).
     *
     * At ingest and not at submit, for the reason an append resolves its group
     * here: the field the order will be worked against is the one that exists
     * on the tick the order lands, and a cluster chosen a tick earlier could
     * already have been emptied by somebody else's wing.
     *
     * The leg the submit built pointed wherever the client clicked. It is
     * replaced, because a Mine names no destination -- the field a wing works
     * is the one it is standing in, and *which rocks* is the game's answer
     * rather than the player's aim. The facing is kept, so a commander who
     * turned their fleet before ordering still gets the arrangement they set
     * up.
     */
    if (pending.kind == OrderKind::Mine)
    {
      submitted.cluster = RichestCluster(m_site, submitted.oreFilter);
      if (submitted.cluster >= m_site.clusterCount)
      {
        /*
         * A field with nothing this filter accepts. Done on the spot -- an
         * empty field is not an error, it is news (ADR-024 §4a), and the toast
         * the client raises off a completed order is the right way to say so.
         * Refusing would tell the player they did something wrong.
         */
        submitted.legCount = 0;
        submitted.legIndex = 0;
        submitted.state = OrderState::Done;
        submitted.doneTick = m_tick;
      }
      else
      {
        const SiteCluster& cluster = m_site.clusters[submitted.cluster];
        submitted.legCount = 1;
        submitted.legIndex = 0;
        submitted.legs[0].anchorMetres = XMFLOAT2{ClampToPlayArea(Neuron::CentimetresToMetres(cluster.xCm)),
                                                  ClampToPlayArea(Neuron::CentimetresToMetres(cluster.yCm))};
      }
    }

    /*
     * A warp **spools where it stands** (ADR-016 §5).
     *
     * It takes the group table like any order -- so it is acked, replaced by
     * the next order, and drawn as a ghost -- but it has no legs: the fleet
     * holds, the clock runs, and when it finishes the ships leave the world on
     * the transfer bus. Holding rather than drifting is what makes "cancelled
     * by a replacing order" mean something: the fleet is exactly where the
     * player left it.
     *
     * The spool is the **slowest member's**, for the same reason the transit is:
     * a fleet arrives together, so it leaves together.
     */
    if (pending.kind == OrderKind::Warp)
    {
      float spoolSeconds = 0.0f;
      for (std::uint16_t index = 0; index < submitted.memberCount; ++index)
      {
        std::uint32_t slot = 0;
        if (FindSlot(submitted.members[index], slot))
        {
          spoolSeconds = std::max(spoolSeconds, ShipClass(static_cast<HullClass>(m_classes[slot])).spoolSeconds);
          m_guidances[slot] = Guidance{};
        }
      }
      submitted.spoolUntilTick = m_tick + static_cast<std::uint32_t>(spoolSeconds / TICK_SECONDS);
      submitted.legCount = 0;
      submitted.legIndex = 0;
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

    /*
     * A group with no leg left has nothing to solve, and asking anyway would
     * read `legs[legIndex]` past the plan.
     *
     * It was unreachable until E2: every other kind is `Done` the moment its
     * legs run out, and a `Done` group is already skipped above. A **working
     * Mine order** is the first group that outlives its own plan -- it has
     * arrived at its cluster and is cycling -- so the guard that used to be
     * implied has to be written. It also settles what a casualty does to a
     * mining wing: the survivors keep the stations the cluster put them in
     * rather than closing the hole, which is what "no movement the player did
     * not order" means for a fleet already parked (ruling R5).
     */
    if (group.legIndex >= group.legCount)
    {
      continue;
    }
    if (group.legDeadlineTick == 0 || group.memberCount != group.solvedMemberCount)
    {
      ApplyLeg(group);
    }
  }
}

/*
 * Rings for a player's click, expressed as multiples of the fleet's own
 * bounding radius rather than as metres (ADR-026 §3).
 *
 * `PARKING_RING_METRES` can be absolute because a station's doorway is a fixed
 * piece of geometry. A destination is not: the same two numbers would be a
 * generous slide for three Interceptors and barely a nudge for a sixty-ship
 * line. One bounding radius clears the fleet's own footprint; two gives it a
 * second try without carrying it somewhere the player would not recognise.
 */
constexpr float PLACEMENT_RING_EXTENTS[] = {1.0f, 2.0f};

bool World::FindClearPlacement(std::span<const ShipId> _ships, FormationId _formation, float _facingRadians,
                               const XMFLOAT2& _askedMetres, float _approachBearingRadians,
                               std::uint32_t _ignoreServerOrderId, XMFLOAT2& _outAnchorMetres) const
{
  if (_ships.empty() || _ships.size() > MAX_SHIPS_PER_ORDER)
  {
    return false;
  }

  WorldClassLookup lookup{this};
  FormationStation stations[MAX_SHIPS_PER_ORDER];

  float largestSpacing = 0.0f;
  for (const ShipId shipId : _ships)
  {
    largestSpacing = std::max(largestSpacing, ShipClass(WorldClassLookup::Of(shipId, &lookup)).formationSpacingMetres);
  }

  /*
   * Solved once at the asked point, for two things at once: whether it is free,
   * and how big this fleet is. The rings below are multiples of that size, so
   * the fleet's own footprint is what decides how far "nearby" reaches.
   *
   * The facing never changes. Only the anchor moves -- that is what "slides
   * whole" means, and it is why this solve can be reused as the shape at every
   * candidate.
   */
  const std::uint32_t placed = SolveFormation(_formation, _ships, &WorldClassLookup::Of, &lookup, _askedMetres, _facingRadians,
                                              std::span<FormationStation>{stations});
  if (placed == 0)
  {
    return false;
  }
  const float bounding = FormationExtentMetres(std::span<const FormationStation>{stations, placed}, _askedMetres) + largestSpacing;

  // The asked point first, always. A destination with room in it must never
  // move, or every order in an empty sky would drift by a ring.
  if (IsPlacementFree(std::span<const FormationStation>{stations, placed}, &WorldClassLookup::Of, &lookup, _ships, _askedMetres,
                      bounding, _ignoreServerOrderId, true))
  {
    _outAnchorMetres = _askedMetres;
    return true;
  }

  constexpr float BEARING_STEP = DirectX::XM_2PI / static_cast<float>(PARKING_BEARINGS);

  for (const float ringExtents : PLACEMENT_RING_EXTENTS)
  {
    const float ring = ringExtents * bounding;
    for (std::uint32_t step = 0; step < PARKING_BEARINGS; ++step)
    {
      // The same alternating fan the parking ring scans, and the same reason:
      // a fixed order, so "which placement" is a function of the world and not
      // of the order two fleets happened to arrive in.
      const std::uint32_t stepsOut = (step + 1) / 2;
      const float side = (step % 2 == 0) ? 1.0f : -1.0f;
      const float bearing = _approachBearingRadians + static_cast<float>(stepsOut) * side * BEARING_STEP;

      const XMFLOAT2 candidate{_askedMetres.x + std::cos(bearing) * ring, _askedMetres.y + std::sin(bearing) * ring};

      const std::uint32_t candidatePlaced = SolveFormation(_formation, _ships, &WorldClassLookup::Of, &lookup, candidate,
                                                           _facingRadians, std::span<FormationStation>{stations});
      if (candidatePlaced == 0)
      {
        continue;
      }
      if (!IsPlacementFree(std::span<const FormationStation>{stations, candidatePlaced}, &WorldClassLookup::Of, &lookup, _ships,
                           candidate, bounding, _ignoreServerOrderId, true))
      {
        continue;
      }

      _outAnchorMetres = candidate;
      return true;
    }
  }

  return false;
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

  OrderGroupLeg& leg = _group.legs[_group.legIndex];

  /*
   * Solve, then slide (ADR-026 §5) -- **when the leg becomes active, and only
   * then**.
   *
   * `legDeadlineTick == 0` is what "this leg has not run yet" means: the
   * deadline is stamped at the bottom of this function, so a zero here is a
   * first activation and anything else is the membership re-solve a casualty
   * triggers. A fleet that loses a ship mid-flight must not have its
   * destination moved under it; it closes the hole and flies on.
   *
   * At submission would have been easier and would have been wrong. A queued
   * third leg may fly minutes after it was accepted, and a placement computed
   * then is a precise answer about a world that has since moved.
   *
   * The slid anchor is written **back into the leg**, so everything downstream
   * inherits it without being told: the ETA measures the real distance, the
   * ghost draws the real destination, and -- the one that matters -- the next
   * fleet's own scan sees this group's final-leg anchor where the fleet is
   * actually going.
   *
   * A `Warp` is exempt: it does not fly to a point, it spools and leaves, and
   * ADR-016 §3 already promises clean water at the far end.
   */
  if (_group.legDeadlineTick == 0 && _group.kind == OrderKind::Move)
  {
    // Where the fleet is coming from, so a blocked destination is approached
    // from the near side and the slide stops short rather than overshooting.
    XMFLOAT2 centroid{0.0f, 0.0f};
    for (std::uint32_t index = 0; index < livingCount; ++index)
    {
      std::uint32_t slot = 0;
      if (FindSlot(living[index], slot))
      {
        centroid.x += m_positions[slot].x;
        centroid.y += m_positions[slot].y;
      }
    }
    centroid.x /= static_cast<float>(livingCount);
    centroid.y /= static_cast<float>(livingCount);

    const float approach = std::atan2(centroid.y - leg.anchorMetres.y, centroid.x - leg.anchorMetres.x);

    XMFLOAT2 placement{};
    if (FindClearPlacement(std::span<const ShipId>{living, livingCount}, _group.formation, leg.facingRadians, leg.anchorMetres,
                           approach, _group.serverOrderId, placement))
    {
      leg.anchorMetres = placement;
    }
    // Otherwise the asked point stands and the fleet flies to it: ADR-015 parks
    // it at contact range and the deadline ends the leg (ADR-026 §4). Never a
    // refusal, and never a wedge.
  }

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
  if (_group.state == OrderState::Done)
  {
    return -1.0f; // Nothing under way, so nothing to be due.
  }

  // A working Mine order has no leg and is very much under way: what it is due
  // to finish is the cluster, not a journey (ADR-024 §4d).
  if (_group.legIndex >= _group.legCount)
  {
    return _group.kind == OrderKind::Mine ? MiningEtaSeconds(_group) : -1.0f;
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

void World::DepartFinishedWarps()
{
  /*
   * A warp that has finished spooling leaves (ADR-016 §5, ADR-017 §9).
   *
   * The ships go out through the same seam a dock uses -- `TransferOut`, id and
   * class and wing intact -- and the registry decides where and when they
   * arrive, because that needs the universe and a world does not have one.
   *
   * **Collected first, transferred second, and that is not tidiness.**
   * `TransferOut` despawns, despawning forgets the ship in its group, and
   * forgetting the last member *erases the group* -- so transferring while
   * iterating `m_groups` would delete the entry being read. The list of
   * departures is taken while nothing is being removed, and the removals happen
   * after.
   */
  struct Departure
  {
    AnchorId anchor = INVALID_ID;
    FormationId formation = FormationId::Line;
    std::uint16_t memberCount = 0;
    ShipId members[MAX_SHIPS_PER_ORDER] = {};
  };

  std::vector<Departure> leaving;
  for (OrderGroup& group : m_groups)
  {
    if (group.kind != OrderKind::Warp || group.state == OrderState::Done || m_tick < group.spoolUntilTick)
    {
      continue;
    }

    Departure departure;
    departure.anchor = group.anchor;
    departure.formation = group.formation;
    departure.memberCount = group.memberCount;
    std::copy(group.members, group.members + group.memberCount, departure.members);
    leaving.push_back(departure);

    // Marked before anything is removed, so the group that survives the
    // despawns (one whose members did not all leave) is not asked to depart
    // again next tick.
    group.state = OrderState::Done;
    group.doneTick = m_tick;
  }

  for (const Departure& departure : leaving)
  {
    TransferRequest request;
    request.kind = TransferKind::Transit;
    request.anchor = departure.anchor;
    request.formation = departure.formation;
    for (std::uint16_t index = 0; index < departure.memberCount; ++index)
    {
      TransferMember member;
      if (TransferOut(departure.members[index], member) && !request.AddMember(member))
      {
        break;
      }
    }
    if (request.memberCount > 0)
    {
      m_filed.push_back(request);
    }
  }
}

void World::GroupAdvance()
{
  DepartFinishedWarps();

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
      /*
       * A Mine order does not *end* when its leg does -- that is when it
       * begins (ADR-024 §4b). The fleet has reached the cluster; `Mining` takes
       * it from here and is what eventually sets `Done`, by one of the two
       * exits that mean the work is over.
       *
       * It stays `Underway` rather than gaining a fourth `OrderState`, because
       * `etaSeconds` already reports what an order is doing and a mining group
       * reports the cluster instead of the leg (§4d). A new state would be a
       * wire value, a client branch and a schema bump for a distinction the
       * ETA already draws.
       */
      if (group.kind == OrderKind::Mine)
      {
        group.state = OrderState::Underway;
        continue;
      }
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
