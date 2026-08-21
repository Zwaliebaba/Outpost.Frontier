#include "pch.h"

#include "CppUnitTest.h"

#include "DurableState.h"
#include "EconomyParse.h"
#include "Refining.h"
#include "Station.h"
#include "StationMessages.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "World.h"
#include "WorldRegistry.h"

#include "ByteReader.h"
#include "ByteWriter.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * E4b: refining, tiers and the projects (ADR-024 §6, build order E4b).
 *
 * The claim is that **a refinery is a ledger**. Everything here is one of three
 * questions about that: does the arithmetic add up at every batch size and
 * tier, do the two locks (a tier and a band) refuse what they say they refuse,
 * and does a communal project complete exactly once however many people are
 * pushing on it.
 *
 * No test here draws a random number, because nothing in the subject does
 * (§6b): batching is the deterministic economy's answer to yield variance, so
 * a refund is exact rather than expected.
 */

namespace GameLogicTests
{

namespace
{

[[nodiscard]] const EconomyDef& Economy()
{
  static const EconomyDef economy = []
  {
    static constexpr const char* CANDIDATES[] = {
      "GameData/Economy/Economy.json",
      "../GameData/Economy/Economy.json",
      "../../GameData/Economy/Economy.json",
      "../../../GameData/Economy/Economy.json",
      "../../../../GameData/Economy/Economy.json",
    };
    for (const char* candidate : CANDIDATES)
    {
      std::ifstream file(candidate, std::ios::binary);
      if (!file)
      {
        continue;
      }
      std::ostringstream buffer;
      buffer << file.rdbuf();

      EconomyDef parsed;
      std::vector<EconomyDiagnostic> diagnostics;
      Assert::IsTrue(ParseEconomy(buffer.str(), parsed, diagnostics), L"the committed economy does not parse");
      return parsed;
    }
    Assert::Fail(L"the committed Economy.json was not found from the test working directory");
    return EconomyDef{};
  }();
  return economy;
}

/// A universe big enough to hold stations in more than one security band,
/// which is what the industrial map's tests are about.
[[nodiscard]] const UniverseDef& Universe()
{
  static UniverseDef universe;
  static bool baked = false;
  if (!baked)
  {
    baked = true;
    UniverseGenConfig recipe;
    recipe.regionCount = 3;
    recipe.constellationsPerRegion = 2;
    recipe.systemCount = 18;
    (void)GenerateUniverse(recipe, Economy().sites, universe);
  }
  return universe;
}

void MakeRegistry(WorldRegistry& _registry)
{
  RegistryConfig config;
  config.sessionSeed = 0xE4B;
  _registry.Reset(&Universe(), &Economy(), config);
}

/*
 * The first station the bake placed in a given band, **skipping the starter**.
 *
 * The starter station is authored a tier above the rest (ADR-024 §6c, so the
 * on-ramp reaches Quantum Matrix at home), which in High-Sec is already its
 * band's cap -- a fine station to play at and the wrong one to test an upgrade
 * project against, since it has nothing left to build.
 */
[[nodiscard]] AnchorId StationInBand(SecurityBand _band)
{
  const AnchorId starter = Universe().StartAnchorId();
  for (const SolarSystem& system : Universe().systems)
  {
    if (Economy().BandFor(system.security) != _band)
    {
      continue;
    }
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind == AnchorKind::Station && anchor.id != starter)
      {
        return anchor.id;
      }
    }
  }
  return INVALID_ID;
}

/// Puts ore straight into a commander's Bay. The transfer path is E3's and has
/// its own suite; what this file is about starts once the ore is committed.
void StockBay(WorldRegistry& _registry, Neuron::PlayerId _owner, AnchorId _station, std::uint32_t _units)
{
  DurableState state;
  CaptureDurableState(_registry, state);
  StationBay bay;
  bay.owner = _owner;
  bay.station = _station;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    bay.oreUnits[ore] = _units;
  }
  state.bays.push_back(bay);

  WorldRegistry replacement;
  std::vector<PersistenceDiagnostic> diagnostics;
  MakeRegistry(_registry);
  Assert::IsTrue(_registry.LoadDurable(state, diagnostics), L"the fixture could not stock a Bay");
}

