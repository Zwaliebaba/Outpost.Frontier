#include "pch.h"

#include "SiteField.h"

#include "Random.h"

#include <algorithm>

namespace Game
{
namespace
{

/*
 * How far apart two cluster centres are kept, as a share of the field radius.
 *
 * A sixth, which is loose enough that twelve clusters fit a disc without the
 * rejection loop giving up (twelve circles of r/12 cover a twelfth of the area)
 * and tight enough that a field reads as a scatter of separate rocks rather
 * than one smear. It is a layout number, not a rule anything validates against,
 * which is why it is a divisor here and not content.
 */
constexpr std::int64_t CLUSTER_SEPARATION_DIVISOR = 6;

/// How many times a centre is re-drawn before the layout settles for what it
/// has. Bounded because this runs at spin-up on the sim thread and a rejection
/// loop with no ceiling is a hang waiting for a full field; the fallback is the
/// last draw, which is inside the disc and merely closer to a neighbour than
/// the layout would have liked.
constexpr std::uint32_t CLUSTER_PLACEMENT_ATTEMPTS = 64;

/*
 * A point in the disc of `_radiusCm`, by rejection.
 *
 * Rejection rather than polar, and the reason is the guard this file sits
 * behind: polar needs a sine, the tree's only integer sine is `FixedAngle`, and
 * `FixedAngle` is universe-placement code that per-tick headers may not reach
 * for (ADR-009 §2). Two draws and a comparison need none of it, are exact in
 * integers, and give a uniform disc rather than the centre-heavy one a naive
 * polar draw produces.
 *
 * The squares are `int64` deliberately: a 12 km field is 1,200,000 cm and its
 * square is 1.4e12, which is four orders of magnitude past what an `int32`
 * counts.
 */
void DrawInDisc(Neuron::Pcg32& _rng, std::int64_t _radiusCm, std::int64_t& _outX, std::int64_t& _outY) noexcept
{
  const auto span = static_cast<std::uint32_t>(_radiusCm * 2 + 1);
  for (;;)
  {
    const std::int64_t x = static_cast<std::int64_t>(_rng.NextBelow(span)) - _radiusCm;
    const std::int64_t y = static_cast<std::int64_t>(_rng.NextBelow(span)) - _radiusCm;
    if (x * x + y * y <= _radiusCm * _radiusCm)
    {
      _outX = x;
      _outY = y;
      return;
    }
  }
}

/*
 * Hands `_total` units to `_count` clusters by weight, exactly.
 *
 * Largest-remainder: floor each share, then give the leftover out one unit at a
 * time by descending fractional part, ties by index. The alternative -- floor
 * and let the remainder evaporate -- would leave a pool the last cycle could
 * never reach, and a field that cannot be finished is a field that never
 * reports itself exhausted.
 *
 * A selection pass over at most twelve entries rather than a sort, because the
 * comparison has to be total: `std::sort` is not required to be stable and two
 * clusters with the same remainder must not be able to swap places between two
 * builds of the same layout.
 */
void ShareOut(std::uint32_t _total, const std::uint32_t* _weights, std::uint8_t _count, std::uint32_t* _outUnits) noexcept
{
  std::uint64_t weightSum = 0;
  for (std::uint8_t index = 0; index < _count; ++index)
  {
    weightSum += _weights[index];
  }
  if (_total == 0 || weightSum == 0)
  {
    for (std::uint8_t index = 0; index < _count; ++index)
    {
      _outUnits[index] = 0;
    }
    return;
  }

  std::uint64_t remainders[MAX_SITE_CLUSTERS] = {};
  std::uint32_t assigned = 0;
  for (std::uint8_t index = 0; index < _count; ++index)
  {
    const std::uint64_t scaled = static_cast<std::uint64_t>(_total) * _weights[index];
    _outUnits[index] = static_cast<std::uint32_t>(scaled / weightSum);
    remainders[index] = scaled % weightSum;
    assigned += _outUnits[index];
  }

  for (std::uint32_t left = _total - assigned; left > 0; --left)
  {
    std::uint8_t best = 0;
    for (std::uint8_t index = 1; index < _count; ++index)
    {
      if (remainders[index] > remainders[best])
      {
        best = index; // Strictly greater, so a tie keeps the lower index.
      }
    }
    ++_outUnits[best];
    remainders[best] = 0;
  }
}

} // namespace

std::uint32_t SiteCluster::TotalRemaining() const noexcept
{
  std::uint32_t total = 0;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    total += remainingUnits[ore];
  }
  return total;
}

