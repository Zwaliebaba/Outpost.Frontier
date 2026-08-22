#include "pch.h"
#include "CppUnitTest.h"

#include "StationScreen.h"
#include "StationView.h"
#include "UiDrawList.h"

#include <cstdint>
#include <iterator>
#include <span>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The station screen's geometry (`station-screen.png` §1, ADR-020 §5.1, §7,
 * T3b).
 *
 * ADR-020 §5.1 asks for one place that lays a control out and hit-tests it, and
 * the argument for the rule is a test that could not otherwise be written: with
 * the draw and the input handler each holding their own arithmetic, "the thing
 * you press is the thing you see" is not a claim anything can check, because
 * the two halves never meet. They meet in `StationScreen.h`, so here it is
 * checked.
 *
 * The numbers below are the print's, at the size it was authored -- 1440x900 --
 * which is what ADR-020 §7 means by calling the prints normative. Everything
 * else in the file is what happens when the window is not that size, which is
 * the half a print cannot state.
 */

namespace NeuronClientTests
{
namespace
{

constexpr std::uint32_t PRINT_WIDTH = 1440;
constexpr std::uint32_t PRINT_HEIGHT = 900;

/// The print's seven tabs, in its order, with the tags it draws against the six
/// that have no service behind them yet. Ids deliberately unlike the indices,
/// because a hit hands back the game's number and nothing else.
constexpr StationTab PRINT_TABS[] = {
    {"HANGAR", nullptr, 101, true},   {"CARGO", "SOON", 102, false},   {"REFINERY", "SOON", 103, false},
    {"REPAIR", "FREE", 104, false},   {"REFIT", "FUTURE", 105, false}, {"CONSTRUCT", "FUTURE", 106, false},
    {"MARKET", "FUTURE", 107, false},
};

[[nodiscard]] StationGroup Wing(const char* _name, std::uint16_t _idTag, std::uint16_t _chips)
{
  StationGroup group;
  group.name = _name;
  group.idTag = _idTag;
  group.chipCount = _chips;
  return group;
}

[[nodiscard]] StationChip Ship(std::uint32_t _id, std::uint16_t _group)
{
  StationChip chip;
  chip.shipId = _id;
  chip.className = "FIGHTER";
  chip.group = _group;
  return chip;
}

/// How many chip rows the layout actually emits for a column it cannot run out
/// of chips for. The number `StationVisibleRows` has to agree with.
[[nodiscard]] std::uint32_t RowsDrawn(const UiRect& _panel, float _scale, const StationScreenTuning& _tuning)
{
  StationChip chips[64];
  for (std::uint32_t index = 0; index < std::size(chips); ++index)
  {
    chips[index] = Ship(index + 1, 0);
  }
  const StationGroup groups[] = {Wing("ONE", 1, static_cast<std::uint16_t>(std::size(chips)))};

  StationColumnRect columns[1];
  StationChipRect laid[64];
  return BuildStationColumns(groups, chips, _panel, 0, _scale, _tuning, columns, laid).chips;
}

} // namespace

TEST_CLASS(StationScreenLayoutTests)
{
public:
  TEST_METHOD(ThePrintsZonesComeOutAtThePrintsSize)
  {
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::AreEqual(46.0f, layout.statusBar.height, 1e-4f, L"status bar");
    Assert::AreEqual(46.0f, layout.tabRow.y, 1e-4f, L"tab row sits under it");
    Assert::AreEqual(52.0f, layout.tabRow.height, 1e-4f, L"tab row");
    Assert::AreEqual(98.0f, layout.roster.y, 1e-4f, L"content starts under both");
    Assert::AreEqual(1032.0f, layout.roster.width, 1e-4f, L"the print's roster panel");
    Assert::AreEqual(408.0f, layout.composer.width, 1e-4f, L"the print's composer panel");
    Assert::AreEqual(1032.0f, layout.composer.x, 1e-4f, L"and it starts where the roster ends");
  }

  TEST_METHOD(TheZonesTileTheViewportWithoutAGap)
  {
    /*
     * Not decoration: the surface covers the world (ADR-020 §1), so a seam
     * between two zones is a strip of last frame's tactical view showing
     * through a screen that is supposed to have replaced it.
     */
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::AreEqual(layout.statusBar.Bottom(), layout.tabRow.y, 1e-4f);
    Assert::AreEqual(layout.tabRow.Bottom(), layout.roster.y, 1e-4f);
    Assert::AreEqual(layout.tabRow.Bottom(), layout.composer.y, 1e-4f);
    Assert::AreEqual(layout.roster.Right(), layout.composer.x, 1e-4f);
    Assert::AreEqual(layout.viewport.Right(), layout.composer.Right(), 1e-4f);
    Assert::AreEqual(layout.viewport.Bottom(), layout.roster.Bottom(), 1e-4f);
    Assert::AreEqual(layout.viewport.Bottom(), layout.composer.Bottom(), 1e-4f);
  }

  TEST_METHOD(ScaleMultipliesTheChromeAndTheRosterTakesWhatIsLeft)
  {
    // R9's entire permitted flexibility: the numbers are the print's times the
    // scale. Nothing reflows, so the roster is the only thing that changes
    // shape, and it changes by subtraction.
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(2880, 1800, 2.0f, tuning);

    Assert::AreEqual(92.0f, layout.statusBar.height, 1e-4f);
    Assert::AreEqual(104.0f, layout.tabRow.height, 1e-4f);
    Assert::AreEqual(816.0f, layout.composer.width, 1e-4f);
    Assert::AreEqual(2064.0f, layout.roster.width, 1e-4f);
    Assert::AreEqual(2.0f, layout.scale, 1e-4f);
  }

  TEST_METHOD(AScaleOfZeroIsReadAsOne)
  {
    // A scale of zero would divide the chip pitch into nothing and put every
    // row at the same y. Refused at the door, like `ResolveUiLayout`'s.
    const StationScreenTuning tuning;
    const StationScreenLayout zero = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 0.0f, tuning);
    const StationScreenLayout one = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::AreEqual(one.scale, zero.scale, 1e-4f);
    Assert::AreEqual(one.tabRow.height, zero.tabRow.height, 1e-4f);
    Assert::AreEqual(one.undock.height, zero.undock.height, 1e-4f);
  }

