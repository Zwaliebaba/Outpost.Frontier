#include "pch.h"

#include "World.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace DirectX;

/*
 * The movement half of `World`: the tables, the tick, and the systems that fly
 * ships -- `Steering`, `Integrate`, `Separate` and the contact model. The order
 * half -- `SubmitOrder`, the group table and its lifecycle -- is
 * `WorldOrders.cpp`, and the seam between the two is `Guidance`: a group writes
 * it, steering reads it, and neither half needs the other's machinery
 * (ADR-005 §1).
 */

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

/// Class top speed for a raw class byte -- what `MakeWayOffset` measures its
/// lookahead against, because a jammed ship's *actual* speed is zero.
[[nodiscard]] float MaxSpeedOf(std::uint8_t _rawClass) noexcept
{
  return ShipClass(static_cast<HullClass>(_rawClass)).maxSpeedMetresPerSec;
}

/// Whether this ship is still travelling to where it was told to be. The same
/// question `Steering` asks to decide whether to fly, asked once more to decide
/// who may be asked to make way: a ship that has arrived is a ship with nowhere
/// to be, whether it holds a station or holds where it spawned.
[[nodiscard]] bool IsUnderway(const Guidance& _guidance, const DirectX::XMFLOAT2& _position) noexcept
{
  if (_guidance.mode != GuidanceMode::Seek)
  {
    return false;
  }
  const float dx = _guidance.targetXMetres - _position.x;
  const float dy = _guidance.targetYMetres - _position.y;
  return dx * dx + dy * dy > World::ARRIVAL_TOLERANCE_METRES * World::ARRIVAL_TOLERANCE_METRES;
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

/*
 * Making way (ADR-021): where a ship with nowhere to be should stand while
 * someone flies past, as a displacement from the berth it is standing on.
 *
 * ADR-015 gave the *mover* two ways to cope with traffic and gave traffic no
 * way to cope with the mover. That is only half of how a lane clears: braking
 * and deflection both fail closed -- a hull sitting on the one line a ship must
 * fly ends with the ship parked against it -- and the ship that could most
 * cheaply fix it is the one that is not going anywhere. So the idle ship steps
 * aside, and the step is expressed as a *target*, not a shove: it seeks its
 * temporary berth through the same envelope, avoidance and arrival machinery as
 * any ordered move, so nothing here can break a hull's speed or turn limits.
 *
 * **Three things make this stable, and each is doing real work.**
 *
 * *The corridor is tested against the berth, not the hull.* A ship that stepped
 * aside is, by construction, no longer in the way -- so a test on its live
 * position would say "clear" the instant it started moving, and it would drift
 * back into the lane and out of it and back again. Its berth does not move, so
 * the answer holds still until the mover has actually gone past.
 *
 * *The trigger and the destination are the same number.* A berth further off
 * the course than `MAKE_WAY_CLEARANCE_FACTOR` is left alone; a berth inside it
 * is stood aside to exactly that line. The displacement therefore falls to zero
 * as the berth approaches the boundary, and there is no discontinuity for a
 * marginal case to oscillate across.
 *
 * *A journey shorter than the room being made for it is not a journey.* This is
 * what stops making way from feeding itself: a ship on its way *back* from a
 * sidestep looks, to everyone else, exactly like a short-haul mover, and
 * without this line a cluster of idle ships could take turns clearing lanes for
 * each other's returns. A real order is orders of magnitude longer than a
 * sidestep, so nothing anyone actually asked for is refused by it.
 *
 * Returning is not a mechanism. When the last mover is past, no blocker is
 * found, the displacement is zero, the effective target is the berth again, and
 * the ship flies home under the same seek it stepped aside with.
 *
 * Deterministic by the same construction as the rest (ADR-005 §5): slot-order
 * iteration over positions, classes and guidance -- none of which `Steering`
 * writes -- plus the ship's own heading, which `Steering` writes only after
 * asking this; pure table lookups, no clock, no RNG, no `XM*Est`.
 */
[[nodiscard]] bool MakeWayOffset(std::uint32_t _slot, std::span<const DirectX::XMFLOAT2> _positions, std::span<const std::uint8_t> _classes,
                                 std::span<const Guidance> _guidances, float _headingRadians, DirectX::XMFLOAT2& _outOffset) noexcept
{
  const Guidance& berth = _guidances[_slot];
  const float ownRadius = ContactRadiusOf(_classes[_slot]);

  // The nearest mover only, for the reason deflection takes the nearest blocker
  // only: standing aside from two courses at once stands aside from neither.
  float nearestAhead = std::numeric_limits<float>::max();
  float nearestSide = 0.0f;
  float nearestClearance = 0.0f;
  float nearestPerpX = 0.0f;
  float nearestPerpY = 0.0f;

  for (std::uint32_t other = 0; other < _positions.size(); ++other)
  {
    if (other == _slot || _guidances[other].mode != GuidanceMode::Seek)
    {
      continue;
    }

    // The course wanted, not the heading flown: the heading is bent by the
    // avoidance this ship is about to make unnecessary, and a corridor that
    // moved with the mover's swerve would be a corridor chasing its own tail.
    const float courseX = _guidances[other].targetXMetres - _positions[other].x;
    const float courseY = _guidances[other].targetYMetres - _positions[other].y;
    const float courseLength = std::sqrt(courseX * courseX + courseY * courseY);

    const float contact = ownRadius + ContactRadiusOf(_classes[other]);
    const float clearance = World::MAKE_WAY_CLEARANCE_FACTOR * contact;
    if (courseLength <= clearance)
    {
      continue; // Arrived, holding, or on a hop shorter than the room asked for.
    }

    const float dirX = courseX / courseLength;
    const float dirY = courseY / courseLength;

    const float relX = berth.targetXMetres - _positions[other].x;
    const float relY = berth.targetYMetres - _positions[other].y;

    const float ahead = relX * dirX + relY * dirY;
    if (ahead <= 0.0f || ahead >= courseLength)
    {
      continue; // Behind the mover, or past the point it stops at anyway.
    }
    // Too far up the lane to be worth clearing yet -- and, at a top speed of
    // zero, always true, which is how an *anchored* hull under orders is
    // excluded without a branch of its own. A station cannot travel, so its
    // course reaches nothing and nobody owes it a lane.
    if (ahead > World::MAKE_WAY_LOOKAHEAD_SECONDS * MaxSpeedOf(_classes[other]))
    {
      continue;
    }

    const float side = dirX * relY - dirY * relX;
    if (std::fabs(side) >= clearance)
    {
      continue; // The berth is already clear of the lane.
    }

    // Berthed on the mover's destination: there is nowhere better to be, and
    // stepping aside would only mean stepping back inside it when it parks.
    // The mover brakes and parks adjacent instead, which is ADR-015 §5's
    // designed outcome and stays the designed outcome.
    const float gapX = berth.targetXMetres - _guidances[other].targetXMetres;
    const float gapY = berth.targetYMetres - _guidances[other].targetYMetres;
    if (gapX * gapX + gapY * gapY < contact * contact)
    {
      continue;
    }

    if (ahead < nearestAhead)
    {
      nearestAhead = ahead;
      nearestSide = side;
      nearestClearance = clearance;
      nearestPerpX = -dirY; // Unit vector toward positive side, i.e. the mover's port.
      nearestPerpY = dirX;
    }
  }

  if (nearestAhead == std::numeric_limits<float>::max())
  {
    return false;
  }

  /*
   * Which side to stand on: the shorter walk, or the shorter turn?
   *
   * Distance alone is the obvious answer and it is the wrong one. Ships cannot
   * strafe (ADR-005 §2), so a hull asked to step the way it is pointing *away*
   * from must swing its whole length round first -- a Frigate facing north,
   * told to clear thirty metres to the south, spends six seconds turning to
   * cover a distance it would have covered in one. Measured against a fast
   * mover that is the entire encounter: it finishes turning about when the
   * traffic it was clearing for has already gone past.
   *
   * So both are costed in seconds and the cheaper wins. The comparison is
   * stable under its own outcome -- once the hull starts turning toward the
   * side it picked, that side only gets cheaper -- and it stays stable against
   * the mover, because deflection bends away from whichever side the hull went
   * (ADR-015 §2), which keeps the berth's own lean pointing the same way. A tie
   * goes to starboard, the convention deflection breaks its symmetry with.
   */
  const ShipClassInfo& own = ShipClass(static_cast<HullClass>(_classes[_slot]));
  const auto secondsToClear = [&](float _wantedSide) noexcept -> float
  {
    const float sign = _wantedSide > 0.0f ? 1.0f : -1.0f;
    float seconds = 0.0f;
    if (own.turnRateRadiansPerSec > 0.0f)
    {
      const float toSide = std::atan2(sign * nearestPerpY, sign * nearestPerpX);
      seconds += std::fabs(WrapAngle(toSide - _headingRadians)) / own.turnRateRadiansPerSec;
    }
    if (own.maxSpeedMetresPerSec > 0.0f)
    {
      seconds += std::fabs(_wantedSide - nearestSide) / own.maxSpeedMetresPerSec;
    }
    return seconds;
  };
  const float wantedSide = secondsToClear(nearestClearance) < secondsToClear(-nearestClearance) ? nearestClearance : -nearestClearance;
  const float step = wantedSide - nearestSide;
  _outOffset = XMFLOAT2{nearestPerpX * step, nearestPerpY * step};
  return true;
}

/*
 * Whether anything is standing in this ship's berth.
 *
 * The one condition on flying home. A ship that made way -- or that a passing
 * hull shouldered off station -- belongs back where it was put, but not badly
 * enough to fly into someone who has since taken the spot: that would be a
 * shove dressed up as a homecoming, and `Separate` would spend the rest of the
 * session pushing the two of them apart.
 *
 * It is also what keeps an authored stack settled. Two ships spawned on one
 * point are separated apart by `Separate` and then both want the same berth
 * back; each sees the other standing in it and stays put, which is the only
 * stable answer available when two berths are the same berth.
 */
[[nodiscard]] bool BerthIsClear(std::uint32_t _slot, std::span<const DirectX::XMFLOAT2> _positions, std::span<const std::uint8_t> _classes,
                                const Guidance& _berth) noexcept
{
  const float ownRadius = ContactRadiusOf(_classes[_slot]);
  for (std::uint32_t other = 0; other < _positions.size(); ++other)
  {
    if (other == _slot)
    {
      continue;
    }
    const float dx = _positions[other].x - _berth.targetXMetres;
    const float dy = _positions[other].y - _berth.targetYMetres;
    const float contact = ownRadius + ContactRadiusOf(_classes[other]);
    if (dx * dx + dy * dy < contact * contact)
    {
      return false;
    }
  }
  return true;
}

} // namespace

