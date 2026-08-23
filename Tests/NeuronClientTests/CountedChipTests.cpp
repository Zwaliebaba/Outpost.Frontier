#include "pch.h"
#include "CppUnitTest.h"

#include "CountedChip.h"

#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The counted chip (U3d-c, ADR-022 §5d, `tactical-icon-system.png` §6).
 *
 * The last step of U3d, and the one thing on it that is a *sentence* rather
 * than a number: `culledCount` reaches the client and the question is what the
 * player is told. ADR-022 states the rule twice over -- *"Culling is stated to
 * the player, never silent"* and *"the player is never told a grid is empty
 * when it is not; they are told how many they are not being shown"* -- so the
 * two things worth asserting are that a withheld count is said and that a zero
 * one is not.
 *
 * Nothing here needs a window: a count goes in and a string and a rect come
 * out.
 */

namespace NeuronClientTests
{

TEST_CLASS(CountedChipTests)
{
public:
  TEST_METHOD(NothingWithheldSaysNothing)
  {
    /*
     * The half of the rule that is easy to get backwards. ADR-022 asks for the
     * *count* never to be silent; it does not ask for a chip on every frame of
     * every grid. A chip reading "none hidden" is a chip a player has to read
     * before they can ignore it, on the surface where attention is the budget.
     */
    char buffer[COUNTED_CHIP_TEXT_BYTES] = {};
    Assert::AreEqual(std::string(), std::string(CountedChipText(0, buffer, sizeof buffer)),
                     L"a fully-sent grid draws no chip");
  }

  TEST_METHOD(AWithheldCountIsStatedAndReadsAsAnAddition)
  {
    /*
     * `+` because the count is in addition to what is on screen rather than a
     * total. A player reading `12 NOT SHOWN` beside eight visible hulls should
     * not have to work out whether the twelve includes the eight.
     */
    char buffer[COUNTED_CHIP_TEXT_BYTES] = {};
    const std::string said = CountedChipText(12, buffer, sizeof buffer);
    Assert::IsTrue(said.find("12") != std::string::npos, L"the number is in the sentence");
    Assert::IsTrue(said.front() == '+', L"and reads as an addition rather than a total");
    Assert::IsTrue(said.find("NOT SHOWN") != std::string::npos, L"in the game's own words");
  }

  TEST_METHOD(TheWiresLargestCountStillFits)
  {
    /*
     * `DeltaHeader.culledCount` is a `u16`, so this is the longest sentence the
     * protocol can ask for -- and `COUNTED_CHIP_TEXT_BYTES` is sized from it
     * rather than from a grid's 1,024-entity cap, because a buffer sized to the
     * likely case is a buffer that truncates on the unlikely one.
     */
    char buffer[COUNTED_CHIP_TEXT_BYTES] = {};
    const std::string said = CountedChipText(MAX_COUNTED_CHIP_VALUE, buffer, sizeof buffer);
    Assert::IsTrue(said.find("65535") != std::string::npos, L"the wire's largest count is not truncated");
    Assert::IsTrue(said.size() < COUNTED_CHIP_TEXT_BYTES, L"and it fits the buffer with its terminator");
  }

  TEST_METHOD(ACountPastTheWireIsPinnedRatherThanWrapped)
  {
    // Only reachable if somebody widens the header and forgets this file. A
    // pinned maximum is imprecise; a wrapped one would report `+7` for 65,543,
    // which is the kind of wrong a player would act on.
    char buffer[COUNTED_CHIP_TEXT_BYTES] = {};
    const std::string said = CountedChipText(MAX_COUNTED_CHIP_VALUE + 8, buffer, sizeof buffer);
    Assert::IsTrue(said.find("65535") != std::string::npos, L"a count past the wire pins to the wire's largest");
    Assert::IsTrue(said.find("+7 ") == std::string::npos, L"rather than wrapping to a small, plausible lie");
  }

  TEST_METHOD(ABufferTooSmallIsRefusedRatherThanOverrun)
  {
    // The `UiDrawList` contract applied to a smaller thing: a caller that hands
    // over nothing gets nothing back rather than a write past the end.
    Assert::AreEqual(std::string(), std::string(CountedChipText(5, nullptr, 0)), L"no buffer, no sentence");
    char one[1] = {'x'};
    const std::string said = CountedChipText(5, one, sizeof one);
    Assert::IsTrue(said.empty(), L"and a one-byte buffer comes back terminated and empty");
  }

  TEST_METHOD(TheChipSitsInsideTheZoneItWasGiven)
  {
    /*
     * Bottom-left of the *world* zone rather than of the viewport, because the
     * statement is about this view and the HUD is a border around it. Clamped
     * to the zone's own origin: the world rect does not start at the viewport's
     * corner, so a clamp against zero would put the chip under the roster.
     */
    const CountedChipTuning tuning;
    const UiRect world{240.0f, 60.0f, 900.0f, 700.0f};
    const UiRect chip = CountedChipRect(world, 13, 8.0f, 1.0f, tuning);

    Assert::IsTrue(chip.x >= world.x, L"the chip starts inside the zone");
    Assert::IsTrue(chip.Right() <= world.Right() + 0.001f, L"and ends inside it");
    Assert::IsTrue(chip.Bottom() <= world.Bottom() + 0.001f, L"and sits above the zone's floor");
    Assert::IsTrue(chip.y >= world.y, L"and below its ceiling");
    Assert::IsTrue(chip.y > world.y + world.height * 0.5f, L"in the lower half, which is where it belongs");
  }

  TEST_METHOD(TheChipIsSizedAgainstTheWordItWillHold)
  {
    // The caller measures the string with the same `TextCellCount` it draws
    // with and hands the count over, so the chip cannot be sized against a
    // different string than the one inside it.
    const CountedChipTuning tuning;
    const UiRect world{0.0f, 0.0f, 1200.0f, 800.0f};

    const UiRect narrow = CountedChipRect(world, 8, 8.0f, 1.0f, tuning);
    const UiRect wide = CountedChipRect(world, 16, 8.0f, 1.0f, tuning);
    Assert::IsTrue(wide.width > narrow.width, L"a longer word makes a wider chip");
    Assert::AreEqual(64.0f, wide.width - narrow.width, 0.001f, L"by exactly the cells it added");
  }

  TEST_METHOD(AZoneTooSmallForTheChipClampsRatherThanInverting)
  {
    // `ResolveUiLayout`'s posture: dragging a window small must not put a quad
    // with a flipped winding on screen.
    const CountedChipTuning tuning;
    const UiRect tiny{10.0f, 10.0f, 4.0f, 3.0f};
    const UiRect chip = CountedChipRect(tiny, 13, 8.0f, 1.0f, tuning);

    Assert::IsTrue(chip.width >= 0.0f, L"never a negative width");
    Assert::IsTrue(chip.height >= 0.0f, L"nor a negative height");
    Assert::IsTrue(chip.x >= tiny.x, L"and never outside the zone it was given");
    Assert::IsTrue(chip.y >= tiny.y);
  }

  TEST_METHOD(TheChipScalesWithTheInterface)
  {
    // Everything on this HUD is its design size times the UI scale (R9), and a
    // readout is no exception -- it is not a touch target, so the 48 px floor
    // does not apply and there is nothing to hold it up at small scales.
    const CountedChipTuning tuning;
    const UiRect world{0.0f, 0.0f, 1200.0f, 800.0f};

    const UiRect small = CountedChipRect(world, 13, 8.0f * 0.8f, 0.8f, tuning);
    const UiRect large = CountedChipRect(world, 13, 8.0f * 1.6f, 1.6f, tuning);
    Assert::AreEqual(tuning.height * 0.8f, small.height, 0.001f, L"the chip scales down");
    Assert::AreEqual(tuning.height * 1.6f, large.height, 0.001f, L"and up");
    Assert::IsTrue(large.width > small.width, L"and its word scales with it");
  }
};

} // namespace NeuronClientTests