/// The same, for alloys, so the project tests do not have to cook two and a
/// half thousand Plates to get to the question they are about.
void StockBayAlloys(WorldRegistry& _registry, std::span<const Neuron::PlayerId> _owners, AnchorId _station, std::uint32_t _units)
{
  DurableState state;
  CaptureDurableState(_registry, state);
  for (const Neuron::PlayerId owner : _owners)
  {
    StationBay bay;
    bay.owner = owner;
    bay.station = _station;
    for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
    {
      bay.alloyUnits[alloy] = _units;
    }
    state.bays.push_back(bay);
  }
  std::sort(state.bays.begin(), state.bays.end(), [](const StationBay& _a, const StationBay& _b)
            { return _a.station != _b.station ? _a.station < _b.station : _a.owner < _b.owner; });

  std::vector<PersistenceDiagnostic> diagnostics;
  MakeRegistry(_registry);
  Assert::IsTrue(_registry.LoadDurable(state, diagnostics), L"the fixture could not stock a Bay with alloys");
}

[[nodiscard]] StationCommand Contribute(AnchorId _station, AlloyId _alloy, std::uint32_t _units)
{
  StationCommand command;
  command.orderSeq = 2;
  command.verb = StationVerb::ProjectContribute;
  command.station = _station;
  command.alloy = _alloy;
  command.units = _units;
  return command;
}

[[nodiscard]] StationCommand Start(AnchorId _station, AlloyId _alloy, std::uint32_t _batch)
{
  StationCommand command;
  command.orderSeq = 1;
  command.verb = StationVerb::RefineStart;
  command.station = _station;
  command.alloy = _alloy;
  command.units = _batch;
  return command;
}

constexpr Neuron::PlayerId ONE = 1;
constexpr Neuron::PlayerId TWO = 2;

} // namespace

