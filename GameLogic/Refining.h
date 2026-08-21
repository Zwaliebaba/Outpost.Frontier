#pragma once

#include "EconomyDef.h"
#include "Ids.h"

#include "Wire.h"

#include <cstdint>

/*
 * Refining: what a job costs, makes and takes (ADR-024 §6, build order E4b).
 *
 * The arithmetic half, pure and integer-only, for the reason every other
 * number in this economy is: a refinery is a **ledger**, and two machines that
 * disagreed about a rounding would disagree about somebody's property. There
 * is no RNG anywhere in this file and there is deliberately nowhere to put one
 * -- batching is the deterministic economy's answer to yield variance
 * (ADR-024 §6b), so the refund is exact, floored per material, and the books
 * add up by construction rather than on average.
 *
 * The job *records* live at the universe layer beside the Bays and the site
 * ledgers, on the same terms: worlds forget, ledgers do not, and a refinery
 * that stopped when its station's grid tore down would be a refinery nobody
 * could walk away from.
 */

namespace Game
{

/*
 * What one job will cost, produce, refund and take -- decided **once, at
 * submission**, and then carried.
 *
 * Not recomputed at completion, and that is a ruling rather than an
 * optimisation: the print shows this arithmetic before the player commits
 * (D-P3), so the numbers they agreed to are the numbers they get. A station
 * whose tier rose while a job ran does not retroactively improve it, and one
 * that could not have fallen anyway. It also makes the durable record
 * self-contained: a reloaded job needs no tier table to know what it owes.
 */
struct RefinePlan
{
  /// False when the recipe, the batch size or the tier does not exist. Every
  /// other field is then zero and means nothing.
  bool valid = false;

  /// Debited from the Bay at submission (ADR-024 §6b).
  std::uint32_t inputUnits[ORE_COUNT] = {};

  /// Credited back at completion, floored per material. Zero at tier 1, which
  /// the print still shows -- "TIER 2 WOULD RETURN 5" is the upgrade pitch.
  std::uint32_t refundUnits[ORE_COUNT] = {};

  /// Alloy units credited at completion: the batch size, exactly. One unit of
  /// output per unit of batch is the recipe sheet's own shape (§6a).
  std::uint32_t outputUnits = 0;

  /// How long it runs, in ticks. The tick is the only clock (ADR-002 §1), so
  /// this is where seconds stop being the unit.
  std::uint32_t durationTicks = 0;

  /// Station fuel, read and honoured and zero in the content until a market
  /// exists to sell Ionized Slurry (ADR-024 §6d).
  std::uint32_t fuelUnits = 0;

