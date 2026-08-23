#include "pch.h"
#include "CppUnitTest.h"

#include "Gesture.h"

#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The gesture seam (I1, ADR-020's 2026-08-22 amendment).
 *
 * ADR-020 D15.4 said "no pointer abstraction beyond the mouse is built,
 * reserved or hinted at"; the amendment that reversed it made the abstraction
 * the seam and the mouse an expression of it. This is that abstraction's whole
 * vocabulary -- tap, drag, long-press, second finger, pinch -- and it is
 * testable here, before a touch display exists, which is what makes I1 the
 * first slice of the input phase rather than part of a screen.
 *
 * **Nothing here needs a window.** Contacts go in as plain structs and gestures
 * come out, so the state machine that decides whether a finger wanted the
 * camera or the order surface is asserted rather than judged by hand on a
 * device nobody has yet.
 */

namespace NeuronClientTests
{
namespace
{

/// One frame's worth of contacts, built by naming them.
struct ContactFrame
{
  InputFrame frame;

  ContactFrame() { frame.windowFocused = true; }

  ContactFrame& Contact(std::uint32_t _id, std::int32_t _x, std::int32_t _y, PointerPhase _phase)
  {
    frame.contacts[frame.contactCount] = PointerContact{_id, _x, _y, _phase};
    ++frame.contactCount;
    return *this;
  }
};

/// One frame at 60 Hz. The dwell is 350 ms, so a long press is about 21 of
/// these -- the loops below run past that rather than counting exactly, because
/// what is asserted is that it fires once, not which frame it lands on.
constexpr float FRAME_SECONDS = 1.0f / 60.0f;

/*
 * A press and a lift at one point, with `_idleFrames` of nothing before it, and
 * the run this tap belongs to.
 *
 * The idle frames are the point rather than padding: the gap between two taps
 * is measured across frames that carry no contact at all, which is precisely
 * where an interval aged only on the frames that reach the state machine would
 * stop counting -- and the window would then depend on how fast the player
 * lifted rather than on how long they waited.
 */
[[nodiscard]] std::uint32_t TapAt(GestureRecognizer& _gestures, const GestureTuning& _tuning, std::int32_t _x,
                                  std::int32_t _y, int _idleFrames)
{
  for (int frame = 0; frame < _idleFrames; ++frame)
  {
    _gestures.Update(ContactFrame().frame, _tuning, FRAME_SECONDS);
  }
  _gestures.Update(ContactFrame().Contact(1, _x, _y, PointerPhase::Down).frame, _tuning, FRAME_SECONDS);
  return _gestures.Update(ContactFrame().Contact(1, _x, _y, PointerPhase::Up).frame, _tuning, FRAME_SECONDS).tapCount;
}

} // namespace

TEST_CLASS(GestureTests)
{
public:
  TEST_METHOD(APressThatLiftsWhereItLandedIsATap)
  {
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(gestures.State().phase == GesturePhase::Pressing, L"a contact going down is a press");
    Assert::IsFalse(gestures.State().tapped, L"a press is not a tap until it lifts");
    Assert::AreEqual(0.0f, gestures.State().deltaX, 0.0f, L"the frame a contact lands on has no movement in it");

    // Inside the slop, so it is still the same press: a finger that moved four
    // pixels did not change its mind.
    const GestureState& lifted = gestures.Update(ContactFrame().Contact(1, 104, 103, PointerPhase::Up).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(lifted.tapped, L"a press that lifts inside the slop is a tap");
    Assert::AreEqual(104.0f, lifted.tapX, 0.0f, L"the tap is where the finger lifted, not where it landed");
    Assert::AreEqual(103.0f, lifted.tapY, 0.0f);
    Assert::IsTrue(lifted.phase == GesturePhase::None, L"and the gesture is over");
  }

  TEST_METHOD(AContactThatWandersPastTheSlopIsADragAndNeverATap)
  {
    /*
     * The fork the whole input model turns on: **drag pans, long-press
     * commands**. Which one a contact became is decided by whether it moved
     * before it dwelled, and a drag that lifts must not also read as a tap --
     * that would select whatever the camera happened to stop over.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    const GestureState& moved = gestures.Update(ContactFrame().Contact(1, 130, 100, PointerPhase::Held).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(moved.phase == GesturePhase::Dragging, L"moving past the slop is a drag");
    Assert::AreEqual(30.0f, moved.deltaX, 0.0f, L"the frame's own movement is what a pan reads");
    Assert::AreEqual(0.0f, moved.dwellProgress, 0.0f, L"and the dwell is abandoned rather than paused");

    const GestureState& lifted = gestures.Update(ContactFrame().Contact(1, 130, 100, PointerPhase::Up).frame, tuning, FRAME_SECONDS);
    Assert::IsFalse(lifted.tapped, L"a drag that lifts is not a tap");
  }

  TEST_METHOD(TheDwellIsVisibleFromTheFirstFrameAndFiresExactlyOnce)
  {
    /*
     * `puck-and-wheel.png`'s step 1: *"The dwell is visible from the first
     * millisecond -- a ring that fills -- so a player who did not mean to
     * long-press knows to lift."* A recognizer that only reported the completed
     * press could not draw that ring, which is why `dwellProgress` is a level
     * rather than the edge being the whole answer.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(gestures.State().dwellProgress > 0.0f, L"the ring fills from the first frame");
    Assert::IsFalse(gestures.State().longPressed, L"and the press has not fired yet");

    std::uint32_t fired = 0;
    for (std::uint32_t frame = 0; frame < 60; ++frame)
    {
      if (gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Held).frame, tuning, FRAME_SECONDS).longPressed)
      {
        ++fired;
      }
    }
    Assert::AreEqual<std::uint32_t>(1, fired, L"a long press is an edge and fires once, however long the finger stays");
    Assert::IsTrue(gestures.State().phase == GesturePhase::Holding, L"and holds the contact until it lifts");
    Assert::AreEqual(1.0f, gestures.State().dwellProgress, 0.0f, L"with the ring full");

    const GestureState& lifted = gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Up).frame, tuning, FRAME_SECONDS);
    Assert::IsFalse(lifted.tapped, L"a completed long press is not also a tap on the way out");
    Assert::IsTrue(lifted.phase == GesturePhase::None);
  }

  TEST_METHOD(WanderingPartWayThroughTheDwellHandsTheContactToTheCamera)
  {
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    for (std::uint32_t frame = 0; frame < 10; ++frame)
    {
      gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Held).frame, tuning, FRAME_SECONDS);
    }
    Assert::IsTrue(gestures.State().phase == GesturePhase::Pressing, L"ten frames is not yet the dwell");
    Assert::IsTrue(gestures.State().dwellProgress > 0.0f && gestures.State().dwellProgress < 1.0f, L"part-way through it");

    gestures.Update(ContactFrame().Contact(1, 140, 100, PointerPhase::Held).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(gestures.State().phase == GesturePhase::Dragging, L"wandering takes the contact to the camera");

    std::uint32_t fired = 0;
    for (std::uint32_t frame = 0; frame < 60; ++frame)
    {
      if (gestures.Update(ContactFrame().Contact(1, 140, 100, PointerPhase::Held).frame, tuning, FRAME_SECONDS).longPressed)
      {
        ++fired;
      }
    }
    Assert::AreEqual<std::uint32_t>(0, fired, L"and the dwell does not complete later from a standstill mid-drag");
  }

  TEST_METHOD(TwoFingersSeparatingArePinchingAndTakeTheDwellWithThem)
  {
    /*
     * A pinch is the one thing allowed to take a press away from the order
     * surfaces: two fingers spreading is a player zooming, and a puck opening
     * underneath is a command they did not ask for.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    gestures.Update(
      ContactFrame().Contact(1, 100, 100, PointerPhase::Held).Contact(2, 200, 100, PointerPhase::Down).frame, tuning,
      FRAME_SECONDS);
    Assert::IsTrue(gestures.State().secondContactDown, L"the second contact is reported as soon as it lands");
    Assert::IsTrue(gestures.State().phase == GesturePhase::Pressing, L"a finger resting alongside is not yet a pinch");

    const GestureState& pinched = gestures.Update(
      ContactFrame().Contact(1, 100, 100, PointerPhase::Held).Contact(2, 300, 100, PointerPhase::Held).frame, tuning,
      FRAME_SECONDS);
    Assert::IsTrue(pinched.phase == GesturePhase::Pinching, L"separating past the pinch slop is a pinch");
    // 200 px from 100 px, and asserted on the *entry* frame: a scale of 1 there
    // and 2 on the next frame is a zoom that jumps.
    Assert::AreEqual(2.0f, pinched.pinchScale, 0.01f, L"the scale is the separation over the one it started at");
    Assert::AreEqual(200.0f, pinched.pinchCentreX, 0.0f, L"and the centre is the midpoint of the two");

    std::uint32_t fired = 0;
    for (std::uint32_t frame = 0; frame < 60; ++frame)
    {
      if (gestures
            .Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Held).Contact(2, 300, 100, PointerPhase::Held).frame,
                    tuning, FRAME_SECONDS)
            .longPressed)
      {
        ++fired;
      }
    }
    Assert::AreEqual<std::uint32_t>(0, fired, L"a pinch never lets the dwell complete underneath it");
  }

  TEST_METHOD(ASecondFingerThatRestsStillDoesNotCancelTheLongPress)
  {
    /*
     * The distinction the pinch slop exists for. A *held* second finger is how
     * the design appends an order to the queue rather than replacing it, so it
     * must not read as a pinch -- otherwise the append gesture would cancel the
     * very command it is qualifying.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    std::uint32_t fired = 0;
    for (std::uint32_t frame = 0; frame < 60; ++frame)
    {
      if (gestures
            .Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Held).Contact(2, 200, 100, PointerPhase::Held).frame,
                    tuning, FRAME_SECONDS)
            .longPressed)
      {
        ++fired;
      }
    }
    Assert::AreEqual<std::uint32_t>(1, fired, L"a held second finger lets the dwell complete");
    Assert::IsTrue(gestures.State().secondContactDown, L"and is still reported");
    Assert::AreEqual(200.0f, gestures.State().secondX, 0.0f, L"where it actually is, for the facing twist to read");
  }

  TEST_METHOD(TheSecondFingerLiftingMidPinchLeavesADragRatherThanAFrozenScale)
  {
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    gestures.Update(
      ContactFrame().Contact(1, 100, 100, PointerPhase::Held).Contact(2, 200, 100, PointerPhase::Down).frame, tuning,
      FRAME_SECONDS);
    gestures.Update(
      ContactFrame().Contact(1, 100, 100, PointerPhase::Held).Contact(2, 300, 100, PointerPhase::Held).frame, tuning,
      FRAME_SECONDS);
    Assert::IsTrue(gestures.State().phase == GesturePhase::Pinching);

    const GestureState& alone = gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Held).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(alone.phase == GesturePhase::Dragging, L"one finger left of a pinch is a drag");
    Assert::AreEqual(1.0f, alone.pinchScale, 0.0f, L"and the scale stops rather than holding its last value");
    Assert::IsFalse(alone.secondContactDown);
  }

  TEST_METHOD(AnUnfocusedFrameDropsTheGestureAndReportsNoEdge)
  {
    /*
     * A finger the window stopped hearing about may already have lifted, and a
     * dwell that completed while the player was in another application would
     * fire an order they never gave.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);

    InputFrame elsewhere;
    elsewhere.windowFocused = false;
    const GestureState& gone = gestures.Update(elsewhere, tuning, FRAME_SECONDS);
    Assert::IsFalse(gone.tapped, L"an unfocused frame reports no tap");
    Assert::IsFalse(gone.longPressed, L"and no long press");
    Assert::IsTrue(gone.phase == GesturePhase::None, L"and drops whatever was in progress");
  }

  TEST_METHOD(AContactThatVanishesWithoutLiftingIsNotAClickThePlayerMade)
  {
    /*
     * A well-behaved window sends an `Up`; a lost capture or a device reset does
     * not. Synthesising a tap from a contact that simply stopped being reported
     * would put a selection wherever the finger happened to be.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);

    const ContactFrame empty;
    const GestureState& gone = gestures.Update(empty.frame, tuning, FRAME_SECONDS);
    Assert::IsFalse(gone.tapped, L"a vanished contact taps nothing");
    Assert::IsTrue(gone.phase == GesturePhase::None);
  }

  TEST_METHOD(AStrayLaterContactCannotDisplaceTheGestureInProgress)
  {
    /*
     * Why the frame carries five contacts and the recognizer follows the two
     * oldest: a palm or a resting knuckle landing later must not become the
     * finger that places an order. It becomes the *second* contact at worst,
     * which the surfaces can ignore.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(7, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    const GestureState& crowded = gestures.Update(
      ContactFrame().Contact(7, 100, 100, PointerPhase::Held).Contact(9, 400, 400, PointerPhase::Down).frame, tuning,
      FRAME_SECONDS);

    Assert::AreEqual(100.0f, crowded.originX, 0.0f, L"the primary is still the first finger that went down");
    Assert::AreEqual(100.0f, crowded.originY, 0.0f);
    Assert::AreEqual(400.0f, crowded.secondX, 0.0f, L"and the newcomer is the second contact");
  }

  TEST_METHOD(CancelReportsNothingAtAll)
  {
    /*
     * The property that makes `Cancel` usable from a surface opening under the
     * finger: a cancelled gesture must not fire the tap or the long press it
     * was on its way to. It is also the distinction that caught a real bug --
     * ending a gesture and cancelling one are two operations, because a tap
     * *ends* the gesture and has to survive its own cleanup.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 100, 100, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    gestures.Cancel();

    Assert::IsTrue(gestures.State().phase == GesturePhase::None, L"cancel drops the gesture");
    Assert::IsFalse(gestures.State().tapped, L"and reports no tap");
    Assert::IsFalse(gestures.State().longPressed, L"and no long press");
  }

  TEST_METHOD(TheTapEdgeFiresOnBothTapsOfADoubleSoNothingWaits)
  {
    /*
     * The decision I2 turns on (Plan-of-Record §1, rule 2). The roster's rule
     * is that a tap takes the wing and a double tap *also* frames it -- so a
     * consumer acts on the first tap and adds on the second, and `tapped` has
     * to fire on both for that to be possible.
     *
     * The alternative is holding every tap for the double-tap window to find
     * out whether a second is coming, which is a third of a second of nothing
     * on the gesture a player makes most -- and the same delay in front of an
     * order surface once I3 reaches it.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    gestures.Update(ContactFrame().Contact(1, 200, 150, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    const GestureState& first =
        gestures.Update(ContactFrame().Contact(1, 200, 150, PointerPhase::Up).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(first.tapped, L"the first tap fires the frame it lifts");
    Assert::AreEqual<std::uint32_t>(1, first.tapCount, L"and says it is the first of its run");

    gestures.Update(ContactFrame().Contact(1, 200, 150, PointerPhase::Down).frame, tuning, FRAME_SECONDS);
    const GestureState& second =
        gestures.Update(ContactFrame().Contact(1, 200, 150, PointerPhase::Up).frame, tuning, FRAME_SECONDS);
    Assert::IsTrue(second.tapped, L"the second fires too, rather than replacing the first");
    Assert::AreEqual<std::uint32_t>(2, second.tapCount, L"which is what tells a consumer to add the framing");
  }

  TEST_METHOD(TapsPairOnlyWhenTheyAreBothQuickAndNear)
  {
    /*
     * Both conditions, because either alone is a coincidence: two taps a second
     * apart on one roster row are two decisions, and two taps at opposite
     * corners of the screen are not aimed at the same thing however fast they
     * came.
     *
     * The distance uses the whole 48 px target floor rather than a fraction of
     * it -- two taps are "the same place" exactly when they could be aiming at
     * the same thing -- which is also what stops a tap on a *neighbouring* row
     * reading as a double on the one above it.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    Assert::AreEqual<std::uint32_t>(1, TapAt(gestures, tuning, 100, 100, 0), L"the first tap is a single");
    Assert::AreEqual<std::uint32_t>(2, TapAt(gestures, tuning, 100, 100, 1), L"a quick second tap nearby is a double");
    Assert::AreEqual<std::uint32_t>(3, TapAt(gestures, tuning, 100, 100, 1), L"and a third keeps counting");

    Assert::AreEqual<std::uint32_t>(1, TapAt(gestures, tuning, 400, 400, 1),
                                    L"a tap out of reach of the last one starts a new run");

    // Thirty idle frames at 60 Hz is half a second, comfortably past the
    // 350 ms window -- and every one of them carries no contact at all.
    Assert::AreEqual<std::uint32_t>(1, TapAt(gestures, tuning, 400, 400, 30),
                                    L"and so does a tap that arrives after the window has closed");
  }

  TEST_METHOD(AGestureTakenAwayMidRunDoesNotPairWithTheNextTap)
  {
    /*
     * A double tap straddling a cancel is two unrelated presses. The case that
     * makes it matter is `OnSurfaceChanged`: a surface opens under the finger
     * between the two taps, and the one that lands on the new screen must not
     * arrive as the second half of a gesture the player made on the old one.
     */
    GestureRecognizer gestures;
    const GestureTuning tuning;

    Assert::AreEqual<std::uint32_t>(1, TapAt(gestures, tuning, 100, 100, 0));
    gestures.Cancel();
    Assert::AreEqual<std::uint32_t>(1, TapAt(gestures, tuning, 100, 100, 1),
                                    L"the tap after a cancel starts a run rather than finishing one");
  }
};

} // namespace NeuronClientTests
