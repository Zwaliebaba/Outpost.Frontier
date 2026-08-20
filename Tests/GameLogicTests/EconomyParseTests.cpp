#include "pch.h"
#include "CppUnitTest.h"

#include "EconomyDef.h"
#include "EconomyParse.h"
#include "ShipClass.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * The economy content layer (Build Order E1a).
 *
 * These are that slice's acceptance criteria written as a suite, and almost all
 * of them run **against the committed `Economy.json`** rather than against a
 * fixture built in code. That is deliberate and it is U1's lesson: the bake's
 * two invisible defects were caught only because its round-trip ran on the file
 * that ships. A fixture proves the parser; the committed file proves the
 * content, and the content is what a player gets.
 *
 * The suite splits into three kinds of check, and the split is the design:
 *
 *   - **Structure and referential integrity** -- what `ParseEconomy` refuses.
 *     Content the two halves could disagree about has no degraded mode.
 *   - **Balance shape** -- what the ADR promised the economy would be like:
 *     rarer ore is slower and denser, every alloy compresses, higher tiers
 *     dominate. These live here rather than in the parser so that retuning the
 *     economy is editing a file, not editing code.
 *   - **The hash** -- that comments, whitespace and key order never move it and
 *     that a changed number always does. It rides the handshake, so this is the
 *     property that decides whether a client is refused.
 */

namespace GameLogicTests
{

namespace
{

/*
 * The committed file, found by walking up from wherever the test host runs.
 *
 * `GameLogicTests` may reference only GameLogic and NeuronCore (the Dependency
 * Map), so `Outpost::ResolveContentPath` is out of reach and this is the small
 * local equivalent. The walk is bounded and the failure is loud: a suite that
 * silently skipped when it could not find the content would report green for
 * the one file it exists to check.
 */
[[nodiscard]] std::string ReadCommittedEconomyText()
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
    std::string text = buffer.str();

    /*
     * Line endings are normalised out before anything matches against this.
     *
     * `.gitattributes` says `* text=auto`, so this file is LF in the repository
     * and CRLF in a Windows working tree -- and the fixtures below search for
     * multi-line snippets. Without this the suite would pass wherever it was
     * written and fail on the platform that gates it, which is the worst
     * failure mode a test can have.
     */
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
  }

  Assert::Fail(L"the committed Economy.json was not found from the test working directory");
  return {};
}

[[nodiscard]] EconomyDef ParseCommitted()
{
  EconomyDef economy;
  std::vector<EconomyDiagnostic> diagnostics;
  const std::string text = ReadCommittedEconomyText();
  if (!ParseEconomy(text, economy, diagnostics))
  {
    std::string message = "the committed economy does not parse:";
    for (const EconomyDiagnostic& diagnostic : diagnostics)
    {
      message += "\n  " + diagnostic.Text("Economy.json");
    }
    const std::wstring wide(message.begin(), message.end());
    Assert::Fail(wide.c_str());
  }
  return economy;
}

/// The committed text with one substring swapped -- how every refusal test
/// below builds its broken file, so each one differs from the shipping content
/// by exactly the fault it is about.
[[nodiscard]] std::string WithReplacement(std::string_view _find, std::string_view _replaceWith)
{
  std::string text = ReadCommittedEconomyText();
  const std::size_t at = text.find(_find);
  Assert::AreNotEqual(std::string::npos, at, L"the fixture edit no longer matches the committed file");
  text.replace(at, _find.size(), _replaceWith);
  return text;
}

/// Parses text that is expected to be refused, and hands back the diagnostics.
[[nodiscard]] std::vector<EconomyDiagnostic> ExpectRefused(const std::string& _text)
{
  EconomyDef economy;
  std::vector<EconomyDiagnostic> diagnostics;
  Assert::IsFalse(ParseEconomy(_text, economy, diagnostics), L"content that should have been refused was accepted");
  Assert::IsFalse(diagnostics.empty(), L"a refusal with no diagnostic tells nobody what is wrong");

  // A refused parse hands back nothing, never half an economy: a caller that
  // ignored the bool would otherwise run on content nobody validated.
  Assert::AreEqual(std::uint32_t{0}, economy.mining.cycleSeconds, L"a refused parse still filled the economy in");
  return diagnostics;
}

