#pragma once

#include <cstdint>
#include <string_view>

/*
 * Input, and what the camera makes of it.
 *
 * Two halves, deliberately separated:
 *
 *  - `InputFrame` is what happened this frame, already reduced to logical
 *    buttons and actions. Window fills it from Win32 messages, because that is
 *    where the virtual-key codes belong.
 *  - `MapCameraInput` turns that into a `CameraIntent`. It is a pure function
 *    of the frame, the tuning and the elapsed time, so the binding scheme can
 *    be tested without a window and without a device.
 *
 * The bindings are chosen to survive the slices that follow. Left-drag became
 * box-select (S8) and right-drag the order puck (S9), so neither was spent
 * here: the camera takes the middle button, the wheel, the screen edge and the
 * keyboard, and Alt is what turns a middle-drag from a pan into an orbit.
 * Picking a binding that S9 had to take back would have been a worse kind of
 * cheap.
 *
 * `SelectAdd` and `QueueOrder` are both Shift and are still two actions. They
 * never collide -- one qualifies a left drag and the other a right drag -- and
 * keeping them separate is what makes the binding table honest: a player who
 * moves "append to queue" off Shift must not silently take "add to selection"
 * with it.
 *
 * `CycleParameter` is a **stand-in for a surface**. The command wheel's
 * formation sub-ring is what chooses a formation (`puck-and-wheel.png` §3), and
 * it arrives with the Ui pass in S11; until then a key steps the same list the
 * wheel will draw, so S10's formations are reachable without inventing a widget
 * that S11 would have to take back. Named for what it does rather than for
 * formations, because the list is whatever the game says the parameter means.
 */

namespace Neuron
{

class IsoCamera;

enum class InputButton : std::uint8_t
{
  Left,
  Right,
  Middle
};

inline constexpr std::uint32_t INPUT_BUTTON_COUNT = 3;

/*
 * Logical actions, not keys. Window owns the virtual-key table; everything
 * downstream of it speaks in intent, which is what makes rebinding a change in
 * one file rather than a search across the client.
 */
enum class InputAction : std::uint8_t
{
  PanLeft,
  PanRight,
  PanForward,
  PanBack,
  YawLeft,
  YawRight,
  ZoomIn,
  ZoomOut,
  ResetView,
  Modifier,         // Alt: turns a middle-drag into an orbit.
  SelectAdd,        // Shift: a click adjusts the selection instead of replacing it.
  QueueOrder,       // Shift: an order appends to the queue instead of replacing it.
  CycleParameter,   // F: steps the order parameter the puck will send (S10's formations).
  ToggleDiagnostics, // F1: shows/hides the Tier-1 counters strip (S14). The
                     // print's rule: no new gesture -- the toggle is a setting,
                     // plus an F-key where a keyboard exists (debug-hud.png §6).
  ToggleFeedFreeze,  // F10: the induced stall S7 owed and S14 needs. Cuts the
                     // snapshot feed where it stands, so the staleness path --
                     // the STALE marker, the top bar's chip, the strip's SNAP
                     // row -- can be seen at all. A healthy loopback session
                     // never goes stale on its own, which left every one of
                     // those unjudgeable. Deliberately absent from the HUD: an
                     // on-screen "feed cut" badge would alter the very picture
                     // this exists to let someone look at.

  Back,              // Escape: leave whatever is in front of you (ADR-020 §2).
                     // An *action* rather than a key the surfaces each test for
                     // themselves, because it is the one input with a routing
                     // order written into the design -- focused field first to
                     // cancel an edit, then the active surface to go back, then
                     // nothing -- and an order can only be honoured where the
                     // routing is. Arrives with T3, the first slice where there
                     // is anywhere to go back *to*.

