#include "pch.h"
#include "CppUnitTest.h"

#include "InputMap.h"
#include "InputRouter.h"
#include "SurfaceStack.h"
#include "UiDrawList.h"
#include "UiFocus.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The navigation machinery (ADR-020 §1-§3, T3a).
 *
 * Three types that only mean anything together: which screen is live, who owns
 * the keyboard, and who got this frame's input. None of them touches a device,
 * which is what lets the whole of "the thing you press is the thing you see"
 * be checked on a machine with no GPU -- the same property that makes
 * `CommandRow` testable, applied to the part of the client that navigates.
 */

namespace NeuronClientTests
{
namespace
{

/// A frame with a focused window and nothing pressed -- the state every test
/// below starts from and then says one thing about.
[[nodiscard]] InputFrame QuietFrame()
{
  InputFrame frame;
  frame.windowFocused = true;
  frame.viewportWidth = 1440;
  frame.viewportHeight = 900;
  return frame;
}

[[nodiscard]] InputFrame FrameWithLeftPressAt(std::int32_t _x, std::int32_t _y)
{
  InputFrame frame = QuietFrame();
  frame.cursorX = _x;
  frame.cursorY = _y;
  frame.buttonPressed[static_cast<std::uint32_t>(InputButton::Left)] = true;
  frame.buttonDown[static_cast<std::uint32_t>(InputButton::Left)] = true;
  return frame;
}

[[nodiscard]] InputFrame FrameWithWheelAt(float _steps, std::int32_t _x, std::int32_t _y)
{
  InputFrame frame = QuietFrame();
  frame.cursorX = _x;
  frame.cursorY = _y;
  frame.wheelSteps = _steps;
  return frame;
}

[[nodiscard]] InputFrame FrameWithAction(InputAction _action)
{
  InputFrame frame = QuietFrame();
  frame.actionPressed[static_cast<std::uint32_t>(_action)] = true;
  frame.actionDown[static_cast<std::uint32_t>(_action)] = true;
  return frame;
}

constexpr UiRect PANEL{100.0f, 100.0f, 200.0f, 200.0f};
constexpr UiRect ELSEWHERE{600.0f, 600.0f, 100.0f, 100.0f};

/*
 * The two frames a mouse click actually produces, mirroring
 * `Window::UpdateMouseContact` field for field (R30).
 *
 * Modelled rather than approximated, because the whole point of the pair below
 * is *when* each half arrives: the window fills contact zero from the left
 * button, so a click goes `Down` on the frame the button falls and `Up` on the
 * frame it rises -- and the recogniser reports a tap on the second of those.
 * A helper that set both in one frame would test a machine this client does not
 * have.
 */
[[nodiscard]] InputFrame MouseDownAt(std::int32_t _x, std::int32_t _y)
{
  InputFrame frame = FrameWithLeftPressAt(_x, _y);
  frame.contacts[0] = PointerContact{0u, _x, _y, PointerPhase::Down};
  frame.contactCount = 1;
  return frame;
}

[[nodiscard]] InputFrame MouseUpAt(std::int32_t _x, std::int32_t _y)
{
  InputFrame frame = QuietFrame();
  frame.cursorX = _x;
  frame.cursorY = _y;
  frame.buttonReleased[static_cast<std::uint32_t>(InputButton::Left)] = true;
  frame.contacts[0] = PointerContact{0u, _x, _y, PointerPhase::Up};
  frame.contactCount = 1;
  return frame;
}

/// One frame at 60 Hz, the same step `GestureTests` uses.
constexpr float FRAME_SECONDS = 1.0f / 60.0f;

/// A field on the hangar, which is the first thing in this game that will hold
/// focus (a wing rename -- ADR-017 §6).
constexpr WidgetId WING_NAME_FIELD = 7;

} // namespace

TEST_CLASS(SurfaceStackTests)
{
public:
  TEST_METHOD(TheStackStartsOnItsBaseAndIsNeverEmpty)
  {
    const SurfaceStack stack{SurfaceId::Tactical};
    Assert::AreEqual<std::uint32_t>(1, stack.Depth());
    Assert::IsTrue(stack.Active() == SurfaceId::Tactical);
    Assert::IsTrue(stack.Base() == SurfaceId::Tactical);
  }

  TEST_METHOD(APushGoesToTheSurfaceAndReportsBothEnds)
  {
    SurfaceStack stack{SurfaceId::Tactical};
    const SurfaceChange change = stack.Push(SurfaceId::Station);

    Assert::IsTrue(change.changed);
    Assert::IsTrue(change.exited == SurfaceId::Tactical);
    Assert::IsTrue(change.entered == SurfaceId::Station);
    Assert::IsTrue(stack.Active() == SurfaceId::Station);
    Assert::AreEqual<std::uint32_t>(2, stack.Depth());
  }

  TEST_METHOD(BackTacticalAndBackAreOneMechanism)
  {
    /*
     * ADR-020 §1's claim, as a test rather than as a sentence: a surface already
     * in the stack is *returned to*, so `◀ TACTICAL` from the hangar is the same
     * operation as `◀ BACK` from settings and neither needs its own path.
     */
    SurfaceStack stack{SurfaceId::Tactical};
    (void)stack.Push(SurfaceId::Map);
    (void)stack.Push(SurfaceId::Station);

    const SurfaceChange viaPush = stack.Push(SurfaceId::Map);
    Assert::IsTrue(viaPush.changed);
    Assert::IsTrue(viaPush.entered == SurfaceId::Map);
    Assert::AreEqual<std::uint32_t>(2, stack.Depth(), L"a surface in the stack is returned to, not pushed again");

    const SurfaceChange viaBack = stack.Back();
    Assert::IsTrue(viaBack.changed);
    Assert::IsTrue(viaBack.entered == SurfaceId::Tactical);
    Assert::AreEqual<std::uint32_t>(1, stack.Depth());
  }

  TEST_METHOD(NavigatingInCirclesDoesNotBuildALadder)
  {
    // The failure the pop-back rule exists to prevent: a player who flips
    // between two screens twenty times must not be twenty steps from home.
    SurfaceStack stack{SurfaceId::Tactical};
    for (std::uint32_t lap = 0; lap < 20; ++lap)
    {
      (void)stack.Push(SurfaceId::Map);
      (void)stack.Push(SurfaceId::Tactical);
    }

    Assert::AreEqual<std::uint32_t>(1, stack.Depth());
    Assert::IsTrue(stack.Active() == SurfaceId::Tactical);
  }

  TEST_METHOD(PressingTheButtonForTheScreenYouAreLookingAtIsNotANavigation)
  {
    // `changed` false is what stops the hangar reconciling its composer against
    // the roster every time somebody presses STATION while the hangar is up.
    SurfaceStack stack{SurfaceId::Tactical};
    (void)stack.Push(SurfaceId::Station);

    const SurfaceChange again = stack.Push(SurfaceId::Station);
    Assert::IsFalse(again.changed);
    Assert::AreEqual<std::uint32_t>(2, stack.Depth());
    Assert::IsTrue(stack.Active() == SurfaceId::Station);
  }

  TEST_METHOD(BackAtTheBaseGoesNowhereAndSaysSo)
  {
    SurfaceStack stack{SurfaceId::Tactical};
    const SurfaceChange change = stack.Back();

    Assert::IsFalse(change.changed);
    Assert::AreEqual<std::uint32_t>(1, stack.Depth());
    Assert::IsTrue(stack.Active() == SurfaceId::Tactical);
  }

  TEST_METHOD(ExactlyOneSurfaceLeavesPerNavigationEvenPastTwoBreadcrumbs)
  {
    /*
     * The reason `SurfaceChange` is two ids rather than a list: only one surface
     * is live, so only one can have an exit to run. Popping back past two
     * breadcrumbs still exits one screen -- the others stopped being active when
     * they were covered and ran their exits then.
     */
    SurfaceStack stack{SurfaceId::Tactical};
    (void)stack.Push(SurfaceId::Map);
    (void)stack.Push(SurfaceId::System);
    (void)stack.Push(SurfaceId::Station);

    const SurfaceChange change = stack.Push(SurfaceId::Tactical);
    Assert::IsTrue(change.changed);
    Assert::IsTrue(change.exited == SurfaceId::Station, L"the live surface is the one that exits");
    Assert::IsTrue(change.entered == SurfaceId::Tactical);
    Assert::AreEqual<std::uint32_t>(1, stack.Depth());
    Assert::IsFalse(stack.Holds(SurfaceId::System), L"the breadcrumbs above the target go with it");
  }

  TEST_METHOD(AFullStackDropsTheOldestBreadcrumbAndKeepsTheWayHome)
  {
    SurfaceStack stack{SurfaceId::Tactical};
    (void)stack.Push(SurfaceId::Map);
    (void)stack.Push(SurfaceId::System);
    (void)stack.Push(SurfaceId::Station);
    Assert::AreEqual<std::uint32_t>(MAX_SURFACE_DEPTH, stack.Depth());

    const SurfaceChange change = stack.Push(SurfaceId::Settings);

    // The push is never refused -- a control that does nothing is worse than any
    // depth policy -- and the base survives, because it is the way home.
    Assert::IsTrue(change.changed);
    Assert::IsTrue(stack.Active() == SurfaceId::Settings);
    Assert::AreEqual<std::uint32_t>(MAX_SURFACE_DEPTH, stack.Depth());
    Assert::IsTrue(stack.Base() == SurfaceId::Tactical);
    Assert::IsFalse(stack.Holds(SurfaceId::Map), L"the oldest breadcrumb is the one dropped");

    // And ◀ BACK still means "undo the step I just took", which is what dropping
    // from the bottom rather than the top buys.
    Assert::IsTrue(stack.Back().entered == SurfaceId::Station);
  }

  TEST_METHOD(RebasingLeavesNowhereToGoBackTo)
  {
    // A player who has logged in cannot go back to logging in, which a push
    // could not express.
    SurfaceStack stack{SurfaceId::Login};
    const SurfaceChange change = stack.Rebase(SurfaceId::Tactical);

    Assert::IsTrue(change.changed);
    Assert::IsTrue(change.exited == SurfaceId::Login);
    Assert::IsTrue(change.entered == SurfaceId::Tactical);
    Assert::AreEqual<std::uint32_t>(1, stack.Depth());
    Assert::IsFalse(stack.Holds(SurfaceId::Login));
    Assert::IsFalse(stack.Back().changed);
  }

  TEST_METHOD(RebasingOntoTheSurfaceAlreadyStandingAloneChangesNothing)
  {
    // A reconnect landing back where it was. Reporting an entry would re-run
    // entry work on a client that did not move.
    SurfaceStack stack{SurfaceId::Tactical};
    Assert::IsFalse(stack.Rebase(SurfaceId::Tactical).changed);

    // But re-basing away from a *stack* is a real change, even onto its base.
    (void)stack.Push(SurfaceId::Station);
    Assert::IsTrue(stack.Rebase(SurfaceId::Tactical).changed);
  }

  TEST_METHOD(TheWorldHalfIsRecordedBeneathTacticalAndNothingElse)
  {
    /*
     * ADR-020 §1's table. It is where U5's node budget comes from: a full-screen
     * surface skips Opaque, Nebula and OverlayWorld and the resolve between
     * them, and this is the predicate that decides it.
     */
    Assert::IsTrue(SurfaceRecordsWorld(SurfaceId::Tactical));

    Assert::IsFalse(SurfaceRecordsWorld(SurfaceId::Map));
    Assert::IsFalse(SurfaceRecordsWorld(SurfaceId::System));
    Assert::IsFalse(SurfaceRecordsWorld(SurfaceId::Station));
    Assert::IsFalse(SurfaceRecordsWorld(SurfaceId::Settings));
    Assert::IsFalse(SurfaceRecordsWorld(SurfaceId::Login));
    Assert::IsFalse(SurfaceRecordsWorld(SurfaceId::Queue));
  }

  TEST_METHOD(PreSessionSurfacesCarryNoToastLayer)
  {
    // Nothing can happen before there is a session, so there is nothing to
    // announce over one: UPDATE REQUIRED is a screen, not a notice.
    for (std::uint32_t id = 0; id < SURFACE_ID_COUNT; ++id)
    {
      const auto surface = static_cast<SurfaceId>(id);
      Assert::IsTrue(SurfaceCarriesToasts(surface) == !SurfaceIsPreSession(surface));
    }

    Assert::IsFalse(SurfaceIsPreSession(SurfaceId::Settings), L"settings is in-session");
    Assert::IsTrue(SurfaceIsPreSession(SurfaceId::Login));
    Assert::IsTrue(SurfaceIsPreSession(SurfaceId::ReconnectUnderFire));
  }
};

TEST_CLASS(UiFocusTests)
{
public:
  TEST_METHOD(AtMostOneOwnerExists)
  {
    UiFocus focus;
    Assert::IsFalse(focus.Held());

    focus.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);
    Assert::IsTrue(focus.HeldBy(SurfaceId::Station, WING_NAME_FIELD));

    // The second claim does not queue behind the first, it replaces it.
    focus.Claim(SurfaceId::Settings, 3, FocusKind::BindingCapture);
    Assert::IsFalse(focus.HeldBy(SurfaceId::Station, WING_NAME_FIELD));
    Assert::IsTrue(focus.HeldBy(SurfaceId::Settings, 3));
  }