  /// What is actually consumed: inputs minus refund. Carried rather than
  /// recomputed because it is the number the faucet/sink ledger (R24) wants
  /// and the one a test asserts the books against.
  [[nodiscard]] std::uint32_t ConsumedUnits(OreId _ore) const noexcept;
};

/*
 * The batch the content authors for this size, or null.
 *
 * Sizes are **authored and closed**, not arbitrary: 1, 10 and 50 today. A
 * player asking for 37 is refused rather than quietly rounded, because a
 * silently-adjusted batch is a different job from the one the form priced.
 */
[[nodiscard]] const RefineBatchInfo* FindRefineBatch(const RefiningInfo& _refining, std::uint32_t _batchUnits) noexcept;

/*
 * Prices a job (ADR-024 §6a, §6b, §6c).
 *
 * **The order of the arithmetic is the contract**, stated because two builds
 * that applied the same three integer factors in a different sequence would
 * price the same job differently and nothing else would notice:
 *
 *   seconds = refineSeconds x batchUnits, then x the batch's timePct / 100,
 *   then x the tier's timePct / 100; ticks = seconds x TICKS_PER_SECOND.
 *
 * **Both discounts apply.** ADR-024 §6b's worked example ("22.5 minutes" for a
 * 50-batch of Plates at T3) reads as though only the batch factor did, and
 * that is a slip in the prose rather than in the design: §6a's units-per-slot
 * rates are undiscounted, and a tier `timePct` that never multiplied anything
 * would be authored content nothing reads. See the note in §6b.
 *
 * `_tier` is the station's, 1-based, and the recipe's own tier is **not**
 * checked here -- that is `RecipeLocked`, a validation answer with a reason
 * code, and this function prices what it is asked to price.
 */
[[nodiscard]] RefinePlan PlanRefine(const EconomyDef& _economy, AlloyId _alloy, std::uint32_t _batchUnits,
                                    std::uint8_t _tier) noexcept;

/*
 * The highest tier a station in this band may ever reach (ADR-024 §6c).
 *
 * The industrial map in one function: **no High-Sec station can ever cook
 * Nova-Steel**, so endgame industry runs ore into the dark and alloys out of
 * it, and both legs are escortable and interdictable when combat arrives.
 */
[[nodiscard]] std::uint8_t BandTierCap(const EconomyDef& _economy, SecurityBand _band) noexcept;

/*
 * May a station at this tier cook this alloy?
 *
 * The tier's `recipeTierCap` against the alloy's own tier, which is the whole
 * of the rule -- and the reason the print never hides a locked row: a lock is
 * either a project on the same screen or a reason to fly somewhere else, and
 * both are things a player should be able to see.
 */
[[nodiscard]] bool TierCooks(const EconomyDef& _economy, std::uint8_t _tier, AlloyId _alloy) noexcept;

/*
 * The authored floor for a station's refinery (ADR-024 §6c).
 *
 * T1 everywhere, T2 in the starter system so the on-ramp reaches Quantum
 * Matrix at home. Derived rather than baked, which keeps it out of the anchor
 * table and out of `universeHash`: a station's *current* tier is durable state
 * only once somebody has raised it, exactly as a site ledger exists only once
 * somebody has mined it.
 */
[[nodiscard]] std::uint8_t AuthoredStationTier(bool _isStarterSystem) noexcept;

/*
 * One job as the universe layer holds it (ADR-024 §6b).
 *
 * Per `(owner, station)` and never per station: slots are the player's, so
 * there is no station traffic to browse, no queue-jumping to design, and
 * `RefineryBusy` is always about your own ten (D-P3). The privacy is in the
 * key, exactly as it is for a Bay.
 */
struct RefineJob
{
  Neuron::PlayerId owner = Neuron::INVALID_PLAYER_ID;
  AnchorId station = INVALID_ID;
  AlloyId alloy = AlloyId::FerrocitePlates;

  /// Filing order within `(owner, station)`. The queue drains in it, and it is
  /// what makes "in order" a fact about the record rather than about how the
  /// vector happened to be sorted.
  std::uint32_t sequence = 0;

  std::uint32_t batchUnits = 0;

  /// Priced at submission and carried (see `RefinePlan`).
  std::uint32_t inputUnits[ORE_COUNT] = {};
  std::uint32_t refundUnits[ORE_COUNT] = {};
  std::uint32_t outputUnits = 0;
  std::uint32_t durationTicks = 0;

  /*
   * The tick it finishes at, or zero while it is still queued.
   *
   * Zero *is* the queued state rather than a separate flag: a job has started
   * exactly when it knows when it ends, and two fields that could disagree
   * about that would eventually disagree about it.
   */
  std::uint32_t completeTick = 0;

  [[nodiscard]] bool Running() const noexcept { return completeTick != 0; }
};

/*
 * A station's communal upgrade project (ADR-024 §6c).
 *
 * **Communal and irreversible**: no player owns a station, anyone may invest
 * alloys, and when the last unit lands the tier rises permanently for
 * everyone. So there is no owner in the key -- unlike a Bay, this is
 * deliberately everybody's -- and D19's event record is where "who built it"
 * lives, because a running total cannot answer that and a history in the
 * durable set would grow without bound.
 */
struct UpgradeProject
{
  AnchorId station = INVALID_ID;

  /// Which tier this project buys. A station has at most one open project --
  /// the next one -- so this is a fact about the station rather than a key.
  std::uint8_t toTier = 0;

  std::uint32_t contributedUnits[ALLOY_COUNT] = {};

  /// Is every authored cost met? The completion test, in one place, because
  /// "exactly once" is a property of asking the same question the same way.
  [[nodiscard]] bool IsComplete(const RefineryUpgradeProject& _cost) const noexcept;
};

/// A station whose refinery has been raised above its authored floor. Present
/// only once somebody has raised it -- the site ledger's discipline, applied
/// to the one other thing this phase makes permanent.
struct StationTier
{
  AnchorId station = INVALID_ID;
  std::uint8_t tier = 0;
};

} // namespace Game
