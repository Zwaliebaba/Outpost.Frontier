#include "pch.h"

#include "Eta.h"

#include "World.h"

#include <algorithm>
#include <cmath>

namespace Game
{

float TravelSeconds(HullClass _hullClass, float _distanceMetres) noexcept
{
  const ShipClassInfo& info = ShipClass(_hullClass);

  // The ship stops when it is within tolerance, so that last couple of metres
  // is not travelled and must not be charged for. It is two metres against
  // journeys measured in kilometres and makes no visible difference to a long
  // leg; it is what keeps a *short* one from reporting a fraction of a second
  // of braking that never happens.
  const float reach = _distanceMetres - World::ARRIVAL_TOLERANCE_METRES;
  if (reach <= 0.0f)
  {
    return 0.0f; // Already there.
  }

  if (info.maxSpeedMetresPerSec <= 0.0f || info.accelMetresPerSecSq <= 0.0f)
  {
    return -1.0f; // A Structure. Not slow -- unable.
  }

  /*
   * The distance a full accelerate-and-brake costs: v^2/2a each way.
   *
   * Above it the profile is a trapezoid and below it a triangle that never
   * reaches top speed. The two agree exactly at the boundary -- both give
   * 2v/a -- which is worth knowing, because a discontinuity here would show up
   * as an ETA that jumped while the player dragged the puck across it.
   */
  const float rampDistance = info.maxSpeedMetresPerSec * info.maxSpeedMetresPerSec / info.accelMetresPerSecSq;
  if (reach >= rampDistance)
  {
    return reach / info.maxSpeedMetresPerSec + info.maxSpeedMetresPerSec / info.accelMetresPerSecSq;
  }
  return 2.0f * std::sqrt(reach / info.accelMetresPerSecSq);
}

float GroupTravelSeconds(std::span<const TravelLeg> _legs) noexcept
{
  float slowest = -1.0f;
  for (const TravelLeg& leg : _legs)
  {
    const float seconds = TravelSeconds(leg.hullClass, leg.distanceMetres);
    if (seconds < 0.0f)
    {
      continue; // Cannot make it; not a reason to say nothing about the rest.
    }
    slowest = std::max(slowest, seconds);
  }
  return slowest;
}

} // namespace Game
