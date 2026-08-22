#pragma once

#include <cstdint>

/*
 * Which screen is live, and how the player got there (ADR-020 §1).
 *
 * Until T3 this client had one surface -- the tactical HUD -- and a single live
 * surface needs no stack, no router and no focus owner (ADR-020 §5a). The
 * hangar is the first screen that *navigates*, so this is the first slice that
 * has to answer "where does ◀ BACK go" with something other than "nowhere".
 *
 * **A surface is a value, not an object.** There is no base class, no virtual
 * `OnEnter`, and no retained-mode tree behind this file -- ADR-020's "what this
 * deliberately does not do" list rules out all three. What a surface *is* lives
 * with the screen that draws it; what this file owns is which one is live, what
 * the frame loop must do about that, and what changed when the player pressed
 * something. Entry and exit are reported as a `SurfaceChange` and run by the
 * caller, which is what keeps the whole file device-free and testable.
 *
 * **The stack is small on purpose.** Four is not a budget for navigation depth,
 * it is the observation that a player is never more than a few steps from where
 * they started -- and the rule that makes it hold is `Push`, not the cap: a
 * surface already in the stack is *returned to* rather than pushed again, so
 * TACTICAL → MAP → TACTICAL is two entries and not three, however long a player
 * circles.
 */

namespace Neuron
{

/*
 * Every screen this client can show. Closed, and deliberately so: a surface
 * that is not on this list cannot be reached, which is what makes the frame
 * loop's two-way choice below exhaustive rather than a default case.
 *
 * The first five are in-session. The rest are pre-session
 * (`session-surfaces.png` §1) and exist here now, before any of them is drawn,
 * because the two families differ in what the frame does beneath them -- and a
 * list that grew a second family later would be a list that had already decided
 * the wrong thing about the first.
 */
enum class SurfaceId : std::uint8_t
{
  Tactical = 0,
  Map,
  System,
  Station,
  Settings,

  Login,
  UpdateRequired,
  CharacterCreate,
  Resume,
  Queue,
  ReconnectUnderFire
};

inline constexpr std::uint32_t SURFACE_ID_COUNT = 11;

/// `SurfaceIsPreSession` splits the enum by comparison rather than by a table,
/// so this is the seam between the two families and adding an in-session
/// surface after it would silently make it pre-session. This is that build
/// failure.
static_assert(static_cast<std::uint32_t>(SurfaceId::ReconnectUnderFire) + 1u == SURFACE_ID_COUNT,
              "SURFACE_ID_COUNT must count every SurfaceId");
static_assert(static_cast<std::uint32_t>(SurfaceId::Login) == 5u,
              "the pre-session family must start at Login -- SurfaceIsPreSession splits the enum there");

/*
 * How deep the stack goes.
 *
 * Four holds the base plus three steps away from it, which is more than any
 * path the corpus draws: the deepest is TACTICAL → MAP → SYSTEM → STATION, and
 * every screen carries a direct route home besides. It is a fixed array in the
 * client rather than a growth policy -- a HUD must not allocate to describe
 * where the player is.
 */
inline constexpr std::uint32_t MAX_SURFACE_DEPTH = 4;

/*
 * Whether the frame records the world half beneath this surface (ADR-020 §1,
 * ADR-006 §1's fixed pass list).
 *
 * **No pass is added, removed, reordered or branched by this.** The frame loop
 * already records the list in two halves; this chooses between two calls it
 * already makes. A full-screen surface clears the back buffer directly, skips
 * Opaque, Nebula and OverlayWorld and the resolve between them, and records
 * `Ui` -- which is where U5's node budget comes from and why `settings.png`'s
 * LIVE PREVIEW is Ui content rather than a suspended world view.
 *
 * True for `Tactical` alone: the world *is* that screen, and the HUD is a
 * border around it.
 */
[[nodiscard]] constexpr bool SurfaceRecordsWorld(SurfaceId _surface) noexcept
{
  return _surface == SurfaceId::Tactical;
}

/*
 * Whether this surface is shown before there is a session to show one for.
 *
 * The distinction is not cosmetic: pre-session surfaces have no `WorldView`
 * data behind them at all, which is why screens-as-HUD-modes was never
 * available to this client, and it is why the toast layer stops at the session
 * boundary below.
 */
[[nodiscard]] constexpr bool SurfaceIsPreSession(SurfaceId _surface) noexcept
{
  return static_cast<std::uint32_t>(_surface) >= static_cast<std::uint32_t>(SurfaceId::Login);
}

/*
 * Whether toasts are drawn over this surface (ADR-020 §1's cross-surface
 * layer).
 *
 * The same answer as "is there a session" today, and named separately anyway,
 * because the two are different claims: one is about what data exists, the
 * other about what may occlude a screen. A pre-session surface raises no toast
 * because nothing can happen yet -- UPDATE REQUIRED and the queue position are
 * the screens themselves rather than notices over them.
 */
[[nodiscard]] constexpr bool SurfaceCarriesToasts(SurfaceId _surface) noexcept
{
  return !SurfaceIsPreSession(_surface);
}

/*
 * What one navigation did.
 *
 * **At most one surface stops being active per operation**, which is what makes
 * this two ids rather than a list: exactly one surface is live at a time, so
 * only that one can have an exit to run. A push that pops back past two
 * breadcrumbs still exits one screen -- the others stopped being active when
 * they were covered, and ran their exits then.
 *
 * `changed` is false for a navigation that went nowhere (pressing STATION while
 * the hangar is already up), and then neither id means anything. A caller that
 * ran entry work regardless would reconcile the composer against the roster on
 * every press of a button that did nothing.
 */
struct SurfaceChange
{
  /// The surface that was live and now is not. Its exit runs first: clear
  /// focus, cancel any in-flight drag, destroy no retained state (ADR-020 §1).
  SurfaceId exited = SurfaceId::Tactical;