  /*
   * Enter: finish what you are typing. `Back`'s twin, and it arrives with the
   * first editable field (T3's wing rename).
   *
   * **A convenience over the real commit rather than the commit itself**, which
   * is the reversal's shape applied to a keyboard: touch has no Enter, so a
   * field has to be finishable by tapping away from it, and that blur *is* the
   * commit. This saves a keyboard user the tap. A field with no focus never
   * sees it and nothing else listens, so it does nothing at all today outside
   * one.
   *
   * It reaches the buffer only as an action: `WM_CHAR` delivers `\r`, which
   * `IsStorableCodepoint` refuses along with every other C0 control, so the key
   * cannot arrive as text and as an edge at once.
   */
  Confirm
};

inline constexpr std::uint32_t INPUT_ACTION_COUNT = 17;

/// `InputFrame`'s action arrays are sized by the count above, so an action
/// added without touching it writes past their end rather than failing to
/// build. This is that build failure.
static_assert(static_cast<std::uint32_t>(InputAction::Confirm) + 1u == INPUT_ACTION_COUNT,
              "INPUT_ACTION_COUNT must count every InputAction");

/*
 * How many characters one frame may carry (ADR-020 §3).
 *
 * `WM_CHAR` arrivals between two `ConsumeInput` calls, already assembled from
 * UTF-16 into codepoints. Sixteen is far past what a hand can produce in a
 * frame -- a fast typist at ten characters a second fills one slot every six
 * frames -- so the bound is against a stuck key or a paste-like burst rather
 * than against typing. Overflow drops the excess and keeps the earliest, which
 * is the order they were typed in.
 */
inline constexpr std::uint32_t MAX_FRAME_CHARACTERS = 16;

/*
 * --- contacts (ADR-020's 2026-08-22 amendment) -----------------------------
 *
 * A pointer touching the screen, which is the primary input this game is
 * designed for and the thing the mouse is now expressed *through*.
 *
 * D15.4 said "no pointer abstraction beyond the mouse is built, reserved or
 * hinted at", and the amendment that reversed it is precise about why the
 * replacement is a seam rather than a second path: **a second path would
 * drift, and the drift would be invisible**, because the mouse is what a
 * developer uses and touch is what ships. So there is one contact array,
 * `Window` fills it from whatever messages the machine sends, and everything
 * downstream reads contacts.
 *
 * The button arrays above are *not* superseded. A mouse has three buttons and
 * a finger has none, so they remain what a desk uses for the conveniences that
 * are not part of the touch design -- and no interaction may *require* one,
 * which is the amendment's load-bearing rule.
 */
enum class PointerPhase : std::uint8_t
{
  /// This slot holds nothing. Everything past `contactCount` is this.
  None,

  /// Arrived this frame. An edge, so a surface can take a press once.
  Down,

  /// Still down, and was down last frame too.
  Held,

  /// Left this frame, at the position it left from. An edge for the same
  /// reason `Down` is, and it carries a position because a tap is decided by
  /// where the finger *lifted* as much as by where it landed.
  Up
};

/*
 * One contact, in client-area pixels.
 *
 * `id` is the platform's, opaque and only ever compared: a finger keeps its id
 * from `Down` to `Up`, and that is what makes "the second finger" a thing the
 * recognizer can follow across a frame in which both moved.
 */
struct PointerContact
{
  std::uint32_t id = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  PointerPhase phase = PointerPhase::None;
};

/*
 * How many contacts one frame may carry.
 *
 * **The design spends two** -- the puck places with one and twists arrival
 * facing with the other, the wheel is one, and a pinch is two -- and a third
 * finger means nothing in it. Five is a hand, and the extra three are carried
 * rather than interpreted for one reason: a palm or a resting knuckle that
 * *occupied* one of two slots would lock out the finger that has meaning, and
 * dropping contacts at the window is how that becomes an unreproducible
 * "sometimes the second finger does nothing".
 *
 * The recognizer follows the two **oldest**, which is the rule that makes the
 * extra slots free: a stray contact arriving later cannot displace a gesture
 * already in progress.
 */
inline constexpr std::uint32_t MAX_POINTER_CONTACTS = 5;

/*
 * The id the mouse's contact carries.
 *
 * A constant rather than a counter because a mouse has exactly one, and a value
 * a real digitiser will not mint: platform contact ids start low and count up,
 * so the top of the range is free. Nothing downstream tests for it -- the whole
 * point of the seam is that a click and a finger are the same thing by the time
 * anything reads them -- and it exists so that a machine with both a mouse and
 * a touchscreen cannot have the two collide on an id.
 */
inline constexpr std::uint32_t MOUSE_CONTACT_ID = 0xffffffffu;

/*
 * The keys an editable field spends, and nothing else does (ADR-020 §3, T3).
 *
 * **A third question rather than more actions**, and the split is the one this
 * file already draws: an action asks *"was this button pressed"*, text asks
 * *"what did the player mean to write"*, and these ask *"how did they mean to
 * move or delete"*. Backspace and the arrows have no meaning outside a field
 * and never will -- so binding them as actions would put six entries in
 * `ActionSurvivesTextEditing` whose answer is the same and whose question is
 * not really the table's.
 *
 * It also keeps `Home` honest. `ResetView` is bound to it, and the table's own
 * comment has to explain that a field takes the key on two counts; with the
 * field's keys in their own channel, the explanation is a fact about two
 * bindings on one key rather than a rule about an action.
 *
 * Shift is not here: `InputAction::SelectAdd` already carries it, and a second
 * spelling of one modifier is how two of them come to disagree.
 */
enum class TextEditKey : std::uint8_t
{
  Backspace = 0,
  Delete,
  Left,
  Right,
  Home,
  End
};

inline constexpr std::uint32_t TEXT_EDIT_KEY_COUNT = 6;

/// One frame of input, already reduced to logical state. Edges (`pressed`,
/// `released`) are separate from levels (`down`) because a detent nudge must
/// fire once per press and a pan must run every frame the key is held.
struct InputFrame
{
  std::int32_t cursorX = 0; // Client-area pixels, origin top-left.
  std::int32_t cursorY = 0;
  std::int32_t cursorDeltaX = 0;
  std::int32_t cursorDeltaY = 0;