void World::Reset(std::uint64_t _seed) noexcept
{
  /*
   * A reset world is **unowned** (ADR-007 §7). Not claimed here, because the
   * thread that seeds a world is not necessarily the thread that ticks it: the
   * composition root builds the start grid on Main and the server runs it on
   * Sim, and a claim taken here would record the builder and fire on the first
   * tick. Ownership is taken by the first thread that mutates it instead.
   */
  m_owner.Release();

  m_tick = 0;
  m_slotById.clear();

  // Identity goes with the state. A reset world is nobody's grid until the
  // registry says otherwise, and keeping a stale anchor here would let a Dock
  // be judged against the grid this world used to be.
  m_anchor = INVALID_ID;
  m_stationShip = INVALID_SHIP_ID;
  m_jumpAnchor = INVALID_ID;
  m_gateShip = INVALID_SHIP_ID;
  m_reachable.clear();

  m_filed.clear();

  m_ids.clear();
  m_classes.clear();
  m_wings.clear();
  m_positions.clear();
  m_velocities.clear();
  m_headings.clear();
  m_guidances.clear();
  m_hulls.clear();
  m_shields.clear();
  m_protectedUntil.clear();

  m_groups.clear();
  m_pending.clear();
  m_nextOrderId = 1;
  m_lastOrderSeqProcessed = 0;

  m_random.Seed(_seed);
}

