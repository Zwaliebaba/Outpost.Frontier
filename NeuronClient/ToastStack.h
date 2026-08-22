#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/*
 * The notification stack (`alerts-and-toasts.png`).
 *
 * That sheet exists because ADR-003 §9.6 asserted "a priority toast stack with
 * combat suppressing all but critical" against a taxonomy that did not exist.
 * Everything here is that sentence made testable: five priorities, one
 * suppression rule, and a placement that keeps the viewport clear.
 *
 * **Priority is the only axis.** It decides dwell, placement and suppression
 * together -- there is no separate "is this important" flag to disagree with it,
 * and no timestamp, because the stack is chronological and a clock reads as
 * data nobody uses mid-fight.
 *
 * **Coalescing is mandatory, not an optimisation.** The sheet says so in those
 * words: one market order can fill forty times, and forty rows is the stack
 * becoming the thing that suppresses the game. Same source inside a window
 * collapses to a count.
 *
 * **What the MVP actually raises is one toast**, and it is the one S9 owes: an
 * order refused, carrying the game's reason string. The sheet calls that the
 * BOUNCE state and is explicit that it is not a new component -- it is this
 * stack showing the same reason the 150 ms ghost bounce is already showing, so
 * that one event has one explanation on two surfaces
 * (`puck-and-wheel.png` §4's BounceParity, from either the local pre-check or
 * the authority).
 *
 * Device-free: time is a parameter, text is a string, and placement is a
 * decision this file makes but does not draw.
 */

namespace Neuron
{

/*
 * The five levels, in the order they sort. Lower is louder.
 *
 * The values are the sheet's own rows, and they are an enumeration rather than
 * a set of booleans because the sheet is explicit that this is *the only axis*:
 * a toast's dwell, its surface and whether combat holds it back are all read
 * from this one number.
 */
enum class ToastPriority : std::uint8_t
{
  /// Hull critical, flagship destroyed, station under attack, account security.
  /// Centre-top, one action, and it does not consume a stack slot.
  Critical = 0,

  /// Ship lost, **order rejected by the authority**, criminal flag, docking
  /// denied. Top slot of the stack, and the reason text is mandatory.
  Urgent = 1,

  /// Market fill, contract accepted, industry job complete, probe resolved.
  Routine = 2,

  /// Corp member online, sovereignty tick, intel ping outside the interest set.
  Ambient = 3,

  /// Skill training complete, long industry job finished while away. In-client
  /// it is a toast; out of client it is the push path the sheet defers (§4).
  WallTime = 4
};

inline constexpr std::uint32_t TOAST_PRIORITY_COUNT = 5;

/// How long each level stays, in seconds. `Critical` is the sheet's "until
/// acted" and has no dwell at all -- it waits for `Dismiss`.
[[nodiscard]] double ToastDwellSeconds(ToastPriority _priority) noexcept;

/// Whether combat holds this level back. Critical and Urgent are the sheet's
/// two ALWAYS rows; the rest are QUEUED.
[[nodiscard]] bool ToastSurvivesCombat(ToastPriority _priority) noexcept;

/// One row: an icon's worth of priority, a one-line head, a one-line detail.
struct Toast
{
  ToastPriority priority = ToastPriority::Routine;

  /*
   * What "same source" means, for coalescing.
   *
   * An opaque number the raiser chooses -- a market order id, a reason code, a
   * ship id. Two toasts coalesce when their priority *and* their source agree
   * and the second arrives inside the window, so a raiser that wants every
   * event on its own row simply varies this.
   */
  std::uint32_t sourceKey = 0;

  /// 1, or how many events this row now stands for. The sheet draws it as the
  /// "x40" suffix on a coalesced head.
  std::uint32_t count = 1;

  std::string head;
  std::string detail;

  /// When it became visible. Not when it was raised: a toast held back by
  /// combat starts its dwell when it is finally shown, or a suppressed backlog
  /// would drain and vanish in the same instant.
  double shownSeconds = 0.0;

  /// When it goes, or negative for "until acted".
  double expiresSeconds = -1.0;

  /*
   * The one thing a critical can offer the player besides being read
   * (`alerts-and-toasts.png` 2).
   *
   * `actionLabel` is the game's word -- JUMP TO, VIEW, UNDOCK -- drawn and never
   * read; `actionKey` is the raiser's own number, handed back untouched when the
   * chip is pressed. The engine learns neither what the word means nor what the
   * number identifies, which is the same arrangement `OrderKindOption::name` and
   * `payload` already have (ADR-014 2b).
   *
   * Null label means no chip, which is a critical that is purely information
   * and behaves exactly as one always has.
   */
  const char* actionLabel = nullptr;
  std::uint32_t actionKey = 0;

  /*
   * When the player acted, or negative for not yet.
   *
   * A critical waits for the player and then **leaves on its own**: an acted row
   * that stayed until dismissed a second time would make the act feel like it
   * did nothing, and one that vanished instantly would take the confirmation
   * with it. Thirty seconds is long enough to read what happened and short
   * enough that a row nobody looked at again is gone.
   */
  double actedSeconds = -1.0;

  [[nodiscard]] bool HasAction() const noexcept { return actionLabel != nullptr; }
  [[nodiscard]] bool Acted() const noexcept { return actedSeconds >= 0.0; }