[[nodiscard]] bool AnyDiagnosticMentions(const std::vector<EconomyDiagnostic>& _diagnostics, std::string_view _text)
{
  for (const EconomyDiagnostic& diagnostic : _diagnostics)
  {
    if (diagnostic.message.find(_text) != std::string::npos || diagnostic.path.find(_text) != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CLASS(EconomyContentTests)
{
public:

  // ------------------------------------------------------------ structure

  TEST_METHOD(TheCommittedEconomyParses)
  {
    const EconomyDef economy = ParseCommitted();

    Assert::AreEqual(std::uint32_t{40}, economy.mining.cycleSeconds, L"the mining cycle is not what ADR-024 §4b prices");
    Assert::AreEqual(std::uint32_t{86400}, economy.sites.regenSeconds, L"the regeneration epoch is not one day");
    Assert::AreEqual(std::uint32_t{120000}, economy.Cargo(HullClass::Miner).oreHoldLitres, L"the Miner's ore hold moved");
    Assert::AreEqual(std::uint32_t{400000}, economy.Cargo(HullClass::Hauler).generalHoldLitres, L"the Hauler's hold moved");

    // Every ore and alloy is named, because a label with no text is a thing the
    // player cannot be told about.
    for (std::uint8_t index = 0; index < ORE_COUNT; ++index)
    {
      Assert::IsFalse(economy.ores[index].name.empty(), L"an ore has no name");
    }
    for (std::uint8_t index = 0; index < ALLOY_COUNT; ++index)
    {
      Assert::IsFalse(economy.alloys[index].name.empty(), L"an alloy has no name");
    }
  }

  TEST_METHOD(EveryRecipeInputNamesARealOre)
  {
    const std::vector<EconomyDiagnostic> diagnostics = ExpectRefused(WithReplacement(R"("ore": "astracite", "units": 1)",
                                                                                     R"("ore": "astracyte", "units": 1)"));
    Assert::IsTrue(AnyDiagnosticMentions(diagnostics, "astracyte"), L"the refusal does not name the ore it did not know");
    Assert::IsTrue(AnyDiagnosticMentions(diagnostics, "inputs"), L"the refusal does not name the path to the fault");
  }

  TEST_METHOD(CargoIsRefusedForHullsThatCannotCarryIt)
  {
    // A station is a hull class because a structure is a ship-table entry that
    // never moves. That does not make it a freighter -- it stores in a Bay.
    const std::vector<EconomyDiagnostic> station = ExpectRefused(
      WithReplacement(R"("Carrier":     { "generalHoldLitres": 90000 })",
                      R"("Carrier":     { "generalHoldLitres": 90000 }, "Structure": { "generalHoldLitres": 5000 })"));
    Assert::IsTrue(AnyDiagnosticMentions(station, "Station Bay"), L"a hold on a station was not refused for the right reason");

    // A reserved class can never spawn, so a hold for it is one nothing fills.
    const std::vector<EconomyDiagnostic> reserved = ExpectRefused(
      WithReplacement(R"("Carrier":     { "generalHoldLitres": 90000 })",
                      R"("Carrier":     { "generalHoldLitres": 90000 }, "Cruiser": { "generalHoldLitres": 5000 })"));
    Assert::IsTrue(AnyDiagnosticMentions(reserved, "reserved"), L"a hold on a reserved class was not refused");
  }

  TEST_METHOD(CompositionsAndWeightsMustTotalOneHundred)
  {
    const std::vector<EconomyDiagnostic> composition =
      ExpectRefused(WithReplacement(R"("high": [90, 10, 0])", R"("high": [90, 10, 5])"));
    Assert::IsTrue(AnyDiagnosticMentions(composition, "must total 100"), L"a composition summing to 105 was accepted");

    const std::vector<EconomyDiagnostic> weights =
      ExpectRefused(WithReplacement(R"("high": [65, 25, 10], "low")", R"("high": [65, 25, 20], "low")"));
    Assert::IsTrue(AnyDiagnosticMentions(weights, "must total 100"), L"archetype weights summing to 110 were accepted");
  }

  TEST_METHOD(FractionalNumbersAreRefusedRatherThanRounded)
  {
    // ADR-012 §C7 applied to balance: a hold that has stopped being an exact
    // number of litres is two clients disagreeing about when it is full.
    const std::vector<EconomyDiagnostic> diagnostics =
      ExpectRefused(WithReplacement(R"("oreHoldLitres": 120000)", R"("oreHoldLitres": 120000.5)"));
    Assert::IsTrue(AnyDiagnosticMentions(diagnostics, "whole number"), L"a fractional litre count was rounded instead of refused");
  }

  TEST_METHOD(AnUnknownKeyIsRefused)
  {
    const std::vector<EconomyDiagnostic> diagnostics =
      ExpectRefused(WithReplacement(R"("queueDepthPerPlayer": 10)", R"("queueDepthPerPlayer": 10, "mysteryKnob": 3)"));
    Assert::IsTrue(AnyDiagnosticMentions(diagnostics, "mysteryKnob"), L"an unknown key passed unremarked");
  }

  TEST_METHOD(EveryFaultIsReportedInOnePass)
  {
    // ADR-012 §C8: authoring balance means fixing several things at once, and a
    // parser that surfaces them one run at a time turns that into several runs.
    std::string text = ReadCommittedEconomyText();
    const auto swap = [&text](std::string_view _find, std::string_view _replaceWith) {
      const std::size_t at = text.find(_find);
      Assert::AreNotEqual(std::string::npos, at, L"a fixture edit no longer matches the committed file");
      text.replace(at, _find.size(), _replaceWith);
    };

    swap(R"("ore": "astracite", "units": 1)", R"("ore": "astracyte", "units": 1)");
    swap(R"("high": [90, 10, 0])", R"("high": [90, 10, 5])");
    swap(R"("queueDepthPerPlayer": 10)", R"("queueDepthPerPlayer": 10, "mysteryKnob": 3)");

    const std::vector<EconomyDiagnostic> diagnostics = ExpectRefused(text);
    Assert::IsTrue(diagnostics.size() >= 3, L"three faults were authored and fewer than three were reported");
    Assert::IsTrue(AnyDiagnosticMentions(diagnostics, "astracyte"), L"the unknown ore went unreported");
    Assert::IsTrue(AnyDiagnosticMentions(diagnostics, "must total 100"), L"the bad composition went unreported");
    Assert::IsTrue(AnyDiagnosticMentions(diagnostics, "mysteryKnob"), L"the unknown key went unreported");

    // Every diagnostic can be acted on: a line to look at, and a message.
    for (const EconomyDiagnostic& diagnostic : diagnostics)
    {
      Assert::IsFalse(diagnostic.message.empty(), L"a diagnostic with no message");
      Assert::AreNotEqual(std::uint32_t{0}, diagnostic.line, L"a diagnostic with no line to look at");
    }
  }

  // -------------------------------------------------------- balance shape

  TEST_METHOD(RarerOreIsSlowerToMineAndDenserInValue)
  {
    const EconomyDef economy = ParseCommitted();

    Assert::IsTrue(economy.Ore(OreId::FerroChroma).valueIndexPct < economy.Ore(OreId::Astracite).valueIndexPct &&
                     economy.Ore(OreId::Astracite).valueIndexPct < economy.Ore(OreId::Nebulite).valueIndexPct,
                   L"the value index does not rise with rarity");
    Assert::IsTrue(economy.mining.unitsPerCycle[0] > economy.mining.unitsPerCycle[1] &&
                     economy.mining.unitsPerCycle[1] > economy.mining.unitsPerCycle[2],
                   L"cycle yield does not fall with rarity");
  }

  TEST_METHOD(EveryAlloyCompressesItsOre)
  {
    // The property that makes refining near the source the efficient move --
    // and the alloy convoy the richest target per litre in the game.
    const EconomyDef economy = ParseCommitted();
    for (std::uint8_t index = 0; index < ALLOY_COUNT; ++index)
    {
      const AlloyId alloy = static_cast<AlloyId>(index);
      Assert::IsTrue(economy.Alloy(alloy).unitVolumeLitres < economy.InputVolumeLitres(alloy),
                     L"an alloy takes at least as much space as the ore it was made from");
    }
  }

  TEST_METHOD(HigherRefineryTiersStrictlyDominate)
  {
    const EconomyDef economy = ParseCommitted();
    for (std::uint8_t tier = 2; tier <= REFINERY_TIER_COUNT; ++tier)
    {
      const RefineryTierInfo& lower = economy.Tier(static_cast<std::uint8_t>(tier - 1));
      const RefineryTierInfo& upper = economy.Tier(tier);
      Assert::IsTrue(upper.slotsPerPlayer >= lower.slotsPerPlayer, L"a higher tier offers fewer slots");
      Assert::IsTrue(upper.recipeTierCap >= lower.recipeTierCap, L"a higher tier cooks less");
      Assert::IsTrue(upper.refundPct > lower.refundPct, L"a higher tier does not refund better");
      Assert::IsTrue(upper.timePct <= lower.timePct, L"a higher tier is slower");
    }
  }

  TEST_METHOD(ARefundNeverReachesTheBatchThatEarnsIt)
  {
    // A refund at 100% would make refining free; above it, the refinery mints
    // ore. The parser bounds it and this states why the bound is there.
    const EconomyDef economy = ParseCommitted();
    for (std::uint8_t tier = 1; tier <= REFINERY_TIER_COUNT; ++tier)
    {
      Assert::IsTrue(economy.Tier(tier).refundPct < 100, L"a tier refunds its whole input");
    }

    // And at the largest batch the arithmetic still consumes more than it
    // returns, which is the property the percentage exists to guarantee.
    const RefineBatchInfo& largest = economy.refining.batches[economy.refining.batchCount - 1];
    const RefineryTierInfo& best = economy.Tier(REFINERY_TIER_COUNT);
    const std::uint32_t consumed = largest.units * economy.Alloy(AlloyId::FerrocitePlates).Input(OreId::FerroChroma);
    const std::uint32_t refunded = consumed * best.refundPct / 100;
    Assert::IsTrue(refunded < consumed, L"the best tier's largest batch refunds everything it consumed");
  }

  TEST_METHOD(BiggerBatchesNeverCostMoreTime)
  {
    const EconomyDef economy = ParseCommitted();
    Assert::IsTrue(economy.refining.batchCount >= 2, L"there is no batching to speak of");
    for (std::uint8_t index = 1; index < economy.refining.batchCount; ++index)
    {
      Assert::IsTrue(economy.refining.batches[index].units > economy.refining.batches[index - 1].units,
                     L"batches are not in ascending order after parsing");
      Assert::IsTrue(economy.refining.batches[index].timePct <= economy.refining.batches[index - 1].timePct,
                     L"a bigger batch costs more time per unit");
    }
  }

  TEST_METHOD(NovaSteelIsBornOutsideHighSec)
  {
    // ADR-024 §6c's industrial map, as a property rather than as a paragraph:
    // no High-Sec station can ever cook Tier 3, so endgame industry runs ore
    // into the dark and alloys out of it.
    const EconomyDef economy = ParseCommitted();
    const std::uint8_t highCap = economy.refining.bandTierCaps[static_cast<std::uint8_t>(SecurityBand::High)];
    Assert::IsTrue(economy.Tier(highCap).recipeTierCap < economy.Alloy(AlloyId::NovaSteel).tier,
                   L"a High-Sec station can cook Nova-Steel");

    const std::uint8_t nullCap = economy.refining.bandTierCaps[static_cast<std::uint8_t>(SecurityBand::Null)];
    Assert::IsTrue(economy.Tier(nullCap).recipeTierCap >= economy.Alloy(AlloyId::NovaSteel).tier,
                   L"Nova-Steel cannot be cooked anywhere");
    Assert::IsTrue(nullCap > highCap, L"the tier ceiling does not rise toward danger");
  }

  TEST_METHOD(HighSecNebulaPocketsAreFaded)
  {
    // Ruling 1b: Tier 1 stays craftable in every band while no market exists to
    // buy Nebulite from -- at grade I, thin, with the hazard still on.
    const EconomyDef economy = ParseCommitted();
    const SiteArchetypeInfo& pocket = economy.Archetype(SiteArchetype::NebulaPocket);
    // Widened to `unsigned` deliberately: CppUnitTest resolves `AreEqual`
    // through a `ToString` overload, and this corpus has no specialisation for
    // a narrow integer type -- so an assertion on the `std::uint8_t` itself is
    // a compile error on the one toolchain that gates.
    Assert::AreEqual(1u, static_cast<unsigned>(pocket.gradeCapByBand[static_cast<std::uint8_t>(SecurityBand::High)]),
                     L"High-Sec nebula pockets are not capped at grade I");
    Assert::IsTrue(pocket.compositionPct[static_cast<std::uint8_t>(SecurityBand::High)][static_cast<std::uint8_t>(OreId::Nebulite)] >
                     0,
                   L"a High-Sec pocket holds no Nebulite at all");
  }

  TEST_METHOD(EveryBandCanFeedTierOne)
  {
    // The other half of ruling 1b, stated from the recipe's side: both Tier 1
    // recipes are satisfiable from ore that spawns in every band.
    const EconomyDef economy = ParseCommitted();
    for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
    {
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        bool available = false;
        for (std::uint8_t archetype = 0; archetype < SITE_ARCHETYPE_COUNT && !available; ++archetype)
        {
          available = economy.sites.archetypes[archetype].compositionPct[band][ore] > 0;
        }
        Assert::IsTrue(available, L"an ore is unobtainable in a whole security band");
      }
    }
  }

  TEST_METHOD(SecurityValuesFallInTheBandsTheBakeAuthors)
  {
    // The bake's three region archetypes are 60-95, 25-70 and 0-35, and the
    // thresholds here were chosen to match them.
    const EconomyDef economy = ParseCommitted();
    Assert::IsTrue(economy.BandFor(95) == SecurityBand::High, L"95 is not High-Sec");
    Assert::IsTrue(economy.BandFor(60) == SecurityBand::High, L"the High floor is not inclusive");
    Assert::IsTrue(economy.BandFor(59) == SecurityBand::Low, L"59 is not Low-Sec");
    Assert::IsTrue(economy.BandFor(25) == SecurityBand::Low, L"the Low floor is not inclusive");
    Assert::IsTrue(economy.BandFor(24) == SecurityBand::Null, L"24 is not Null-Sec");
    Assert::IsTrue(economy.BandFor(0) == SecurityBand::Null, L"0 is not Null-Sec");
  }

  TEST_METHOD(TheSiteFieldFitsTheGridItSitsOn)
  {
    // A field wider than the 40 km grid (ADR-001) is content the bake could
    // never place. Refused where it is authored rather than where it is baked.
    const EconomyDef economy = ParseCommitted();
    Assert::IsTrue(economy.sites.fieldRadiusMaxMetres + economy.sites.warpInStandoffMetres <= 40000,
                   L"the widest field plus its standoff does not fit a grid");
    Assert::IsTrue(economy.sites.fieldRadiusMinMetres <= economy.sites.fieldRadiusMaxMetres, L"the field radius range is inverted");
    Assert::IsTrue(economy.sites.clusterCountMin >= 1 && economy.sites.clusterCountMin <= economy.sites.clusterCountMax,
                   L"the cluster count range is unusable");
  }

  TEST_METHOD(EveryTierAboveTheFloorCanBeBuilt)
  {
    const EconomyDef economy = ParseCommitted();
    for (std::uint8_t tier = 2; tier <= REFINERY_TIER_COUNT; ++tier)
    {
      Assert::IsTrue(economy.refining.upgradeProjects[tier - 1].Exists(), L"a tier exists that nobody can build");
    }
    // And tier 1 is the bake's authored floor rather than a project.
    Assert::IsFalse(economy.refining.upgradeProjects[0].Exists(), L"the authored floor has an upgrade project");
  }

  // ---------------------------------------------------------------- hash

  TEST_METHOD(TheHashIgnoresCommentsWhitespaceAndKeyOrder)
  {
    const EconomyDef committed = ParseCommitted();
    const std::uint64_t expected = ComputeEconomyHash(committed);
    Assert::AreNotEqual(std::uint64_t{0}, expected, L"the economy hashed to zero");

    // Reordering an array is the sharpest form of this: nothing about the
    // economy changed, and a hash that folded file order would say otherwise.
    // The parser stores everything at an index its taxonomy fixes, so there is
    // no file order left to fold.
    const std::string reordered =
      WithReplacement(R"({ "id": "ferroChroma", "name": "Ferro-Chroma", "unitVolumeLitres": 300, "valueIndexPct": 100 },
    { "id": "astracite",   "name": "Astracite",    "unitVolumeLitres": 200, "valueIndexPct": 220 },)",
                      R"({ "id": "astracite",   "name": "Astracite",    "unitVolumeLitres": 200, "valueIndexPct": 220 },
    { "id": "ferroChroma", "name": "Ferro-Chroma", "unitVolumeLitres": 300, "valueIndexPct": 100 },)");

    EconomyDef variant;
    std::vector<EconomyDiagnostic> diagnostics;
    Assert::IsTrue(ParseEconomy(reordered, variant, diagnostics), L"the reordered economy did not parse");
    Assert::AreEqual(expected, ComputeEconomyHash(variant), L"reordering the ore list moved the content hash");
  }

  TEST_METHOD(TheHashMovesWhenAValueDoes)
  {
    const EconomyDef committed = ParseCommitted();
    const std::uint64_t expected = ComputeEconomyHash(committed);

    EconomyDef variant;
    std::vector<EconomyDiagnostic> diagnostics;
    const std::string changed = WithReplacement(R"("oreHoldLitres": 120000)", R"("oreHoldLitres": 120001)");
    Assert::IsTrue(ParseEconomy(changed, variant, diagnostics), L"the edited economy did not parse");
    Assert::AreNotEqual(expected, ComputeEconomyHash(variant), L"one litre of difference did not move the content hash");
  }

  TEST_METHOD(MixingContentHashesDependsOnBoth)
  {
    // The handshake carries one number, so an economy change has to be able to
    // move it on its own -- otherwise mixing would have quietly dropped the
    // guard this slice exists to add.
    Assert::AreNotEqual(MixContentHashes(1, 2), MixContentHashes(1, 3), L"the mix ignores the economy hash");
    Assert::AreNotEqual(MixContentHashes(1, 2), MixContentHashes(2, 2), L"the mix ignores the universe hash");
    Assert::AreNotEqual(MixContentHashes(1, 2), MixContentHashes(2, 1), L"the mix cannot tell the two hashes apart");
    Assert::AreEqual(MixContentHashes(7, 9), MixContentHashes(7, 9), L"the mix is not a function of its inputs");
  }
};

} // namespace GameLogicTests
