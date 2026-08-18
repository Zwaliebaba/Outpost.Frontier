#include "pch.h"
#include "CppUnitTest.h"

#include "ToastStack.h"
#include "UiDrawList.h"
#include "UiLayout.h"

#include <cmath>
#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The Ui pass's device-free half (Build Order S11, ADR-006 §10).
 *
 * Three things, and none of them needs a device: where the zones are, what goes
 * in a draw list, and which notifications are showing. The pass that turns the
 * third into pixels is the only part a GPU has to confirm.
 *
 * The toast tests are the ones that carry weight, because
 * `alerts-and-toasts.png` is unusually specific -- five priorities, one
 * suppression rule keyed on a replicated flag, mandatory coalescing, and a cap
 * that drops oldest first. Each of those is a sentence on the sheet and an
 * assertion here.
 */

namespace NeuronClientTests
{
namespace
{

[[nodiscard]] UiLayout Layout(std::uint32_t _width = 1600, std::uint32_t _height = 900, float _scale = 1.0f)
{
  const UiTuning tuning;
  return ResolveUiLayout(_width, _height, _scale, tuning);
}

} // namespace

TEST_CLASS(UiLayoutTests)
{
public:
  TEST_METHOD(TheChromeSurroundsTheWorldAndNeverOverlapsIt)
  {
    // The print draws the HUD as a border, not an overlay: nothing composites
    // over the plane. That is what makes the world rect the leftovers.
    const UiTuning tuning;
    const UiLayout layout = Layout();

    Assert::AreEqual(0.0f, layout.topBar.y, L"the status row is at the top");
    Assert::AreEqual(900.0f, layout.commandRow.Bottom(), L"and the command row on the floor");
    Assert::AreEqual(layout.commandRow.y, layout.contextBar.Bottom(), L"with the context bar directly above it");

    Assert::AreEqual(layout.topBar.Bottom(), layout.world.y, L"the world starts below the status row");
    Assert::AreEqual(layout.contextBar.y, layout.world.Bottom(), L"and ends above the context bar");
    Assert::AreEqual(layout.roster.Right(), layout.world.x, L"between the roster");
    Assert::AreEqual(layout.abilityRack.x, layout.world.Right(), L"and the ability rack");

    Assert::IsTrue(layout.world.width > 1000.0f && layout.world.height > 700.0f, L"and it is most of the screen");
    (void)tuning;
  }

  TEST_METHOD(EverythingScalesWithTheMultiplierAndTheWorldAbsorbsIt)
  {
    /*
     * The settings sheet makes 0.8-1.6x a requirement, and this is what that
     * means structurally: the chrome grows, the world shrinks by exactly what
     * the chrome took, and no zone is a fraction of the viewport. A HUD sized
     * in fractions would shrink its own text on a small screen, which is the
     * opposite of what a scale control is for.
     */
    const UiLayout small = Layout(1600, 900, 0.8f);
    const UiLayout normal = Layout(1600, 900, 1.0f);
    const UiLayout large = Layout(1600, 900, 1.6f);

    Assert::IsTrue(small.roster.width < normal.roster.width, L"the roster grows with the scale");
    Assert::IsTrue(large.roster.width > normal.roster.width);
    Assert::AreEqual(normal.roster.width * 1.6f, large.roster.width, 0.01f, L"linearly");

    Assert::IsTrue(large.world.width < normal.world.width, L"and the world gives up the difference");
    Assert::AreEqual(1600.0f, large.topBar.width, L"the full-width zones stay full width");
  }

  TEST_METHOD(TheScaleIsClampedToWhatTheSettingsSheetOffers)
  {
    // Outside 0.8-1.6 the text stops matching the panels it sits in, which is
    // worse than refusing the setting.
    Assert::AreEqual(0.8f, Layout(1600, 900, 0.1f).scale, L"clamped low");
    Assert::AreEqual(1.6f, Layout(1600, 900, 9.0f).scale, L"clamped high");
    Assert::AreEqual(1.0f, Layout(1600, 900, 1.0f).scale, L"and left alone in range");
  }

  TEST_METHOD(AViewportTooSmallForItsOwnChromeCollapsesRatherThanInverting)
  {
    // Reachable by dragging a window small. A layout that produced negative
    // sizes here would put a quad with a flipped winding on screen.
    const UiLayout layout = Layout(80, 60, 1.6f);

    Assert::IsTrue(layout.world.width >= 0.0f, L"the world has no negative width");
    Assert::IsTrue(layout.world.height >= 0.0f, L"nor height");
    Assert::IsTrue(layout.roster.width <= 80.0f, L"and no zone is wider than the window");
    Assert::IsTrue(layout.topBar.height <= 60.0f, L"nor taller");
  }

  TEST_METHOD(ToastsStackUpwardFromTheContextBarAndNeverCoverIt)
  {
    /*
     * `alerts-and-toasts.png` §2, and the sheet says why in one line: the bar
     * is where orders are issued, and a toast that covers ATTACK mid-fight is a
     * lost engagement. Anchored to the bar rather than to the viewport, so the
     * rule holds at every scale.
     */
    const UiTuning tuning;
    const UiLayout layout = Layout();

    const UiRect top = layout.ToastSlot(0, tuning);
    const UiRect second = layout.ToastSlot(1, tuning);

    Assert::IsTrue(top.Bottom() <= layout.contextBar.y, L"clear of the context bar");
    Assert::IsTrue(second.Bottom() < top.y, L"and the stack grows upward from there");
    Assert::IsTrue(top.Right() <= layout.abilityRack.x, L"clear of the ability rack too");

    const UiLayout scaled = Layout(1600, 900, 1.6f);
    Assert::IsTrue(scaled.ToastSlot(0, tuning).Bottom() <= scaled.contextBar.y, L"still clear at 1.6x");
    Assert::IsTrue(scaled.ToastSlot(4, tuning).Bottom() <= scaled.contextBar.y, L"for every slot the sheet allows");
  }

  TEST_METHOD(TheCriticalToastIsCentredNearTheTopOnItsOwnSurface)
  {
    // Centre-top because it is the one class that may interrupt, and it is near
    // the eye's resting point during a fight. It does not join the stack.
    const UiTuning tuning;
    const UiLayout layout = Layout();

    const float centre = layout.criticalToast.x + layout.criticalToast.width * 0.5f;
    Assert::AreEqual(800.0f, centre, 0.01f, L"centred horizontally");
    Assert::IsTrue(layout.criticalToast.y > layout.topBar.Bottom(), L"below the status row");
    Assert::IsTrue(layout.criticalToast.Bottom() < layout.world.Bottom() * 0.5f, L"and high in the view");

    // Two surfaces, and the split is the sheet's: they must not overlap, or the
    // one class allowed to interrupt would land on top of the stack it is
    // supposed to be separate from. Asserted as a rect test rather than as a
    // guess about which side it falls on -- the first draft of this assertion
    // guessed, and guessed the wrong side.
    const UiRect top = layout.ToastSlot(0, tuning);
    const bool separated = layout.criticalToast.Right() <= top.x || top.Right() <= layout.criticalToast.x ||
                           layout.criticalToast.Bottom() <= top.y || top.Bottom() <= layout.criticalToast.y;
    Assert::IsTrue(separated, L"the critical surface and the bottom-right stack do not overlap");
  }
};

TEST_CLASS(UiDrawListTests)
{
public:
  TEST_METHOD(AQuadAndItsTextComeBackOut)
  {
    UiDrawList list;
    list.AddQuad(UiRect{10.0f, 20.0f, 30.0f, 40.0f}, 0xff00ff00u);
    list.AddText(11.0f, 21.0f, 1, 0xffffffffu, "41 SHIPS");

    Assert::AreEqual<std::size_t>(1, list.Quads().size());
    Assert::AreEqual(30.0f, list.Quads()[0].rect.width);
    Assert::AreEqual<std::size_t>(1, list.Runs().size());
    Assert::AreEqual<std::uint8_t>(1, list.Runs()[0].sizeIndex);
    Assert::IsTrue(list.Text(list.Runs()[0]) == "41 SHIPS", L"the run points at its own characters");
  }

  TEST_METHOD(RunsPointIntoOnePooledBufferAndDoNotDisturbEachOther)
  {
    // The reason a run carries an offset rather than a `string_view`: the
    // buffer grows, and a view into it would dangle the moment another run was
    // added. This test is the one that would catch that.
    UiDrawList list;
    list.AddText(0.0f, 0.0f, 0, 0xffffffffu, "TALON");
    list.AddText(0.0f, 10.0f, 0, 0xffffffffu, "ANVIL");
    list.AddText(0.0f, 20.0f, 0, 0xffffffffu, "SPUR");

    Assert::AreEqual<std::size_t>(3, list.Runs().size());
    Assert::IsTrue(list.Text(list.Runs()[0]) == "TALON");
    Assert::IsTrue(list.Text(list.Runs()[1]) == "ANVIL");
    Assert::IsTrue(list.Text(list.Runs()[2]) == "SPUR");
    Assert::AreEqual<std::size_t>(14, list.CharacterCount(), L"and the buffer is exactly the characters");
  }

  TEST_METHOD(NothingDegenerateGetsAnInstance)
  {
    // A layout that collapses to nothing is how an empty roster or a zero-width
    // bar reads, so it is not an error -- but it is not an instance either.
    UiDrawList list;
    list.AddQuad(UiRect{0.0f, 0.0f, 0.0f, 10.0f}, 0xffffffffu);
    list.AddQuad(UiRect{0.0f, 0.0f, 10.0f, -4.0f}, 0xffffffffu);
    list.AddText(0.0f, 0.0f, 0, 0xffffffffu, "");

    Assert::AreEqual<std::size_t>(0, list.Quads().size(), L"no zero-area quads");
    Assert::AreEqual<std::size_t>(0, list.Runs().size(), L"no empty runs");
  }

  TEST_METHOD(ABorderIsFourBarsThatDoNotOverlap)
  {
    /*
     * Four quads rather than a rect with a hole, because this pass draws filled
     * quads and nothing else. They must not double up: the moment a border is
     * drawn with alpha, an overlapping corner is twice as bright as the edge
     * leading into it.
     */
    UiDrawList list;
    list.AddBorder(UiRect{0.0f, 0.0f, 100.0f, 50.0f}, 2.0f, 0xff00ff00u);

    Assert::AreEqual<std::size_t>(4, list.Quads().size());

    float area = 0.0f;
    for (const UiQuad& quad : list.Quads())
    {
      area += quad.rect.width * quad.rect.height;
    }
    // Perimeter bars with no shared corners: 2 full-width bars plus 2 sides of
    // what is left between them.
    Assert::AreEqual(2.0f * 100.0f * 2.0f + 2.0f * 2.0f * 46.0f, area, 0.01f, L"and cover each pixel once");
  }

  TEST_METHOD(ClearKeepsCapacitySoAFrameDoesNotAllocate)
  {
    UiDrawList list;
    for (int i = 0; i < 64; ++i)
    {
      list.AddQuad(UiRect{0.0f, 0.0f, 4.0f, 4.0f}, 0xffffffffu);
      list.AddText(0.0f, 0.0f, 0, 0xffffffffu, "0123456789");
    }
    list.Clear();

    Assert::AreEqual<std::size_t>(0, list.Quads().size());
    Assert::AreEqual<std::size_t>(0, list.Runs().size());
    Assert::AreEqual<std::size_t>(0, list.CharacterCount());

    // Refilling must not reallocate; the observable proxy is that the text
    // buffer still starts at offset zero and the contents are right.
    list.AddText(1.0f, 2.0f, 0, 0xffffffffu, "NET");
    Assert::AreEqual<std::uint32_t>(0, list.Runs()[0].textOffset, L"the buffer was reused, not rebuilt");
    Assert::IsTrue(list.Text(list.Runs()[0]) == "NET");
  }
};

TEST_CLASS(ToastStackTests)
{
public:
  TEST_METHOD(EachPriorityHasTheDwellTheSheetGivesIt)
  {
    Assert::IsTrue(ToastDwellSeconds(ToastPriority::Critical) < 0.0, L"critical waits to be acted on");
    Assert::AreEqual(12.0, ToastDwellSeconds(ToastPriority::Urgent), L"urgent: 12 s");
    Assert::AreEqual(8.0, ToastDwellSeconds(ToastPriority::Routine), L"routine: 8 s");
    Assert::AreEqual(5.0, ToastDwellSeconds(ToastPriority::Ambient), L"ambient: 5 s");
    Assert::AreEqual(8.0, ToastDwellSeconds(ToastPriority::WallTime), L"wall-time: 8 s");

    Assert::IsTrue(ToastSurvivesCombat(ToastPriority::Critical), L"critical is one of the two ALWAYS rows");
    Assert::IsTrue(ToastSurvivesCombat(ToastPriority::Urgent), L"and urgent is the other");
    Assert::IsFalse(ToastSurvivesCombat(ToastPriority::Routine), L"the rest are queued");
    Assert::IsFalse(ToastSurvivesCombat(ToastPriority::Ambient));
    Assert::IsFalse(ToastSurvivesCombat(ToastPriority::WallTime));
  }

  TEST_METHOD(ARefusedOrderIsAnUrgentToastCarryingItsReason)
  {
    // The one toast the MVP raises, and the one S9 has owed since the bounce
    // landed: same event, two surfaces, one reason string.
    ToastStack toasts;
    Assert::IsTrue(toasts.Raise(ToastPriority::Urgent, 5, "ORDER REJECTED", "outside the operating area", 10.0));

    Assert::AreEqual<std::size_t>(1, toasts.Visible().size());
    Assert::IsTrue(toasts.Visible()[0].head == "ORDER REJECTED");
    Assert::IsTrue(toasts.Visible()[0].detail == "outside the operating area", L"the reason text is mandatory on this level");

    toasts.Advance(21.9);
    Assert::AreEqual<std::size_t>(1, toasts.Visible().size(), L"twelve seconds is twelve seconds");
    toasts.Advance(22.0);
    Assert::IsTrue(toasts.Empty(), L"and then it goes");
  }

  TEST_METHOD(ACriticalWaitsToBeActedOnAndNothingElseDoes)
  {
    ToastStack toasts;
    Assert::IsTrue(toasts.Raise(ToastPriority::Critical, 1, "HULL CRITICAL", "18% hull", 10.0));
    Assert::IsTrue(toasts.Raise(ToastPriority::Ambient, 2, "PROBE RESOLVED", "", 10.0));

    toasts.Advance(1000.0);
    Assert::AreEqual<std::size_t>(1, toasts.Visible().size(), L"the ambient one dwelt and went");
    Assert::IsTrue(toasts.Visible()[0].priority == ToastPriority::Critical, L"the critical is still waiting");
    Assert::IsTrue(toasts.Visible()[0].WaitsForAction());

    toasts.Dismiss(0);
    Assert::IsTrue(toasts.Empty(), L"until it is acted on");
  }

  TEST_METHOD(SameSourceCoalescesIntoACountRatherThanARow)
  {
    /*
     * "Mandatory, not an optimisation" -- the sheet's own words. One market
     * order can fill forty times, and forty rows is the stack becoming the
     * thing that suppresses the game.
     */
    ToastStack toasts;
    Assert::IsTrue(toasts.Raise(ToastPriority::Routine, 77, "SELL ORDER FILLED", "2,400 x Tritanite", 10.0));
    Assert::IsFalse(toasts.Raise(ToastPriority::Routine, 77, "SELL ORDER FILLED", "4,800 x Tritanite", 11.0),
                    L"the second fold returns false: something happened *again*");
    Assert::IsFalse(toasts.Raise(ToastPriority::Routine, 77, "SELL ORDER FILLED", "9,600 x Tritanite", 12.0));

    Assert::AreEqual<std::size_t>(1, toasts.Visible().size(), L"one row");
    Assert::AreEqual<std::uint32_t>(3, toasts.Visible()[0].count, L"standing for three events");
    Assert::IsTrue(toasts.Visible()[0].detail == "9,600 x Tritanite", L"showing the latest aggregate");
  }

  TEST_METHOD(ADifferentSourceOrALaterWindowIsItsOwnRow)
  {
    // The other half of coalescing, and the reason `sourceKey` is the raiser's
    // choice: a raiser that wants every event on its own row varies it.
    ToastStack toasts;
    Assert::IsTrue(toasts.Raise(ToastPriority::Routine, 1, "A", "", 10.0));
    Assert::IsTrue(toasts.Raise(ToastPriority::Routine, 2, "B", "", 10.0), L"a different source is a different row");
    Assert::AreEqual<std::size_t>(2, toasts.Visible().size());

    Assert::IsTrue(toasts.Raise(ToastPriority::Routine, 1, "A", "", 10.0 + ToastStack::COALESCE_WINDOW_SECONDS + 0.1),
                   L"and past the window, so is the same source");
    Assert::AreEqual<std::size_t>(3, toasts.Visible().size());
  }

  TEST_METHOD(ACoalescingRowRestartsItsDwell)
  {
    // A fill that kept its original expiry would vanish while its own count
    // was still climbing.
    ToastStack toasts;
    Assert::IsTrue(toasts.Raise(ToastPriority::Ambient, 1, "PING", "", 10.0));
    Assert::IsFalse(toasts.Raise(ToastPriority::Ambient, 1, "PING", "", 14.0));

    toasts.Advance(18.9);
    Assert::AreEqual<std::size_t>(1, toasts.Visible().size(), L"five seconds from the *latest* event");
    toasts.Advance(19.1);
    Assert::IsTrue(toasts.Empty());
  }

  TEST_METHOD(TheStackShowsFiveAndDropsTheOldestQuietestFirst)
  {
    // The sheet states the cap on the Ambient row, but it is a property of the
    // surface: the stack must clear the roster drawer and the puck's working
    // area, so it is five rows whatever raised them.
    ToastStack toasts;
    for (std::uint32_t index = 0; index < 8; ++index)
    {
      Assert::IsTrue(toasts.Raise(ToastPriority::Ambient, index, "PING", "", 10.0 + index * 0.1));
    }

    Assert::AreEqual<std::size_t>(ToastStack::MAX_VISIBLE, toasts.Visible().size(), L"five visible");
    Assert::AreEqual<std::uint64_t>(3, toasts.DroppedCount(), L"and three dropped");

    // Newest first within a level, so the survivors are the last five raised.
    Assert::AreEqual<std::uint32_t>(7, toasts.Visible()[0].sourceKey, L"newest at the top");
    Assert::AreEqual<std::uint32_t>(3, toasts.Visible()[4].sourceKey, L"oldest survivor at the bottom");
  }

  TEST_METHOD(AnUrgentRowTakesTheTopSlotFromAQuieterOne)
  {
    // The sheet puts Urgent in the top slot explicitly. Sorting by priority
    // first is what delivers that without a special case at the draw site.
    ToastStack toasts;
    Assert::IsTrue(toasts.Raise(ToastPriority::Ambient, 1, "PING", "", 10.0));
    Assert::IsTrue(toasts.Raise(ToastPriority::Urgent, 2, "ORDER REJECTED", "no such ship", 11.0));

    Assert::IsTrue(toasts.Visible()[0].priority == ToastPriority::Urgent, L"loudest first");
    Assert::IsTrue(toasts.Visible()[1].priority == ToastPriority::Ambient);
  }

  TEST_METHOD(ACriticalDoesNotConsumeAStackSlot)
  {
    /*
     * "Centre-top, one action, blocks nothing else from stacking below." A
     * critical that ate a slot would silently cost the player a routine row at
     * exactly the moment the stack is busiest.
     */
    ToastStack toasts;
    Assert::IsTrue(toasts.Raise(ToastPriority::Critical, 99, "HULL CRITICAL", "18% hull", 10.0));
    for (std::uint32_t index = 0; index < ToastStack::MAX_VISIBLE; ++index)
    {
      Assert::IsTrue(toasts.Raise(ToastPriority::Routine, index, "FILL", "", 10.0));
    }

    Assert::AreEqual<std::size_t>(ToastStack::MAX_VISIBLE + 1, toasts.Visible().size(), L"five in the stack plus the critical");
    Assert::AreEqual<std::size_t>(1, toasts.CriticalCount(), L"and the criticals lead, as one contiguous run");
    Assert::AreEqual<std::uint64_t>(0, toasts.DroppedCount(), L"nothing was pushed out");
  }

  TEST_METHOD(CombatQueuesTheQuietOnesAndLetsTheLoudOnesThrough)
  {
    ToastStack toasts;
    toasts.SetCombat(true, 10.0);

    Assert::IsTrue(toasts.Raise(ToastPriority::Urgent, 1, "ORDER REJECTED", "no such ship", 10.0));
    Assert::IsTrue(toasts.Raise(ToastPriority::Routine, 2, "FILL", "", 10.0));
    Assert::IsTrue(toasts.Raise(ToastPriority::Ambient, 3, "PING", "", 10.0));

    Assert::AreEqual<std::size_t>(1, toasts.Visible().size(), L"only the urgent one is shown");
    Assert::AreEqual<std::size_t>(2, toasts.SuppressedCount(), L"the rest are queued, not dropped");
  }

  TEST_METHOD(ASuppressedBacklogDrainsWhenCombatClearsAndItsDwellStartsThen)
  {
    /*
     * The reason a toast records when it was *shown* rather than when it was
     * raised. A backlog whose dwell had been running while it was invisible
     * would drain and expire in the same instant -- the player would see the
     * flag clear and nothing else, which is exactly the silent-drop the sheet
     * forbids.
     */
    ToastStack toasts;
    toasts.SetCombat(true, 10.0);
    Assert::IsTrue(toasts.Raise(ToastPriority::Routine, 1, "FILL", "", 10.0));

    toasts.Advance(60.0);
    Assert::IsTrue(toasts.Empty(), L"still held back fifty seconds later");
    Assert::AreEqual<std::size_t>(1, toasts.SuppressedCount());

    toasts.SetCombat(false, 60.0);
    Assert::AreEqual<std::size_t>(1, toasts.Visible().size(), L"and shown the moment the flag clears");
    Assert::AreEqual<std::size_t>(0, toasts.SuppressedCount());

    toasts.Advance(67.9);
    Assert::AreEqual<std::size_t>(1, toasts.Visible().size(), L"with its full eight seconds, starting now");
    toasts.Advance(68.1);
    Assert::IsTrue(toasts.Empty());
  }

  TEST_METHOD(ClearingDropsEverythingIncludingTheBacklog)
  {
    // A disconnect. Last session's refusals over this session's fleet would be
    // the same mistake as keeping its ghosts.
    ToastStack toasts;
    toasts.SetCombat(true, 10.0);
    Assert::IsTrue(toasts.Raise(ToastPriority::Urgent, 1, "ORDER REJECTED", "no such ship", 10.0));
    Assert::IsTrue(toasts.Raise(ToastPriority::Routine, 2, "FILL", "", 10.0));

    toasts.Clear();
    Assert::IsTrue(toasts.Empty());
    Assert::AreEqual<std::size_t>(0, toasts.SuppressedCount());
  }
};

TEST_CLASS(DragRectangleTests)
{
public:
  TEST_METHOD(ADragInAnyDirectionGivesThePositiveRectangle)
  {
    /*
     * The selection box S8 deferred, and the whole of what it needed.
     *
     * A drag up-and-left is as ordinary as one down-and-right, and the naive
     * `{startX, startY, currentX - startX, currentY - startY}` gives it a
     * negative width. `AddQuad` declines a non-positive size, so the box would
     * simply not draw on half the gestures people make -- which is why the
     * companion test below asserts that a collapsed box costs no instance:
     * that refusal is the behaviour this one depends on.
     */
    const UiRect expected{10.0f, 20.0f, 90.0f, 60.0f};
    const float corners[][4] = {{10.0f, 20.0f, 100.0f, 80.0f},  // down-right
                                {100.0f, 80.0f, 10.0f, 20.0f},  // up-left
                                {100.0f, 20.0f, 10.0f, 80.0f},  // down-left
                                {10.0f, 80.0f, 100.0f, 20.0f}}; // up-right
    for (const auto& corner : corners)
    {
      const UiRect box = UiRect::FromCorners(corner[0], corner[1], corner[2], corner[3]);
      Assert::AreEqual(expected.x, box.x, 0.001f);
      Assert::AreEqual(expected.y, box.y, 0.001f);
      Assert::AreEqual(expected.width, box.width, 0.001f);
      Assert::AreEqual(expected.height, box.height, 0.001f);
      Assert::IsTrue(box.width >= 0.0f && box.height >= 0.0f);
    }
  }

  TEST_METHOD(AZeroSizeDragIsStillAWellFormedRectangle)
  {
    // The first frame of a drag, before the cursor has moved at all. Zero-size
    // rather than inverted, and `AddQuad` then declines to add it.
    const UiRect box = UiRect::FromCorners(50.0f, 50.0f, 50.0f, 50.0f);
    Assert::AreEqual(0.0f, box.width, 0.001f);
    Assert::AreEqual(0.0f, box.height, 0.001f);

    UiDrawList list;
    list.AddQuad(box, 0xffffffffu);
    Assert::AreEqual<std::size_t>(0, list.Quads().size(), L"a collapsed box costs no instance");
  }
};

TEST_CLASS(OrientedQuadTests)
{
public:
  TEST_METHOD(ASegmentBecomesACentreALengthAndAUnitAxis)
  {
    /*
     * The contract the vertex shader reads, and the one thing about the Ui
     * pass that cannot be run on this machine. `rect` stops meaning
     * top-left-and-size: for an oriented quad it is centre-and-(length,
     * thickness), swept along a unit axis.
     */
    UiDrawList list;
    list.AddSegment(100.0f, 100.0f, 100.0f, 200.0f, 3.0f, 0xff00ff00u);

    Assert::AreEqual<std::size_t>(1, list.Quads().size());
    const UiQuad& quad = list.Quads()[0];
    Assert::IsTrue(quad.oriented);
    Assert::AreEqual(100.0f, quad.rect.x, 0.001f, L"x of the centre");
    Assert::AreEqual(150.0f, quad.rect.y, 0.001f, L"y of the centre, halfway along");
    Assert::AreEqual(100.0f, quad.rect.width, 0.001f, L"width is the length");
    Assert::AreEqual(3.0f, quad.rect.height, 0.001f, L"height is the thickness");
    Assert::AreEqual(0.0f, quad.axisX, 0.001f);
    Assert::AreEqual(1.0f, quad.axisY, 0.001f);
  }

  TEST_METHOD(ASegmentDrawnBackwardsIsTheSameSegment)
  {
    // The centre is the anchor and neither end is privileged, so the only
    // difference a reversal can make is the sign of the axis -- and a quad
    // swept from -0.5 to +0.5 covers the same pixels either way.
    UiDrawList forward;
    UiDrawList backward;
    forward.AddSegment(10.0f, 10.0f, 90.0f, 50.0f, 2.0f, 0xffffffffu);
    backward.AddSegment(90.0f, 50.0f, 10.0f, 10.0f, 2.0f, 0xffffffffu);

    const UiQuad& a = forward.Quads()[0];
    const UiQuad& b = backward.Quads()[0];
    Assert::AreEqual(a.rect.x, b.rect.x, 0.001f);
    Assert::AreEqual(a.rect.y, b.rect.y, 0.001f);
    Assert::AreEqual(a.rect.width, b.rect.width, 0.001f);
    Assert::AreEqual(a.axisX, -b.axisX, 0.001f);
    Assert::AreEqual(a.axisY, -b.axisY, 0.001f);
  }

  TEST_METHOD(TheAxisIsAlwaysUnitLength)
  {
    // A non-unit axis draws the quad at the wrong size, because the shader
    // multiplies it by half the length rather than normalising.
    const float ends[][4] = {{0.0f, 0.0f, 1000.0f, 3.0f}, {0.0f, 0.0f, 3.0f, 1000.0f}, {5.0f, 5.0f, 6.0f, 6.0f}};
    for (const auto& end : ends)
    {
      UiDrawList list;
      list.AddSegment(end[0], end[1], end[2], end[3], 2.0f, 0xffffffffu);
      const UiQuad& quad = list.Quads()[0];
      const float length = std::sqrt(quad.axisX * quad.axisX + quad.axisY * quad.axisY);
      Assert::AreEqual(1.0f, length, 0.0001f);
    }
  }

  TEST_METHOD(ADegenerateSegmentAddsNothing)
  {
    // A dash that landed exactly on the end of a lane is a rounding result and
    // not a mark. There is no direction to give it, and normalising would
    // divide by zero.
    UiDrawList list;
    list.AddSegment(50.0f, 50.0f, 50.0f, 50.0f, 2.0f, 0xffffffffu);
    list.AddSegment(0.0f, 0.0f, 100.0f, 0.0f, 0.0f, 0xffffffffu);
    Assert::AreEqual<std::size_t>(0, list.Quads().size());
  }

  TEST_METHOD(APlainQuadIsNotOriented)
  {
    // The default, and what every panel and every glyph in the HUD relies on:
    // the shader's axis-aligned branch is the one they go through, and a stray
    // `oriented` would send a panel through the sweep with a zero axis.
    UiDrawList list;
    list.AddQuad(UiRect{0.0f, 0.0f, 10.0f, 10.0f}, 0xffffffffu);
    list.AddBorder(UiRect{0.0f, 0.0f, 40.0f, 40.0f}, 1.0f, 0xffffffffu);
    for (const UiQuad& quad : list.Quads())
    {
      Assert::IsFalse(quad.oriented);
      Assert::AreEqual(0.0f, quad.axisX, 0.0f);
      Assert::AreEqual(0.0f, quad.axisY, 0.0f);
    }
  }
};

} // namespace NeuronClientTests