  TEST_METHOD(AViewportSmallerThanItsOwnChromeClampsRatherThanInverting)
  {
    /*
     * Dragging a window down to nothing must not put a quad with a flipped
     * winding on screen -- `ResolveUiLayout`'s posture, and worth restating
     * here because this layout has far more rects to get wrong: the panels are
     * allowed to overlap, but none of them may have a negative size.
     */
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(200, 40, 1.0f, tuning);

    const UiRect every[] = {layout.statusBar, layout.tabRow,  layout.roster,     layout.composer,
                            layout.selectionHead, layout.formation, layout.undock, layout.parking,
                            layout.assignWing,    layout.newWing};
    for (const UiRect& rect : every)
    {
      Assert::IsTrue(rect.width >= 0.0f, L"no negative width");
      Assert::IsTrue(rect.height >= 0.0f, L"no negative height");
    }
    Assert::IsTrue(layout.statusBar.Bottom() <= 40.0f, L"and nothing taller than the window it is in");
  }

  TEST_METHOD(ANarrowWindowShrinksTheComposerRatherThanGivingTheRosterANegativeWidth)
  {
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(300, PRINT_HEIGHT, 1.0f, tuning);

    Assert::AreEqual(300.0f, layout.composer.width, 1e-4f);
    Assert::AreEqual(0.0f, layout.roster.width, 1e-4f);
    Assert::AreEqual(0.0f, layout.composer.x, 1e-4f);
  }

