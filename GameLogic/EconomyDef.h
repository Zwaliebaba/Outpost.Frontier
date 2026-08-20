#pragma once

#include "ShipClass.h"

#include <cstdint>
#include <string>
#include <string_view>

/*
 * The economy definition (ADR-024 §7, build order E1a).
 *
 * What `Economy.json` becomes once it has been read: ores and what they are
 * worth, alloy recipes and what they cost in time, per-hull cargo capacity, the
 * mining cycle, the shape of a mining site, and the refinery's tiers.
 *
 * **This is the first balance content in the tree**, and it arrives the way
 * ADR-012 §D13 said balance would when it stopped wanting to be code: through
 * the same parser the universe uses, guarded by a hash the handshake refuses a
 * mismatch on. The ship class table stays compiled -- movement is not economy,
 * and the two answer to different suites.
 *
 * **The taxonomies are closed; the numbers are content.** `OreId`, `AlloyId`
 * and `SiteArchetype` are enums here rather than rows in the file, because each
 * is a wire value, an array index and an icon index at once -- exactly what
 * `HullClass` is, and for exactly the reason ADR-009 §6 closed that one. A file
 * naming an ore this enum does not know is a diagnostic, not a fourth ore.
 *
 * **Everything is stored indexed by its taxonomy, never in file order**, which
 * is what makes `ComputeEconomyHash` order-independent by construction rather
 * than by remembering to sort: there is no file order left to hash. Reordering
 * the keys in `Economy.json` cannot move the number, and changing a value
 * always does.
 *
 * **Integers throughout.** Litres, seconds, units and whole percentages. Cargo
 * arithmetic must be exact on both halves -- a hold is full when
 * `usedLitres + unitVolume > capacityLitres` -- and a float here would let two
 * clients round to different answers about the same hold.
 */

namespace Game
{

/*
 * The three ores (ADR-024 §2). Ordered as the recipe sheet reads them, and the
 * order every per-ore array in this file and on the wire uses.
 */
enum class OreId : std::uint8_t
{
  FerroChroma = 0,
  Astracite = 1,
  Nebulite = 2
};

inline constexpr std::uint8_t ORE_COUNT = 3;

/// The five alloys (ADR-024 §6a), in tier order. An alloy id crosses the wire
/// in a Bay record, so these renumber never.
enum class AlloyId : std::uint8_t
{
  FerrocitePlates = 0,
  AstraGlass = 1,
  ChromiteConduit = 2,
  QuantumMatrix = 3,
  NovaSteel = 4
};

inline constexpr std::uint8_t ALLOY_COUNT = 5;

/// What kind of place a mining site is (ADR-024 §3b). Also its hazard, its
/// composition and, eventually, its art.
enum class SiteArchetype : std::uint8_t
{
  FerrousField = 0,
  IrradiatedBelt = 1,
  NebulaPocket = 2
};

inline constexpr std::uint8_t SITE_ARCHETYPE_COUNT = 3;

/*
 * The security band a system falls in (ADR-024 §3c), read off the system's
 * baked `security` value against `bandSecurityFloors`.
 *
 * A band is derived, never authored: a 60+ system inside a low-band region is
 * possible and stays High-behaving, which is flavour the map's gradient already
 * draws rather than a bug.
 */
enum class SecurityBand : std::uint8_t
{
  High = 0,
  Low = 1,
  Null = 2
};

inline constexpr std::uint8_t SECURITY_BAND_COUNT = 3;

/// Grades run I..V and are stored at `grade - 1`.
inline constexpr std::uint8_t SITE_GRADE_COUNT = 5;

/// Refinery tiers run 1..4 (T1, T2, T3, and Null-only T3-C) and are stored at
/// `tier - 1`.
inline constexpr std::uint8_t REFINERY_TIER_COUNT = 4;

/// How many batch sizes a refinery may offer. Three are authored; the fourth is
/// headroom, so adding one is a content edit rather than a header edit.
inline constexpr std::uint8_t MAX_REFINE_BATCHES = 4;

struct OreInfo
{
  std::string name;

  /// One unit's volume. The whole cargo system counts in these.
  std::uint32_t unitVolumeLitres = 0;

  /*
   * The tuning anchor future pricing hangs off, as a whole percentage with
   * Ferro-Chroma at 100.
   *
   * Not a price -- no currency exists -- and *derived* rather than felt:
   * inverse effective mining rate times an access-risk multiplier for how much
   * of the supply sits in shootable space.
   */
  std::uint32_t valueIndexPct = 0;
};

struct AlloyInfo
{
  std::string name;

