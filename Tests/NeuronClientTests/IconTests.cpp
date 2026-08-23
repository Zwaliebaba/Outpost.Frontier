#include "pch.h"
#include "CppUnitTest.h"

#include "IconDensity.h"
#include "IconSystem.h"
#include "OverlayMark.h" // The bar whose width the ladder's first boundary is.
#include "UiLayout.h"     // And the target floor the merge grid is.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The tactical icon system (`tactical-icon-system.png`, 07b).
 *
 * The sheet makes four claims that are checkable without a screen, and this
 * file is those four: that the taxonomy's *"nothing that moves is ever drawn
 * symmetric"* is a property of the table rather than a habit; that the 20 px
 * floor is a floor rather than a clamp -- a glyph below it is **replaced**, not
 * shrunk; that §3's flag rings *"separate with colour removed entirely"*; and
 * that §6's merge is the pure function of replicated fields it says it is.
 *
 * The fifth claim -- that eleven silhouettes really do separate at 20 px on a
 * real panel -- is R3 F13 and cannot be settled here. That is the whole of what
 * this slice leaves to a display.
 *
 * The tables below are hand-written rather than the game's: the rules belong to
 * this library and the eleven shapes belong to the composition root
 * (`Outpost/HullIcons.h`, asserted against the plate in `OutpostTests`).
 */