  /*
   * Whether this row is waiting for the player rather than for the clock.
   *
   * Still just "has no expiry", and deliberately unchanged: acting on a row
   * *gives* it one (the collapse below), so an acted critical stops waiting
   * without this predicate having to know that acts exist. Teaching it about
   * `actedSeconds` instead would have left the row with no expiry and the
   * sweep's `now >= expiresSeconds` comparing against -1, which erases it the
   * instant it is acted on.
   */
  [[nodiscard]] bool WaitsForAction() const noexcept { return expiresSeconds < 0.0; }
};

/// How long an acted critical stays before it collapses on its own
/// (`alerts-and-toasts.png` 2).
inline constexpr double TOAST_ACTED_COLLAPSE_SECONDS = 30.0;

class ToastStack
{
public:
  /*
   * How many rows the bottom-right stack shows at once.
   *
   * The sheet states it on the Ambient row -- "drops oldest first at 5 visible"
   * -- but it is a property of the surface rather than of that level: the stack
   * sits inside thumb reach on a tablet and must clear both the roster drawer
   * and the order puck's working area. Criticals are centre-top and do not
   * count against it.
   */
  static constexpr std::size_t MAX_VISIBLE = 5;

  /// How long after a row is raised another from the same source folds into it.
  /// Long enough that a burst of fills is one row, short enough that two
  /// unrelated refusals a minute apart are two.
  static constexpr double COALESCE_WINDOW_SECONDS = 6.0;

  /// Beyond this the queue itself is dropping the oldest. Suppressed toasts are
  /// queued rather than dropped, and a fight long enough to overrun this has
  /// already told the player everything the backlog would.
  static constexpr std::size_t MAX_QUEUED = 64;

  /*
   * Raises one, or folds it into the row it repeats.
   *
   * Returns true when a new row appeared and false when it coalesced, which is
   * the difference between "something happened" and "something happened again"
   * -- the audio cue (S15) wants exactly that distinction.
   */
  bool Raise(ToastPriority _priority, std::uint32_t _sourceKey, std::string _head, std::string _detail, double _nowSeconds);

  /*
   * The same, with a chip on it.
   *
   * `_actionLabel` must outlive the row -- a literal, or a string the game owns,
   * exactly as `RosterRow::name` is. It is not copied because it is not the
   * engine's to hold: a label the stack owned would be a word the engine had
   * taken a copy of, and copies are how vocabulary leaks across a seam.
   *
   * Coalescing is unchanged: a second event from the same source folds into the
   * row and the *first* row's action stands, because the chip the player is
   * already reaching for must not change what it does under their finger.
   */
  bool RaiseWithAction(ToastPriority _priority, std::uint32_t _sourceKey, std::string _head, std::string _detail,
                       const char* _actionLabel, std::uint32_t _actionKey, double _nowSeconds);

  /*
   * Records that the player pressed a row's chip, and returns its key.
   *
   * The stack does not act -- it has no idea what the key means. It marks the
   * row acted, starts its thirty seconds, and hands the number back to the
   * caller, who is the one that knows.
   *
   * Returns false for an index with no action, so a caller cannot mistake "0"
   * for a key it was never given.
   */
  [[nodiscard]] bool Act(std::size_t _index, std::uint32_t& _outActionKey, double _nowSeconds);

  /// Expires dwelt rows and admits queued ones. Once a frame.
  void Advance(double _nowSeconds);

  /*
   * Combat holds back everything but Critical and Urgent.
   *
   * **Queued, not dropped** -- the sheet is explicit, and it is why a held
   * toast's dwell starts when it is shown rather than when it was raised. The
   * flag it keys on is replicated (the sheet names `PvpFlags.engagedUntilTick`,
   * the same field that blocks docking), which is what makes suppression a pure
   * function of state rather than a heuristic to tune.
   *
   * **Nothing drives it in the MVP**, because the MVP has no combat. It is
   * built because queue-not-drop is a property of the *type* -- retrofitting it
   * would mean rewriting when every dwell starts -- and because a backlog that
   * is invisible as well as silent is the failure the sheet's SUPPRESSED row
   * exists to prevent. `SuppressedCount` is what a HUD draws that row from.
   */
  void SetCombat(bool _inCombat, double _nowSeconds);
  [[nodiscard]] bool InCombat() const noexcept { return m_inCombat; }
  [[nodiscard]] std::size_t SuppressedCount() const noexcept { return m_queued.size(); }

  /// Dismisses a row the player acted on. Criticals wait for this and nothing
  /// else; any other row may also be dismissed early.
  void Dismiss(std::size_t _index);

  /// Everything, for a disconnect. Last session's refusals over this session's
  /// fleet would be the same mistake as keeping its ghosts.
  void Clear() noexcept;

  /*
   * The rows to draw, loudest first and newest first within a level.
   *
   * Criticals lead because the sheet puts them centre-top on their own surface;
   * a caller draws `[0, CriticalCount())` there and the rest bottom-right. That
   * split is a contiguous range for the same reason the overlay's rings are:
   * two surfaces are two draws, not a per-row flag.
   */
  [[nodiscard]] std::span<const Toast> Visible() const noexcept { return m_visible; }
  [[nodiscard]] std::size_t CriticalCount() const noexcept;
  [[nodiscard]] bool Empty() const noexcept { return m_visible.empty(); }

  /// Rows dropped because the stack was already full. Should stay at zero in a
  /// quiet session; a rising number means the game is talking over itself.
  [[nodiscard]] std::uint64_t DroppedCount() const noexcept { return m_dropped; }

private:
  void Admit(Toast _toast, double _nowSeconds);
  void SortAndCap();

  std::vector<Toast> m_visible;
  std::vector<Toast> m_queued; // Raised while suppressed, oldest first.
  bool m_inCombat = false;
  std::uint64_t m_dropped = 0;
};

} // namespace Neuron
