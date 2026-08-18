#include "pch.h"

#include "Eta.h"

#include "World.h"

#include <algorithm>
#include <cmath>

namespace Game
{

float TravelSeconds(HullClass _hullClass, float _distanceMetres) noexcept
{
  return RemainingSeconds(_hullClass, _distanceMetres, 0.0f);
}

float RemainingSeconds(HullClass _hullClass, float _distanceMetres, float _speedMetresPerSec) noexcept
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

  const float top = info.maxSpeedMetresPerSec;
  const float accel = info.accelMetresPerSecSq;

  // Clamped rather than trusted: a velocity that came off the wire, or one
  // sampled the tick a class table changed under it, must not make the rest of
  // this arithmetic imaginary.
  const float speed = std::clamp(_speedMetresPerSec, 0.0f, top);

  /*
   * A trapezoid from wherever it already is: run up from `speed` to `top`,
   * cruise, brake to rest.
   *
   * `climb` is the ground the run-up takes and `brake` the ground the stop
   * takes. Above their sum the profile is a trapezoid; below it a triangle that
   * turns round at some peak short of `top`, and the two agree exactly at the
   * boundary -- a discontinuity here would show as an ETA that jumped while the
   * player dragged the puck across one particular radius.
   */
  const float climb = (top * top - speed * speed) / (2.0f * accel);
  const float brake = top * top / (2.0f * accel);
  if (reach >= climb + brake)
  {
    return (top - speed) / accel + (reach - climb - brake) / top + top / accel;
  }

  // The peak it does reach: the speed at which running up and braking exactly
  // consume the distance, from (p^2 - u^2)/2a + p^2/2a = d.
  const float peak = std::sqrt((2.0f * accel * reach + speed * speed) * 0.5f);
  return (peak - speed) / accel + peak / accel;
}

float GroupTravelSeconds(std::span<const TravelLeg> _legs) noexcept
{
  float slowest = -1.0f;
  for (const TravelLeg& leg : _legs)
  {
    const float seconds = RemainingSeconds(leg.hullClass, leg.distanceMetres, leg.speedMetresPerSec);
    if (seconds < 0.0f)
    {
      continue; // Cannot make it; not a reason to say nothing about the rest.
    }
    slowest = std::max(slowest, seconds);
  }
  return slowest;
}

} // namespace Game
