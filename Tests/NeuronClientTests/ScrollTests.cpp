#include "pch.h"
#include "CppUnitTest.h"

#include "UiScrollState.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The one scrolling list (ADR-020 §4, T3a).
 *
 * `HudRoster.h` deferred scrolling as "a surface rather than a bigger number".
 * It is neither, and this is what it is instead: an offset in rows, a content
 * count, a visible count and a clamp, serving the roster, the map's route
 * panel, the hangar's wing columns and the settings body without any of them
 * inventing a second one.
 */

namespace NeuronClientTests
{
namespace
{

/// A list of `_content` rows in a window of `_visible`, scrolled nowhere yet.
[[nodiscard]] UiScrollState ListOf(std::uint32_t _content, std::uint32_t _visible)
{
  UiScrollState list;
  list.SetExtent(_content, _visible);
  return list;
}

} // namespace

TEST_CLASS(UiScrollStateTests)
{
public:
  TEST_METHOD(AListThatFitsNeverScrolls)
  {
    UiScrollState list = ListOf(4, 8);

    Assert::AreEqual<std::uint32_t>(0, list.MaxOffset());
    Assert::IsFalse(list.CanScrollUp());
    Assert::IsFalse(list.CanScrollDown());

    list.ScrollBy(5);
    Assert::AreEqual<std::uint32_t>(0, list.Offset(), L"there is nowhere for it to go");
  }

  TEST_METHOD(TheLastRowIsTheLastPlaceItStops)
  {
    // Scrolling past `MaxOffset` would show air below the final row, which is a
    // list that has lost its footing rather than one that has reached its end.
    UiScrollState list = ListOf(20, 8);
    Assert::AreEqual<std::uint32_t>(12, list.MaxOffset());

    list.ScrollBy(100);
    Assert::AreEqual<std::uint32_t>(12, list.Offset());
    Assert::IsTrue(list.CanScrollUp());
    Assert::IsFalse(list.CanScrollDown());
  }

  TEST_METHOD(ScrollingUpPastTheTopStopsRatherThanWrapping)
  {
    // A list that jumped from its first row to its last would lose the player's
    // place in exactly the moment they were looking for it.
    UiScrollState list = ListOf(20, 8);
    list.ScrollBy(4);
    Assert::AreEqual<std::uint32_t>(4, list.Offset());

    list.ScrollBy(-100);
    Assert::AreEqual<std::uint32_t>(0, list.Offset());
  }

  TEST_METHOD(TheOffsetClampsWhenTheContentShrinksUnderIt)
  {
    /*
     * The hangar case, and the reason `SetExtent` runs every frame from live
     * data: a player scrolled to the bottom of a roster, whose ships then
     * undocked from another surface, is holding an offset past the end of their
     * own content until this corrects it.
     */
    UiScrollState list = ListOf(20, 8);
    list.ScrollBy(12);
    Assert::AreEqual<std::uint32_t>(12, list.Offset());

    list.SetExtent(10, 8);
    Assert::AreEqual<std::uint32_t>(2, list.Offset(), L"the last full window of what is left");

    list.SetExtent(0, 8);
    Assert::AreEqual<std::uint32_t>(0, list.Offset(), L"an emptied hangar is at the top");
  }

  TEST_METHOD(OnlyWholeRowsAreEverVisible)
  {
    // A list that reported the half row it has room for would then have a row to
    // draw and nowhere to draw it. The alternative is a clip rectangle on
    // `UiInstance`, whose stride ADR-006 §10a deliberately pins.
    Assert::AreEqual<std::uint32_t>(8, VisibleRowCount(160.0f, 20.0f));
    Assert::AreEqual<std::uint32_t>(8, VisibleRowCount(179.0f, 20.0f), L"the straddling ninth row is not one");
    Assert::AreEqual<std::uint32_t>(0, VisibleRowCount(19.0f, 20.0f));
  }

  TEST_METHOD(ADegenerateZoneAsksForNoRowsRatherThanCrashing)
  {
    // Reachable by dragging a window small, and a division that produced an
    // infinity here would put a quad somewhere strange on screen.
    Assert::AreEqual<std::uint32_t>(0, VisibleRowCount(160.0f, 0.0f));
    Assert::AreEqual<std::uint32_t>(0, VisibleRowCount(0.0f, 20.0f));
    Assert::AreEqual<std::uint32_t>(0, VisibleRowCount(-160.0f, 20.0f));
  }

  TEST_METHOD(TheVisibleSpanIsHalfOpenAndTheCullFollowsIt)
  {
    UiScrollState list = ListOf(20, 8);
    list.ScrollBy(5);

    Assert::AreEqual<std::uint32_t>(5, list.FirstVisibleRow());
    Assert::AreEqual<std::uint32_t>(13, list.EndVisibleRow());

    Assert::IsFalse(list.RowVisible(4));
    Assert::IsTrue(list.RowVisible(5));
    Assert::IsTrue(list.RowVisible(12));
    Assert::IsFalse(list.RowVisible(13), L"the span is half open");

    Assert::AreEqual<std::uint32_t>(0, list.RowSlot(5));
    Assert::AreEqual<std::uint32_t>(7, list.RowSlot(12));
  }

  TEST_METHOD(TheSpanStopsAtTheContentRatherThanAtTheWindow)
  {
    // Eight rows of room and three rows of content is three rows drawn, not
    // three plus five slots of nothing.
    const UiScrollState list = ListOf(3, 8);
    Assert::AreEqual<std::uint32_t>(0, list.FirstVisibleRow());
    Assert::AreEqual<std::uint32_t>(3, list.EndVisibleRow());
    Assert::IsFalse(list.RowVisible(3));
  }

  TEST_METHOD(AHighResolutionWheelEventuallyMovesTheList)
  {
    /*
     * `InputFrame::wheelSteps` is fractional on purpose -- rounding makes a
     * smooth wheel feel notched twice -- so a list that truncated each frame's
     * contribution on its own would never move at all under one: a fifth of a
     * notch is less than a row every time.
     */
    UiScrollState list = ListOf(40, 8);

    list.ScrollByWheel(-0.2f);
    list.ScrollByWheel(-0.2f);
    Assert::AreEqual<std::uint32_t>(1, list.Offset(), L"0.4 notches is 1.2 rows");

    list.ScrollByWheel(-0.2f);
    list.ScrollByWheel(-0.2f);
    list.ScrollByWheel(-0.2f);
    Assert::AreEqual<std::uint32_t>(3, list.Offset(), L"one whole notch is three rows");
  }

  TEST_METHOD(AwayFromTheUserScrollsTowardsTheTop)
  {
    // The direction every other list on the machine goes, and the one place the
    // sign is flipped.
    UiScrollState list = ListOf(40, 8);
    list.ScrollBy(10);

    list.ScrollByWheel(1.0f);
    Assert::AreEqual<std::uint32_t>(7, list.Offset());

    list.ScrollByWheel(-1.0f);
    Assert::AreEqual<std::uint32_t>(10, list.Offset());
  }

  TEST_METHOD(TravelBankedAgainstAnEndIsNotSpentComingBack)
  {
    // A wheel spun hard against the bottom must not have to be spun back through
    // the same distance before the list responds.
    UiScrollState list = ListOf(12, 8);
    list.ScrollByWheel(-100.0f);
    Assert::AreEqual<std::uint32_t>(4, list.Offset(), L"at the end");

    // A fifth of a notch never reaches a row on its own, so this is exactly the
    // travel that would bank invisibly if it were kept.
    list.ScrollByWheel(-0.2f);
    Assert::AreEqual<std::uint32_t>(4, list.Offset());

    list.ScrollByWheel(1.0f);
    Assert::AreEqual<std::uint32_t>(1, list.Offset(), L"one notch back is three rows back, immediately");
  }

  TEST_METHOD(AWheelWithNoNotchesChangesNothing)
  {
    UiScrollState list = ListOf(40, 8);
    list.ScrollBy(6);
    list.ScrollByWheel(0.0f);
    Assert::AreEqual<std::uint32_t>(6, list.Offset());
  }

  TEST_METHOD(ResettingGoesToTheTopAndForgetsTheWheel)
  {
    // A different station's hangar is not the same list scrolled: entry resets
    // it, remainder and all.
    UiScrollState list = ListOf(40, 8);
    list.ScrollBy(9);
    list.ScrollByWheel(-0.2f);

    list.Reset();
    Assert::AreEqual<std::uint32_t>(0, list.Offset());

    list.ScrollByWheel(-0.2f);
    Assert::AreEqual<std::uint32_t>(0, list.Offset(), L"the banked fifth of a notch went with it");
  }

  TEST_METHOD(TheChevronKnowsWhichWayThereIsMore)
  {
    // What `∨ 8/8` on the print is drawn from.
    UiScrollState list = ListOf(20, 8);
    Assert::IsFalse(list.CanScrollUp());
    Assert::IsTrue(list.CanScrollDown());

    list.ScrollBy(6);
    Assert::IsTrue(list.CanScrollUp());
    Assert::IsTrue(list.CanScrollDown());

    list.ScrollBy(6);
    Assert::IsTrue(list.CanScrollUp());
    Assert::IsFalse(list.CanScrollDown());
  }
};

} // namespace NeuronClientTests
