#include "pch.h"

#include "SiteEpoch.h"

#include "FixedAngle.h"

#include "Random.h"

namespace Game
{
namespace
{

/*
 * The stream every epoch draw comes from.
 *
 * One `Pcg32` seeded from (anchor, epoch) rather than a hash of them, because
 * ADR-005 §5 makes a seeded PCG32 the only randomness this tree has and a
 * second convention would be a second thing to prove identical on two
 * machines. The anchor goes in the high half so that consecutive epochs of one
 * site and consecutive sites in one epoch both walk different streams -- an
 * xor of two small numbers would collide those two axes against each other.
 */
[[nodiscard]] Neuron::Pcg32 EpochStream(AnchorId _anchor, std::uint32_t _epochIndex) noexcept
{
  const std::uint64_t seed = (static_cast<std::uint64_t>(_anchor) << 32) | static_cast<std::uint64_t>(_epochIndex);
  return Neuron::Pcg32{seed};
}

} // namespace

std::uint32_t SiteEpochIndex(std::uint32_t _shardTick, AnchorId _anchor, std::uint32_t _epochTicks) noexcept
{
  if (_epochTicks == 0)
  {
    return 0; // A shard configured never to regenerate sits in epoch 0 forever.
  }

  // The stagger is added rather than subtracted so the arithmetic cannot
  // underflow at tick 0 -- which is the tick every test starts at, and would
  // therefore be the one case a subtraction got wrong everywhere at once.
  const std::uint32_t stagger = static_cast<std::uint32_t>((static_cast<std::uint64_t>(_anchor) * 2654435761ull) % _epochTicks);
  return (_shardTick + stagger) / _epochTicks;
}

SitePlacement SiteEpochPlacement(const UniversePos& _systemCentre, AnchorId _anchor, const SiteSpec& _site,
                                 std::uint32_t _epochIndex, std::int64_t _warpInStandoffMetres) noexcept
{
  Neuron::Pcg32 rng = EpochStream(_anchor, _epochIndex);

  const std::uint32_t ringBearing = rng.NextBelow(ANGLE_STEPS);
  const std::uint32_t approachBearing = rng.NextBelow(ANGLE_STEPS);

  SitePlacement placement;
  placement.origin = _systemCentre;
  const UniversePos onRing = PolarOffset(_site.orbitRingRadiusMetres, ringBearing);
  placement.origin.x += onRing.x;
  placement.origin.y += onRing.y;

  // Outside the rocks by the authored standoff, facing back in at them. The
  // radius is built in centimetres because a local offset is centimetres --
  // converting at the end would round the standoff and not the field.
  const std::int64_t warpInRadiusCm = static_cast<std::int64_t>(_site.fieldRadiusCm) + _warpInStandoffMetres * 100;
  const UniversePos warpIn = PolarOffset(warpInRadiusCm, approachBearing);
  placement.warpInPoint = LocalOffsetCm{static_cast<std::int32_t>(warpIn.x), static_cast<std::int32_t>(warpIn.y)};
  placement.warpInFacingTurns16 =
      static_cast<std::uint16_t>(((approachBearing + ANGLE_STEPS / 2) % ANGLE_STEPS) * TURNS16_PER_ANGLE_STEP);

  placement.layoutSalt = _site.layoutSeed ^ rng.Next();
  return placement;
}

} // namespace Game
