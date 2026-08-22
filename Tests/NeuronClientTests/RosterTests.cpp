#include "pch.h"
#include "CppUnitTest.h"

#include "HudRoster.h"
#include "UiDrawList.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The ELSEWHERE column (`tactical-hud.png`, ADR-017 §1, T3a).
 *
 * The blocks were laid out inside the draw until T3, which was fine while the
 * button on them was paint: nothing pressed it, so nothing could disagree with
 * it. T3 makes it the way into the hangar, and ADR-020 §5.1 is the rule that
 * follows -- laid out and hit-tested in one place, or the thing you press is
 * not the thing you see and no test can catch it, because the two halves never
 * meet.
 *
 * This is that test, which is only possible because they now do.
 */

namespace NeuronClientTests
{
namespace
{

/// The roster column at the print's proportions: 132 wide, running down the
/// left under the top bar.
[[nodiscard]] UiRect Column(float _height = 600.0f)
{
  return UiRect{0.0f, 46.0f, 132.0f, _height};
}

[[nodiscard]] LocationBlock Docked(std::uint16_t _anchor, const char* _name)
{
  LocationBlock block;
  block.name = _name;
  block.anchor = _anchor;
  block.shipCount = 3;
  block.buttonLabel = "STATION";
  block.stateLabel = "DOCKED";
  return block;
}

[[nodiscard]] LocationBlock OnTheGridBeingWatched(std::uint16_t _anchor)
{
  LocationBlock block = Docked(_anchor, "HERE");
  block.inScene = true;
  return block;
}

} // namespace

TEST_CLASS(RosterChipHitTests)
{
public:
  TEST_METHOD(APressFindsTheRowUnderIt)
  {
    // The gesture the print promised and S11 left open: `RosterRow::groupId`
    // has been "what a click on the row would hand back to the game, once rows
    // are clickable" since it was written.
    const RosterColumnTuning tuning;
    const UiRect column = Column();

    for (std::uint32_t row = 0; row < 5; ++row)
    {
      const UiRect chip = RosterChipRect(column, 1.0f, tuning, row);
      const std::uint32_t hit =
          HitRosterChip(column, 5, 1.0f, tuning, chip.x + chip.width * 0.5f, chip.y + chip.height * 0.5f);
      Assert::AreEqual<std::uint32_t>(row, hit, L"the row under the press is the row the draw put there");
    }
  }

  TEST_METHOD(APressInTheGapBetweenRowsHitsNothing)
  {
    const RosterColumnTuning tuning;
    const UiRect column = Column();

    const UiRect first = RosterChipRect(column, 1.0f, tuning, 0);
    const UiRect second = RosterChipRect(column, 1.0f, tuning, 1);
    Assert::IsTrue(second.y > first.Bottom(), L"there is a gap to press in");

    Assert::AreEqual<std::uint32_t>(
        4, HitRosterChip(column, 4, 1.0f, tuning, first.x + 4.0f, first.Bottom() + 1.0f), L"and it is not a row");
  }

  TEST_METHOD(APressOutsideTheColumnHitsNothing)
  {
    const RosterColumnTuning tuning;
    const UiRect column = Column();
    const UiRect chip = RosterChipRect(column, 1.0f, tuning, 0);

    Assert::AreEqual<std::uint32_t>(4, HitRosterChip(column, 4, 1.0f, tuning, column.Right() + 20.0f, chip.y + 4.0f),
                                    L"right of the column");
    Assert::AreEqual<std::uint32_t>(4, HitRosterChip(column, 4, 1.0f, tuning, chip.x + 4.0f, column.y - 10.0f),
                                    L"above the heading");
  }

  TEST_METHOD(APressCannotLandOnARowTheColumnHadNoRoomToDraw)
  {
    /*
     * The one way a clipped list lies about what is under a finger. The draw
     * stops at the panel's bottom edge; a hit test that did not would select a
     * wing whose row is not on screen, from a press on whatever *is* down
     * there -- and no screenshot would show it.
     */
    const RosterColumnTuning tuning;
    const UiRect shallow = Column(120.0f);
    constexpr std::uint32_t ROWS = 12;

    const std::uint32_t drawn = RosterChipsThatFit(shallow, ROWS, 1.0f, tuning);
    Assert::IsTrue(drawn > 0 && drawn < ROWS, L"the column clips, which is what this test is about");

    const UiRect lastDrawn = RosterChipRect(shallow, 1.0f, tuning, drawn - 1);
    Assert::AreEqual<std::uint32_t>(
        drawn - 1, HitRosterChip(shallow, ROWS, 1.0f, tuning, lastDrawn.x + 4.0f, lastDrawn.y + 4.0f),
        L"the last row that fitted is pressable");

    const UiRect firstDropped = RosterChipRect(shallow, 1.0f, tuning, drawn);
    Assert::AreEqual<std::uint32_t>(
        ROWS, HitRosterChip(shallow, ROWS, 1.0f, tuning, firstDropped.x + 4.0f, firstDropped.y + 4.0f),
        L"and the first one that did not is nothing at all");
  }

  TEST_METHOD(AColumnWithNoRoomAnswersNoRow)
  {
    const RosterColumnTuning tuning;
    const UiRect flat = Column(0.0f);
    Assert::AreEqual<std::uint32_t>(0, RosterChipsThatFit(flat, 4, 1.0f, tuning));
    Assert::AreEqual<std::uint32_t>(4, HitRosterChip(flat, 4, 1.0f, tuning, flat.x + 4.0f, flat.y + 4.0f));
  }

  TEST_METHOD(AnEmptyRosterIsNotPressable)
  {
    const RosterColumnTuning tuning;
    const UiRect column = Column();
    const UiRect where = RosterChipRect(column, 1.0f, tuning, 0);
    Assert::AreEqual<std::uint32_t>(0, HitRosterChip(column, 0, 1.0f, tuning, where.x + 4.0f, where.y + 4.0f),
                                    L"no rows means the miss answer, which is the count");
  }

  TEST_METHOD(TheRowsMoveWithTheUiScaleAndSoDoesTheHit)
  {
    // R9's whole permitted flexibility, and the reason the hit test takes the
    // scale rather than remembering one: a press is tested against the rect the
    // draw used *this* frame.
    const RosterColumnTuning tuning;
    const UiRect column = Column();

    const UiRect atOne = RosterChipRect(column, 1.0f, tuning, 2);
    const UiRect atTwo = RosterChipRect(column, 2.0f, tuning, 2);
    Assert::IsTrue(atTwo.y > atOne.y, L"the third row is further down at twice the scale");

    const float x = atTwo.x + 4.0f;
    const float y = atTwo.y + 4.0f;
    Assert::AreEqual<std::uint32_t>(2, HitRosterChip(column, 8, 2.0f, tuning, x, y));

    // The same pixel, a different row -- which is the whole reason the hit test
    // is handed the scale rather than remembering one. A press is tested
    // against the rects the draw used *this* frame.
    const std::uint32_t atOneScale = HitRosterChip(column, 8, 1.0f, tuning, x, y);
    Assert::IsTrue(atOneScale < 8, L"still lands on a row at 1.0x");
    Assert::IsTrue(atOneScale != 2, L"but not the same one, because the rows are packed tighter");
  }
};

TEST_CLASS(LocationBlockColumnTests)
{
public:
  TEST_METHOD(EveryPlaceTheGameNamedGetsABlock)
  {
    const LocationBlock blocks[] = {Docked(11, "VESTA-3"), Docked(12, "CERES-1")};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};

    const std::uint32_t count = BuildLocationBlockColumn(blocks, Column(), 200.0f, 1.0f, RosterColumnTuning{}, laid);

    Assert::AreEqual<std::uint32_t>(2, count);
    Assert::AreEqual<std::uint16_t>(11, laid[0].anchor);
    Assert::AreEqual<std::uint16_t>(12, laid[1].anchor);
    Assert::IsTrue(laid[1].panel.y > laid[0].panel.y, L"the column runs downward");
  }

