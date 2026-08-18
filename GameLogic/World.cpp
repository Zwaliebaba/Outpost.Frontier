#include "pch.h"

#include "World.h"

#include "Eta.h"
#include "Formation.h"

#include "EntityRecord.h"

#include <algorithm>
#include <cmath>

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
   * falls out of every ship seeking its own station; there is no inter-ship
   * avoidance, because stations do not overlap by construction.
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
    if (seeking)
    {
      desiredHeading = std::atan2(XMVectorGetY(toTarget), XMVectorGetX(toTarget));
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

} // namespace Game