  /// 1, 2 or 3. A refinery tier's `recipeTierCap` is compared against this.
  std::uint8_t tier = 0;

  std::uint32_t unitVolumeLitres = 0;
  std::uint32_t refineSeconds = 0;

  /*
   * Station fuel per job, reserved at zero (ADR-024 §6d).
   *
   * Fuel is the right long-term sink and the right reason for a supply convoy
   * to exist, and fuel before a currency is a resource with no faucet. The
   * field is here, read and honoured, so the market phase is a data patch
   * rather than a schema bump.
   */
  std::uint32_t fuelPerJob = 0;

  /// Indexed by `OreId`; zero means this ore is not an input. An array rather
  /// than a list because a recipe's *contents* are what matter and its written
  /// order is not, so two files that differ only in input order are the same
  /// recipe and hash the same.
  std::uint16_t inputUnits[ORE_COUNT] = {};

  [[nodiscard]] std::uint16_t Input(OreId _ore) const noexcept { return inputUnits[static_cast<std::uint8_t>(_ore)]; }
};

/*
 * What a hull can carry (ADR-024 §5a).
 *
 * Two holds because a Miner hauls badly *by configuration*: its ore hold is
 * large and ore-only, and its general hold is a glovebox, which is the Hauler's
 * employment contract. A hull with both numbers zero carries nothing -- the
 * stations, the gate and the two reserved classes.
 *
 * Capacity never varies per ship instance and never touches the movement
 * envelope: a laden Hauler flies exactly like an empty one. Both are deliberate
 * (ADR-024 §5a) and both are Phase-2 refit territory if they ever change.
 */
struct CargoInfo
{
  std::uint32_t oreHoldLitres = 0;
  std::uint32_t generalHoldLitres = 0;

  [[nodiscard]] bool CarriesAnything() const noexcept { return oreHoldLitres > 0 || generalHoldLitres > 0; }
};

struct MiningInfo
{
  std::uint32_t cycleSeconds = 0;

  /// Indexed by `OreId`. Yield is hardness, not luck: a completed cycle
  /// produces exactly this or what is left of the cluster, with no draw taken,
  /// because an RNG draw in the per-tick path is how a replay stops
  /// reproducing (ADR-005).
  std::uint32_t unitsPerCycle[ORE_COUNT] = {};
};

struct SiteGradeInfo
{
  std::uint32_t poolUnits = 0;

  /// Scales the archetype's hazard, as a whole percentage. A grade-V field is
  /// three times as rich and three times as unpleasant.
  std::uint32_t hazardScalePct = 0;
};

enum class HazardKind : std::uint8_t
{
  None = 0,
  Radiation = 1,
  Nebula = 2
};

/*
 * What a site does to the ships working it (ADR-024 §3b).
 *
 * Flat rather than a variant: `kind` says which fields mean anything, and the
 * rest are zero. That keeps the record trivially hashable and keeps the parser
 * from needing a second shape per archetype.
 *
 * Each hazard has a **Phase-1 half** that lands with this economy -- slower
 * cycles, dampened sensors, a laser lockout, none of which needs a damage model
 * -- and a **combat-era half** whose numbers are authored now and read by
 * nothing, the way `OrderReason::CombatEngaged` is numbered and returned by
 * nothing. The combat ADR should find these rather than invent them.
 */
struct SiteHazard
{
  HazardKind kind = HazardKind::None;

  // Radiation: a stack per completed cycle in-field, each stretching the next.
  std::uint16_t cycleSlowPerStackPct = 0;
  std::uint16_t stackDecaySeconds = 0;
  std::uint16_t burnStackThreshold = 0; // Reserved: hull burn, combat-era.

  // Nebula: detection range factor by grade (I..V), and the heat a cycle adds.
  std::uint8_t sensorDampingPctByGrade[SITE_GRADE_COUNT] = {};
  std::uint16_t heatPerCycle = 0;
  std::uint16_t heatThreshold = 0;
  std::uint16_t heatDecayPerSecond = 0;
  std::uint16_t lockoutSeconds = 0;
  std::uint16_t pacedCycleSlowPct = 0;
  std::uint16_t detonationThreshold = 0; // Reserved: gas-pocket AoE, combat-era.
};

struct SiteArchetypeInfo
{
  std::string name;
  SiteHazard hazard;

  /// Share of the pool per band, in `OreId` order, summing to 100. A field is
  /// mostly its archetype's ore and never purely -- a belt is worth working
  /// even when it is not what you came for.
  std::uint8_t compositionPct[SECURITY_BAND_COUNT][ORE_COUNT] = {};