  TEST_METHOD(TheFleetOnScreenIsNotAlsoAPlaceElsewhere)
  {
    /*
     * The ordinary case -- one fleet, standing where you are looking -- is a
     * column with nothing in it. Listing it would be one fleet counted twice on
     * one HUD, with nothing to tell the player which count is the lie.
     */
    const LocationBlock blocks[] = {OnTheGridBeingWatched(9)};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};

    Assert::AreEqual<std::uint32_t>(0, BuildLocationBlockColumn(blocks, Column(), 200.0f, 1.0f, RosterColumnTuning{}, laid));
  }

  TEST_METHOD(ASkippedBlockLeavesNoGapBehindIt)
  {
    // The skip happens before the pen moves, so the blocks that *are* drawn are
    // contiguous -- a gap would read as a block that failed to draw.
    const LocationBlock blocks[] = {OnTheGridBeingWatched(9), Docked(11, "VESTA-3"), Docked(12, "CERES-1")};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};

    const RosterColumnTuning tuning;
    const std::uint32_t count = BuildLocationBlockColumn(blocks, Column(), 200.0f, 1.0f, tuning, laid);

    Assert::AreEqual<std::uint32_t>(2, count);
    Assert::AreEqual(200.0f, laid[0].panel.y, 1e-4f, L"the first block drawn starts at the top of the column");
    Assert::AreEqual(200.0f + tuning.blockHeight + tuning.blockGap, laid[1].panel.y, 1e-4f);
    Assert::AreEqual<std::uint32_t>(1, laid[0].block, L"the layout points back at the block it is about");
  }

  TEST_METHOD(TheButtonSitsInsideItsOwnBlock)
  {
    // Not an assertion about pixels so much as about containment: a button that
    // hung outside its panel would be pressable over the block beneath it.
    const LocationBlock blocks[] = {Docked(11, "VESTA-3")};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};
    (void)BuildLocationBlockColumn(blocks, Column(), 200.0f, 1.0f, RosterColumnTuning{}, laid);

    const LocationBlockLayout& block = laid[0];
    Assert::IsTrue(block.hasButton);
    Assert::IsTrue(block.button.x >= block.panel.x);
    Assert::IsTrue(block.button.Right() <= block.panel.Right());
    Assert::IsTrue(block.button.y >= block.panel.y);
    Assert::IsTrue(block.button.Bottom() <= block.panel.Bottom());
    Assert::IsTrue(block.button.y > block.panel.y + block.panel.height * 0.5f, L"along the bottom, under the readouts");
  }

  TEST_METHOD(APlaceTheGameGaveNoWordForGetsNoButton)
  {
    /*
     * `LocationBlock::buttonLabel` is null for a block with nowhere to go, and
     * the engine must not invent one: drawing "STATION" itself would be the
     * client deciding that the place is a station and that it has an interior,
     * which are two facts about this game.
     */
    LocationBlock silent = Docked(11, "VESTA-3");
    silent.buttonLabel = nullptr;
    const LocationBlock blocks[] = {silent};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};

    const std::uint32_t count = BuildLocationBlockColumn(blocks, Column(), 200.0f, 1.0f, RosterColumnTuning{}, laid);
    Assert::AreEqual<std::uint32_t>(1, count, L"the block is still a name and a count");
    Assert::IsFalse(laid[0].hasButton);

    // And the hit test never offers it, wherever the press lands.
    const std::span<const LocationBlockLayout> span{laid, count};
    Assert::IsNull(HitLocationBlockButton(span, laid[0].panel.x + 4.0f, laid[0].panel.Bottom() - 4.0f));
  }

  TEST_METHOD(PressingTheButtonFindsTheBlockAndPressingTheBlockDoesNot)
  {
    // The whole panel is not a target: a player reading the count is not asking
    // to be taken somewhere.
    const LocationBlock blocks[] = {Docked(11, "VESTA-3"), Docked(12, "CERES-1")};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};
    const std::uint32_t count = BuildLocationBlockColumn(blocks, Column(), 200.0f, 1.0f, RosterColumnTuning{}, laid);
    const std::span<const LocationBlockLayout> span{laid, count};

    const LocationBlockLayout& second = laid[1];
    const LocationBlockLayout* hit =
        HitLocationBlockButton(span, second.button.x + second.button.width * 0.5f,
                               second.button.y + second.button.height * 0.5f);
    Assert::IsNotNull(hit);
    Assert::AreEqual<std::uint16_t>(12, hit->anchor, L"the press knows which place it was about");

    Assert::IsNull(HitLocationBlockButton(span, second.panel.x + 2.0f, second.panel.y + 2.0f),
                   L"the name and count are a readout, not a control");
    Assert::IsNull(HitLocationBlockButton(span, 900.0f, 900.0f), L"nowhere near the column");
  }

  TEST_METHOD(ABlockThatWouldRunPastTheColumnIsDroppedRatherThanShrunk)
  {
    /*
     * `Tactical`'s declared overflow rule is **drop** (ADR-020 §7), and the
     * count returned is what fitted -- so the caller draws what it was given
     * and never a block half over the command row.
     */
    const RosterColumnTuning tuning;
    const LocationBlock blocks[] = {Docked(11, "A"), Docked(12, "B"), Docked(13, "C")};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};

    // Room for two blocks and a sliver.
    const float top = 100.0f;
    const float room = 2.0f * tuning.blockHeight + tuning.blockGap + 4.0f;
    const UiRect column{0.0f, top, 132.0f, room};

    const std::uint32_t count = BuildLocationBlockColumn(blocks, column, top, 1.0f, tuning, laid);
    Assert::AreEqual<std::uint32_t>(2, count);
    Assert::IsTrue(laid[1].panel.Bottom() <= column.Bottom());
  }

  TEST_METHOD(TheColumnNeverWritesPastTheSpanItWasGiven)
  {
    // A HUD must not allocate to describe itself, so the caller's array is the
    // bound -- and a game reporting more places than it holds must not be the
    // way past it.
    const LocationBlock blocks[] = {Docked(11, "A"), Docked(12, "B"), Docked(13, "C")};
    LocationBlockLayout laid[2] = {};

    Assert::AreEqual<std::uint32_t>(2, BuildLocationBlockColumn(blocks, Column(), 200.0f, 1.0f, RosterColumnTuning{},
                                                                std::span<LocationBlockLayout>{laid, 2}));
  }

  TEST_METHOD(EverythingScalesWithTheUiScale)
  {
    // R9's whole permitted flexibility: constants times the scale, and nothing
    // that reflows.
    const LocationBlock blocks[] = {Docked(11, "A"), Docked(12, "B")};
    const RosterColumnTuning tuning;

    LocationBlockLayout small[MAX_LOCATION_BLOCKS] = {};
    LocationBlockLayout large[MAX_LOCATION_BLOCKS] = {};
    (void)BuildLocationBlockColumn(blocks, Column(), 100.0f, 1.0f, tuning, small);
    (void)BuildLocationBlockColumn(blocks, Column(1200.0f), 100.0f, 2.0f, tuning, large);

    Assert::AreEqual(tuning.blockHeight, small[0].panel.height, 1e-4f);
    Assert::AreEqual(tuning.blockHeight * 2.0f, large[0].panel.height, 1e-4f);
    Assert::AreEqual(tuning.buttonHeight * 2.0f, large[0].button.height, 1e-4f);
  }

  TEST_METHOD(ADegenerateScaleIsTreatedAsOne)
  {
    // The same guard `BuildCommandRow` takes, and for the same reason: a zero
    // scale would collapse every rect to nothing and the HUD would be gone with
    // no error anywhere.
    const LocationBlock blocks[] = {Docked(11, "A")};
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};
    (void)BuildLocationBlockColumn(blocks, Column(), 100.0f, 0.0f, RosterColumnTuning{}, laid);

    Assert::AreEqual(RosterColumnTuning{}.blockHeight, laid[0].panel.height, 1e-4f);
  }
};