  TEST_METHOD(AClaimNamingNothingIsAClear)
  {
    UiFocus focus;
    focus.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);
    focus.Claim(SurfaceId::Station, NO_WIDGET, FocusKind::EditableField);
    Assert::IsFalse(focus.Held());
  }

  TEST_METHOD(AnExitClearsOnlyItsOwnSurfacesFocus)
  {
    /*
     * Why focus names the surface as well as the widget. Clearing on *any* exit
     * would drop the hangar's rename box because the settings sheet closed over
     * it and went away again.
     */
    UiFocus focus;
    focus.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);

    focus.ClearIfOn(SurfaceId::Settings);
    Assert::IsTrue(focus.HeldBy(SurfaceId::Station, WING_NAME_FIELD));

    focus.ClearIfOn(SurfaceId::Station);
    Assert::IsFalse(focus.Held());
  }

  TEST_METHOD(OnlyABindingCaptureClaimsTheWholeKeyboard)
  {
    UiFocus focus;
    Assert::IsFalse(focus.ClaimsWholeKeyboard());

    focus.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);
    Assert::IsFalse(focus.ClaimsWholeKeyboard(), L"a field takes the text channel, not the keyboard");

    focus.Claim(SurfaceId::Settings, 3, FocusKind::BindingCapture);
    Assert::IsTrue(focus.ClaimsWholeKeyboard());
  }
};