  /// The surface that is live now. Its entry runs second: validate retained
  /// state against the data the client holds *now*, and ask for anything
  /// asked-once it lacks.
  SurfaceId entered = SurfaceId::Tactical;

  bool changed = false;
};

/*
 * The stack itself.
 *
 * Never empty: it is constructed on a base and every operation leaves at least
 * that base standing, so `Active()` always has an answer and no caller needs a
 * "no surface" case. The base is the surface the session lands in -- `Login`
 * before there is one, `Tactical` after -- and moving between those two
 * families is `Rebase` rather than a push, because a player who has logged in
 * cannot go back to logging in.
 */
class SurfaceStack
{
public:
  /// Starts on `_base`. No entry is reported: the first surface is not
  /// navigated to, it is where the client begins.
  explicit SurfaceStack(SurfaceId _base = SurfaceId::Tactical) noexcept : m_entries{_base}, m_depth{1} {}

  [[nodiscard]] SurfaceId Active() const noexcept { return m_entries[m_depth - 1]; }
  [[nodiscard]] SurfaceId Base() const noexcept { return m_entries[0]; }
  [[nodiscard]] std::uint32_t Depth() const noexcept { return m_depth; }

  /// Whether this surface is anywhere in the stack -- live, or a breadcrumb
  /// under one.
  [[nodiscard]] bool Holds(SurfaceId _surface) const noexcept;

  /*
   * Goes to `_surface`.
   *
   * **A surface already in the stack is returned to, not pushed onto.** The
   * stack pops back to it, so `◀ TACTICAL` from the hangar and `◀ BACK` from
   * settings are one mechanism rather than two, and a player who navigates in
   * circles does not build a ladder under themselves.
   *
   * At full depth the *oldest breadcrumb* is dropped -- the entry just above
   * the base -- rather than the push being refused. Two reasons, and the first
   * is the one that matters: a refused push is a control that does nothing,
   * which is worse than any depth policy. The second is that dropping from the
   * bottom keeps ◀ BACK meaning "undo the last step I took", where dropping the
   * top would make it skip the screen the player just came from. The base
   * itself is never dropped; it is the way home.
   */
  SurfaceChange Push(SurfaceId _surface) noexcept;

  /*
   * One step back, towards the base.
   *
   * A no-op at the base, reported as `changed == false` rather than refused:
   * ◀ BACK on the surface a session lands in has nowhere to go, and the caller
   * that draws the chip is the one that decides whether to draw it at all.
   */
  SurfaceChange Back() noexcept;

  /*
   * Throws the whole stack away and starts again on `_base`.
   *
   * The session boundary in one call: logging in, being disconnected, or
   * arriving at `UpdateRequired` are all "everything before this is
   * unreachable", which a push cannot express -- it would leave the login
   * screen sitting under the game as somewhere ◀ BACK could reach.
   *
   * Reports an exit for the surface that was live, so focus and any in-flight
   * drag are unwound exactly as they are on an ordinary navigation.
   */
  SurfaceChange Rebase(SurfaceId _base) noexcept;

private:
  SurfaceId m_entries[MAX_SURFACE_DEPTH] = {};

  /// Always at least 1 -- see the class comment. Every mutator below preserves
  /// it, and `Active()` reads `m_depth - 1` on that promise.
  std::uint32_t m_depth = 1;
};

} // namespace Neuron