ShipId World::Spawn(const ShipSpawn& _spawn, ShipId _shipId)
{
  NEURON_ASSERT_OWNER(m_owner);

  if (!HullClassHasContent(_spawn.hullClass))
  {
    return INVALID_SHIP_ID; // Fighter and Cruiser are ids, not ships.
  }
  if (_shipId == INVALID_SHIP_ID)
  {
    return INVALID_SHIP_ID; // The allocator ran out, or nobody asked it.
  }

  const ShipId shipId = _shipId;
  if (shipId < m_slotById.size() && m_slotById[shipId] != INVALID_SHIP_ID)
  {
    // Refused rather than asserted: the caller owns the id space, so a
    // collision is a question about the allocator, and returning the invalid id
    // is how every other refusal here answers.
    return INVALID_SHIP_ID;
  }
  m_slotById.resize(std::max(m_slotById.size(), static_cast<std::size_t>(shipId) + 1), INVALID_SHIP_ID);
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

  // Undocking spawns a ship full *and* protected, and both are the roster's
  // doing: it holds no gauges, so there is nothing to come back damaged
  // (ADR-017 §1), and it stamps the window on the way out (§5).
  m_protectedUntil.push_back(_spawn.protectedUntilTick);
  return shipId;
}

bool World::IsProtected(ShipId _shipId, std::uint32_t _tick) const noexcept
{
  std::uint32_t slot = 0;
  if (!FindSlot(_shipId, slot))
  {
    return false;
  }
  return _tick < m_protectedUntil[slot];
}