TEST_CLASS(InputRouterTests)
{
public:
  TEST_METHOD(AWheelNotchOverAListDoesNotCostTheClickInTheSameFrame)
  {
    /*
     * The reason the channels are three flags and not one. A player scrolling a
     * wing column with a click still pending has done two things, and a router
     * that consumed both on the first claim would make the click disappear with
     * nothing to blame.
     */
    InputFrame frame = FrameWithLeftPressAt(150, 150);
    frame.wheelSteps = 1.0f;

    InputRouter router;
    router.Begin(frame, UiFocus{});

    Assert::AreEqual(1.0f, router.ClaimWheelIn(PANEL), 1e-6f);
    Assert::IsFalse(router.WheelAvailable());
    Assert::IsTrue(router.PointerAvailable(), L"the wheel claim must not take the pointer with it");
    Assert::IsTrue(router.Pressed(InputButton::Left));
  }

  TEST_METHOD(AClaimedPointerIsGoneForEveryLaterStage)
  {
    InputRouter router;
    router.Begin(FrameWithLeftPressAt(150, 150), UiFocus{});

    Assert::IsTrue(router.Pressed(InputButton::Left));
    Assert::IsTrue(router.ClaimPointerIn(PANEL));

    // The world stage, asking the same question and getting the other answer.
    Assert::IsFalse(router.Pressed(InputButton::Left));
    Assert::IsFalse(router.Down(InputButton::Left));
    Assert::IsFalse(router.PointerAvailable());
  }

  TEST_METHOD(TheSecondClaimantIsToldItWasTooLate)
  {
    // A stage can tell "I took this" from "somebody above me did", which is the
    // difference between acting and double-acting on one gesture.
    InputRouter router;
    router.Begin(FrameWithLeftPressAt(150, 150), UiFocus{});

    Assert::IsTrue(router.ClaimPointer());
    Assert::IsFalse(router.ClaimPointer());
  }

  TEST_METHOD(APressOutsideTheRectClaimsNothing)
  {
    InputRouter router;
    router.Begin(FrameWithLeftPressAt(650, 650), UiFocus{});

    Assert::IsFalse(router.ClaimPointerIn(PANEL));
    Assert::IsTrue(router.PointerAvailable());
    Assert::IsTrue(router.ClaimPointerIn(ELSEWHERE));
  }

  TEST_METHOD(AWheelClaimNeedsNotchesAndNotOnlyAPosition)
  {
    // Claiming on hover alone would stop the camera zooming any time the cursor
    // came to rest on a panel.
    InputRouter router;
    router.Begin(FrameWithWheelAt(0.0f, 150, 150), UiFocus{});

    Assert::AreEqual(0.0f, router.ClaimWheelIn(PANEL), 1e-6f);
    Assert::IsTrue(router.WheelAvailable());
  }

  TEST_METHOD(TheWheelGoesToTheListUnderTheCursorAndNotToTheOneBeside)
  {
    InputRouter router;
    router.Begin(FrameWithWheelAt(-2.0f, 150, 150), UiFocus{});

    Assert::AreEqual(0.0f, router.ClaimWheelIn(ELSEWHERE), 1e-6f);
    Assert::AreEqual(-2.0f, router.ClaimWheelIn(PANEL), 1e-6f);
    Assert::AreEqual(0.0f, router.WheelSteps(), 1e-6f, L"a claimed wheel reports nothing to later stages");
  }

  TEST_METHOD(WTypesOrPansButNeverBoth)
  {
    /*
     * ADR-020 §2's headline rule. `W` is `PanForward`'s key and a letter, so a
     * focused field has to take it -- and F1 must still work while typing,
     * because a diagnostics toggle that stopped working inside a rename box
     * would be a strange thing to debug.
     */
    UiFocus field;
    field.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);

    InputRouter typing;
    typing.Begin(FrameWithAction(InputAction::PanForward), field);
    Assert::IsFalse(typing.Pressed(InputAction::PanForward));
    Assert::IsFalse(typing.Down(InputAction::PanForward));

    InputRouter panning;
    panning.Begin(FrameWithAction(InputAction::PanForward), UiFocus{});
    Assert::IsTrue(panning.Pressed(InputAction::PanForward));

    InputRouter diagnostics;
    diagnostics.Begin(FrameWithAction(InputAction::ToggleDiagnostics), field);
    Assert::IsTrue(diagnostics.Pressed(InputAction::ToggleDiagnostics), L"F1 still works while typing");
  }

  TEST_METHOD(TheEditingKeysBelongToAFocusedFieldToo)
  {
    /*
     * The half that is easy to miss: `PanForward` is `W` *and* `Up`, and
     * `ResetView` is `Home`. Both keys are editing keys inside a field, so
     * suppressing the action is right on both counts -- which is why the rule
     * can be per action even though printability is per key.
     */
    UiFocus field;
    field.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);

    InputRouter router;
    router.Begin(FrameWithAction(InputAction::ResetView), field);
    Assert::IsFalse(router.Pressed(InputAction::ResetView));

    Assert::IsFalse(ActionSurvivesTextEditing(InputAction::PanForward));
    Assert::IsFalse(ActionSurvivesTextEditing(InputAction::ResetView));
    Assert::IsFalse(ActionSurvivesTextEditing(InputAction::CycleParameter));
    Assert::IsTrue(ActionSurvivesTextEditing(InputAction::Back));
    Assert::IsTrue(ActionSurvivesTextEditing(InputAction::SelectAdd));

    /*
     * `Confirm` is the opposite answer from `Back`, and the pair is what the
     * table exists to make somebody decide (T3).
     *
     * Escape means something to a *surface* as well as to a field -- go back --
     * so it is routed and reaches one when no field wants it. Enter means
     * nothing to any surface in this client, so letting it through would be
     * reserving a key for a future somebody.
     */
    Assert::IsFalse(ActionSurvivesTextEditing(InputAction::Confirm));
  }

  TEST_METHOD(EnterIsTheFieldsAndReachesNothingBehindIt)
  {
    UiFocus field;
    field.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);

    InputRouter router;
    router.Begin(FrameWithAction(InputAction::Confirm), field);
    Assert::IsFalse(router.Pressed(InputAction::Confirm), L"a focused field swallows it whole");

    // And with no field, it still goes nowhere: nothing else listens for it.
    InputRouter bare;
    bare.Begin(FrameWithAction(InputAction::Confirm), UiFocus{});
    Assert::IsTrue(bare.Pressed(InputAction::Confirm), L"unsuppressed when nobody is typing");
  }

  TEST_METHOD(EscapeReachesTheFieldBeforeTheSurface)
  {
    /*
     * ADR-020 §2 spells the order out: focused field first (cancel the edit),
     * then the surface (go back), then nothing. The field cancels by *claiming*
     * rather than by being suppressed, because with no field focused the same
     * press has to reach the surface.
     */
    UiFocus field;
    field.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);

    InputRouter router;
    router.Begin(FrameWithAction(InputAction::Back), field);

    // The field's stage sees it...
    Assert::IsTrue(router.Pressed(InputAction::Back));
    Assert::IsTrue(router.ClaimKeyboard());

    // ...and the surface's does not.
    Assert::IsFalse(router.Pressed(InputAction::Back));
    Assert::IsFalse(router.KeyboardAvailable());
  }

  TEST_METHOD(EscapeReachesTheSurfaceWhenNoFieldWantsIt)
  {
    InputRouter router;
    router.Begin(FrameWithAction(InputAction::Back), UiFocus{});
    Assert::IsTrue(router.Pressed(InputAction::Back));
  }

  TEST_METHOD(ABindingCaptureTakesEvenTheFunctionKeys)
  {
    // The next key pressed is the answer it is waiting for, and a diagnostics
    // toggle that fired instead would be a binding the player could not set.
    UiFocus capture;
    capture.Claim(SurfaceId::Settings, 3, FocusKind::BindingCapture);

    InputRouter router;
    router.Begin(FrameWithAction(InputAction::ToggleDiagnostics), capture);
    Assert::IsFalse(router.Pressed(InputAction::ToggleDiagnostics));
    Assert::IsFalse(router.Pressed(InputAction::Back));
  }

  TEST_METHOD(OnlyTheFocusOwnerCanReadWhatWasTyped)
  {
    // "At most one focus owner" made structural: a widget that is not the owner
    // cannot read a character even by asking.
    InputFrame frame = QuietFrame();
    frame.characters[0] = U'a';
    frame.characterCount = 1;

    InputRouter unfocused;
    unfocused.Begin(frame, UiFocus{});
    Assert::IsTrue(unfocused.Characters().empty());

    UiFocus field;
    field.Claim(SurfaceId::Station, WING_NAME_FIELD, FocusKind::EditableField);
    InputRouter focused;
    focused.Begin(frame, field);
    Assert::AreEqual<std::size_t>(1, focused.Characters().size());
    Assert::IsTrue(focused.Characters()[0] == U'a');
  }

  TEST_METHOD(AnUnfocusedWindowRoutesNothingAndSaysWhy)
  {
    /*
     * A click and a key edge that arrived while the player was in another
     * window belong to that window. Starting every channel claimed is how no
     * stage has to remember to ask.
     */
    InputFrame frame = FrameWithLeftPressAt(150, 150);
    frame.actionPressed[static_cast<std::uint32_t>(InputAction::PanForward)] = true;
    frame.wheelSteps = 3.0f;
    frame.windowFocused = false;

    InputRouter router;
    router.Begin(frame, UiFocus{});

    Assert::IsTrue(router.WindowDeactivated());
    Assert::IsFalse(router.Pressed(InputButton::Left));
    Assert::IsFalse(router.Pressed(InputAction::PanForward));
    Assert::AreEqual(0.0f, router.WheelSteps(), 1e-6f);
    Assert::IsFalse(router.ClaimPointer(), L"there is nothing to claim");
  }

  TEST_METHOD(TheCursorIsReadableWhoeverOwnsThePointer)
  {
    // Position is not a claimable resource -- a stage that has lost the pointer
    // may still need to know where it is to draw a hover state it does not act
    // on.
    InputRouter router;
    router.Begin(FrameWithLeftPressAt(150, 175), UiFocus{});
    Assert::IsTrue(router.ClaimPointer());

    Assert::AreEqual(150.0f, router.CursorX(), 1e-6f);
    Assert::AreEqual(175.0f, router.CursorY(), 1e-6f);
    Assert::AreEqual<std::uint32_t>(1440, router.Frame().viewportWidth);
  }

  /*
   * --- the tap claim (R30) -----------------------------------------------
   *
   * `ClaimPointerIn` asks a mouse's question -- "did a button go down in this
   * rect" -- and a finger has no button. `ClaimTapIn` asks the one both answer.
   */

  TEST_METHOD(ATapClaimsWhereItLifted)
  {
    // The lift point and not the cursor: they differ by up to the slop, and the
    // lift is the last place the player saw their finger.
    GestureState gesture;
    gesture.tapped = true;
    gesture.tapX = 150.0f;
    gesture.tapY = 150.0f;

    InputRouter router;
    router.Begin(QuietFrame(), UiFocus{});
    router.NoteGesture(gesture);

    Assert::IsFalse(router.ClaimTapIn(ELSEWHERE), L"a rect the tap did not land in claims nothing");
    Assert::IsTrue(router.ClaimTapIn(PANEL));
    Assert::IsFalse(router.PointerAvailable(), L"and it spends the pointer like any other claim");
  }

  TEST_METHOD(AClaimedPointerHidesTheTapFromEveryLaterStage)
  {
    // The same sentence the button edges carry, which is why `ClaimTapIn` asks
    // through `Gesture()` rather than off the frame: a second way in would route
    // around the claim.
    GestureState gesture;
    gesture.tapped = true;
    gesture.tapX = 150.0f;
    gesture.tapY = 150.0f;

    InputRouter router;
    router.Begin(QuietFrame(), UiFocus{});
    router.NoteGesture(gesture);

    Assert::IsTrue(router.ClaimTapIn(PANEL));
    Assert::IsFalse(router.Gesture().tapped, L"the world asks the same question and gets the other answer");
    Assert::IsFalse(router.ClaimTapIn(PANEL), L"and the rect that took it cannot take it twice");
  }

  TEST_METHOD(ADragOverAControlActuatesNothing)
  {
    // A press used to fire the moment it landed. A drag that begins on a control
    // now does nothing at all -- the same rule the camera pan carries (a gesture
    // may only begin where it is allowed to), pointed at the chrome.
    GestureState gesture;
    gesture.phase = GesturePhase::Dragging;
    gesture.x = 150.0f;
    gesture.y = 150.0f;
    gesture.originX = 150.0f;
    gesture.originY = 150.0f;

    InputRouter router;
    router.Begin(QuietFrame(), UiFocus{});
    router.NoteGesture(gesture);

    Assert::IsFalse(router.ClaimTapIn(PANEL));
    Assert::IsTrue(router.PointerAvailable());
  }

  /*
   * **The defect R30's conversion was standing on**, asserted rather than
   * described.
   *
   * I2 rewrote the roster to read the gesture and left it behind a
   * `Pressed(InputButton::Left)` gate. Both halves were right; the gate could
   * not open, because a press edge is the frame a contact goes *down* and a tap
   * is the frame it comes *up*. Two frames, never one -- so a tap could not take
   * a wing, a second tap could not frame one, and a long-press could not add.
   *
   * This drives the real recogniser over the real frames `Window` produces, so
   * it fails if either side of that pair ever changes its mind.
   */
  TEST_METHOD(APressEdgeAndTheTapItBecomesAreNeverTheSameFrame)
  {
    GestureRecognizer gestures;
    const GestureTuning tuning;

    const InputFrame down = MouseDownAt(150, 150);
    const GestureState onDown = gestures.Update(down, tuning, FRAME_SECONDS);
    Assert::IsTrue(down.Pressed(InputButton::Left), L"the press edge is on the frame the button falls");
    Assert::IsFalse(onDown.tapped, L"and no tap has happened yet");

    const InputFrame up = MouseUpAt(150, 150);
    const GestureState onUp = gestures.Update(up, tuning, FRAME_SECONDS);
    Assert::IsTrue(onUp.tapped, L"the tap arrives on the lift");
    Assert::IsFalse(up.Pressed(InputButton::Left), L"and the press edge is long gone");

    // Which is the whole finding: a control gated on the press edge and reading
    // the tap is a control that can never act.
    InputRouter router;
    router.Begin(up, UiFocus{});
    router.NoteGesture(onUp);
    Assert::IsFalse(router.Pressed(InputButton::Left));
    Assert::IsTrue(router.ClaimTapIn(PANEL));
  }
};

} // namespace NeuronClientTests