  /// Wheel notches this frame, positive away from the user. Fractional on
  /// high-resolution wheels, and kept fractional -- rounding makes a smooth
  /// wheel feel notched twice.
  float wheelSteps = 0.0f;

  bool buttonDown[INPUT_BUTTON_COUNT] = {};
  bool buttonPressed[INPUT_BUTTON_COUNT] = {};
  bool buttonReleased[INPUT_BUTTON_COUNT] = {};

  bool actionDown[INPUT_ACTION_COUNT] = {};
  bool actionPressed[INPUT_ACTION_COUNT] = {};

  /// Edges only: a field deletes once per press. Auto-repeat arrives as more
  /// presses from the platform, which is where the repeat rate is configured
  /// and the one place it should be read from.
  bool editKeyPressed[TEXT_EDIT_KEY_COUNT] = {};

  /*
   * What was typed this frame, as codepoints rather than as key edges
   * (ADR-020 §3).
   *
   * Separate from the action arrays because it is a different question: an
   * action asks "was this button pressed", and text asks "what did the player
   * mean to write" -- which is the layout's answer and not the key's, and which
   * a dead key or a composition path can produce with no key edge of its own.
   *
   * **Codepoints, assembled in `Window`.** `WM_CHAR` delivers UTF-16 code
   * units, so an astral character arrives as two messages; pairing them where
   * the messages are is the only place the pairing is unambiguous, and it keeps
   * every reader of this struct free of a half-character case to handle.
   */
  char32_t characters[MAX_FRAME_CHARACTERS] = {};
  std::uint32_t characterCount = 0;

  /*
   * What is touching the screen, oldest first (ADR-020's amendment).
   *
   * Oldest first is a contract rather than an artefact: `GestureRecognizer`
   * takes the first two, so the order is what decides which finger places and
   * which one twists. `Window` maintains it -- a contact keeps its slot until
   * it lifts, and a new one lands at the end.
   *
   * The mouse fills slot zero from its left button, which is what "the mouse is
   * expressed through the seam" means in one line. Its other two buttons stay
   * where they are: they are a desk's conveniences, not fingers.
   */
  PointerContact contacts[MAX_POINTER_CONTACTS] = {};
  std::uint32_t contactCount = 0;

  std::uint32_t viewportWidth = 0;
  std::uint32_t viewportHeight = 0;
  bool windowFocused = false;
  bool cursorInsideWindow = false;