namespace NeuronClientTests
{
namespace
{

/// A movers-and-a-structure table, in slot order, with the plate's own
/// proportions for the two rows that stand for the extremes.
[[nodiscard]] std::array<IconGlyph, 4> SampleTable()
{
  return {{
    {IconFamily::Strike, 0.40f, 0.64f}, // The plate's interceptor: a thin spike.
    {IconFamily::Line, 0.90f, 1.00f},   // Its battleship: the unit box.
    {IconFamily::Support, 1.00f, 0.84f},
    {IconFamily::Static, 0.84f, 0.84f}, // Its structure: the only symmetric glyph it draws.
  }};
}

/// A candidate at a place, with everything else at its quiet default.
[[nodiscard]] IconMergeCandidate At(float _x, float _y, StandingColour _standing = StandingColour::Own)
{
  IconMergeCandidate candidate;
  candidate.planeMetres = DirectX::XMFLOAT2{_x, _y};
  candidate.standing = _standing;
  return candidate;
}

/// One metre per pixel, so a merge cell is exactly the 48 px floor in metres and
/// the arithmetic in each test reads as the number the rule states.
constexpr float ONE_TO_ONE = 1.0f;

} // namespace

TEST_CLASS(IconTaxonomyTests)
{
public:
  TEST_METHOD(SymmetricExactlyWhenItCannotPoint)
  {
    /*
     * §1's *"nothing that moves is ever drawn symmetric, so heading is readable
     * at every size"* and §2's *"never heading-oriented"* are one rule stated
     * twice. Held as an equivalence, it is checkable -- which is the only
     * reason a reader can trust that a symmetric mark is a mark that does not
     * move.
     */
    Assert::IsTrue(ValidIconGlyphTable(SampleTable()));

    auto asymmetricStatic = SampleTable();
    asymmetricStatic[3].widthFraction = 0.60f;
    Assert::IsFalse(ValidIconGlyphTable(asymmetricStatic),
                    L"a heading-less glyph drawn asymmetric reads as pointing somewhere it cannot point");

    auto symmetricMover = SampleTable();
    symmetricMover[1].widthFraction = symmetricMover[1].heightFraction;
    Assert::IsFalse(ValidIconGlyphTable(symmetricMover), L"and a mover drawn symmetric has lost its heading");
  }

  TEST_METHOD(AProportionOutsideTheUnitBoxIsRefusedRatherThanClamped)
  {
    // A row that says 1.4 meant something this library cannot guess, and a
    // clamp would draw the guess. Content, so refused rather than asserted.
    auto tooWide = SampleTable();
    tooWide[0].widthFraction = 1.4f;
    Assert::IsFalse(ValidIconGlyphTable(tooWide));

    auto empty = SampleTable();
    empty[0].heightFraction = 0.0f;
    Assert::IsFalse(ValidIconGlyphTable(empty));
  }

  TEST_METHOD(TheFamilyIsWhatDecidesWhetherAGlyphCarriesAHeading)
  {
    // One source for the rule, so a second reader cannot spell it differently.
    Assert::IsTrue(IconHeadingOriented(IconFamily::Strike));
    Assert::IsTrue(IconHeadingOriented(IconFamily::Line));
    Assert::IsTrue(IconHeadingOriented(IconFamily::Support));
    Assert::IsFalse(IconHeadingOriented(IconFamily::Static));
  }
};

TEST_CLASS(IconChannelTests)
{
public:
  TEST_METHOD(SizeIsTheHullsSilhouetteProjected)
  {
    // A diameter, not a radius: `SilhouetteRadiusMetres` answers "how far does
    // this hull reach" and the plate's boxes are widths.
    Assert::AreEqual(40.0f, IconLongestAxisPixels(20.0f, 1.0f), 1e-4f);
    Assert::AreEqual(20.0f, IconLongestAxisPixels(20.0f, 2.0f), 1e-4f);

    // A hull with no stated size, or a camera with no scale, is not a glyph
    // drawn at a guess -- it is zero, which the ladder reads as the merge.
    Assert::AreEqual(0.0f, IconLongestAxisPixels(0.0f, 1.0f), 1e-6f);
    Assert::AreEqual(0.0f, IconLongestAxisPixels(20.0f, 0.0f), 1e-6f);
  }

  TEST_METHOD(TheLadderLandsOnThePlatesOwnRows)
  {
    /*
     * §5's legibility table names its rows: 48 px *"U2 touch target"*, 32 px
     * *"tactical default"*, 24 px *"dense engagement"*, 20 px *"legibility
     * floor"*, 14 px *"fails -- merge instead"*.
     *
     * The two boundaries are derived rather than chosen -- the bars retire at
     * their own 24 px width, and the merge begins under the 20 px floor -- so
     * this asserts that the derivation lands on the plate rather than near it.
     */
    const IconTuning tuning;
    Assert::IsTrue(IconTierFor(48.0f, tuning) == IconTier::Tactical);
    Assert::IsTrue(IconTierFor(32.0f, tuning) == IconTier::Tactical);
    Assert::IsTrue(IconTierFor(24.0f, tuning) == IconTier::Tactical, L"the bar is exactly as wide as the glyph");
    Assert::IsTrue(IconTierFor(23.9f, tuning) == IconTier::Operational, L"and a hair narrower retires the bars");
    Assert::IsTrue(IconTierFor(20.0f, tuning) == IconTier::Operational, L"the floor still draws a glyph");
    Assert::IsTrue(IconTierFor(19.9f, tuning) == IconTier::Strategic, L"below it the glyph is replaced");
    Assert::IsTrue(IconTierFor(14.0f, tuning) == IconTier::Strategic);
  }

  TEST_METHOD(AGlyphIsReplacedBelowTheFloorAndNeverShrunkToIt)
  {
    // §6: *"A glyph never shrinks below its legible floor; it is replaced."*
    // The distinction is the whole ladder -- a clamp would keep drawing hulls
    // at positions a merge is supposed to stop claiming.
    const IconTuning tuning;
    const float tiny = IconLongestAxisPixels(2.0f, 1.0f); // 4 px.
    Assert::AreEqual(4.0f, tiny, 1e-4f, L"the size channel reports the truth");
    Assert::IsTrue(IconTierFor(tiny, tuning) == IconTier::Strategic, L"and the ladder is what refuses to draw it");
  }

  TEST_METHOD(TheTwoBoundariesAreTheNumbersTheyClaimToBe)
  {
    /*
     * Both thresholds are *derived*, and this is what stops the derivation
     * becoming a coincidence.
     *
     * Bars retire at their own width, because a readout wider than the thing it
     * describes has stopped being attached to it -- so `barRetirePixels` is
     * `OverlayTuning`'s 24 px bar, spelled here rather than reached for across a
     * header the icon system otherwise does not depend on.
     *
     * The merge grid is ADR-020 §8's touch floor, because a chip is *pressed*
     * where a glyph is *read* -- the plate's own distinction between the two
     * floors, applied to the thing that replaces a glyph.
     */
    const OverlayTuning overlay;
    Assert::AreEqual(overlay.barHalfWidthPixels * 2.0f, IconTuning{}.barRetirePixels, 1e-6f,
                     L"the bars retire at exactly their own width");
    Assert::AreEqual(TARGET_FLOOR_PIXELS, IconDensityTuning{}.mergeCellPixels, 1e-6f,
                     L"and the merge is as coarse as a finger");

    // And the two floors are two measurements, so neither may be the other.
    Assert::AreNotEqual(TARGET_FLOOR_PIXELS, IconTuning{}.legibilityFloorPixels,
                        L"what an eye tells apart is not what a finger can hit");
  }

  TEST_METHOD(AnInvertedTuningStillPutsTheFloorFirst)
  {
    // A bar threshold below the floor would merge glyphs away while their bars
    // were still being drawn. Ordered rather than trusted to arrive ordered.
    IconTuning tuning;
    tuning.barRetirePixels = 8.0f;
    Assert::IsTrue(IconTierFor(19.0f, tuning) == IconTier::Strategic);
    Assert::IsTrue(IconTierFor(21.0f, tuning) == IconTier::Tactical);
  }

  TEST_METHOD(TheFourFlagRingsSeparateWithColourRemoved)
  {
    /*
     * §3's load-bearing claim: *"Ring geometry alone separates the four flag
     * states with colour removed entirely -- swap the palette above to check
     * it."* It is what lets a hostile Criminal and a neutral Criminal both read
     * as attackable while still reading as different parties, and it is why the
     * flag is a ring rather than a hue in the first place.
     */
    const std::array<IconFlag, 4> flags{IconFlag::None, IconFlag::Engaged, IconFlag::Suspect, IconFlag::Criminal};
    for (std::size_t left = 0; left < flags.size(); ++left)
    {
      for (std::size_t right = left + 1; right < flags.size(); ++right)
      {
        const IconRing a = IconRingFor(flags[left]);
        const IconRing b = IconRingFor(flags[right]);
        const bool differs = a.radiusPerLongestAxis != b.radiusPerLongestAxis || a.strokePixels != b.strokePixels ||
                             a.dashCount != b.dashCount || a.haloed != b.haloed || a.pulses != b.pulses;
        Assert::IsTrue(differs, L"two flag states drawn identically once colour is taken away");
      }
    }

    // And the unflagged state draws nothing at all, which is already the whole
    // of "there is no ring" without a second field to say so.
    Assert::AreEqual(0.0f, IconRingFor(IconFlag::None).radiusPerLongestAxis, 1e-6f);
    Assert::IsTrue(IconRingFor(IconFlag::Criminal).radiusPerLongestAxis > 0.0f);
  }

  TEST_METHOD(AFlagRingIsDrawnOnTheSelectionAndOnLockedTargetsOnly)
  {
    // Flags are Public, so every ring in view could legally be drawn -- and a
    // lawless region at the 1,024-entity cap would then be a screen of rings.
    Assert::IsFalse(IconDrawsFlagRing(false, false));
    Assert::IsTrue(IconDrawsFlagRing(true, false));
    Assert::IsTrue(IconDrawsFlagRing(false, true));
  }
};

TEST_CLASS(IconMarkerTests)
{
public:
  TEST_METHOD(MarkersAreAdditiveAndNeverExclusive)
  {
    // §4: *"composed over the glyph -- additive, never mutually exclusive"*. A
    // ship can be selected, frozen and targeted at once, and anything that
    // ranked them would be a decision nobody took.
    IconSubject subject;
    subject.selected = true;
    subject.stale = true;
    subject.attackerLockComplete = true;

    const std::uint8_t markers = IconMarkersFor(subject);
    Assert::IsTrue((markers & ICON_MARKER_SELECTED) != 0);
    Assert::IsTrue((markers & ICON_MARKER_STALE) != 0);
    Assert::IsTrue((markers & ICON_MARKER_TARGETING_ME) != 0);
  }

  TEST_METHOD(TargetingMeAppearsOnlyOnceTheAttackersLockHasCompleted)
  {
    /*
     * The one marker that leaks, and 04 §6.3 bounds it: *"it may appear only
     * once the attacker's lock has completed, never during the sweep. Drawn
     * earlier it becomes a free early-warning system that the replication
     * audience explicitly refuses."*
     *
     * Ruled and enforced now, before the combat phase writes anything against
     * it -- which is the same reason `OrderKindNamesDestination`'s default was
     * replaced by a decision before I1 built on it.
     */
    IconSubject sweeping;
    sweeping.attackerLockComplete = false;
    Assert::IsTrue((IconMarkersFor(sweeping) & ICON_MARKER_TARGETING_ME) == 0);

    // And no other field can recover it: a viewer's own lock progress says
    // nothing about anybody's lock on them.
    sweeping.lockProgress = 0.99f;
    sweeping.selected = true;
    Assert::IsTrue((IconMarkersFor(sweeping) & ICON_MARKER_TARGETING_ME) == 0,
                   L"the sweep is exactly what the disclosure rule withholds");

    IconSubject locked;
    locked.attackerLockComplete = true;
    Assert::IsTrue((IconMarkersFor(locked) & ICON_MARKER_TARGETING_ME) != 0);
  }

  TEST_METHOD(TheSweepAndItsCompletionAreTwoMarksAndNotOne)
  {
    // §4 draws LOCKING as a sweep whose arc is the progress and LOCKED as four
    // corners that snap in on completion, so the last frame of one must not
    // also be the first frame of the other.
    IconSubject half;
    half.lockProgress = 0.5f;
    Assert::IsTrue((IconMarkersFor(half) & ICON_MARKER_LOCKING) != 0);
    Assert::IsTrue((IconMarkersFor(half) & ICON_MARKER_LOCKED) == 0);

    IconSubject done;
    done.lockProgress = 1.0f;
    Assert::IsTrue((IconMarkersFor(done) & ICON_MARKER_LOCKING) == 0);
    Assert::IsTrue((IconMarkersFor(done) & ICON_MARKER_LOCKED) != 0);

    const IconSubject none;
    Assert::AreEqual<std::uint8_t>(0, IconMarkersFor(none), L"an unlocked, unselected, live ship wears nothing");
  }

  TEST_METHOD(TheOffScreenChevronIsSelectionOnly)
  {
    // §4: *"OFF-SCREEN -- edge chevron + range, selection only"*. One chevron
    // per off-screen hull at the entity cap is a border of chevrons.
    IconSubject unselected;
    unselected.offScreen = true;
    Assert::IsTrue((IconMarkersFor(unselected) & ICON_MARKER_OFF_SCREEN) == 0);

    IconSubject selected = unselected;
    selected.selected = true;
    Assert::IsTrue((IconMarkersFor(selected) & ICON_MARKER_OFF_SCREEN) != 0);
  }
};

TEST_CLASS(IconResolveTests)
{
public:
  TEST_METHOD(TheProportionsAreAppliedSoTheLongestAxisIsTheStatedOne)
  {
    // Two multiplications here rather than at every call site, which is the
    // same argument `SceneEntity` makes for being one record instead of three
    // arrays kept in lockstep.
    const auto table = SampleTable();
    IconRequest request;
    request.slot = 0; // 0.40 x 0.64 -- the plate's interceptor.
    request.silhouetteRadiusMetres = 20.0f;

    IconSpec spec;
    Assert::IsTrue(ResolveIcon(table, request, ONE_TO_ONE, IconTuning{}, spec));
    Assert::AreEqual(40.0f, spec.longestAxisPixels, 1e-4f);
    Assert::AreEqual(40.0f, spec.heightPixels, 1e-4f, L"the long axis is the stated size");
    Assert::AreEqual(25.0f, spec.widthPixels, 1e-3f, L"and the short one keeps the plate's ratio");
    Assert::IsTrue(spec.headingOriented);
    Assert::IsTrue(spec.tier == IconTier::Tactical);
  }

  TEST_METHOD(ASlotTheTableHasNoRowForDrawsNothing)
  {
    // `BuildScene`'s posture for a hull class with no mesh, one channel over:
    // substituting slot zero would put a glyph on screen that is not the ship
    // the server is simulating.
    const auto table = SampleTable();
    IconSpec spec;

    IconRequest unknown;
    unknown.slot = INVALID_ICON_SLOT;
    unknown.silhouetteRadiusMetres = 20.0f;
    Assert::IsFalse(ResolveIcon(table, unknown, ONE_TO_ONE, IconTuning{}, spec));

    IconRequest pastTheEnd = unknown;
    pastTheEnd.slot = static_cast<IconSlot>(table.size());
    Assert::IsFalse(ResolveIcon(table, pastTheEnd, ONE_TO_ONE, IconTuning{}, spec));
  }

  TEST_METHOD(TheFlagRingIsCarriedButNotDrawnOnAnUnselectedHull)
  {
    // The information is not withheld -- the flag is on the spec, one tap away
    // -- it is the *ring* that is reserved for what you are about to shoot or
    // be shot by.
    const auto table = SampleTable();
    IconRequest request;
    request.slot = 1;
    request.silhouetteRadiusMetres = 20.0f;
    request.flag = IconFlag::Criminal;

    IconSpec spec;
    Assert::IsTrue(ResolveIcon(table, request, ONE_TO_ONE, IconTuning{}, spec));
    Assert::IsTrue(spec.flag == IconFlag::Criminal, L"the state is known");
    Assert::IsFalse(spec.drawsRing, L"and not drawn on an unselected hull");

    request.subject.selected = true;
    Assert::IsTrue(ResolveIcon(table, request, ONE_TO_ONE, IconTuning{}, spec));
    Assert::IsTrue(spec.drawsRing);
    Assert::IsTrue(spec.ring.haloed, L"the criminal's ring is the one that survives a bright hull");
  }

  TEST_METHOD(AnUnflaggedHullNeverDrawsARingHoweverItIsSelected)
  {
    const auto table = SampleTable();
    IconRequest request;
    request.slot = 1;
    request.silhouetteRadiusMetres = 20.0f;
    request.subject.selected = true;
    request.subject.lockProgress = 1.0f;

    IconSpec spec;
    Assert::IsTrue(ResolveIcon(table, request, ONE_TO_ONE, IconTuning{}, spec));
    Assert::IsFalse(spec.drawsRing);
  }
};

TEST_CLASS(IconDensityTests)
{
public:
  TEST_METHOD(ShipsInOneCellBecomeOneChipThatStatesItsExtent)
  {
    /*
     * §6's strategic rung: *"Extent outline plus a count -- the honest
     * aggregate. No hull is drawn at a position the client cannot justify, and
     * the chip says exactly what is known: how many, roughly where."*
     */
    const std::array<IconMergeCandidate, 3> candidates{At(10.0f, 10.0f), At(30.0f, 20.0f), At(20.0f, 40.0f)};

    std::array<IconCluster, 8> clusters{};
    std::array<std::uint32_t, 8> survivors{};
    const IconMergeResult result = MergeIcons(candidates, ONE_TO_ONE, IconDensityTuning{}, clusters, survivors);

    Assert::AreEqual<std::uint32_t>(1, result.clusters);
    Assert::AreEqual<std::uint32_t>(0, result.survivors);
    Assert::AreEqual<std::uint32_t>(0, result.dropped);
    Assert::AreEqual<std::uint32_t>(3, clusters[0].count);

    // The bounding box of the three, as a centre and a half-extent: a fleet in
    // line is long and thin, and a radius would claim ground it is nowhere near.
    Assert::AreEqual(20.0f, clusters[0].centreMetres.x, 1e-4f);
    Assert::AreEqual(25.0f, clusters[0].centreMetres.y, 1e-4f);
    Assert::AreEqual(10.0f, clusters[0].halfExtentMetres.x, 1e-4f);
    Assert::AreEqual(15.0f, clusters[0].halfExtentMetres.y, 1e-4f);
  }

  TEST_METHOD(TheMergeNeverMixesStandings)
  {
    // The plate draws `▲ 84` and `× 137` as two chips over overlapping ground
    // rather than one chip of 221: a count that spanned both would answer "how
    // many ships", which is a question nobody in a fight is asking.
    const std::array<IconMergeCandidate, 4> candidates{At(10.0f, 10.0f, StandingColour::Own),
                                                       At(12.0f, 12.0f, StandingColour::Own),
                                                       At(11.0f, 11.0f, StandingColour::Hostile),
                                                       At(13.0f, 13.0f, StandingColour::Hostile)};

    std::array<IconCluster, 8> clusters{};
    std::array<std::uint32_t, 8> survivors{};
    const IconMergeResult result = MergeIcons(candidates, ONE_TO_ONE, IconDensityTuning{}, clusters, survivors);

    Assert::AreEqual<std::uint32_t>(2, result.clusters);
    Assert::IsTrue(clusters[0].standing == StandingColour::Own);
    Assert::IsTrue(clusters[1].standing == StandingColour::Hostile);
    Assert::AreEqual<std::uint32_t>(2, clusters[0].count);
    Assert::AreEqual<std::uint32_t>(2, clusters[1].count);
  }

  TEST_METHOD(AFlaggedOrSelectedHullSurvivesAsItsOwnGlyph)
  {
    /*
     * §3, in as many words: *"the de-clutter merge never folds a flagged entity
     * into a chip -- a flagged hull always survives as its own glyph."* The
     * selection survives on ADR-022 §5a's argument instead: a ship the client
     * cannot place is a ship it cannot pre-check an order for.
     */
    std::array<IconMergeCandidate, 3> candidates{At(10.0f, 10.0f), At(12.0f, 12.0f), At(14.0f, 14.0f)};
    candidates[1].alwaysDrawn = true;

    std::array<IconCluster, 8> clusters{};
    std::array<std::uint32_t, 8> survivors{};
    const IconMergeResult result = MergeIcons(candidates, ONE_TO_ONE, IconDensityTuning{}, clusters, survivors);

    Assert::AreEqual<std::uint32_t>(1, result.clusters);
    Assert::AreEqual<std::uint32_t>(2, clusters[0].count, L"and the chip counts only what it actually folded");
    Assert::AreEqual<std::uint32_t>(1, result.survivors);
    Assert::AreEqual<std::uint32_t>(1, survivors[0]);
  }

  TEST_METHOD(AGroupOfOneIsDrawnRatherThanCounted)
  {
    // A chip reading `1` has replaced a glyph with a worse drawing of the same
    // fact. The merge is only a gain where it is actually aggregating.
    const std::array<IconMergeCandidate, 2> candidates{At(10.0f, 10.0f), At(500.0f, 500.0f)};

    std::array<IconCluster, 8> clusters{};
    std::array<std::uint32_t, 8> survivors{};
    const IconMergeResult result = MergeIcons(candidates, ONE_TO_ONE, IconDensityTuning{}, clusters, survivors);

    Assert::AreEqual<std::uint32_t>(0, result.clusters);
    Assert::AreEqual<std::uint32_t>(2, result.survivors);
    Assert::AreEqual<std::uint32_t>(0, survivors[0], L"survivors come back in candidate order");
    Assert::AreEqual<std::uint32_t>(1, survivors[1]);
  }

  TEST_METHOD(ZoomingOutFoldsWhatZoomingInDrawsSeparately)
  {
    /*
     * The ladder in one assertion: the same three ships, the same code, two
     * camera scales. Nothing else about the input changes -- which is what §6
     * means by *"a pure function of replicated fields"*, and why two clients
     * handed one snapshot cannot disagree about what they merged.
     */
    const std::array<IconMergeCandidate, 3> candidates{At(0.0f, 0.0f), At(100.0f, 0.0f), At(200.0f, 0.0f)};

    std::array<IconCluster, 8> clusters{};
    std::array<std::uint32_t, 8> survivors{};

    // Close in: a 48 px cell is 48 m, so 100 m apart is three separate cells.
    // (`near` and `far` are Windows SDK macros -- naming them that is how you
    // spend an afternoon on an error inside a header you did not write.)
    const IconMergeResult closeIn = MergeIcons(candidates, 1.0f, IconDensityTuning{}, clusters, survivors);
    Assert::AreEqual<std::uint32_t>(0, closeIn.clusters);
    Assert::AreEqual<std::uint32_t>(3, closeIn.survivors);

    // Far out: a 48 px cell is 4,800 m and all three share it.
    const IconMergeResult farOut = MergeIcons(candidates, 100.0f, IconDensityTuning{}, clusters, survivors);
    Assert::AreEqual<std::uint32_t>(1, farOut.clusters);
    Assert::AreEqual<std::uint32_t>(0, farOut.survivors);
    Assert::AreEqual<std::uint32_t>(3, clusters[0].count);
  }

  TEST_METHOD(NoCameraScaleDrawsEverythingRatherThanFoldingEverything)
  {
    // The safe direction: a frame that skips the merge is busy, and a frame
    // that merged the whole fleet into one chip has thrown it away over a
    // number that had not arrived yet.
    const std::array<IconMergeCandidate, 3> candidates{At(0.0f, 0.0f), At(1.0f, 0.0f), At(2.0f, 0.0f)};

    std::array<IconCluster, 8> clusters{};
    std::array<std::uint32_t, 8> survivors{};
    const IconMergeResult result = MergeIcons(candidates, 0.0f, IconDensityTuning{}, clusters, survivors);

    Assert::AreEqual<std::uint32_t>(0, result.clusters);
    Assert::AreEqual<std::uint32_t>(3, result.survivors);
  }

  TEST_METHOD(ATooSmallSpanIsReportedRatherThanSwallowed)
  {
    // No silent caps. A merge that quietly omitted ships would be the failure
    // §6 exists to prevent, arriving from the direction nobody was watching.
    const std::array<IconMergeCandidate, 6> candidates{At(0.0f, 0.0f),     At(1.0f, 0.0f),     At(500.0f, 0.0f),
                                                       At(501.0f, 0.0f),   At(1000.0f, 0.0f),  At(1001.0f, 0.0f)};

    std::array<IconCluster, 1> clusters{};
    std::array<std::uint32_t, 8> survivors{};
    const IconMergeResult result = MergeIcons(candidates, ONE_TO_ONE, IconDensityTuning{}, clusters, survivors);

    Assert::AreEqual<std::uint32_t>(1, result.clusters);
    Assert::AreEqual<std::uint32_t>(4, result.dropped, L"the four with nowhere to go are counted, not hidden");
  }

  TEST_METHOD(TheChipSaysHowManyAndInWhoseMarker)
  {
    /*
     * Colour is never the only carrier (§1), so the chip's marker is a second
     * carrier of the same standing. The plate draws own and hostile; the other
     * two are this slice's, chosen to stay distinct in glyphs the atlas already
     * bakes.
     */
    char buffer[ICON_CLUSTER_TEXT_BYTES] = {};
    const std::string own = IconClusterText(StandingColour::Own, 84, buffer, sizeof buffer);
    Assert::IsTrue(own.find("84") != std::string::npos);

    std::vector<std::string> markers;
    for (const StandingColour standing : {StandingColour::Own, StandingColour::Allied, StandingColour::Neutral,
                                          StandingColour::Hostile})
    {
      char row[ICON_CLUSTER_TEXT_BYTES] = {};
      markers.emplace_back(IconClusterText(standing, 7, row, sizeof row));
    }
    for (std::size_t left = 0; left < markers.size(); ++left)
    {
      for (std::size_t right = left + 1; right < markers.size(); ++right)
      {
        Assert::AreNotEqual(markers[left], markers[right], L"two standings share a chip marker");
      }
    }
  }

  TEST_METHOD(AChipOfNothingIsNotDrawn)
  {
    // `CountedChipText`'s rule, and the same silence: a chip reading zero is a
    // chip that has to be read before it can be ignored.
    char buffer[ICON_CLUSTER_TEXT_BYTES] = {};
    Assert::AreEqual(std::string(), std::string(IconClusterText(StandingColour::Own, 0, buffer, sizeof buffer)));
  }
};

} // namespace NeuronClientTests