std::uint32_t SiteField::Remaining(OreId _ore) const noexcept
{
  std::uint32_t total = 0;
  for (std::uint8_t index = 0; index < clusterCount; ++index)
  {
    total += clusters[index].Remaining(_ore);
  }
  return total;
}

std::uint32_t SiteField::TotalRemaining() const noexcept
{
  std::uint32_t total = 0;
  for (std::uint8_t index = 0; index < clusterCount; ++index)
  {
    total += clusters[index].TotalRemaining();
  }
  return total;
}

std::uint32_t SiteLedger::TotalRemaining() const noexcept
{
  std::uint32_t total = 0;
  for (std::uint8_t index = 0; index < clusterCount; ++index)
  {
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      total += remainingUnits[index][ore];
    }
  }
  return total;
}

void StoreSiteField(const SiteField& _field, SiteLedger& _outLedger) noexcept
{
  _outLedger.clusterCount = _field.clusterCount;
  for (std::uint8_t index = 0; index < MAX_SITE_CLUSTERS; ++index)
  {
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      // Every slot, not only the live ones: a ledger that kept a shrunken
      // epoch's leftovers past the array it wrote them in would hash
      // differently from the one that never held them.
      _outLedger.remainingUnits[index][ore] =
        index < _field.clusterCount ? _field.clusters[index].remainingUnits[ore] : 0;
    }
  }
}

void ApplySiteLedger(const SiteLedger& _ledger, SiteField& _field) noexcept
{
  for (std::uint8_t index = 0; index < _field.clusterCount && index < _ledger.clusterCount; ++index)
  {
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      _field.clusters[index].remainingUnits[ore] = _ledger.remainingUnits[index][ore];
    }
  }
}

SiteField BuildSiteField(const SitesInfo& _sites, SiteArchetype _archetype, std::uint8_t _grade,
                         std::int32_t _fieldRadiusCm, std::uint32_t _layoutSalt,
                         const std::uint32_t (&_poolUnits)[ORE_COUNT]) noexcept
{
  SiteField field;
  field.archetype = _archetype;
  field.grade = _grade;
  field.fieldRadiusCm = _fieldRadiusCm;
  if (_grade == 0 || _grade > SITE_GRADE_COUNT || _fieldRadiusCm <= 0)
  {
    return field; // Not a site, and `Exists` says so.
  }

  /*
   * One stream, seeded by the salt alone.
   *
   * The salt already carries the anchor and the epoch (`SiteEpoch`), so mixing
   * either in again would tie this function to how the salt was made -- and the
   * point of taking a salt rather than the pair is that a test can hand it one
   * and get a field.
   */
  Neuron::Pcg32 rng{_layoutSalt};

  const auto low = static_cast<std::uint32_t>(std::max<std::uint32_t>(1, _sites.clusterCountMin));
  const auto high = static_cast<std::uint32_t>(std::clamp<std::uint32_t>(
    std::max(_sites.clusterCountMax, low), low, MAX_SITE_CLUSTERS));
  field.clusterCount = static_cast<std::uint8_t>(low + rng.NextBelow(high - low + 1));

  // Every centre, then the ores. The sequence is the contract (`SiteField.h`).
  const auto radius = static_cast<std::int64_t>(_fieldRadiusCm);
  const std::int64_t separation = radius / CLUSTER_SEPARATION_DIVISOR;
  for (std::uint8_t index = 0; index < field.clusterCount; ++index)
  {
    std::int64_t x = 0;
    std::int64_t y = 0;
    for (std::uint32_t attempt = 0; attempt < CLUSTER_PLACEMENT_ATTEMPTS; ++attempt)
    {
      DrawInDisc(rng, radius, x, y);
      bool clear = true;
      for (std::uint8_t other = 0; other < index && clear; ++other)
      {
        const std::int64_t dx = x - field.clusters[other].xCm;
        const std::int64_t dy = y - field.clusters[other].yCm;
        clear = dx * dx + dy * dy >= separation * separation;
      }
      if (clear)
      {
        break;
      }
    }
    field.clusters[index].xCm = static_cast<std::int32_t>(x);
    field.clusters[index].yCm = static_cast<std::int32_t>(y);
  }

  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    /*
     * A weight per cluster, so the field is uneven and one cluster is worth
     * flying to. The range is drawn even for an ore the site does not hold, so
     * that the *stream* does not depend on which ores are present -- two fields
     * differing only in composition would otherwise place their remaining ores
     * differently, and the reason would be invisible.
     */
    std::uint32_t weights[MAX_SITE_CLUSTERS] = {};
    for (std::uint8_t index = 0; index < field.clusterCount; ++index)
    {
      weights[index] = 1u + rng.NextBelow(100u);
    }

    std::uint32_t units[MAX_SITE_CLUSTERS] = {};
    ShareOut(_poolUnits[ore], weights, field.clusterCount, units);
    for (std::uint8_t index = 0; index < field.clusterCount; ++index)
    {
      field.clusters[index].remainingUnits[ore] = units[index];
    }
  }
  return field;
}