  [[nodiscard]] bool Down(InputButton _button) const noexcept { return buttonDown[static_cast<std::uint32_t>(_button)]; }
  [[nodiscard]] bool Pressed(InputButton _button) const noexcept { return buttonPressed[static_cast<std::uint32_t>(_button)]; }
  [[nodiscard]] bool Released(InputButton _button) const noexcept { return buttonReleased[static_cast<std::uint32_t>(_button)]; }
  [[nodiscard]] bool Down(InputAction _action) const noexcept { return actionDown[static_cast<std::uint32_t>(_action)]; }
  [[nodiscard]] bool Pressed(InputAction _action) const noexcept { return actionPressed[static_cast<std::uint32_t>(_action)]; }
};

/// Feel, in one place. Values are starting points to be tuned against the
/// prints, not decisions -- which is why they are here and not spread through
/// the mapping code.
/*
 * Which hand holds the device (`settings.png` §1's INPUT section, N3).
 *
 * Two values and **never inferred**, which is the print's own emphasis: *"a
 * wheel that silently re-orders itself is worse than one that is merely
 * mirrored"*. A client that guessed from where the first touch landed would
 * re-order the command wheel under a player who simply reached across.
 *
 * Here rather than in `Gesture.h` because that file decides nothing and this is
 * a decision -- and because what reads it is a *surface* laying itself out (I3's
 * wheel), which is the same kind of thing `CameraTuning` below is for.
 */
enum class Handedness : std::uint8_t
{
  Right = 0,
  Left = 1
};

/*
 * A config word to an arrangement, falling back to the drawn one.
 *
 * `ResolveHudPalette`'s posture exactly, and for the reason that file gives: a
 * client that will not start because a preference is mistyped is the wrong
 * failure. An unknown word gets the print's arrangement and the player notices
 * that their setting did not take, which is recoverable; a refusal to launch is
 * not.
 */
[[nodiscard]] constexpr Handedness ResolveHandedness(std::string_view _name) noexcept
{
  return _name == "left" ? Handedness::Left : Handedness::Right;
}

/// And back, for the writer. The words are the config file's, spelled once so a
/// round trip cannot rename a setting.
[[nodiscard]] constexpr const char* HandednessName(Handedness _handedness) noexcept
{
  return _handedness == Handedness::Left ? "left" : "right";
}

struct CameraTuning
{
  float orbitRadiansPerPixel = 0.006f;      // A screen width is a little over half a turn.
  float keyboardPanPixelsPerSecond = 900.0f;
  float edgePanPixelsPerSecond = 700.0f;
  std::int32_t edgePanMarginPixels = 8;
  bool edgePanEnabled = true;
};

/// What the camera should do about it. Deltas, not state: the camera owns the
/// state, and an intent that carried absolutes would be a second copy of it.
struct CameraIntent
{
  float orbitRadians = 0.0f;
  float zoomSteps = 0.0f;
  float panRightPixels = 0.0f;
  float panUpPixels = 0.0f;
  std::int32_t yawDetentSteps = 0;
  bool snapYaw = false;
  bool resetView = false;
};

/*
 * `_dragPixelsX`/`_dragPixelsY` are the primary contact's movement this frame
 * while it is *dragging*, and zero otherwise (I2, Plan-of-Record §1's rule 1:
 * **tap selects, drag pans**).
 *
 * Two floats rather than a `GestureState`, and the reason is a cycle rather
 * than taste: `Gesture.h` reads `InputFrame` from this header, so this header
 * cannot read `GestureState` back. Numbers cross the boundary instead, which
 * costs the caller one line and keeps the camera's intent decided in one place
 * -- including for a test, which can pan without owning a recognizer.
 *
 * The sign is the middle-drag's, deliberately the same call: the plane follows
 * the contact and the focus moves against it. A finger and a mouse that panned
 * opposite ways would be the "the map fights me" feel on whichever one the
 * developer was not using.
 */
[[nodiscard]] CameraIntent MapCameraInput(const InputFrame& _input, const CameraTuning& _tuning, float _deltaSeconds,
                                          float _dragPixelsX, float _dragPixelsY) noexcept;

/// Applies an intent in the order the player perceives it: orbit and zoom
/// change what "right" and "up" mean, so panning last is the difference between
/// a drag that tracks the cursor and one that lags a frame behind it.
void ApplyCameraIntent(IsoCamera& _camera, const CameraIntent& _intent) noexcept;

} // namespace Neuron