TEST_CLASS(RosterColumnGeometryTests)
{
public:
  TEST_METHOD(ChipsStackAtAFixedPitch)
  {
    const RosterColumnTuning tuning;
    const UiRect column = Column();

    const UiRect first = RosterChipRect(column, 1.0f, tuning, 0);
    const UiRect second = RosterChipRect(column, 1.0f, tuning, 1);

    Assert::AreEqual(tuning.chipHeight, first.height, 1e-4f);
    Assert::AreEqual(first.y + tuning.chipHeight + tuning.chipGap, second.y, 1e-4f);
    Assert::AreEqual(first.x, second.x, 1e-4f);
  }

  TEST_METHOD(AnOverflowingChipIsReportedRatherThanClamped)
  {
    // Clamping would stack every overflowing row on the last visible one, which
    // draws as one unreadable chip rather than as a list that ran out of room.
    const RosterColumnTuning tuning;
    const UiRect column{0.0f, 0.0f, 132.0f, 3.0f * tuning.chipHeight};

    const std::uint32_t fitted = RosterChipsThatFit(column, 16, 1.0f, tuning);
    Assert::IsTrue(fitted < 16);
    Assert::IsTrue(RosterChipRect(column, 1.0f, tuning, fitted).Bottom() > column.Bottom(),
                   L"the first chip that did not fit really does not fit");
    Assert::IsTrue(RosterChipRect(column, 1.0f, tuning, fitted - 1).Bottom() <= column.Bottom());
  }

  TEST_METHOD(AColumnWithNoRoomFitsNoChips)
  {
    const RosterColumnTuning tuning;
    Assert::AreEqual<std::uint32_t>(0, RosterChipsThatFit(UiRect{0.0f, 0.0f, 132.0f, 10.0f}, 8, 1.0f, tuning));
  }

  TEST_METHOD(TheBlocksBeginWhereTheDrawUsedToPutThem)
  {
    /*
     * The column's vertical arithmetic moved out of `BuildHud` so that the
     * block button could be hit-tested where it is laid out (ADR-020 §5.1).
     * This is the proof that the move did not also move the column: the running
     * total the draw used to keep, written out here, against the function that
     * replaced it.
     */
    const RosterColumnTuning tuning;
    const UiRect column = Column();
    constexpr float SCALE = 1.0f;

    for (std::uint32_t rows = 0; rows <= 6; ++rows)
    {
      float pen = column.y + tuning.padding * SCALE + tuning.headingHeight * SCALE;
      for (std::uint32_t row = 0; row < rows; ++row)
      {
        if (pen + tuning.chipHeight * SCALE > column.Bottom()) { break; }
        pen += (tuning.chipHeight + tuning.chipGap) * SCALE;
      }
      pen += tuning.chipGap * SCALE;
      pen += tuning.headingHeight * SCALE;

      Assert::AreEqual(pen, RosterBlocksTop(column, rows, SCALE, tuning), 1e-4f);
    }
  }

  TEST_METHOD(TheHeadingSitsOneLineAboveTheFirstBlock)
  {
    // The caller draws `ELSEWHERE` at `blocksTop - headingHeight`, and only when
    // a block actually fitted -- "no ships anywhere but here" is not a place,
    // and a heading over nothing says it is.
    const RosterColumnTuning tuning;
    const UiRect column = Column();

    const float blocksTop = RosterBlocksTop(column, 2, 1.0f, tuning);
    LocationBlockLayout laid[MAX_LOCATION_BLOCKS] = {};
    const LocationBlock blocks[] = {Docked(11, "VESTA-3")};
    const std::uint32_t count = BuildLocationBlockColumn(blocks, column, blocksTop, 1.0f, tuning, laid);

    Assert::AreEqual<std::uint32_t>(1, count);
    Assert::AreEqual(blocksTop, laid[0].panel.y, 1e-4f);
    Assert::IsTrue(blocksTop - tuning.headingHeight > RosterChipRect(column, 1.0f, tuning, 1).Bottom(),
                   L"the heading clears the last chip");
  }
};

} // namespace NeuronClientTests