  TEST_METHOD(TheComposerStacksItsControlsInThePrintsOrder)
  {
    // What is selected, the formation it leaves in, the action, the promise the
    // action makes, and the wing assignment under it.
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::IsTrue(layout.selectionHead.Bottom() <= layout.formation.y, L"selection above formation");
    Assert::IsTrue(layout.formation.Bottom() <= layout.undock.y, L"formation above UNDOCK");
    Assert::IsTrue(layout.undock.Bottom() <= layout.parking.y, L"UNDOCK above the diagram");
    Assert::IsTrue(layout.parking.Bottom() <= layout.assignWing.y, L"diagram above the wing dropdown");
    Assert::IsTrue(layout.assignWing.Bottom() <= layout.newWing.y, L"dropdown above NEW WING");
  }

  TEST_METHOD(EveryComposerControlIsInsideTheComposerPanel)
  {
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    const UiRect controls[] = {layout.selectionHead, layout.formation,  layout.undock,
                               layout.parking,       layout.assignWing, layout.newWing};
    for (const UiRect& rect : controls)
    {
      Assert::IsTrue(rect.x >= layout.composer.x, L"left");
      Assert::IsTrue(rect.Right() <= layout.composer.Right() + 1e-3f, L"right");
      Assert::IsTrue(rect.y >= layout.composer.y, L"top");
      Assert::IsTrue(rect.Bottom() <= layout.composer.Bottom() + 1e-3f, L"bottom");
    }
  }

  TEST_METHOD(UndockIsTheLargestControlAndTheOnlyOneWithItsOwnHeight)
  {
    // The print's 68 px, and its reason: it is the only primary action on the
    // screen, and it is always drawn -- disabled with a reason rather than
    // hidden, so a player never hunts for a button that went away.
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::AreEqual(68.0f, layout.undock.height, 1e-4f);
    Assert::IsTrue(layout.undock.height > layout.formation.height, L"taller than a dropdown");
    Assert::IsTrue(layout.undock.height > layout.newWing.height, L"and than the secondary action");
  }

  TEST_METHOD(TheParkingDiagramIsSquareAndCentredInTheSlackItIsGiven)
  {
    /*
     * ADR-017 §4 made the player a promise about self-parking, and the diagram
     * is where they see it kept. A ring system drawn out of round is a readout
     * of a different arrangement, so the diagram holds its aspect and gives
     * back the slack (ADR-020 §7).
     */
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::AreEqual(layout.parking.width, layout.parking.height, 1e-3f, L"square");
    Assert::AreEqual(364.0f, layout.parking.width, 1e-3f, L"and as wide as the column allows");

    const float leftAir = layout.parking.x - (layout.composer.x + 22.0f);
    const float rightAir = (layout.composer.Right() - 22.0f) - layout.parking.Right();
    Assert::AreEqual(leftAir, rightAir, 1e-3f, L"centred across");
  }

  TEST_METHOD(AShortWindowLetterboxesTheDiagramRatherThanSquashingIt)
  {
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, 600, 1.0f, tuning);

    Assert::AreEqual(layout.parking.width, layout.parking.height, 1e-3f, L"still square");
    Assert::IsTrue(layout.parking.width < 364.0f, L"and narrower than the column, because the room ran out first");

    const float topAir = layout.parking.y - layout.undock.Bottom();
    const float bottomAir = layout.assignWing.y - layout.parking.Bottom();
    Assert::IsTrue(topAir >= -1e-3f && bottomAir >= -1e-3f, L"and it fits between the controls either side");
  }

  TEST_METHOD(TheDiagramGivesUpItsRoomBeforeTheControlsUnderItDo)
  {
    // The reason it is laid out from both ends: every control has a height a
    // finger needs and the diagram does not, so the diagram is what shrinks.
    const StationScreenTuning tuning;
    const StationScreenLayout tall = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationScreenLayout squat = ResolveStationScreen(PRINT_WIDTH, 560, 1.0f, tuning);

    Assert::AreEqual(tall.newWing.height, squat.newWing.height, 1e-4f, L"NEW WING keeps its height");
    Assert::AreEqual(tall.undock.height, squat.undock.height, 1e-4f, L"so does UNDOCK");
    Assert::IsTrue(squat.parking.height < tall.parking.height, L"the diagram is what paid");
    Assert::IsTrue(squat.newWing.Bottom() <= 560.0f + 1e-3f, L"and nothing hangs off the bottom");
  }
};

