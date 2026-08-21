#pragma once

#include "EconomyDef.h"
#include "Ids.h"

#include <cstdint>

/*
 * A mining field as the tick sees it (ADR-024 §3d, §4b, §4c, build order E2).
 *
 * The bake authors a site's *pool*; the ledger at the universe layer holds what
 * is left of it; and this is the shape both of them take on a live grid: a
 * handful of **clusters**, each at a local offset with its own remaining units
 * per ore. A Mine order names a cluster, a cycle debits one, and a cluster that
 * runs out ends the order.
 *
 * **No rock is an entity** (ADR-024 §4c). The sim tracks `(site, cluster, ore)`
 * and nothing finer; the client derives the visible rocks from the same salt
 * this file splits by, so both halves draw and count the same field without a
 * byte of it crossing the wire. Per-rock entities would have spent the u16
 * occupant window three times over and put every pebble in the world hash.
 *
 * **Per-tick code, deliberately.** Nothing here names a universe coordinate or
 * the universe definition: a cluster is a *local* offset in the grid's own
 * centimetres, which is the same line `Anchor::warpInPoint` sits on. Where the
 * field's grid sits on the plane, and which epoch it belongs to, is
 * `SiteEpoch`'s -- spin-up work and map work, never the tick's (ADR-009 §2).
 *
 * **Integers throughout, and no float anywhere near a pool.** Two machines that
 * disagreed by an ulp about how much ore a cluster holds would disagree about
 * when it emptied, which is a divergence a replay cannot recover from.
 */

namespace Game
{

/*
 * The tick rate, as an integer.
 *
 * `World::TICK_SECONDS` is the same rate as a float and is what `Integrate`
 * multiplies by; this is what *durations* are converted with, for the reason
 * `GATE_JUMP_TICKS` is written in ticks (ADR-016 §5): `40 / 0.05f` is not 800
 * on every path that evaluates it, and a mining cycle one tick short on one
 * machine is a hold that fills a tick early on it. Every seconds-to-ticks
 * conversion in this phase is a multiplication by this.
 *
 * It lives here rather than beside `TICK_SECONDS` because this is the lowest
 * header that needs it -- `World.h` includes this one, not the other way round
 * -- and `World.h` asserts the two agree so they cannot drift apart.
 */
inline constexpr std::uint32_t TICKS_PER_SECOND = 20;

/*
 * The most clusters a field may hold.
 *
 * `SitesInfo::clusterCountMax` is content and this is the storage it has to fit
 * in, so `BuildSiteField` clamps rather than trusting the file -- the same
 * arrangement `MAX_REFINE_BATCHES` has. Twelve is ADR-024 §4b's own upper
 * bound, and the array is inline because a field is world state and world state
 * does not allocate per tick.
 */
inline constexpr std::uint8_t MAX_SITE_CLUSTERS = 12;

/*
 * How many radiation stacks one ship may carry (ADR-024 §3b).
 *
 * A bound rather than a balance number: stacks accrue one per cycle and shed
 * only outside the field, so nothing in the design stops them climbing, and an
 * unbounded count would eventually overflow the cycle-time arithmetic that
 * multiplies by it. 255 is far past the ~75 cycles it takes to fill the largest
 * ore hold, so no session reaches it by mining -- it exists so that one that
 * somehow does is slow rather than wrong.
 */
inline constexpr std::uint8_t RADIATION_STACK_CAP = 255;

/*
 * One cluster: where it is on the grid, and what is left in it.
 *
 * Centimetres in the grid's local frame, which is the frame an `OrderLeg`
 * carries and the frame the renderer draws -- so the leg that sends a wing to a
 * cluster is the cluster's own number rather than a conversion of it.
 */
struct SiteCluster
{
  std::int32_t xCm = 0;
  std::int32_t yCm = 0;

  /// Indexed by `OreId`. Zero means this cluster never held that ore or has
  /// been emptied of it, and the two are the same fact to everything that asks.
  std::uint32_t remainingUnits[ORE_COUNT] = {};

  [[nodiscard]] std::uint32_t Remaining(OreId _ore) const noexcept
  {
    return remainingUnits[static_cast<std::uint8_t>(_ore)];
  }

  [[nodiscard]] std::uint32_t TotalRemaining() const noexcept;
};

/*
 * The whole field.
 *
 * `grade` is zero on a grid that is not a site, which is what "is there a field
 * here?" is answered by -- the same arrangement `SiteSpec::grade` has in the
 * bake, carried through so the two agree on the sentinel.
 */
struct SiteField
{
  SiteArchetype archetype = SiteArchetype::FerrousField;

  /// 1..5, and 0 for "this grid has no field".
  std::uint8_t grade = 0;

  /// The rocks' extent from the grid origin. What decides whether a ship is
  /// *in* the field, which is what radiation decay and the client's fog both
  /// hang off.
  std::int32_t fieldRadiusCm = 0;

  std::uint8_t clusterCount = 0;
  SiteCluster clusters[MAX_SITE_CLUSTERS];

  [[nodiscard]] bool Exists() const noexcept { return grade != 0 && clusterCount != 0; }

  [[nodiscard]] std::uint32_t Remaining(OreId _ore) const noexcept;
  [[nodiscard]] std::uint32_t TotalRemaining() const noexcept;
};

/*
 * What is left of a field, as the *universe layer* holds it (ADR-024 §3d,
 * ADR-025 §2).
 *
 * The durable twin of `SiteField`, and deliberately narrower: the bake's pools
 * are the pristine truth and this is what has been taken out of them since the
 * last epoch. Cluster positions are absent because they are derivable -- the
 * salt rebuilds them -- and a durable record should hold what cannot be
 * recomputed and nothing else. That is also the record ADR-025's journal
 * writes, so this shape is the one persistence inherits rather than a second
 * one to translate into.
 *
 * **Worlds forget; ledgers do not.** A site's grid can tear down with a
 * half-eaten field and lose nothing, because the number was never the world's
 * to begin with -- it is here, beside the station rosters, folded into the
 * registry's hash the same way.
 */
struct SiteLedger
{
  AnchorId anchor = INVALID_ID;

