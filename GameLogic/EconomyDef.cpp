#include "pch.h"

#include "EconomyDef.h"

#include "Hash.h"

namespace Game
{
namespace
{

using Neuron::HashText;
using Neuron::HashValue;

/*
 * The content's spelling of each id.
 *
 * Indexed by the enum and asserted to be, so the file's vocabulary and the
 * code's cannot drift apart silently: adding an enumerator without a string
 * fails the build rather than producing an id nothing can author.
 */
constexpr const char* ORE_KEYS[ORE_COUNT] = {"ferroChroma", "astracite", "nebulite"};
constexpr const char* ALLOY_KEYS[ALLOY_COUNT] = {"ferrocitePlates", "astraGlass", "chromiteConduit", "quantumMatrix", "novaSteel"};
constexpr const char* ARCHETYPE_KEYS[SITE_ARCHETYPE_COUNT] = {"ferrousField", "irradiatedBelt", "nebulaPocket"};

constexpr const char* ORE_NAMES[ORE_COUNT] = {"Ferro-Chroma", "Astracite", "Nebulite"};
constexpr const char* ALLOY_NAMES[ALLOY_COUNT] = {"Ferrocite Plates", "Astra-Glass", "Chromite Conduit", "Quantum Matrix",
                                                 "Nova-Steel"};
constexpr const char* ARCHETYPE_NAMES[SITE_ARCHETYPE_COUNT] = {"Ferrous Crust Field", "Irradiated Belt", "Nebula Pocket"};
constexpr const char* BAND_NAMES[SECURITY_BAND_COUNT] = {"High-Sec", "Low-Sec", "Null-Sec"};

/// Folds a string in length-prefixed, so "ab" + "c" and "a" + "bc" cannot
/// produce the same running hash. The same helper `ComputeUniverseHash` uses,
/// and for the same reason.
[[nodiscard]] std::uint64_t HashName(const std::string& _name, std::uint64_t _seed) noexcept
{
  const std::uint64_t withLength = HashValue(static_cast<std::uint32_t>(_name.size()), _seed);
  return HashText(_name, withLength);
}

[[nodiscard]] std::uint64_t HashHazard(const SiteHazard& _hazard, std::uint64_t _seed) noexcept
{
  std::uint64_t hash = HashValue(static_cast<std::uint8_t>(_hazard.kind), _seed);
  hash = HashValue(_hazard.cycleSlowPerStackPct, hash);
  hash = HashValue(_hazard.stackDecaySeconds, hash);
  hash = HashValue(_hazard.burnStackThreshold, hash);
  for (const std::uint8_t damping : _hazard.sensorDampingPctByGrade)
  {
    hash = HashValue(damping, hash);
  }
  hash = HashValue(_hazard.heatPerCycle, hash);
  hash = HashValue(_hazard.heatThreshold, hash);
  hash = HashValue(_hazard.heatDecayPerSecond, hash);
  hash = HashValue(_hazard.lockoutSeconds, hash);
  hash = HashValue(_hazard.pacedCycleSlowPct, hash);
  return HashValue(_hazard.detonationThreshold, hash);
}

[[nodiscard]] bool TryKey(std::string_view _text, const char* const* _keys, std::uint8_t _count, std::uint8_t& _outIndex) noexcept
{
  for (std::uint8_t index = 0; index < _count; ++index)
  {
    if (_text == _keys[index])
    {
      _outIndex = index;
      return true;
    }
  }
  return false;
}

} // namespace

bool RefineryUpgradeProject::Exists() const noexcept
{
  for (const std::uint32_t units : costUnits)
  {
    if (units > 0)
    {
      return true;
    }
  }
  return false;
}

SecurityBand EconomyDef::BandFor(std::uint8_t _security) const noexcept
{
  if (_security >= sites.distribution.highSecurityFloor)
  {
    return SecurityBand::High;
  }
  if (_security >= sites.distribution.lowSecurityFloor)
  {
    return SecurityBand::Low;
  }
  return SecurityBand::Null;
}

std::uint32_t EconomyDef::InputVolumeLitres(AlloyId _alloy) const noexcept
{
  const AlloyInfo& alloy = Alloy(_alloy);
  std::uint32_t litres = 0;
  for (std::uint8_t index = 0; index < ORE_COUNT; ++index)
  {
    litres += static_cast<std::uint32_t>(alloy.inputUnits[index]) * ores[index].unitVolumeLitres;
  }
  return litres;
}

std::uint64_t ComputeEconomyHash(const EconomyDef& _economy)
{
  std::uint64_t hash = Neuron::FNV_OFFSET_BASIS_64;

  for (const OreInfo& ore : _economy.ores)
  {
    hash = HashName(ore.name, hash);
    hash = HashValue(ore.unitVolumeLitres, hash);
    hash = HashValue(ore.valueIndexPct, hash);
  }

  for (const AlloyInfo& alloy : _economy.alloys)
  {
    hash = HashName(alloy.name, hash);
    hash = HashValue(alloy.tier, hash);
    hash = HashValue(alloy.unitVolumeLitres, hash);
    hash = HashValue(alloy.refineSeconds, hash);
    hash = HashValue(alloy.fuelPerJob, hash);
    for (const std::uint16_t units : alloy.inputUnits)
    {
      hash = HashValue(units, hash);
    }
  }

  for (const CargoInfo& cargo : _economy.cargo)
  {
    hash = HashValue(cargo.oreHoldLitres, hash);
    hash = HashValue(cargo.generalHoldLitres, hash);
  }

  hash = HashValue(_economy.mining.cycleSeconds, hash);
  for (const std::uint32_t units : _economy.mining.unitsPerCycle)
  {
    hash = HashValue(units, hash);
  }

  const SitesInfo& sites = _economy.sites;
  hash = HashValue(sites.regenSeconds, hash);
  hash = HashValue(sites.warpInStandoffMetres, hash);
  hash = HashValue(sites.fieldRadiusMinMetres, hash);
  hash = HashValue(sites.fieldRadiusMaxMetres, hash);
  hash = HashValue(sites.arrivalSpreadPctOfField, hash);
  hash = HashValue(sites.clusterCountMin, hash);
  hash = HashValue(sites.clusterCountMax, hash);

  for (const SiteGradeInfo& grade : sites.grades)
  {
    hash = HashValue(grade.poolUnits, hash);
    hash = HashValue(grade.hazardScalePct, hash);
  }

  for (const SiteArchetypeInfo& archetype : sites.archetypes)
  {
    hash = HashName(archetype.name, hash);
    hash = HashHazard(archetype.hazard, hash);
    for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
    {
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        hash = HashValue(archetype.compositionPct[band][ore], hash);
      }
      hash = HashValue(archetype.gradeCapByBand[band], hash);
    }
  }