TEST_CLASS(StationTabRowTests)
{
public:
  TEST_METHOD(EveryTabIsSizedToItsOwnWord)
  {
    /*
     * Not to the widest word, and not to a share of the row. The face is
     * monospace, so a tab's width is what it says plus air -- which is also
     * what makes the drop rule below computable without a measuring pass.
     */
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    StationTabButton buttons[MAX_STATION_TABS];
    const std::uint32_t laid = BuildStationTabRow(PRINT_TABS, layout.tabRow, 1.0f, tuning, buttons);
    Assert::AreEqual<std::uint32_t>(7, laid, L"all seven fit at the print's width");

    // HANGAR is six cells and carries no tag: six cells of word plus two either
    // side of air, at eight pixels a cell.
    Assert::AreEqual(80.0f, buttons[0].rect.width, 1e-3f, L"HANGAR");
    // REFINERY is two cells longer and carries SOON, which is four more plus
    // the space before it.
    Assert::AreEqual(136.0f, buttons[2].rect.width, 1e-3f, L"REFINERY SOON");
  }

  TEST_METHOD(ATabWithATagIsWiderThanTheSameWordWithout)
  {
    const StationScreenTuning tuning;
    const UiRect row{0.0f, 46.0f, 1440.0f, 52.0f};
    const StationTab tabs[] = {{"REPAIR", nullptr, 1, true}, {"REPAIR", "FREE", 2, false}};

    StationTabButton buttons[2];
    Assert::AreEqual<std::uint32_t>(2, BuildStationTabRow(tabs, row, 1.0f, tuning, buttons));
    Assert::AreEqual(40.0f, buttons[1].rect.width - buttons[0].rect.width, 1e-3f, L"FREE and the space before it");
  }

  TEST_METHOD(TabsRunLeftToRightFromTheRowsInsetWithItsGapBetweenThem)
  {
    const StationScreenTuning tuning;
    const UiRect row{0.0f, 46.0f, 1440.0f, 52.0f};

    StationTabButton buttons[MAX_STATION_TABS];
    const std::uint32_t laid = BuildStationTabRow(PRINT_TABS, row, 1.0f, tuning, buttons);

    Assert::AreEqual(26.0f, buttons[0].rect.x, 1e-3f, L"inset from the edge");
    for (std::uint32_t index = 1; index < laid; ++index)
    {
      Assert::AreEqual(2.0f, buttons[index].rect.x - buttons[index - 1].rect.Right(), 1e-3f, L"the row's gap");
    }
    Assert::AreEqual(46.0f, buttons[0].rect.y, 1e-3f, L"and each fills the row's height");
    Assert::AreEqual(52.0f, buttons[0].rect.height, 1e-3f);
  }

  TEST_METHOD(ATabThatWouldRunPastTheEdgeIsDroppedWholeRatherThanShrunk)
  {
    /*
     * `CommandRow`'s rule, and the same argument with more at stake: a row that
     * reflowed at some window width would be a layout engine (R9 says this pass
     * is not one), and worse, the tab under a player's finger would move the
     * day a service landed.
     */
    const StationScreenTuning tuning;
    const UiRect narrow{0.0f, 46.0f, 300.0f, 52.0f};

    StationTabButton buttons[MAX_STATION_TABS];
    const std::uint32_t laid = BuildStationTabRow(PRINT_TABS, narrow, 1.0f, tuning, buttons);

    Assert::AreEqual<std::uint32_t>(2, laid, L"two fit and the rest are dropped");
    Assert::AreEqual(80.0f, buttons[0].rect.width, 1e-3f, L"at full width");
    Assert::AreEqual(112.0f, buttons[1].rect.width, 1e-3f, L"both of them");
    Assert::IsTrue(buttons[1].rect.Right() <= narrow.Right() - 26.0f, L"and inside the inset");
  }

  TEST_METHOD(LayingOutStopsAtTheCallersBuffer)
  {
    const StationScreenTuning tuning;
    const UiRect row{0.0f, 46.0f, 1440.0f, 52.0f};

    StationTabButton buttons[3];
    Assert::AreEqual<std::uint32_t>(3, BuildStationTabRow(PRINT_TABS, row, 1.0f, tuning, buttons));
  }

  TEST_METHOD(APressFindsTheTabUnderItAndHandsBackTheGamesId)
  {
    const StationScreenTuning tuning;
    const UiRect row{0.0f, 46.0f, 1440.0f, 52.0f};

    StationTabButton buttons[MAX_STATION_TABS];
    const std::uint32_t laid = BuildStationTabRow(PRINT_TABS, row, 1.0f, tuning, buttons);
    const std::span<const StationTabButton> all{buttons, laid};

    const StationTabButton* hit = HitStationTab(all, buttons[0].rect.x + 4.0f, 60.0f);
    Assert::IsNotNull(hit);
    Assert::AreEqual<std::uint16_t>(101, hit->id, L"the game's number, not the index");
    Assert::AreEqual<std::uint32_t>(0, hit->tab);
  }

  TEST_METHOD(ADisabledTabRefusesThePressRatherThanReportingItself)
  {
    /*
     * The print draws all seven and greys six. A hit test that handed back a
     * disabled tab would put the check in every caller, and the day one caller
     * forgot, a player would press FUTURE and get a blank screen.
     */
    const StationScreenTuning tuning;
    const UiRect row{0.0f, 46.0f, 1440.0f, 52.0f};

    StationTabButton buttons[MAX_STATION_TABS];
    const std::uint32_t laid = BuildStationTabRow(PRINT_TABS, row, 1.0f, tuning, buttons);
    const std::span<const StationTabButton> all{buttons, laid};

    const UiRect& cargo = buttons[1].rect;
    Assert::IsFalse(buttons[1].enabled, L"CARGO has no service behind it yet");
    Assert::IsNull(HitStationTab(all, cargo.x + cargo.width * 0.5f, 60.0f));
  }

  TEST_METHOD(APressInTheGapBetweenTabsHitsNothing)
  {
    const StationScreenTuning tuning;
    const UiRect row{0.0f, 46.0f, 1440.0f, 52.0f};

    StationTabButton buttons[MAX_STATION_TABS];
    const std::uint32_t laid = BuildStationTabRow(PRINT_TABS, row, 1.0f, tuning, buttons);
    const std::span<const StationTabButton> all{buttons, laid};

    Assert::IsNull(HitStationTab(all, buttons[0].rect.Right() + 1.0f, 60.0f), L"between the first two");
    Assert::IsNull(HitStationTab(all, 4.0f, 60.0f), L"in the inset before the first");
    Assert::IsNull(HitStationTab(all, buttons[laid - 1].rect.Right() + 40.0f, 60.0f), L"past the last");
    Assert::IsNull(HitStationTab(all, buttons[0].rect.x + 4.0f, 20.0f), L"above the row");
  }
};