std::uint8_t RichestCluster(const SiteField& _field, OreFilter _filter) noexcept
{
  std::uint8_t best = MAX_SITE_CLUSTERS;
  std::uint32_t bestUnits = 0;
  for (std::uint8_t index = 0; index < _field.clusterCount; ++index)
  {
    std::uint32_t units = 0;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      if (OreFilterMatches(_filter, static_cast<OreId>(ore)))
      {
        units += _field.clusters[index].remainingUnits[ore];
      }
    }
    // Strictly greater, so a tie keeps the lower index and two machines looking
    // at the same field send their wings to the same rocks.
    if (units > bestUnits)
    {
      best = index;
      bestUnits = units;
    }
  }
  return best;
}

bool ClusterOre(const SiteField& _field, std::uint8_t _cluster, OreFilter _filter, OreId& _outOre) noexcept
{
  if (_cluster >= _field.clusterCount)
  {
    return false;
  }
  bool found = false;
  std::uint32_t bestUnits = 0;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    const std::uint32_t units = _field.clusters[_cluster].remainingUnits[ore];
    if (units == 0 || !OreFilterMatches(_filter, static_cast<OreId>(ore)))
    {
      continue;
    }
    if (!found || units > bestUnits)
    {
      // Ties by `OreId` order, which is the order the recipe sheet reads and
      // the order every per-ore array in the economy uses.
      _outOre = static_cast<OreId>(ore);
      bestUnits = units;
      found = true;
    }
  }
  return found;
}

HazardKind FieldHazard(const EconomyDef& _economy, const SiteField& _field) noexcept
{
  return _field.Exists() ? _economy.Archetype(_field.archetype).hazard.kind : HazardKind::None;
}

std::uint32_t MiningCycleTicks(const EconomyDef& _economy, const SiteField& _field,
                               std::uint8_t _radiationStacks) noexcept
{
  const std::uint32_t base = _economy.mining.cycleSeconds * TICKS_PER_SECOND;
  if (FieldHazard(_economy, _field) != HazardKind::Radiation || _radiationStacks == 0)
  {
    return base;
  }
  const SiteHazard& hazard = _economy.Archetype(_field.archetype).hazard;
  const std::uint64_t slowPct = static_cast<std::uint64_t>(_radiationStacks) * hazard.cycleSlowPerStackPct *
                                _economy.Grade(_field.grade).hazardScalePct / 100u;
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(base) * (100u + slowPct) / 100u);
}

std::uint16_t CycleHeat(const EconomyDef& _economy, const SiteField& _field) noexcept
{
  if (FieldHazard(_economy, _field) != HazardKind::Nebula)
  {
    return 0;
  }
  const std::uint64_t heat = static_cast<std::uint64_t>(_economy.Archetype(_field.archetype).hazard.heatPerCycle) *
                             _economy.Grade(_field.grade).hazardScalePct / 100u;
  return static_cast<std::uint16_t>(std::min<std::uint64_t>(heat, 0xffffu));
}

std::uint8_t SensorDampingPct(const EconomyDef& _economy, const SiteField& _field) noexcept
{
  if (FieldHazard(_economy, _field) != HazardKind::Nebula)
  {
    return 100; // Nothing dampens, so everything is seen at its own range.
  }
  const std::uint8_t pct = _economy.Archetype(_field.archetype).hazard.sensorDampingPctByGrade[_field.grade - 1];
  return pct == 0 ? 100 : pct;
}

} // namespace Game