  const SiteDistributionInfo& distribution = sites.distribution;
  hash = HashValue(distribution.highSecurityFloor, hash);
  hash = HashValue(distribution.lowSecurityFloor, hash);
  for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
  {
    hash = HashValue(distribution.siteCountWeights[band][0], hash);
    hash = HashValue(distribution.siteCountWeights[band][1], hash);
    for (std::uint8_t archetype = 0; archetype < SITE_ARCHETYPE_COUNT; ++archetype)
    {
      hash = HashValue(distribution.archetypeWeights[band][archetype], hash);
    }
    hash = HashValue(distribution.gradeMin[band], hash);
    hash = HashValue(distribution.gradeMax[band], hash);
    hash = HashValue(distribution.maxSameArchetypePerSystem[band], hash);
  }
  hash = HashValue(static_cast<std::uint8_t>(distribution.highSecFirstSiteArchetype), hash);
  hash = HashValue(distribution.fadedPocketSystemsPerHighRegion, hash);
  hash = HashValue(distribution.minRegionOreGrade, hash);

  const RefiningInfo& refining = _economy.refining;
  hash = HashValue(refining.batchCount, hash);
  for (std::uint8_t index = 0; index < refining.batchCount; ++index)
  {
    hash = HashValue(refining.batches[index].units, hash);
    hash = HashValue(refining.batches[index].timePct, hash);
  }
  hash = HashValue(refining.queueDepthPerPlayer, hash);

  for (const RefineryTierInfo& tier : refining.tiers)
  {
    hash = HashValue(tier.slotsPerPlayer, hash);
    hash = HashValue(tier.recipeTierCap, hash);
    hash = HashValue(tier.refundPct, hash);
    hash = HashValue(tier.timePct, hash);
  }
  for (const std::uint8_t cap : refining.bandTierCaps)
  {
    hash = HashValue(cap, hash);
  }
  for (const RefineryUpgradeProject& project : refining.upgradeProjects)
  {
    for (const std::uint32_t units : project.costUnits)
    {
      hash = HashValue(units, hash);
    }
  }

  return hash;
}

std::uint64_t MixContentHashes(std::uint64_t _universeHash, std::uint64_t _economyHash) noexcept
{
  return HashValue(_economyHash, HashValue(_universeHash, Neuron::FNV_OFFSET_BASIS_64));
}

const char* OreIdName(OreId _ore) noexcept
{
  return ORE_NAMES[static_cast<std::uint8_t>(_ore)];
}

const char* AlloyIdName(AlloyId _alloy) noexcept
{
  return ALLOY_NAMES[static_cast<std::uint8_t>(_alloy)];
}

const char* SiteArchetypeName(SiteArchetype _archetype) noexcept
{
  return ARCHETYPE_NAMES[static_cast<std::uint8_t>(_archetype)];
}

const char* SecurityBandName(SecurityBand _band) noexcept
{
  return BAND_NAMES[static_cast<std::uint8_t>(_band)];
}

const char* OreContentKey(OreId _ore) noexcept
{
  return ORE_KEYS[static_cast<std::uint8_t>(_ore)];
}

bool TryOreId(std::string_view _text, OreId& _outOre) noexcept
{
  std::uint8_t index = 0;
  if (!TryKey(_text, ORE_KEYS, ORE_COUNT, index))
  {
    return false;
  }
  _outOre = static_cast<OreId>(index);
  return true;
}

bool TryAlloyId(std::string_view _text, AlloyId& _outAlloy) noexcept
{
  std::uint8_t index = 0;
  if (!TryKey(_text, ALLOY_KEYS, ALLOY_COUNT, index))
  {
    return false;
  }
  _outAlloy = static_cast<AlloyId>(index);
  return true;
}

bool TrySiteArchetype(std::string_view _text, SiteArchetype& _outArchetype) noexcept
{
  std::uint8_t index = 0;
  if (!TryKey(_text, ARCHETYPE_KEYS, SITE_ARCHETYPE_COUNT, index))
  {
    return false;
  }
  _outArchetype = static_cast<SiteArchetype>(index);
  return true;
}

} // namespace Game
