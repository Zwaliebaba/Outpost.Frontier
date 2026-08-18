#include "pch.h"

#include "World.h"

#include "Eta.h"
#include "Formation.h"

#include "EntityRecord.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace DirectX;

namespace Game
{
namespace
{

/// Brings an angle into (-pi, pi]. DirectXMath's own, so the wrap is the same
/// one the camera and the wire use -- and not an `Est` function (ADR-010 §6).
[[nodiscard]] float WrapAngle(float _radians) noexcept
{
  return XMScalarModAngle(_radians);
}

[[nodiscard]] float ClampToPlayArea(float _metres) noexcept
{
  return std::clamp(_metres, -World::PLAY_AREA_HALF_EXTENT_METRES, World::PLAY_AREA_HALF_EXTENT_METRES);
}

/// Below this heading error the turn limit is not applied at all -- it would be
/// dividing by something near zero to produce a bound far above any hull's top
/// speed, which is arithmetic nobody needs to do.
constexpr float MIN_TURN_LIMIT_ERROR_RADIANS = 1.0e-4f;

/*
 * The speed a ship can still be doing at this distance and stop on the target.
 *
 * The textbook answer is `v = sqrt(2 a s)`, and it is wrong here -- not
 * approximately, but in a way that diverges. That curve is exactly achievable
 * only in continuous time. Stepped at 20 Hz a ship sits fractionally above it,
 * therefore covers fractionally more ground per tick, therefore finds the curve
 * has dropped by more than one tick of braking, therefore falls further behind.
 * The error compounds all the way in: an Interceptor braking flat-out from
 * cruise arrived 1.2 m from its target still doing 38 m/s, sailed through, and
 * settled 5.9 m past it -- three times the arrival tolerance.
 *
 * This is the same equation solved for the steps actually taken:
 *
 *     v = -a dt / 2 + sqrt((a dt / 2)^2 + 2 a s)
 *
 * It is zero at `s = 0`, tends to `sqrt(2 a s)` as the step vanishes, and sits
 * strictly below it in between -- which is the margin that lets a ship track it
 * rather than chase it. Arrival then falls out of the profile instead of being
 * a special case, and the tolerance is subtracted first so the curve reaches
 * zero at the edge of the ring rather than at its centre.
 */
[[nodiscard]] float ArrivalSpeed(float _distanceMetres, float _accelMetresPerSecSq) noexcept
{
  const float braking = _distanceMetres - World::ARRIVAL_TOLERANCE_METRES;
  if (braking <= 0.0f || _accelMetresPerSecSq <= 0.0f)
  {
    return 0.0f;
  }

  const float halfStep = 0.5f * _accelMetresPerSecSq * World::TICK_SECONDS;
  return std::sqrt(halfStep * halfStep + 2.0f * _accelMetresPerSecSq * braking) - halfStep;
}

/// The contact radius for a raw class byte off the tables. The byte was
/// validated at spawn, so the clamping lookup is enough.
[[nodiscard]] float ContactRadiusOf(std::uint8_t _rawClass) noexcept
{
  return ShipClass(static_cast<HullClass>(_rawClass)).collisionRadiusMetres;
}

/// A hull the contact model treats as terrain: it cannot move, so nothing may
/// push it and it never scans traffic. `Structure`, in practice.
[[nodiscard]] bool IsAnchored(std::uint8_t _rawClass) noexcept
{
  return ShipClass(static_cast<HullClass>(_rawClass)).maxSpeedMetresPerSec <= 0.0f;
}

/// What the traffic around a ship allows it to do this tick (ADR-015 §2).
struct TrafficSteer
{
  /// The course to fly: toward the target, deflected sideways when something
  /// sits in the corridor.
  float desiredHeadingRadians = 0.0f;