  /// Per-band grade ceiling, or 0 for none. This is where ruling 1b lives:
  /// High-Sec nebula pockets are capped at grade I -- faded, thin, hazard still
  /// on -- so Tier 1 stays craftable in every band while no market exists to
  /// buy Nebulite from.
  std::uint8_t gradeCapByBand[SECURITY_BAND_COUNT] = {};
};

/*
 * What the bake reads, and the only block in this file it reads (ADR-024 §7's
 * division of custody: the bake consumes this, the runtime consumes the rest).
 *
 * The guarantees are data rather than numbers hard-coded in the generator, so
 * E1b's invariant suite checks the file's own claims instead of a second copy
 * of them.
 */
struct SiteDistributionInfo
{
  /// A system is High at or above the first, Low at or above the second, Null
  /// below it.
  std::uint8_t highSecurityFloor = 0;
  std::uint8_t lowSecurityFloor = 0;

  /// Per band, the weights of a 2-site and a 3-site system.
  std::uint8_t siteCountWeights[SECURITY_BAND_COUNT][2] = {};

  /// Per band, weights in `SiteArchetype` order.
  std::uint8_t archetypeWeights[SECURITY_BAND_COUNT][SITE_ARCHETYPE_COUNT] = {};

  /// Per band, the inclusive grade range a site may roll.
  std::uint8_t gradeMin[SECURITY_BAND_COUNT] = {};
  std::uint8_t gradeMax[SECURITY_BAND_COUNT] = {};

  /// The new-player floor: every High-Sec system's first site is this.
  SiteArchetype highSecFirstSiteArchetype = SiteArchetype::FerrousField;

  std::uint8_t maxSameArchetypePerSystem[SECURITY_BAND_COUNT] = {};

  /// Ruling 1b's guarantee, made measurable for the bake's invariants.
  std::uint8_t fadedPocketSystemsPerHighRegion = 0;

  /// Every region covers all three ores at this grade or better.
  std::uint8_t minRegionOreGrade = 0;
};

struct SitesInfo
{
  /*
   * The epoch (ADR-024 §3d, ruling R7). One day, at a per-site offset staggered
   * from its anchor id, and it does three things at once: the pool refills, the
   * site's bearing on its orbit ring rotates, and the rock layout reshuffles.
   * The field re-forms overnight, so scouting goes stale and a camp costs live
   * presence rather than knowledge.
   */
  std::uint32_t regenSeconds = 0;

  std::uint32_t warpInStandoffMetres = 0;
  std::uint32_t fieldRadiusMinMetres = 0;
  std::uint32_t fieldRadiusMaxMetres = 0;

  /// Arrivals scatter across this share of the field radius rather than onto a
  /// point. This is the half of the anti-camp design that actually bites: under
  /// anchor-only warp an attacker reaches the site the same way a miner does,
  /// so what dilutes a camp is where they land, not where the field is.
  std::uint32_t arrivalSpreadPctOfField = 0;

  std::uint32_t clusterCountMin = 0;
  std::uint32_t clusterCountMax = 0;

  SiteGradeInfo grades[SITE_GRADE_COUNT];
  SiteArchetypeInfo archetypes[SITE_ARCHETYPE_COUNT];
  SiteDistributionInfo distribution;
};

/// A batch size and what it does to the job's time, as a whole percentage.
struct RefineBatchInfo
{
  std::uint32_t units = 0;
  std::uint32_t timePct = 0;
};

struct RefineryTierInfo
{
  /// Concurrent jobs **per player**, not shared: a first-come pool on a shard
  /// of hundreds of commanders is a griefable queue, and contention belongs at
  /// the field and the trade lane rather than at the job form.
  std::uint8_t slotsPerPlayer = 0;

  /// The highest alloy tier this refinery will cook.
  std::uint8_t recipeTierCap = 0;

  /// The material-efficiency refund, floored per material at completion. It is
  /// deterministic on purpose: a better station pays better without an RNG draw
  /// anywhere near a ledger.
  std::uint8_t refundPct = 0;

  std::uint8_t timePct = 0;
};

/// What a station's next tier costs, indexed by `AlloyId`; zero means this
/// alloy is not required. Indexed rather than listed for the same reason a
/// recipe's inputs are: contents matter, written order does not.
struct RefineryUpgradeProject
{
  std::uint32_t costUnits[ALLOY_COUNT] = {};

  [[nodiscard]] bool Exists() const noexcept;
};

struct RefiningInfo
{
  std::uint8_t batchCount = 0;
  RefineBatchInfo batches[MAX_REFINE_BATCHES];

  std::uint8_t queueDepthPerPlayer = 0;