  /// Which epoch these units belong to. A ledger whose epoch has passed is not
  /// state, it is history: the pool it describes has already been refilled, and
  /// the next spin-up is what notices.
  std::uint32_t epochIndex = 0;

  /// The salt that laid this epoch's clusters out, so the field can be rebuilt
  /// from the bake without asking the universe again.
  std::uint32_t layoutSalt = 0;

  std::uint8_t clusterCount = 0;
  std::uint32_t remainingUnits[MAX_SITE_CLUSTERS][ORE_COUNT] = {};

  [[nodiscard]] std::uint32_t TotalRemaining() const noexcept;
};

/*
 * Splits a pool into clusters, deterministically, from the epoch's salt.
 *
 * Called at spin-up with the *pristine* pool and again -- with the same
 * arguments -- by anything that wants to know what the field looked like when
 * it formed. It is a pure function of its arguments, so the client derives the
 * same clusters the sim counts against without either replicating them
 * (ADR-024 §4c), and the epoch reshuffles the layout for free because the salt
 * is the only thing that changes.
 *
 * **The order of the draws is the contract.** Cluster count first, then every
 * centre, then the per-ore splits in `OreId` order -- stated because two builds
 * that drew the same numbers in a different sequence would build different
 * fields from the same seed, and nothing else would notice until they disagreed
 * about which cluster was richest.
 *
 * The split is exact: the units handed to the clusters sum to the pool, with
 * the rounding remainder given out one unit at a time by descending fractional
 * part and then by cluster index. A pool that leaked a unit to rounding would
 * be a field that could never be finished.
 */
[[nodiscard]] SiteField BuildSiteField(const SitesInfo& _sites, SiteArchetype _archetype, std::uint8_t _grade,
                                       std::int32_t _fieldRadiusCm, std::uint32_t _layoutSalt,
                                       const std::uint32_t (&_poolUnits)[ORE_COUNT]) noexcept;

/*
 * The richest cluster this filter can work, or `MAX_SITE_CLUSTERS` for none.
 *
 * "Richest" is measured in the ore the filter asked for, and in *everything*
 * when it asked for `Any` -- which is the honest reading of a commander who did
 * not care. Ties go to the lower index so that two machines picking from an
 * identical field pick the same cluster.
 */
[[nodiscard]] std::uint8_t RichestCluster(const SiteField& _field, OreFilter _filter) noexcept;

/*
 * Which ore a cycle against this cluster takes, or false when it holds nothing
 * the filter accepts.
 *
 * Re-asked every cycle rather than fixed when the order was accepted, and the
 * consequence is worth stating: an `Any` order takes whatever is most plentiful
 * *right now*, so it works a cluster down evenly rather than exhausting one ore
 * and moving on. A filter resolved once at submit would instead keep mining an
 * ore that ran out ten cycles ago.
 */
[[nodiscard]] bool ClusterOre(const SiteField& _field, std::uint8_t _cluster, OreFilter _filter,
                              OreId& _outOre) noexcept;

/// Writes what a field has left into a ledger, and reads it back out. Two
/// functions rather than a shared representation, because the two shapes differ
/// on purpose -- the field carries positions the ledger has no business
/// storing, and a struct that served both would store them durably.
void StoreSiteField(const SiteField& _field, SiteLedger& _outLedger) noexcept;
void ApplySiteLedger(const SiteLedger& _ledger, SiteField& _field) noexcept;

/// The hazard this field runs, which is its archetype's. Asked rather than
/// switched on at each call site, so a fourth archetype is one edit.
[[nodiscard]] HazardKind FieldHazard(const EconomyDef& _economy, const SiteField& _field) noexcept;

/*
 * How long one cycle takes, in ticks, for a miner carrying this many radiation
 * stacks (ADR-024 §3b).
 *
 * The slow is computed over the **total** stacks rather than accumulated per
 * stack, and that is arithmetic rather than taste: `3% * hazardScale` floors to
 * 1% on a grade-I belt and to 4% on a grade-II, so a per-stack floor would make
 * the gentlest field bite harder per stack than the design says. One
 * multiplication, one division, one floor.
 *
 * Zero stacks on a field with no radiation is the base cycle, which is what
 * every ferrous field and every nebula pocket pays.
 */
[[nodiscard]] std::uint32_t MiningCycleTicks(const EconomyDef& _economy, const SiteField& _field,
                                             std::uint8_t _radiationStacks) noexcept;

/// The heat one completed cycle adds, scaled by grade. Zero away from a nebula.
[[nodiscard]] std::uint16_t CycleHeat(const EconomyDef& _economy, const SiteField& _field) noexcept;

/*
 * The detection-range factor this field imposes, as a whole percentage, and
 * **100 where nothing dampens** (ADR-024 §3b).
 *
 * Already per-grade in the content, so it is *not* scaled again by
 * `hazardScalePct` -- the table's five numbers are the five grades, and
 * multiplying them by the grade a second time would dampen a grade-V pocket to
 * nothing.
 *
 * It dampens everyone on the grid, which is the design: the ganker closes
 * unseen and also hunts blind. ADR-022's relevance seam is what reads it; until
 * that seam exists this is the number it will read, replicated rather than
 * re-derived so both halves agree about how far anyone can see.
 */
[[nodiscard]] std::uint8_t SensorDampingPct(const EconomyDef& _economy, const SiteField& _field) noexcept;

} // namespace Game