  /// The most speed the nearest obstruction ahead leaves room to stop from.
  float speedCapMetresPerSec = std::numeric_limits<float>::max();
};

/*
 * Avoidance: what a seeking ship does about the ships it would otherwise fly
 * through. Two limits, independent because they answer different failures.
 *
 * **Braking** is the guarantee. Along the heading actually flown, the nearest
 * point of contact with each ship ahead is exact circle arithmetic --
 * `ahead - sqrt(contact^2 - side^2)` -- and speed is capped by the same
 * discrete-step arrival profile that stops a ship on its station, aimed at that
 * point. `ArrivalSpeed` reaches zero a tolerance short, so a blocked ship
 * parks with a gap rather than nosing in; and because the profile grows with
 * distance, far traffic caps nothing and no horizon check is needed.
 *
 * **Deflection** is the manners. In the frame of the course the ship *wants*,
 * the nearest blocker whose corridor it sits in bends the desired heading onto
 * the tangent that clears it by `AVOID_CLEARANCE_FACTOR` of the combined
 * radius. The tangent formula is continuous at the corridor's edge -- when
 * `|side|` equals the clearance the deflection is exactly zero -- so there is
 * no flicker to damp at the boundary. A blocker parked on the target itself is
 * exempt: there is nothing sensible to steer around, so the ship brakes and
 * parks adjacent instead of orbiting what it cannot reach (the leg then ends
 * by its own deadline, ADR-005 §2). Head-on symmetry is broken by convention:
 * dead-ahead traffic is passed to port, and two ships meeting nose to nose each
 * deflect to their own left, which parts them.
 *
 * Deterministic by the same construction as everything else here: slot-order
 * iteration, pure table data, no randomness (ADR-005 §5).
 */
[[nodiscard]] TrafficSteer SteerAroundTraffic(std::uint32_t _slot, std::span<const DirectX::XMFLOAT2> _positions,
                                              std::span<const std::uint8_t> _classes, const ShipClassInfo& _info,
                                              float _headingRadians, float _speedMetresPerSec, const Guidance& _guidance,
                                              float _desiredHeadingRadians, float _distanceToTargetMetres) noexcept
{
  TrafficSteer steer;
  steer.desiredHeadingRadians = _desiredHeadingRadians;
  if (_info.maxSpeedMetresPerSec <= 0.0f)
  {
    return steer; // Anchored: a station neither brakes nor swerves.
  }

  const XMFLOAT2 position = _positions[_slot];
  const float ownRadius = ContactRadiusOf(_classes[_slot]);

  const float flownCos = std::cos(_headingRadians);
  const float flownSin = std::sin(_headingRadians);
  const float wantedCos = std::cos(_desiredHeadingRadians);
  const float wantedSin = std::sin(_desiredHeadingRadians);

  // Deflection answers to the nearest corridor blocker only: summing pushes
  // from a crowd points somewhere no single member justifies, and the nearest
  // one is the one that must be cleared first.
  float nearestAhead = std::numeric_limits<float>::max();
  float nearestSide = 0.0f;
  float nearestClearance = 0.0f;

  const float brakingDistance =
      _speedMetresPerSec * _speedMetresPerSec / (2.0f * _info.accelMetresPerSecSq);

  for (std::uint32_t other = 0; other < _positions.size(); ++other)
  {
    if (other == _slot)
    {
      continue;
    }
    const float relX = _positions[other].x - position.x;
    const float relY = _positions[other].y - position.y;
    const float contact = ownRadius + ContactRadiusOf(_classes[other]);

    // Braking, in the frame of the heading actually flown.
    const float aheadFlown = relX * flownCos + relY * flownSin;
    if (aheadFlown > 0.0f)
    {
      const float sideFlown = flownCos * relY - flownSin * relX;
      if (std::fabs(sideFlown) < contact)
      {
        const float reach = std::sqrt(contact * contact - sideFlown * sideFlown);
        const float untilContact = std::max(aheadFlown - reach, 0.0f);
        steer.speedCapMetresPerSec = std::min(steer.speedCapMetresPerSec, ArrivalSpeed(untilContact, _info.accelMetresPerSecSq));
      }
    }

    // Deflection, in the frame of the course wanted.
    const float targetGapX = _positions[other].x - _guidance.targetXMetres;
    const float targetGapY = _positions[other].y - _guidance.targetYMetres;
    if (targetGapX * targetGapX + targetGapY * targetGapY < contact * contact)
    {
      continue; // Parked on the destination: brake and park adjacent.
    }
    const float aheadWanted = relX * wantedCos + relY * wantedSin;
    if (aheadWanted <= 0.0f || aheadWanted >= _distanceToTargetMetres)
    {
      continue; // Behind, or beyond the point this ship stops at anyway.
    }
    if (aheadWanted > brakingDistance + World::AVOID_LOOKAHEAD_RADII * contact)
    {
      continue; // Too far to matter yet at this speed.
    }
    const float sideWanted = wantedCos * relY - wantedSin * relX;
    const float clearance = World::AVOID_CLEARANCE_FACTOR * contact;
    if (std::fabs(sideWanted) >= clearance)
    {
      continue; // The corridor is already clear.
    }
    if (aheadWanted < nearestAhead)
    {
      nearestAhead = aheadWanted;
      nearestSide = sideWanted;
      nearestClearance = clearance;
    }
  }

  if (nearestAhead < std::numeric_limits<float>::max())
  {
    const float distance = std::sqrt(nearestAhead * nearestAhead + nearestSide * nearestSide);
    const float halfWidth = std::asin(std::min(nearestClearance / distance, 1.0f));
    const float centre = std::atan2(nearestSide, nearestAhead);
    const float toTangent = nearestSide > 0.0f ? centre - halfWidth : centre + halfWidth;
    steer.desiredHeadingRadians = WrapAngle(_desiredHeadingRadians + toTangent);
  }
  return steer;
}

} // namespace

void World::Reset(std::uint64_t _seed) noexcept
{
  m_tick = 0;
  m_slotById.clear();
  m_nextShipId = 0;

  m_ids.clear();
  m_classes.clear();
  m_wings.clear();
  m_positions.clear();
  m_velocities.clear();
  m_headings.clear();
  m_guidances.clear();
  m_hulls.clear();
  m_shields.clear();

  m_groups.clear();
  m_pending.clear();
  m_nextOrderId = 1;
  m_lastOrderSeqProcessed = 0;

  m_random.Seed(_seed);
}

ShipId World::Spawn(const ShipSpawn& _spawn)
{
  if (!HullClassHasContent(_spawn.hullClass))
  {
    return INVALID_SHIP_ID; // Fighter and Cruiser are ids, not ships.
  }
  if (m_nextShipId >= INVALID_SHIP_ID)
  {
    return INVALID_SHIP_ID; // 65k ships in one session is not a real scenario.
  }

  const ShipId shipId = m_nextShipId++;
  m_slotById.resize(static_cast<std::size_t>(shipId) + 1, INVALID_SHIP_ID);
  m_slotById[shipId] = static_cast<ShipId>(m_ids.size());

  m_ids.push_back(shipId);
  m_classes.push_back(static_cast<std::uint8_t>(_spawn.hullClass));
  m_wings.push_back(_spawn.wing);
  m_positions.push_back(XMFLOAT2{ClampToPlayArea(_spawn.xMetres), ClampToPlayArea(_spawn.yMetres)});
  m_velocities.push_back(XMFLOAT2{0.0f, 0.0f});
  m_headings.push_back(WrapAngle(_spawn.headingRadians));

  // A new ship holds where it is, facing where it was placed. The alternative
  // -- an unset guidance -- is a ship that steers toward the origin.
  Guidance guidance;
  guidance.mode = GuidanceMode::Hold;
  guidance.targetXMetres = m_positions.back().x;
  guidance.targetYMetres = m_positions.back().y;
  guidance.arrivalFacingRadians = m_headings.back();
  m_guidances.push_back(guidance);

  m_hulls.push_back(255);
  m_shields.push_back(255);
  return shipId;
}

bool World::Despawn(ShipId _shipId)
{
  std::uint32_t slot = 0;
  if (!FindSlot(_shipId, slot))
  {
    return false;
  }

  // Out of every group first. A group holding a dead id would solve a station
  // for it and leave a gap in the formation, and would keep reporting a leg
  // nobody is flying.
  ForgetShipInGroups(_shipId);
  std::erase_if(m_groups, [](const OrderGroup& _group) { return _group.memberCount == 0; });

  // Swap-and-pop: the arrays stay dense, so iteration order stays array order
  // and nothing has to skip holes. The moved ship's id needs its slot fixed,
  // which is the whole reason the indirection table exists.
  const auto last = static_cast<std::uint32_t>(m_ids.size() - 1);
  if (slot != last)
  {
    m_slotById[m_ids[last]] = static_cast<ShipId>(slot);
    m_ids[slot] = m_ids[last];
    m_classes[slot] = m_classes[last];
    m_wings[slot] = m_wings[last];
    m_positions[slot] = m_positions[last];
    m_velocities[slot] = m_velocities[last];
    m_headings[slot] = m_headings[last];
    m_guidances[slot] = m_guidances[last];
    m_hulls[slot] = m_hulls[last];
    m_shields[slot] = m_shields[last];
  }

  m_slotById[_shipId] = INVALID_SHIP_ID;
  m_ids.pop_back();
  m_classes.pop_back();
  m_wings.pop_back();
  m_positions.pop_back();
  m_velocities.pop_back();
  m_headings.pop_back();
  m_guidances.pop_back();
  m_hulls.pop_back();
  m_shields.pop_back();
  return true;
}

bool World::FindSlot(ShipId _shipId, std::uint32_t& _outSlot) const noexcept
{
  if (_shipId >= m_slotById.size() || m_slotById[_shipId] == INVALID_SHIP_ID)
  {
    return false;
  }
  _outSlot = m_slotById[_shipId];
  return true;
}

float World::SpeedAt(std::uint32_t _slot) const noexcept
{
  if (_slot >= m_velocities.size())
  {
    return 0.0f;
  }
  const XMVECTOR velocity = XMLoadFloat2(&m_velocities[_slot]);
  return XMVectorGetX(XMVector2Length(velocity));
}

void World::Tick(std::uint32_t _tick)
{
  m_tick = _tick;

  IngestOrders();
  GroupAdvance();
  Steering();
  Integrate();
  Separate();
}

ValidationView World::Validation() const noexcept
{
  ValidationView view;
  view.shipIds = m_ids;
  view.queuedLegs = 0; // Per-group, and resolved in SubmitOrder where the group is known.
  return view;
}

OrderVerdict World::SubmitOrder(const OrderSubmit& _order)
{
  // Appending needs to know which group it would append to, and that is the
  // group the *first* named ship already belongs to. One group per order means
  // a selection spanning two groups cannot append to both, so the answer is the
  // first member's -- which is also what the client's ghost is drawn against.
  ValidationView view = Validation();
  if (_order.queueMode == QueueMode::Append && _order.shipCount > 0)
  {
    for (const OrderGroup& group : m_groups)
    {
      const ShipId* found = std::find(group.members, group.members + group.memberCount, _order.shipIds[0]);
      if (found != group.members + group.memberCount)
      {
        view.queuedLegs = group.legCount;
        break;
      }
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
  group.legs[0].anchorMetres =
      XMFLOAT2{ClampToPlayArea(Neuron::CentimetresToMetres(_order.target.xCm)),
               ClampToPlayArea(Neuron::CentimetresToMetres(_order.target.yCm))};
  group.legs[0].facingRadians = WrapAngle(Neuron::HeadingToRadians(_order.target.facingTurns16));
  group.legStartTick = m_tick;
  group.legDeadlineTick = 0; // Sized by ApplyLeg, which is where the stations exist.

  m_pending.push_back(PendingOrder{group, _order.queueMode});
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

    const bool append = pending.queueMode == QueueMode::Append;
    if (append)
    {
      OrderGroup* existing = nullptr;
      for (OrderGroup& group : m_groups)
      {
        if (std::find(group.members, group.members + group.memberCount, submitted.members[0]) !=
            group.members + group.memberCount)
        {
          existing = &group;
          break;
        }
      }
      if (existing != nullptr && existing->legCount < MAX_ORDER_LEGS)
      {
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
    std::erase_if(m_groups, [](const OrderGroup& _group) { return _group.memberCount == 0; });

    submitted.legStartTick = m_tick;
    submitted.legDeadlineTick = 0; // As above -- sized once the stations are solved.
    m_groups.push_back(submitted);
  }
  m_pending.clear();

  for (OrderGroup& group : m_groups)
  {
    if (group.state != OrderState::Done)
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

  /*
   * The deadline, set from the leg's own estimate now that the stations are
   * resolved and `LegEtaSeconds` has something to measure.
   *
   * Here rather than where `legStartTick` is set, because the estimate needs
   * the guidance this function just wrote: before it, a member's remaining
   * distance is whatever the *previous* leg left behind.
   *
   * **Once per leg, and that is the whole reason for the zero check.** This
   * function runs every tick -- it re-solves the formation as ships die -- so
   * computing the deadline unconditionally would push it forward every tick and
   * it would never arrive. A leg that could never complete would then hold its
   * group forever, which is the exact failure the timeout exists to prevent.
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
      continue;
    }
    group.state = OrderState::Arriving;
    ApplyLeg(group);
  }
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
}

void World::Steering()
{
  /*
   * Per ship: turn toward the target and pick a speed, both clamped by class.
   *
   * Ships move along their heading and cannot strafe (ADR-005 §2), so a turn is
   * the only way to change direction and the turn rate is what gives a hull its
   * character -- an Interceptor pivots, a Battleship sweeps. Formation keeping
   * falls out of every ship seeking its own station. Traffic bends the course
   * and caps the speed (`SteerAroundTraffic`, ADR-015 §2): the MVP had no
   * inter-ship avoidance because stations do not overlap by construction, and
   * that same construction is what keeps a formation in flight clear of its own
   * avoidance now that ships flying *between* stations no longer pass through
   * whatever is en route.
   */
  const std::uint32_t count = ShipCount();
  for (std::uint32_t slot = 0; slot < count; ++slot)
  {
    HullClass hullClass = HullClass::Interceptor;
    if (!TryShipClass(m_classes[slot], hullClass))
    {
      continue;
    }
    const ShipClassInfo& info = ShipClass(hullClass);

    const Guidance& guidance = m_guidances[slot];
    const XMVECTOR position = XMLoadFloat2(&m_positions[slot]);
    const XMVECTOR target = XMVectorSet(guidance.targetXMetres, guidance.targetYMetres, 0.0f, 0.0f);
    const XMVECTOR toTarget = XMVectorSubtract(target, position);
    const float distance = XMVectorGetX(XMVector2Length(toTarget));

    const bool seeking = guidance.mode == GuidanceMode::Seek && distance > ARRIVAL_TOLERANCE_METRES;

    // Which way to point: at the target while travelling, at the arrival facing
    // once there. A ship that has arrived still turns, which is what makes a
    // fleet end a move facing the same way.
    float desiredHeading = guidance.arrivalFacingRadians;
    float trafficSpeedCap = std::numeric_limits<float>::max();
    if (seeking)
    {
      desiredHeading = std::atan2(XMVectorGetY(toTarget), XMVectorGetX(toTarget));

      // Only a ship going somewhere scans traffic. A holding ship has no course
      // to bend and no speed to cap -- and what it cannot do is get out of the
      // way, which is `Separate`'s problem, not steering's.
      const TrafficSteer steer = SteerAroundTraffic(slot, m_positions, m_classes, info, m_headings[slot], SpeedAt(slot),
                                                    guidance, desiredHeading, distance);
      desiredHeading = steer.desiredHeadingRadians;
      trafficSpeedCap = steer.speedCapMetresPerSec;
    }

    const float headingError = WrapAngle(desiredHeading - m_headings[slot]);
    const float maxTurn = info.turnRateRadiansPerSec * TICK_SECONDS;
    m_headings[slot] = WrapAngle(m_headings[slot] + std::clamp(headingError, -maxTurn, maxTurn));

    /*
     * Speed, from three limits, and all three are load-bearing.
     *
     * The *arrival profile* says how fast a ship may still be going at this
     * distance and stop on the target. The *alignment* factor takes away speed
     * a ship is pointing the wrong way to use. Neither is enough on its own:
     * with only alignment, a Battleship told to reverse onto a point sixty
     * metres behind it circled the target three times before arriving -- its
     * turn radius at cruise is 477 m, so it simply could not get there.
     *
     * The *turn limit* is the fix. Turning through `error` takes `error / omega`
     * seconds; a ship that holds its speed through that covers more ground than
     * it has to spare, sweeps past, and comes round again. Bounding speed by
     * `omega * distance / (2 * error)` keeps the arc inside the distance
     * remaining, with the factor of two the margin that makes the target fall
     * outside the turning circle rather than on it.
     *
     * It vanishes as the heading error does, so a ship flying straight at its
     * target is never slowed by it and a capital ship making a wide 90-degree
     * turn still carves the arc its turn rate implies. Measured across five
     * hulls and five approach geometries: zero orbits, and the shortest path in
     * every hard case.
     */
    float desiredSpeed = 0.0f;
    if (seeking)
    {
      const float alignment = std::clamp(std::cos(headingError), 0.0f, 1.0f);
      desiredSpeed = std::min(info.maxSpeedMetresPerSec, ArrivalSpeed(distance, info.accelMetresPerSecSq)) * alignment;

      const float absError = std::fabs(headingError);
      if (absError > MIN_TURN_LIMIT_ERROR_RADIANS && info.turnRateRadiansPerSec > 0.0f)
      {
        desiredSpeed = std::min(desiredSpeed, info.turnRateRadiansPerSec * distance / (2.0f * absError));
      }

      // And the traffic's limit last: whatever the approach wants, the ship may
      // not carry more speed than lets it stop short of the nearest hull in its
      // path (ADR-015 §2).
      desiredSpeed = std::min(desiredSpeed, trafficSpeedCap);
    }

    const float speed = SpeedAt(slot);
    const float maxSpeedChange = info.accelMetresPerSecSq * TICK_SECONDS;
    const float newSpeed = std::clamp(speed + std::clamp(desiredSpeed - speed, -maxSpeedChange, maxSpeedChange), 0.0f,
                                      info.maxSpeedMetresPerSec);

    // Velocity is always along the *new* heading: this is the no-strafing rule,
    // and writing it here rather than in Integrate keeps Integrate to one line
    // of physics.
    const float heading = m_headings[slot];
    m_velocities[slot] = XMFLOAT2{std::cos(heading) * newSpeed, std::sin(heading) * newSpeed};
  }
}

void World::Integrate()
{
  // Semi-implicit Euler: Steering has already written this tick's velocity, so
  // position moves by the new one rather than the old. It is stable at this
  // step and it is one line, which matters more than its order of accuracy for
  // a sim whose velocities are clamped every tick anyway.
  const std::uint32_t count = ShipCount();
  for (std::uint32_t slot = 0; slot < count; ++slot)
  {
    const XMVECTOR position = XMLoadFloat2(&m_positions[slot]);
    const XMVECTOR velocity = XMLoadFloat2(&m_velocities[slot]);
    const XMVECTOR moved = XMVectorMultiplyAdd(velocity, XMVectorReplicate(TICK_SECONDS), position);

    XMFLOAT2 result{};
    XMStoreFloat2(&result, moved);

    // The grid is bounded (ADR-001 §3) and targets are clamped on ingest, so a
    // ship only reaches here by drifting the last few metres at the edge. The
    // clamp is a floor under the invariant rather than a movement rule.
    m_positions[slot] = XMFLOAT2{ClampToPlayArea(result.x), ClampToPlayArea(result.y)};
  }
}

void World::Separate()
{
  /*
   * Contact resolution (ADR-015 §4): after everything has moved, no two hulls
   * may stay inside each other. Steering exists to keep this from triggering;
   * this exists because steering cannot promise it -- momentum a hull cannot
   * shed, targets under other ships, and authored overlap all end here.
   *
   * The mechanics, and why each is shaped the way it is:
   *
   *   - **Positions move, velocities do not.** This is projection out of an
   *     invalid state, not physics: contact transfers no momentum, ships shove
   *     past each other shoulder-first, and the movement envelope (speed, turn,
   *     acceleration) stays exactly what Steering granted. Anything fancier is
   *     gameplay to design, not a defect to fix.
   *   - **The split is by hull area** (radius squared), so a Battleship walks
   *     through a crowd of Interceptors moving barely at all, and an anchored
   *     hull -- a station -- is terrain: it takes none of the correction, ever.
   *   - **Each pass moves a pair at most a quarter of their combined radius.**
   *     Overlap can be authored (spawn layouts) as well as flown into, and
   *     popping a deep overlap apart in one tick reads as a teleport on the
   *     client; capped, it reads as ships pushing each other off, a few metres
   *     a tick, until clear.
   *   - **Gauss-Seidel in slot order, a fixed number of passes.** Sequential
   *     updates in dense-array order are the determinism rule (ADR-005 §5)
   *     applied to pairs; the fixed pass count keeps the tick's cost bounded,
   *     and whatever a crowded tick leaves unresolved, the next tick continues.
   *     Coincident centres -- a stacked spawn -- part eastward, an arbitrary
   *     axis that only has to be the same one every run.
   */
  const std::uint32_t count = ShipCount();
  for (std::uint32_t pass = 0; pass < SEPARATION_PASSES; ++pass)
  {
    bool touched = false;
    for (std::uint32_t first = 0; first + 1 < count; ++first)
    {
      const float firstRadius = ContactRadiusOf(m_classes[first]);
      const bool firstAnchored = IsAnchored(m_classes[first]);

      for (std::uint32_t second = first + 1; second < count; ++second)
      {
        const bool secondAnchored = IsAnchored(m_classes[second]);
        if (firstAnchored && secondAnchored)
        {
          continue; // Two stations. However authored, not this system's business.
        }

        const float secondRadius = ContactRadiusOf(m_classes[second]);
        const float contact = firstRadius + secondRadius;
        const float relX = m_positions[second].x - m_positions[first].x;
        const float relY = m_positions[second].y - m_positions[first].y;
        const float distanceSq = relX * relX + relY * relY;
        if (distanceSq >= contact * contact)
        {
          continue;
        }

        const float distance = std::sqrt(distanceSq);
        float axisX = 1.0f;
        float axisY = 0.0f;
        if (distance > 1.0e-4f)
        {
          axisX = relX / distance;
          axisY = relY / distance;
        }

        const float step = std::min(contact - distance, contact * SEPARATION_STEP_FACTOR);

        float firstShare = 0.0f;
        float secondShare = 0.0f;
        if (firstAnchored)
        {
          secondShare = 1.0f;
        }
        else if (secondAnchored)
        {
          firstShare = 1.0f;
        }
        else
        {
          const float firstArea = firstRadius * firstRadius;
          const float secondArea = secondRadius * secondRadius;
          firstShare = secondArea / (firstArea + secondArea);
          secondShare = 1.0f - firstShare;
        }

        m_positions[first].x = ClampToPlayArea(m_positions[first].x - axisX * step * firstShare);
        m_positions[first].y = ClampToPlayArea(m_positions[first].y - axisY * step * firstShare);
        m_positions[second].x = ClampToPlayArea(m_positions[second].x + axisX * step * secondShare);
        m_positions[second].y = ClampToPlayArea(m_positions[second].y + axisY * step * secondShare);
        touched = true;
      }
    }
    if (!touched)
    {
      break; // A pass with nothing to do; the next three would find the same.
    }
  }
}

} // namespace Game
