#include "pch.h"

#include "Refining.h"

#include "SiteField.h"

namespace Game
{

std::uint32_t RefinePlan::ConsumedUnits(OreId _ore) const noexcept
{
  const auto index = static_cast<std::uint8_t>(_ore);
  return inputUnits[index] - refundUnits[index];
}

bool UpgradeProject::IsComplete(const RefineryUpgradeProject& _cost) const noexcept
{
  for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
  {
    if (contributedUnits[alloy] < _cost.costUnits[alloy])
    {
      return false;
    }
  }
  return true;
}

const RefineBatchInfo* FindRefineBatch(const RefiningInfo& _refining, std::uint32_t _batchUnits) noexcept
{
  for (std::uint8_t index = 0; index < _refining.batchCount && index < MAX_REFINE_BATCHES; ++index)
  {
    if (_refining.batches[index].units == _batchUnits)
    {
      return &_refining.batches[index];
    }
  }
  return nullptr;
}

RefinePlan PlanRefine(const EconomyDef& _economy, AlloyId _alloy, std::uint32_t _batchUnits, std::uint8_t _tier) noexcept
{
  RefinePlan plan;
  if (static_cast<std::uint8_t>(_alloy) >= ALLOY_COUNT || _tier == 0 || _tier > REFINERY_TIER_COUNT)
  {
    return plan;
  }
  const RefineBatchInfo* batch = FindRefineBatch(_economy.refining, _batchUnits);
  if (batch == nullptr || _batchUnits == 0)
  {
    return plan;
  }

  const AlloyInfo& recipe = _economy.alloys[static_cast<std::uint8_t>(_alloy)];
  const RefineryTierInfo& tier = _economy.Tier(_tier);

  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    plan.inputUnits[ore] = static_cast<std::uint32_t>(recipe.inputUnits[ore]) * _batchUnits;

    /*
     * Floored per material, and per material is the point: a refund computed
     * on the total and split afterwards would have to decide which ore eats
     * the remainder, and there is no answer to that which is not an invention.
     */
    plan.refundUnits[ore] = plan.inputUnits[ore] * tier.refundPct / 100u;
  }

  plan.outputUnits = _batchUnits;
  plan.fuelUnits = recipe.fuelPerJob;

  /*
   * The order is the contract (see the header). Multiplied before either
   * division, so the two whole-percentage factors round once at the end
   * rather than twice in the middle -- which is the difference between a
   * 50-batch pricing the same as fifty singles and pricing five seconds
   * cheaper for no reason anybody chose.
   */
  const std::uint64_t base = static_cast<std::uint64_t>(recipe.refineSeconds) * _batchUnits;
  const std::uint64_t seconds = base * batch->timePct * tier.timePct / 10000u;
  plan.durationTicks = static_cast<std::uint32_t>(seconds * TICKS_PER_SECOND);

  /*
   * A job that priced at zero ticks would complete on the tick it started,
   * which is a job nobody could ever see running. One tick is the floor, and
   * it can only be reached by content that authors a sub-tick recipe.
   */
  if (plan.durationTicks == 0)
  {
    plan.durationTicks = 1;
  }

  plan.valid = true;
  return plan;
}

std::uint8_t BandTierCap(const EconomyDef& _economy, SecurityBand _band) noexcept
{
  const auto index = static_cast<std::uint8_t>(_band);
  return index < SECURITY_BAND_COUNT ? _economy.refining.bandTierCaps[index] : std::uint8_t{0};
}

bool TierCooks(const EconomyDef& _economy, std::uint8_t _tier, AlloyId _alloy) noexcept
{
  if (_tier == 0 || _tier > REFINERY_TIER_COUNT || static_cast<std::uint8_t>(_alloy) >= ALLOY_COUNT)
  {
    return false;
  }
  return _economy.alloys[static_cast<std::uint8_t>(_alloy)].tier <= _economy.Tier(_tier).recipeTierCap;
}

std::uint8_t AuthoredStationTier(bool _isStarterSystem) noexcept
{
  return _isStarterSystem ? std::uint8_t{2} : std::uint8_t{1};
}

} // namespace Game
