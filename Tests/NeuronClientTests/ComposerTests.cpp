#include "pch.h"
#include "CppUnitTest.h"

#include "RosterSelection.h"

#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The undock composer (`station-screen.png` §1-§2, ADR-017 §6a.2, T3b).
 *
 * The station screen's only state. Everything else it draws is the roster, so
 * every defect the screen can have that is not a layout defect is in here: a
 * gesture that does not accumulate, a wave split that loses a ship, or a
 * selection that outlives the ships it names.
 *
 * That last one is the interesting one and it has a date on it. The print left
 * "does the composer persist?" open in §3 precisely because a half-built
 * selection surviving a screen switch is convenient *and* a stale-selection
 * hazard; ADR-017 §6a.2 answered "persists, reconciled", which is two
 * behaviours rather than one and needs both halves tested.
 */

namespace NeuronClientTests
{
namespace
{

/// A wing's worth of docked ships, as the roster would name them.
constexpr std::uint32_t TALON[] = {11, 12, 13};
constexpr std::uint32_t RESERVE[] = {21, 22};

[[nodiscard]] std::vector<std::uint32_t> Ids(const RosterSelection& _selection)
{
  const std::span<const std::uint32_t> ids = _selection.Ids();
  return std::vector<std::uint32_t>{ids.begin(), ids.end()};
}

[[nodiscard]] std::vector<std::uint32_t> WaveIds(const RosterSelection& _selection, std::uint32_t _wave,
                                                 std::uint32_t _cap)
{
  const std::span<const std::uint32_t> ids = _selection.Wave(_wave, _cap);
  return std::vector<std::uint32_t>{ids.begin(), ids.end()};
}

} // namespace

TEST_CLASS(RosterSelectionTests)
{
public:
  TEST_METHOD(ATapTogglesAndSaysWhichWayItWent)
  {
    RosterSelection composer;
    Assert::IsTrue(composer.Empty());

    Assert::IsTrue(composer.Toggle(11), L"in");
    Assert::IsTrue(composer.Holds(11));
    Assert::AreEqual<std::size_t>(1, composer.Count());

    Assert::IsFalse(composer.Toggle(11), L"and out again");
    Assert::IsFalse(composer.Holds(11));
    Assert::IsTrue(composer.Empty());
  }

  TEST_METHOD(TakingAWingExtendsRatherThanReplaces)
  {
    /*
     * The print's whole argument for the gesture: "Talon plus two spare
     * Battleships from Reserve is three gestures, not a drag across sixty
     * chips." A group take that replaced the selection would make it two
     * gestures and the wrong two.
     */
    RosterSelection composer;
    composer.AddAll(TALON);
    composer.AddAll(RESERVE);

    Assert::AreEqual<std::size_t>(5, composer.Count());
    for (const std::uint32_t id : TALON)
    {
      Assert::IsTrue(composer.Holds(id));
    }
    for (const std::uint32_t id : RESERVE)
    {
      Assert::IsTrue(composer.Holds(id));
    }
  }

  TEST_METHOD(TakingAWingTwiceIsStillOneOfEach)
  {
    // Holding a header the player already took must not double the column, or
    // an order would name the same ship twice and the authority would refuse a
    // gesture that looked idempotent.
    RosterSelection composer;
    composer.AddAll(TALON);
    composer.AddAll(TALON);

    Assert::AreEqual<std::size_t>(3, composer.Count());
  }

  TEST_METHOD(ATapInsideATakenWingRemovesJustThatShip)
  {
    // The composition the print describes in one line: take the wing, drop the
    // one you want to leave behind.
    RosterSelection composer;
    composer.AddAll(TALON);
    Assert::IsFalse(composer.Toggle(12));

    Assert::AreEqual<std::size_t>(2, composer.Count());
    Assert::IsTrue(composer.Holds(11));
    Assert::IsFalse(composer.Holds(12));
    Assert::IsTrue(composer.Holds(13));
  }

  TEST_METHOD(TheOrderPickedIsTheOrderKept)
  {
    // Not sorted, and the reason shows up in `Wave`: a fleet splitting in two
    // should split somewhere the player can predict.
    RosterSelection composer;
    composer.Add(13);
    composer.Add(11);
    composer.Add(12);

    const std::vector<std::uint32_t> expected{13, 11, 12};
    Assert::IsTrue(Ids(composer) == expected);
  }

  TEST_METHOD(RemovingFromTheMiddleKeepsTheRestInOrder)
  {
    RosterSelection composer;
    composer.AddAll(TALON);
    composer.Remove(12);

    const std::vector<std::uint32_t> expected{11, 13};
    Assert::IsTrue(Ids(composer) == expected);
  }

  TEST_METHOD(RemovingSomethingNotHeldIsNotAnEdit)
  {
    RosterSelection composer;
    composer.AddAll(TALON);
    composer.Remove(99);

    Assert::AreEqual<std::size_t>(3, composer.Count());
  }

  TEST_METHOD(EntryDropsTheShipsTheRosterNoLongerHolds)
  {
    /*
     * ADR-017 §6a.2's clause, and the one that makes persistence safe. The
     * hazard is concrete: a fleet undocked from another surface while this
     * composer sat there naming it, and a command built from the stale list
     * bounces on ships the player never touched.
     */
    RosterSelection composer;
    composer.AddAll(TALON);
    composer.AddAll(RESERVE);

    // 12 and 21 left the hangar while the player was elsewhere.
    constexpr std::uint32_t STILL_DOCKED[] = {11, 13, 22, 31};
    composer.Reconcile(STILL_DOCKED);

    const std::vector<std::uint32_t> expected{11, 13, 22};
    Assert::IsTrue(Ids(composer) == expected, L"survivors keep the order they were picked in");
    Assert::IsFalse(composer.Holds(31), L"and reconciling never *adds* anything");
  }

  TEST_METHOD(AnEmptiedHangarEmptiesTheComposer)
  {
    RosterSelection composer;
    composer.AddAll(TALON);
    composer.Reconcile({});

    Assert::IsTrue(composer.Empty());
  }

  TEST_METHOD(ReconcilingChangesNothingWhenNothingLeft)
  {
    // The ordinary case, and it must not reorder: the screen reconciles on
    // every open, so a shuffle here would be a shuffle the player sees for no
    // reason.
    RosterSelection composer;
    composer.Add(13);
    composer.Add(11);
    constexpr std::uint32_t AVAILABLE[] = {11, 12, 13};
    composer.Reconcile(AVAILABLE);

    const std::vector<std::uint32_t> expected{13, 11};
    Assert::IsTrue(Ids(composer) == expected);
  }

  TEST_METHOD(ASelectionInsideTheCapIsOneWave)
  {
    RosterSelection composer;
    composer.AddAll(TALON);

    Assert::AreEqual<std::uint32_t>(1, composer.WaveCount(64));
    const std::vector<std::uint32_t> expected{11, 12, 13};
    Assert::IsTrue(WaveIds(composer, 0, 64) == expected);
  }

  TEST_METHOD(SixtySixShipsAreTwoWavesRatherThanOneRefusal)
  {
    /*
     * The print's own example, and its argument: `MAX_SHIPS_PER_ORDER` "is a
     * wire constant the player should never meet as an error". Sixty-six is
     * announced as two waves before the player commits, not refused after.
     */
    RosterSelection composer;
    for (std::uint32_t id = 1; id <= 66; ++id)
    {
      composer.Add(id);
    }

    Assert::AreEqual<std::uint32_t>(2, composer.WaveCount(64));
    Assert::AreEqual<std::size_t>(64, composer.Wave(0, 64).size());
    Assert::AreEqual<std::size_t>(2, composer.Wave(1, 64).size(), L"the remainder, not a second full wave");
  }

  TEST_METHOD(EveryShipIsInExactlyOneWave)
  {
    // The property that matters more than the counts: a split that dropped or
    // repeated a ship would undock the wrong fleet, and the arithmetic that
    // does it is the kind that is right until the boundary.
    RosterSelection composer;
    for (std::uint32_t id = 1; id <= 130; ++id)
    {
      composer.Add(id);
    }

    constexpr std::uint32_t CAP = 64;
    std::vector<std::uint32_t> seen;
    for (std::uint32_t wave = 0; wave < composer.WaveCount(CAP); ++wave)
    {
      const std::span<const std::uint32_t> ids = composer.Wave(wave, CAP);
      seen.insert(seen.end(), ids.begin(), ids.end());
    }

    Assert::AreEqual<std::size_t>(130, seen.size());
    Assert::IsTrue(seen == Ids(composer), L"and in the order the player built it");
  }

  TEST_METHOD(AWaveExactlyFillingTheCapIsNotFollowedByAnEmptyOne)
  {
    // The off-by-one this arithmetic invites: 64 ships at a cap of 64 is one
    // wave, and a second empty one would be a wave the screen announced and
    // nothing to put in it.
    RosterSelection composer;
    for (std::uint32_t id = 1; id <= 64; ++id)
    {
      composer.Add(id);
    }

    Assert::AreEqual<std::uint32_t>(1, composer.WaveCount(64));
    Assert::IsTrue(composer.Wave(1, 64).empty());
  }

  TEST_METHOD(AnEmptyComposerHasNoWaves)
  {
    const RosterSelection composer;
    Assert::AreEqual<std::uint32_t>(0, composer.WaveCount(64));
    Assert::IsTrue(composer.Wave(0, 64).empty());
  }

  TEST_METHOD(ACapOfZeroMeansNothingCanBeSentRatherThanNoLimit)
  {
    /*
     * `WorldView::ShipsPerStationCommand` answers zero from a view with no
     * game. Reading that as "no limit" would build one command naming every
     * ship in the hangar, which is the payload the cap exists to prevent.
     */
    RosterSelection composer;
    composer.AddAll(TALON);

    Assert::AreEqual<std::uint32_t>(0, composer.WaveCount(0));
    Assert::IsTrue(composer.Wave(0, 0).empty());
  }

  TEST_METHOD(TheActionThatConsumesTheSelectionClearsIt)
  {
    // Cleared by UNDOCK rather than by navigation -- the two halves of §6a.2's
    // rule, and this is the half that is not `Reconcile`.
    RosterSelection composer;
    composer.AddAll(TALON);
    composer.Clear();

    Assert::IsTrue(composer.Empty());
    Assert::AreEqual<std::uint32_t>(0, composer.WaveCount(64));
  }
};

} // namespace NeuronClientTests
