#include "pch.h"

#include "World.h"

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

void World::Tick(std::uint32_t _tick, std::span<const ScriptedMove> _moves)
{
  m_tick = _tick;

  IngestOrders(_moves);
  // GroupAdvance belongs here (S10), between the orders arriving and the ships
  // reacting to them.
  Steering();
  Integrate();
  // EmitSnapshot belongs here (S7), after the state is final for this tick.
}

void World::IngestOrders(std::span<const ScriptedMove> _moves)
{
  // Orders are applied in the order they arrive, and within an order in the
  // order the ships are listed. Both are part of the replay contract: the same
  // log has to produce the same assignment every time.
  for (const ScriptedMove& move : _moves)
  {
    const float targetX = ClampToPlayArea(move.targetXMetres);
    const float targetY = ClampToPlayArea(move.targetYMetres);

    for (std::uint32_t i = 0; i < move.shipCount; ++i)
    {
      std::uint32_t slot = 0;
      if (move.shipIds == nullptr || !FindSlot(move.shipIds[i], slot))
      {
        continue; // An order naming a ship that is gone is not an error here;
                  // S9's ValidateOrder is where a client hears about it.
      }

      Guidance& guidance = m_guidances[slot];
      if (move.hold)
      {
        guidance.mode = GuidanceMode::Hold;
        guidance.targetXMetres = m_positions[slot].x;
        guidance.targetYMetres = m_positions[slot].y;
        guidance.arrivalFacingRadians = m_headings[slot];
        continue;
      }

      guidance.mode = GuidanceMode::Seek;
      guidance.targetXMetres = targetX;
      guidance.targetYMetres = targetY;
      guidance.arrivalFacingRadians = WrapAngle(move.arrivalFacingRadians);
    }
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