  RefineryTierInfo tiers[REFINERY_TIER_COUNT];

  /*
   * The highest tier a station in each band may reach -- the industrial map.
   *
   * Nova-Steel is physically born in dangerous space: no High-Sec station can
   * ever cook it, so endgame industry runs ore into the dark and alloys out of
   * it, both legs escortable and both interdictable when combat arrives.
   */
  std::uint8_t bandTierCaps[SECURITY_BAND_COUNT] = {};

  /// Indexed by tier; the entry at index 0 (tier 1) is the authored floor and
  /// has no project.
  RefineryUpgradeProject upgradeProjects[REFINERY_TIER_COUNT];
};

struct EconomyDef
{
  OreInfo ores[ORE_COUNT];
  AlloyInfo alloys[ALLOY_COUNT];

  /// Indexed by `HullClass`. A hull that carries nothing simply has zeroes.
  CargoInfo cargo[HULL_CLASS_COUNT];

  MiningInfo mining;
  SitesInfo sites;
  RefiningInfo refining;

  [[nodiscard]] const OreInfo& Ore(OreId _ore) const noexcept { return ores[static_cast<std::uint8_t>(_ore)]; }
  [[nodiscard]] const AlloyInfo& Alloy(AlloyId _alloy) const noexcept { return alloys[static_cast<std::uint8_t>(_alloy)]; }
  [[nodiscard]] const CargoInfo& Cargo(HullClass _hull) const noexcept { return cargo[static_cast<std::uint8_t>(_hull)]; }
  [[nodiscard]] const SiteArchetypeInfo& Archetype(SiteArchetype _archetype) const noexcept
  {
    return sites.archetypes[static_cast<std::uint8_t>(_archetype)];
  }

  /// Grades and tiers are 1-based in the content and 0-based in storage, which
  /// is exactly the kind of off-by-one worth having one place for.
  [[nodiscard]] const SiteGradeInfo& Grade(std::uint8_t _grade) const noexcept { return sites.grades[_grade - 1]; }
  [[nodiscard]] const RefineryTierInfo& Tier(std::uint8_t _tier) const noexcept { return refining.tiers[_tier - 1]; }

  /// Which band a system's baked security value falls in (ADR-024 §3c).
  [[nodiscard]] SecurityBand BandFor(std::uint8_t _security) const noexcept;

  /// The volume one unit of an alloy's inputs occupies, for the compression
  /// property the envelope suite asserts: every alloy packs its ore into less
  /// space than the ore took.
  [[nodiscard]] std::uint32_t InputVolumeLitres(AlloyId _alloy) const noexcept;
};

/*
 * The content hash, folded into the handshake beside `universeHash`.
 *
 * Canonical over the *parsed* content rather than the bytes, exactly as
 * `ComputeUniverseHash` is (ADR-012 §D12): comments, whitespace and key order
 * never move it, and a changed number always does. Order independence is free
 * here -- every field is stored at an index its taxonomy fixes, so there is no
 * file order left to canonicalise.
 */
[[nodiscard]] std::uint64_t ComputeEconomyHash(const EconomyDef& _economy);

/// The two content hashes as the one number the handshake carries. Mixing costs
/// no wire field, which is why an economy mismatch is refused by machinery that
/// already exists; the price is that the number cannot say *which* file
/// differs, so the boot log states both separately.
[[nodiscard]] std::uint64_t MixContentHashes(std::uint64_t _universeHash, std::uint64_t _economyHash) noexcept;

/// What to call one on screen. Never null, like `FormationName` and for the
/// same reason: a label with no text is a control the player cannot name.
[[nodiscard]] const char* OreIdName(OreId _ore) noexcept;
[[nodiscard]] const char* AlloyIdName(AlloyId _alloy) noexcept;
[[nodiscard]] const char* SiteArchetypeName(SiteArchetype _archetype) noexcept;
[[nodiscard]] const char* SecurityBandName(SecurityBand _band) noexcept;

/// The content's own spelling of an id, for parsing and for diagnostics. These
/// are the strings `Economy.json` uses, not display names.
[[nodiscard]] const char* OreContentKey(OreId _ore) noexcept;
[[nodiscard]] const char* SiteArchetypeContentKey(SiteArchetype _archetype) noexcept;

[[nodiscard]] bool TryOreId(std::string_view _text, OreId& _outOre) noexcept;
[[nodiscard]] bool TryAlloyId(std::string_view _text, AlloyId& _outAlloy) noexcept;
[[nodiscard]] bool TrySiteArchetype(std::string_view _text, SiteArchetype& _outArchetype) noexcept;

} // namespace Game