TEST_CLASS(RefineArithmeticTests)
{
public:
  TEST_METHOD(TheBooksBalanceAtEveryBatchSizeAndEveryTier)
  {
    /*
     * The property the whole slice rests on, asserted over the entire cross
     * product rather than at a sample: for every recipe, batch and tier, the
     * inputs are the recipe's rate times the batch, the refund is that floored
     * by the tier's percentage, and what is consumed is the difference. A
     * refinery that created or destroyed a unit anywhere in that grid would be
     * a faucet or a sink nobody designed.
     */
    const EconomyDef& economy = Economy();
    for (std::uint8_t alloyIndex = 0; alloyIndex < ALLOY_COUNT; ++alloyIndex)
    {
      const auto alloy = static_cast<AlloyId>(alloyIndex);
      const AlloyInfo& recipe = economy.Alloy(alloy);
      for (std::uint8_t batchIndex = 0; batchIndex < economy.refining.batchCount; ++batchIndex)
      {
        const std::uint32_t batch = economy.refining.batches[batchIndex].units;
        for (std::uint8_t tier = 1; tier <= REFINERY_TIER_COUNT; ++tier)
        {
          const RefinePlan plan = PlanRefine(economy, alloy, batch, tier);
          Assert::IsTrue(plan.valid, L"an authored recipe, batch and tier did not price");
          Assert::AreEqual<std::uint32_t>(batch, plan.outputUnits, L"a batch did not produce its own size");

          for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
          {
            const auto oreId = static_cast<OreId>(ore);
            const std::uint32_t expectedInput = static_cast<std::uint32_t>(recipe.Input(oreId)) * batch;
            Assert::AreEqual<std::uint32_t>(expectedInput, plan.inputUnits[ore], L"the inputs are not the rate times the batch");

            const std::uint32_t expectedRefund = expectedInput * economy.Tier(tier).refundPct / 100u;
            Assert::AreEqual<std::uint32_t>(expectedRefund, plan.refundUnits[ore], L"the refund is not the floored percentage");
            Assert::IsTrue(plan.refundUnits[ore] <= plan.inputUnits[ore], L"a refund exceeded what was put in");
            Assert::AreEqual<std::uint32_t>(expectedInput - expectedRefund, plan.ConsumedUnits(oreId),
                                            L"consumed is not inputs minus refund");
          }
        }
      }
    }
  }

  TEST_METHOD(TheAdrsWorkedExampleAddsUpExceptForTheTierDiscount)
  {
    /*
     * ADR-024 §6b's own worked example, checked: "at a T3 station (ME 10%), a
     * 50-batch of Plates consumes 100 FC, refunds 10 FC at completion".
     *
     * The materials are exact. The **time is not** -- the ADR says 22.5 minutes,
     * which is 1,500 seconds with the batch factor applied and the *tier*
     * factor forgotten. Both are authored and both multiply here (§6a's rates
     * are undiscounted, and a tier `timePct` that never multiplied anything
     * would be content nothing reads), so the honest number is 20.25 minutes.
     * The ADR carries the correction; this asserts the arithmetic either way.
     */
    const EconomyDef& economy = Economy();
    const RefinePlan plan = PlanRefine(economy, AlloyId::FerrocitePlates, 50, 3);
    Assert::IsTrue(plan.valid);
    Assert::AreEqual<std::uint32_t>(100, plan.inputUnits[static_cast<std::uint8_t>(OreId::FerroChroma)],
                                    L"a 50-batch of Plates does not consume 100 Ferro-Chroma");
    Assert::AreEqual<std::uint32_t>(10, plan.refundUnits[static_cast<std::uint8_t>(OreId::FerroChroma)],
                                    L"a T3 station does not refund 10 %");
    Assert::AreEqual<std::uint32_t>(90, plan.ConsumedUnits(OreId::FerroChroma), L"the books do not balance at 100 minus 10");

    // 30 s x 50 = 1500, x 90 % (the batch) x 90 % (the tier) = 1215 s = 24,300
    // ticks at 20 Hz.
    Assert::AreEqual<std::uint32_t>(24300, plan.durationTicks, L"the time is not both discounts applied in order");
  }

  TEST_METHOD(ASingleBatchWastesNothingAndEarnsNothing)
  {
    // §6b's own line about batch 1: "tier ME x 1 unit rounds to 0 -- singles
    // waste nothing but earn nothing". The floor is what makes that true, and
    // it is worth pinning because a rounding that went the other way would mint
    // ore out of nowhere on every single-unit job.
    const RefinePlan plan = PlanRefine(Economy(), AlloyId::FerrocitePlates, 1, 4);
    Assert::IsTrue(plan.valid);
    Assert::AreEqual<std::uint32_t>(2, plan.inputUnits[static_cast<std::uint8_t>(OreId::FerroChroma)]);
    Assert::AreEqual<std::uint32_t>(0, plan.refundUnits[static_cast<std::uint8_t>(OreId::FerroChroma)],
                                    L"a single-unit batch minted a refund out of a rounding");
  }

  TEST_METHOD(ABatchSizeNobodyAuthoredHasNoPrice)
  {
    // Refused rather than rounded to the nearest authored size: a silently
    // adjusted batch is a different job from the one the form priced.
    Assert::IsFalse(PlanRefine(Economy(), AlloyId::FerrocitePlates, 37, 2).valid, L"an unauthored batch size was priced");
    Assert::IsFalse(PlanRefine(Economy(), AlloyId::FerrocitePlates, 0, 2).valid, L"a zero batch was priced");
    Assert::IsFalse(PlanRefine(Economy(), AlloyId::FerrocitePlates, 10, 0).valid, L"a tier-zero station priced a job");
  }

  TEST_METHOD(NoHighSecStationCanEverCookNovaSteel)
  {
    /*
     * The industrial map as arithmetic (ADR-024 §6c). High-Sec caps at tier 2,
     * tier 2 caps recipes at tier 2, and Nova-Steel is tier 3 -- so the answer
     * is no at *every* tier a High-Sec station could ever reach, which is a
     * stronger statement than "no at the tier it has today".
     */
    const EconomyDef& economy = Economy();
    const std::uint8_t highCap = BandTierCap(economy, SecurityBand::High);
    Assert::IsTrue(highCap > 0, L"the content authors no High-Sec cap at all");
    for (std::uint8_t tier = 1; tier <= highCap; ++tier)
    {
      Assert::IsFalse(TierCooks(economy, tier, AlloyId::NovaSteel), L"a tier a High-Sec station can reach cooks Nova-Steel");
    }
    // And somewhere it is possible, or the recipe would be unreachable content.
    Assert::IsTrue(TierCooks(economy, BandTierCap(economy, SecurityBand::Null), AlloyId::NovaSteel),
                   L"Nova-Steel cannot be cooked anywhere");
  }
};