bool World::TransferOut(ShipId _shipId, TransferMember& _outMember)
{
  NEURON_ASSERT_OWNER(m_owner);

  std::uint32_t slot = 0;
  if (!FindSlot(_shipId, slot))
  {
    return false;
  }

  // Read before the removal, because `Despawn` swap-and-pops and the slot it
  // reads from is about to hold somebody else.
  _outMember.shipId = _shipId;
  _outMember.hullClass = static_cast<HullClass>(m_classes[slot]);
  _outMember.wing = m_wings[slot];
  return Despawn(_shipId);
}

void World::SetAnchor(AnchorId _anchor, ShipId _stationShip, std::span<const AnchorId> _reachable)
{
  m_anchor = _anchor;
  m_stationShip = _stationShip;
  m_reachable.assign(_reachable.begin(), _reachable.end());
}

void World::SetJump(AnchorId _jumpAnchor, ShipId _gateShip)
{
  m_jumpAnchor = _jumpAnchor;
  m_gateShip = _gateShip;
}

void World::ReleaseOwner() noexcept
{
  // The hand-off ADR-007 §7 sanctions, and the only one this game makes: the
  // composition root populates the start grid on Main, says here that it is
  // done, and the sim thread adopts it on its first tick. Anything that touches
  // the world from two threads *without* passing through here is the bug the
  // owner-assert exists to find.
  m_owner.Release();
}

bool World::Despawn(ShipId _shipId)
{
  NEURON_ASSERT_OWNER(m_owner);

  std::uint32_t slot = 0;
  if (!FindSlot(_shipId, slot))
  {
    return false;
  }

  // Out of every group first. A group holding a dead id would solve a station
  // for it and leave a gap in the formation, and would keep reporting a leg
  // nobody is flying.
  ForgetShipInGroups(_shipId);

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
    m_protectedUntil[slot] = m_protectedUntil[last];
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
  m_protectedUntil.pop_back();
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
  NEURON_ASSERT_OWNER(m_owner);

  m_tick = _tick;

  IngestOrders();
  GroupAdvance();
  Steering();
  Integrate();
  Separate();
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
   *
   * And traffic that is *not* going anywhere gets out of the way (ADR-021).
   * Braking and deflection are both things the mover does, and both fail closed
   * -- a hull parked on the one line a ship has to fly ends with the ship
   * stopped against it. The cheapest fix belongs to the ship with nothing else
   * to do: it steps out of the lane, and flies back to its berth when the lane
   * is clear. It does so by seeking a displaced target through this same loop,
   * so a sidestep obeys the envelope exactly like any ordered move.
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

    /*
     * Guidance is taken by value because making way may displace it (ADR-021),
     * and the displacement lasts one tick and is never stored.
     *
     * That is deliberate rather than incidental. The berth in `m_guidances` is
     * the answer to "where does this ship belong", and a sidestep is not a new
     * answer to that question -- it is a detour from it. Keeping the berth
     * authoritative is what makes flying home free (no traffic, no
     * displacement, seek the berth) and what keeps a temporary shuffle out of
     * the world's hashed state.
     */
    Guidance guidance = m_guidances[slot];
    if (!IsAnchored(m_classes[slot]) && !IsUnderway(guidance, m_positions[slot]))
    {
      // Only a ship that is not going anywhere makes way. One under orders has
      // its own lane to fly, and traffic between the two is ADR-015's business.
      XMFLOAT2 aside{};
      if (MakeWayOffset(slot, m_positions, m_classes, m_guidances, m_headings[slot], aside))
      {
        guidance.mode = GuidanceMode::Seek;
        guidance.targetXMetres = ClampToPlayArea(guidance.targetXMetres + aside.x);
        guidance.targetYMetres = ClampToPlayArea(guidance.targetYMetres + aside.y);
      }
      else if (guidance.mode == GuidanceMode::Hold && BerthIsClear(slot, m_positions, m_classes, guidance))
      {
        /*
         * Nothing to make way for, so go home -- and `Hold` is the one mode
         * that would not, which is why this line exists.
         *
         * `Hold` means "stay where you were put", and until making way that was
         * the same statement as "stay where you are": nothing could displace a
         * held ship except `Separate`, and a shove is not a journey to undo.
         * Now a held ship can be asked to step out of a lane, and a ship that
         * steps aside and then holds *there* has not made way, it has moved
         * house. Reading the mode as the berth it names -- which for a held
         * ship is always where it spawned (`Spawn`) -- is what makes the return
         * leg fall out of the same seek that made the outbound one.
         */
        guidance.mode = GuidanceMode::Seek;
      }
    }

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

      // Only a ship going somewhere scans traffic: a ship standing still has no
      // course to bend and no speed to cap. Note that a ship making way *is*
      // going somewhere by this point -- its displaced berth -- so it avoids
      // traffic on the way out of the lane like anything else under way.
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