TEST_CLASS(StationRosterColumnTests)
{
public:
  TEST_METHOD(AColumnPerWingAcrossThePanel)
  {
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationGroup groups[] = {Wing("TALON", 1, 2), Wing("RESERVE", 2, 1), Wing("UNASSIGNED", 0, 0)};
    const StationChip chips[] = {Ship(11, 0), Ship(12, 0), Ship(21, 1)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts =
        BuildStationColumns(groups, chips, layout.roster, 0, 1.0f, tuning, columns, laid);

    Assert::AreEqual<std::uint32_t>(3, counts.groups);
    Assert::AreEqual(26.0f, columns[0].header.x, 1e-3f, L"inset from the panel");
    Assert::AreEqual(196.0f, columns[0].header.width, 1e-3f);
    Assert::AreEqual(210.0f, columns[1].header.x - columns[0].header.x, 1e-3f, L"a column plus the gap");
    Assert::AreEqual<std::uint32_t>(2, columns[2].group, L"and each names the wing it is");
  }

  TEST_METHOD(EachColumnGetsOnlyItsOwnChipsAndTheyRunDownFromUnderTheHeader)
  {
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationGroup groups[] = {Wing("TALON", 1, 2), Wing("RESERVE", 2, 1)};
    const StationChip chips[] = {Ship(11, 0), Ship(21, 1), Ship(12, 0)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts =
        BuildStationColumns(groups, chips, layout.roster, 0, 1.0f, tuning, columns, laid);

    Assert::AreEqual<std::uint32_t>(3, counts.chips);

    // Talon's two, in roster order, under Talon's header.
    Assert::AreEqual<std::uint32_t>(0, laid[0].chip);
    Assert::AreEqual<std::uint32_t>(2, laid[1].chip, L"the roster's index, not the column's row");
    Assert::AreEqual(columns[0].header.x, laid[0].rect.x, 1e-3f);
    Assert::AreEqual(columns[0].header.Bottom(), laid[0].rect.y, 1e-3f, L"the first chip starts under the header");
    Assert::AreEqual(30.0f, laid[1].rect.y - laid[0].rect.y, 1e-3f, L"a chip plus its gap");

    // Reserve's one, in Reserve's column.
    Assert::AreEqual<std::uint32_t>(1, laid[2].chip);
    Assert::AreEqual(columns[1].header.x, laid[2].rect.x, 1e-3f);
    Assert::AreEqual(columns[1].header.Bottom(), laid[2].rect.y, 1e-3f);
  }

  TEST_METHOD(ScrollingDropsTheRowsAboveAndLiftsTheRestToTheTop)
  {
    /*
     * `UiScrollState`'s whole-row rule applied to the one zone ADR-020 §7 says
     * scrolls. Whole rows because a chip cut in half by the panel edge reads as
     * a list that ran out rather than one that continues.
     */
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationGroup groups[] = {Wing("TALON", 1, 4)};
    const StationChip chips[] = {Ship(11, 0), Ship(12, 0), Ship(13, 0), Ship(14, 0)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts =
        BuildStationColumns(groups, chips, layout.roster, 2, 1.0f, tuning, columns, laid);

    Assert::AreEqual<std::uint32_t>(2, counts.chips, L"the two scrolled past emit nothing");
    Assert::AreEqual<std::uint32_t>(2, laid[0].chip, L"the third ship is now the first row");
    Assert::AreEqual(columns[0].header.Bottom(), laid[0].rect.y, 1e-3f, L"and it sits at the top of the column");
  }

  TEST_METHOD(EveryColumnScrollsTogether)
  {
    // One offset for the panel rather than one per column: two wings drifting
    // out of step would put a chip beside the wrong header, and the header is
    // the only thing that says whose ship it is.
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationGroup groups[] = {Wing("TALON", 1, 2), Wing("RESERVE", 2, 2)};
    const StationChip chips[] = {Ship(11, 0), Ship(12, 0), Ship(21, 1), Ship(22, 1)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts =
        BuildStationColumns(groups, chips, layout.roster, 1, 1.0f, tuning, columns, laid);

    Assert::AreEqual<std::uint32_t>(2, counts.chips);
    Assert::AreEqual(laid[0].rect.y, laid[1].rect.y, 1e-3f, L"the surviving row is the same row in both");
  }

  TEST_METHOD(AChipThatWouldStraddleThePanelsBottomIsNotDrawn)
  {
    const StationScreenTuning tuning;
    // A panel with room for exactly three rows under its header.
    const UiRect panel{0.0f, 98.0f, 1032.0f, 22.0f + 30.0f + 3.0f * 30.0f - 4.0f + 22.0f};
    const StationGroup groups[] = {Wing("TALON", 1, 6)};
    const StationChip chips[] = {Ship(11, 0), Ship(12, 0), Ship(13, 0), Ship(14, 0), Ship(15, 0), Ship(16, 0)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts = BuildStationColumns(groups, chips, panel, 0, 1.0f, tuning, columns, laid);

    Assert::AreEqual<std::uint32_t>(3, counts.chips);
    Assert::IsTrue(laid[2].rect.Bottom() <= panel.Bottom() - 22.0f + 1e-3f, L"and the last one is inside the padding");
  }

  TEST_METHOD(AColumnThatWouldRunPastTheEdgeIsDroppedWithItsChips)
  {
    const StationScreenTuning tuning;
    // Room for two columns and most of a third.
    const UiRect panel{0.0f, 98.0f, 26.0f + 196.0f + 14.0f + 196.0f + 100.0f + 26.0f, 802.0f};
    const StationGroup groups[] = {Wing("A", 1, 1), Wing("B", 2, 1), Wing("C", 3, 1)};
    const StationChip chips[] = {Ship(11, 0), Ship(21, 1), Ship(31, 2)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts = BuildStationColumns(groups, chips, panel, 0, 1.0f, tuning, columns, laid);

    Assert::AreEqual<std::uint32_t>(2, counts.groups, L"the third column is dropped whole");
    Assert::AreEqual<std::uint32_t>(2, counts.chips, L"and its chip goes with it");
  }

  TEST_METHOD(APressFindsTheChipUnderItAndNamesItByRosterIndex)
  {
    // The print's first gesture: tap toggles. What it toggles is a ship, and
    // the index handed back is the roster's, so the caller never has to work
    // out which column it came from.
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationGroup groups[] = {Wing("TALON", 1, 1), Wing("RESERVE", 2, 1)};
    const StationChip chips[] = {Ship(11, 0), Ship(21, 1)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts =
        BuildStationColumns(groups, chips, layout.roster, 0, 1.0f, tuning, columns, laid);
    const std::span<const StationChipRect> all{laid, counts.chips};

    const UiRect& reserve = laid[1].rect;
    const StationChipRect* hit = HitStationChip(all, reserve.x + 8.0f, reserve.y + 8.0f);
    Assert::IsNotNull(hit);
    Assert::AreEqual<std::uint32_t>(1, hit->chip);
    Assert::IsNull(HitStationChip(all, reserve.x + 8.0f, reserve.Bottom() + 2.0f), L"the gap under it is not a chip");
  }

  TEST_METHOD(APressOnAHeaderIsTheWingRatherThanAChip)
  {
    /*
     * The print's second gesture: holding a header takes the whole wing. The
     * header and the first chip share an edge, so this is exactly the boundary
     * a draw and an input handler in separate files would disagree about.
     */
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationGroup groups[] = {Wing("TALON", 1, 1)};
    const StationChip chips[] = {Ship(11, 0)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts =
        BuildStationColumns(groups, chips, layout.roster, 0, 1.0f, tuning, columns, laid);
    const std::span<const StationColumnRect> headers{columns, counts.groups};
    const std::span<const StationChipRect> all{laid, counts.chips};

    const float x = columns[0].header.x + 8.0f;
    const float edge = columns[0].header.Bottom();

    const StationColumnRect* header = HitStationColumnHeader(headers, x, edge - 1.0f);
    Assert::IsNotNull(header);
    Assert::AreEqual<std::uint32_t>(0, header->group);
    Assert::IsNull(HitStationChip(all, x, edge - 1.0f), L"a press on the header is not a press on the chip");

    Assert::IsNull(HitStationColumnHeader(headers, x, edge), L"and one pixel lower is the chip");
    Assert::IsNotNull(HitStationChip(all, x, edge));
  }

  TEST_METHOD(TheScrollIsOverTheTallestColumn)
  {
    const StationChip chips[] = {Ship(11, 0), Ship(21, 1), Ship(22, 1), Ship(23, 1), Ship(31, 2)};

    Assert::AreEqual<std::uint32_t>(3, StationColumnRows(chips, 3), L"Reserve's three");
    Assert::AreEqual<std::uint32_t>(1, StationColumnRows(chips, 1), L"counting only the columns that exist");
    Assert::AreEqual<std::uint32_t>(0, StationColumnRows(chips, 0), L"and an empty station scrolls over nothing");
    Assert::AreEqual<std::uint32_t>(0, StationColumnRows({}, 3));
  }

  TEST_METHOD(TheVisibleCountIsTheCountTheLayoutActuallyDraws)
  {
    /*
     * The reason both numbers live in this file. `UiScrollState` is told how
     * many rows fit, and `BuildStationColumns` decides how many to emit; if
     * those two disagree by one, the last row is either unreachable or the
     * scroll runs one row past the end. Neither is visible in a screenshot.
     *
     * Swept rather than spot-checked, because the disagreement only appears at
     * the panel heights where the slack under the last chip is at least a chip
     * tall and short of a chip and a gap.
     */
    const StationScreenTuning tuning;
    for (std::uint32_t height = 40; height <= 900; height += 1)
    {
      const UiRect panel{0.0f, 98.0f, 1032.0f, static_cast<float>(height)};
      Assert::AreEqual<std::uint32_t>(RowsDrawn(panel, 1.0f, tuning), StationVisibleRows(panel, 1.0f, tuning),
                                      L"the scroll and the layout must count the same rows");
    }
  }

  TEST_METHOD(APanelWithRoomForAChipButNotItsGapStillShowsThatChip)
  {
    // The boundary the sweep above exists to pin: the last row needs no gap
    // under it, so `room / pitch` is the wrong arithmetic by one here.
    const StationScreenTuning tuning;
    const UiRect panel{0.0f, 98.0f, 1032.0f, 22.0f + 30.0f + 27.0f + 22.0f};

    Assert::AreEqual<std::uint32_t>(1, StationVisibleRows(panel, 1.0f, tuning));
    Assert::AreEqual<std::uint32_t>(1, RowsDrawn(panel, 1.0f, tuning));
  }

  TEST_METHOD(APanelWithNoRoomForAChipShowsNone)
  {
    const StationScreenTuning tuning;
    const UiRect tight{0.0f, 98.0f, 1032.0f, 22.0f + 30.0f + 25.0f + 22.0f};
    Assert::AreEqual<std::uint32_t>(0, StationVisibleRows(tight, 1.0f, tuning));

    const UiRect flat{0.0f, 98.0f, 1032.0f, 0.0f};
    Assert::AreEqual<std::uint32_t>(0, StationVisibleRows(flat, 1.0f, tuning), L"and neither does a collapsed panel");
  }

  TEST_METHOD(AnEmptyHangarLaysOutItsColumnsAnyway)
  {
    // Wing 0 is a real column even when nothing is in it -- strays land there,
    // and a column that vanished when it emptied would be a place the player
    // could not drop a ship back into.
    const StationScreenTuning tuning;
    const StationScreenLayout layout = ResolveStationScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const StationGroup groups[] = {Wing("UNASSIGNED", 0, 0)};

    StationColumnRect columns[MAX_STATION_GROUPS];
    StationChipRect laid[MAX_STATION_CHIPS];
    const StationRosterCounts counts = BuildStationColumns(groups, {}, layout.roster, 0, 1.0f, tuning, columns, laid);

    Assert::AreEqual<std::uint32_t>(1, counts.groups);
    Assert::AreEqual<std::uint32_t>(0, counts.chips);
  }
};

} // namespace NeuronClientTests