TEST_CLASS(RefineJobTests)
{
public:
  TEST_METHOD(AJobDebitsAtSubmissionAndCreditsAtCompletion)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    Assert::AreNotEqual<std::uint16_t>(INVALID_ID, station, L"the bake has no High-Sec station");
    StockBay(registry, ONE, station, 1000);

    const std::uint8_t tier = registry.TierAt(station);
    const RefinePlan plan = PlanRefine(Economy(), AlloyId::FerrocitePlates, 10, tier);
    const std::uint32_t before = registry.Bay(ONE, station)->Units(OreId::FerroChroma);

    Assert::IsTrue(registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 10)).accepted,
                   L"a legal refine job was refused");
    Assert::AreEqual<std::uint32_t>(before - plan.inputUnits[0], registry.Bay(ONE, station)->Units(OreId::FerroChroma),
                                    L"the inputs did not debit at submission");
    Assert::AreEqual<std::size_t>(1, registry.RefineJobs().size(), L"the job is not on the books");
    Assert::IsTrue(registry.RefineJobs()[0].Running(), L"a job submitted into an empty refinery did not start");

    const std::uint32_t completeAt = registry.RefineJobs()[0].completeTick;
    for (std::uint32_t tick = 1; tick <= completeAt; ++tick)
    {
      registry.Tick(tick);
    }
    Assert::AreEqual<std::size_t>(0, registry.RefineJobs().size(), L"the job never finished");
    Assert::AreEqual<std::uint32_t>(10, registry.Bay(ONE, station)->Units(AlloyId::FerrocitePlates),
                                    L"the alloy did not credit at completion");
    Assert::AreEqual<std::uint32_t>(before - plan.ConsumedUnits(OreId::FerroChroma),
                                    registry.Bay(ONE, station)->Units(OreId::FerroChroma),
                                    L"the refund did not credit at completion");
  }

  TEST_METHOD(AJobRunsWhileItsStationsGridIsNotEvenLive)
  {
    /*
     * G1's actual claim, one step short of the restart: a refinery is
     * universe-layer work on the shard-global tick, so it advances with no grid
     * spun up, nobody watching and nobody docked. A refinery that needed a live
     * grid would be a refinery nobody could walk away from, and walking away is
     * the feature (ADR-024 §6b).
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    StockBay(registry, ONE, station, 1000);
    Assert::IsTrue(registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 1)).accepted);

    const std::uint32_t completeAt = registry.RefineJobs()[0].completeTick;
    for (std::uint32_t tick = 1; tick <= completeAt; ++tick)
    {
      registry.Tick(tick);
      Assert::AreEqual<std::uint32_t>(0, registry.LiveWorldCount(), L"the fixture spun a grid up and stopped proving anything");
    }
    Assert::AreEqual<std::uint32_t>(1, registry.Bay(ONE, station)->Units(AlloyId::FerrocitePlates),
                                    L"a job did not finish without a grid");
  }

  TEST_METHOD(SlotsFillAndTheQueueDrainsInOrder)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    StockBay(registry, ONE, station, 100000);

    const std::uint8_t tier = registry.TierAt(station);
    const std::uint32_t slots = Economy().Tier(tier).slotsPerPlayer;
    const std::uint32_t depth = Economy().refining.queueDepthPerPlayer;

    for (std::uint32_t index = 0; index < slots + depth; ++index)
    {
      Assert::IsTrue(registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 1)).accepted,
                     L"a job inside the slots and the queue was refused");
    }
    const OrderVerdict full = registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 1));
    Assert::IsFalse(full.accepted, L"a job past the queue depth was accepted");
    Assert::IsTrue(full.reason == OrderReason::RefineryBusy, L"a full queue did not say RefineryBusy");

    std::uint32_t running = 0;
    for (const RefineJob& job : registry.RefineJobs())
    {
      running += job.Running() ? 1u : 0u;
    }
    Assert::AreEqual<std::uint32_t>(slots, running, L"more or fewer jobs are running than there are slots");

    // And the queue drains in filing order: the lowest sequence is always the
    // next to start, which is what makes "in order" a fact about the record.
    std::uint32_t highestRunning = 0;
    std::uint32_t lowestQueued = 0xffffffffu;
    for (const RefineJob& job : registry.RefineJobs())
    {
      if (job.Running())
      {
        highestRunning = std::max(highestRunning, job.sequence);
      }
      else
      {
        lowestQueued = std::min(lowestQueued, job.sequence);
      }
    }
    Assert::IsTrue(highestRunning < lowestQueued, L"a later job started before an earlier one");
  }

  TEST_METHOD(AQueuedJobCancelsWholeAndARunningOneCannot)
  {
    /*
     * The owner's ruling on D-P3's first open question (2026-08-21): a queued
     * job cancels whole and its inputs return to the Bay untouched; a running
     * job cannot be cancelled, its inputs being spent. Nothing is created or
     * destroyed by a cancel, which is the property asserted rather than the
     * verb's existence.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    StockBay(registry, ONE, station, 100000);

    const std::uint32_t slots = Economy().Tier(registry.TierAt(station)).slotsPerPlayer;
    for (std::uint32_t index = 0; index < slots + 1; ++index)
    {
      Assert::IsTrue(registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 10)).accepted);
    }

    std::uint32_t queued = 0;
    std::uint32_t started = 0;
    for (const RefineJob& job : registry.RefineJobs())
    {
      (job.Running() ? started : queued) = job.sequence;
    }

    StationCommand cancel;
    cancel.orderSeq = 9;
    cancel.verb = StationVerb::RefineCancel;
    cancel.station = station;

    const std::uint32_t bayBefore = registry.Bay(ONE, station)->Units(OreId::FerroChroma);
    const std::uint32_t inputs = PlanRefine(Economy(), AlloyId::FerrocitePlates, 10, registry.TierAt(station)).inputUnits[0];

    cancel.sequence = started;
    const OrderVerdict running = registry.SubmitStationCommand(ONE, cancel);
    Assert::IsFalse(running.accepted, L"a running job was cancelled");
    Assert::IsTrue(running.reason == OrderReason::RefineryBusy, L"cancelling a running job did not say RefineryBusy");
    Assert::AreEqual<std::uint32_t>(bayBefore, registry.Bay(ONE, station)->Units(OreId::FerroChroma),
                                    L"a refused cancel moved ore anyway");

    cancel.sequence = queued;
    Assert::IsTrue(registry.SubmitStationCommand(ONE, cancel).accepted, L"a queued job would not cancel");
    Assert::AreEqual<std::uint32_t>(bayBefore + inputs, registry.Bay(ONE, station)->Units(OreId::FerroChroma),
                                    L"a cancelled job did not return exactly what it took");
  }

  TEST_METHOD(ARecipeThisStationCannotCookIsRefusedRecipeLocked)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    StockBay(registry, ONE, station, 100000);

    const OrderVerdict locked = registry.SubmitStationCommand(ONE, Start(station, AlloyId::NovaSteel, 1));
    Assert::IsFalse(locked.accepted, L"a High-Sec station cooked Nova-Steel");
    Assert::IsTrue(locked.reason == OrderReason::RecipeLocked, L"a band cap did not say RecipeLocked");
  }

  TEST_METHOD(AnEmptyBayCannotStartAJob)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    const OrderVerdict broke = registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 10));
    Assert::IsFalse(broke.accepted, L"a job started with nothing to make it from");
    Assert::IsTrue(broke.reason == OrderReason::InsufficientMaterials, L"an empty Bay did not say InsufficientMaterials");
    Assert::AreEqual<std::size_t>(0, registry.RefineJobs().size(), L"a refused job was filed anyway");
  }

  TEST_METHOD(TwoCommandersJobsAreEachOthersBusiness)
  {
    // Slots are the player's, not the station's (ADR-024 §6c): one commander
    // filling their ten must not touch another's, which is the same privacy the
    // roster and the Bay already keep.
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    StockBay(registry, ONE, station, 100000);

    const std::uint32_t slots = Economy().Tier(registry.TierAt(station)).slotsPerPlayer;
    const std::uint32_t depth = Economy().refining.queueDepthPerPlayer;
    for (std::uint32_t index = 0; index < slots + depth; ++index)
    {
      Assert::IsTrue(registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 1)).accepted);
    }
    Assert::IsFalse(registry.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 1)).accepted,
                    L"the first commander's queue is not full");

    // The second commander has no Bay here at all, so the refusal must be about
    // materials rather than about somebody else's queue.
    const OrderVerdict other = registry.SubmitStationCommand(TWO, Start(station, AlloyId::FerrocitePlates, 1));
    Assert::IsTrue(other.reason == OrderReason::InsufficientMaterials,
                   L"one commander's full queue refused another commander's job");
  }
};

TEST_CLASS(RefineProjectTests)
{
public:
  TEST_METHOD(AProjectCompletesExactlyOnceHoweverManyPeoplePushOnIt)
  {
    /*
     * The accept's own case: two commanders contributing the last units in the
     * same tick. The tier rises once, and the loser is refused before a single
     * unit leaves their Bay -- so "exactly once" is a property of the arithmetic
     * rather than of a guard that suppresses a second completion.
     *
     * That is also why the check lives at the contribution rather than on a
     * sweep: a sweep would have to decide what to do with the units the second
     * commander had already handed over, and there is no answer to that which
     * does not either take their property or invent a refund path.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    Assert::AreNotEqual<std::uint16_t>(INVALID_ID, station, L"the bake has no High-Sec station");

    const std::uint8_t tier = registry.TierAt(station);
    Assert::IsTrue(tier < BandTierCap(Economy(), SecurityBand::High), L"the fixture's station is already at its band cap");
    const RefineryUpgradeProject& cost = Economy().refining.upgradeProjects[tier];

    // Which alloy the project wants, and how much.
    AlloyId wanted = AlloyId::FerrocitePlates;
    std::uint32_t total = 0;
    for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
    {
      if (cost.costUnits[alloy] > 0)
      {
        wanted = static_cast<AlloyId>(alloy);
        total = cost.costUnits[alloy];
        break;
      }
    }
    Assert::IsTrue(total > 0, L"the next project costs nothing at all");

    const Neuron::PlayerId owners[] = {ONE, TWO};
    StockBayAlloys(registry, owners, station, 100000);

    // Everything but the last unit of every material, so the next contribution
    // of `wanted` is the one that finishes it.
    for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
    {
      if (cost.costUnits[alloy] == 0)
      {
        continue;
      }
      const std::uint32_t upTo = static_cast<AlloyId>(alloy) == wanted ? cost.costUnits[alloy] - 1 : cost.costUnits[alloy];
      Assert::IsTrue(registry.SubmitStationCommand(ONE, Contribute(station, static_cast<AlloyId>(alloy), upTo)).accepted,
                     L"a contribution inside the project's cost was refused");
    }
    Assert::AreEqual<std::uint8_t>(tier, registry.TierAt(station), L"the tier rose before the project was paid for");

    // Both commanders push on the last unit, in the same tick.
    const OrderVerdict first = registry.SubmitStationCommand(ONE, Contribute(station, wanted, 1));
    const OrderVerdict second = registry.SubmitStationCommand(TWO, Contribute(station, wanted, 1));

    Assert::IsTrue(first.accepted, L"the contribution that completes a project was refused");
    Assert::IsFalse(second.accepted, L"a project took units after it was already complete");
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(tier + 1), registry.TierAt(station),
                                   L"the tier did not rise when the project completed");
    Assert::AreEqual<std::uint32_t>(100000, registry.Bay(TWO, station)->Units(wanted),
                                    L"the losing contributor lost units to a project that was already finished");
  }

  TEST_METHOD(AProjectClampsAtWhatItStillWants)
  {
    // The print's clamp, enforced (D-P3): a project completes exactly once, so
    // it must never take units it will not use. A contribution past what
    // remains is refused rather than partially applied, because a partial
    // acceptance is a quantity the player did not choose.
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    const std::uint8_t tier = registry.TierAt(station);
    const RefineryUpgradeProject& cost = Economy().refining.upgradeProjects[tier];

    AlloyId wanted = AlloyId::FerrocitePlates;
    for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
    {
      if (cost.costUnits[alloy] > 0)
      {
        wanted = static_cast<AlloyId>(alloy);
        break;
      }
    }
    const std::uint32_t total = cost.costUnits[static_cast<std::uint8_t>(wanted)];

    const Neuron::PlayerId owners[] = {ONE};
    StockBayAlloys(registry, owners, station, 100000);

    const OrderVerdict over = registry.SubmitStationCommand(ONE, Contribute(station, wanted, total + 1));
    Assert::IsFalse(over.accepted, L"a project took more than it wanted");
    Assert::IsTrue(over.reason == OrderReason::InsufficientMaterials, L"the clamp did not say InsufficientMaterials");
    Assert::AreEqual<std::uint32_t>(100000, registry.Bay(ONE, station)->Units(wanted), L"a refused contribution moved alloy");

    Assert::IsTrue(registry.SubmitStationCommand(ONE, Contribute(station, wanted, total)).accepted,
                   L"a contribution of exactly what remains was refused");
  }

  TEST_METHOD(AStationAtItsBandCapHasNothingLeftToBuild)
  {
    /*
     * The amber row the print draws (D-P3): geography rather than progress.
     * A High-Sec station raised to its cap has no project at all -- not an
     * empty one, none -- because the next tier is not a thing that can be
     * bought here at any price.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = StationInBand(SecurityBand::High);
    const std::uint8_t cap = BandTierCap(Economy(), SecurityBand::High);

    DurableState state;
    CaptureDurableState(registry, state);
    StationTier row;
    row.station = station;
    row.tier = cap;
    state.stationTiers.push_back(row);

    std::vector<PersistenceDiagnostic> diagnostics;
    MakeRegistry(registry);
    Assert::IsTrue(registry.LoadDurable(state, diagnostics), L"a capped station would not load");
    Assert::AreEqual<std::uint8_t>(cap, registry.TierAt(station), L"the tier did not survive");

    const Neuron::PlayerId owners[] = {ONE};
    StockBayAlloys(registry, owners, station, 100000);
    const OrderVerdict none = registry.SubmitStationCommand(ONE, Contribute(station, AlloyId::FerrocitePlates, 10));
    Assert::IsFalse(none.accepted, L"a station at its band cap accepted a contribution");
    Assert::IsTrue(none.reason == OrderReason::RecipeLocked, L"no project did not say RecipeLocked");
  }
};

TEST_CLASS(RefineDurabilityTests)
{
public:
  TEST_METHOD(ARunningJobSurvivesARestartWithItsCompletionTickUnmoved)
  {
    /*
     * G1's real claim, at the registry (the `selfTest` proves it over a real
     * host): **a refinery that stops when the shard restarts is the demo this
     * phase exists not to be.**
     *
     * The completion tick unmoved is the whole assertion. A job restored with a
     * *duration* rather than a deadline would silently restart its own clock on
     * every restart, so a shard that bounced twice an hour would never finish
     * anything -- and every individual test of it would pass.
     */
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId station = StationInBand(SecurityBand::High);
    StockBay(before, ONE, station, 100000);
    Assert::IsTrue(before.SubmitStationCommand(ONE, Start(station, AlloyId::FerrocitePlates, 50)).accepted);

    // Part-way through: far enough that a restarted clock would be obvious.
    for (std::uint32_t tick = 1; tick <= 500; ++tick)
    {
      before.Tick(tick);
    }
    Assert::AreEqual<std::size_t>(1, before.RefineJobs().size(), L"the fixture's job finished before it could be interrupted");
    const RefineJob running = before.RefineJobs()[0];
    Assert::IsTrue(running.Running(), L"the fixture's job never started");

    std::vector<std::uint8_t> bytes(1u << 20);
    Neuron::ByteWriter writer(bytes);
    Assert::IsTrue(WriteDurableState(before, writer), L"the shard would not serialise");
    bytes.resize(writer.BytesWritten());

    WorldRegistry after;
    MakeRegistry(after);
    Neuron::ByteReader reader(bytes);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(ReadDurableState(reader, after, diagnostics), L"a shard with a job running would not reload");

    Assert::AreEqual<std::size_t>(1, after.RefineJobs().size(), L"the job did not survive the restart");
    const RefineJob reloaded = after.RefineJobs()[0];
    Assert::AreEqual<std::uint32_t>(running.completeTick, reloaded.completeTick, L"the job's completion tick moved across a restart");
    Assert::AreEqual<std::uint32_t>(running.sequence, reloaded.sequence, L"the job came back as a different job");
    Assert::AreEqual(DurableHash(before), DurableHash(after), L"a running job broke the reload proof");

    // And it finishes on the tick it always said it would, into the Bay.
    for (std::uint32_t tick = after.ShardTick() + 1; tick <= reloaded.completeTick; ++tick)
    {
      after.Tick(tick);
    }
    Assert::AreEqual<std::size_t>(0, after.RefineJobs().size(), L"the reloaded job never finished");
    Assert::AreEqual<std::uint32_t>(50, after.Bay(ONE, station)->Units(AlloyId::FerrocitePlates),
                                    L"the reloaded job did not deliver its alloy");
  }

  TEST_METHOD(ACompletedProjectsTierSurvivesARestart)
  {
    // Permanent and communal (ADR-024 §6c): a tier that did not survive a
    // restart would un-build something a dozen commanders paid for.
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId station = StationInBand(SecurityBand::High);
    const std::uint8_t floor = before.TierAt(station);

    DurableState state;
    CaptureDurableState(before, state);
    StationTier row;
    row.station = station;
    row.tier = static_cast<std::uint8_t>(floor + 1);
    state.stationTiers.push_back(row);

    WorldRegistry after;
    MakeRegistry(after);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(after.LoadDurable(state, diagnostics), L"an upgraded station would not load");
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(floor + 1), after.TierAt(station),
                                   L"a built tier did not survive the restart");
  }

  TEST_METHOD(ATierPastTheTableIsRefusedRatherThanIndexed)
  {
    // A corrupt tier must never index the tier table. Bounded where it is read,
    // which is the difference between a diagnostic and a buffer overrun.
    WorldRegistry registry;
    MakeRegistry(registry);
    DurableState state;
    state.nextDynamicShipId = DYNAMIC_SHIP_ID_BASE;
    StationTier row;
    row.station = 12;
    row.tier = static_cast<std::uint8_t>(REFINERY_TIER_COUNT + 1);
    state.stationTiers.push_back(row);

    std::vector<std::uint8_t> bytes(4096);
    Neuron::ByteWriter writer(bytes);
    Assert::IsTrue(WriteDurableState(state, writer), L"the fixture would not serialise");
    bytes.resize(writer.BytesWritten());

    Neuron::ByteReader reader(bytes);
    DurableState read;
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsFalse(ReadDurableState(reader, read, diagnostics), L"an impossible tier was accepted");
    Assert::IsTrue(diagnostics.size() >= 1, L"the refusal said nothing");
  }
};

} // namespace GameLogicTests
