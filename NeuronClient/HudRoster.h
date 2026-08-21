#pragma once

#include <cstdint>

/*
 * The fleet roster, as the game hands it over (`tactical-hud.png`, ADR-014).
 *
 * The print draws a column of rows down the left: a name, a count, and a pair
 * of strips for the group's health. The engine draws exactly that and learns
 * nothing from it -- **a row is a name and four numbers**, and the word for
 * what a row *is* never crosses the seam. This game means a wing; another game
 * on these libraries can mean a squad, a platoon or a convoy, and the pass that
 * draws the panel does not change.
 *
 * **Why the game aggregates rather than the engine.** The engine has the
 * replicated entities and could group them by `EntityRecord::groupId` itself --
 * it is one byte and the field is right there. It must not: it would then have
 * decided that groups are worth showing, that they are named, that the two
 * gauges average rather than take a minimum, and that a group with no members
 * disappears rather than showing as empty. Every one of those is a design
 * question about *this* game, and the seam exists so that they are answered on
 * the side that is allowed to answer them (ADR-014 §2c).
 */

namespace Neuron
{

/// One row. `name` points at storage the world view owns and is valid for its
/// lifetime -- in practice a string the composition root loaded once, the same
/// arrangement `OrderOption::name` and `ReasonText` have.
struct RosterRow
{
  const char* name = nullptr;

  /// Opaque, and echoed rather than read: it is what a click on the row would
  /// hand back to the game, once rows are clickable.
  std::uint16_t groupId = 0;

  std::uint16_t shipCount = 0;

  /// How many of them the player currently has selected. The print highlights
  /// a row that the selection covers, and this is the only thing the engine
  /// needs in order to decide that -- the alternative is the engine matching
  /// selected ids against group membership, which is the aggregation it is not
  /// supposed to be doing.
  std::uint16_t selectedCount = 0;

  /// 0-255, not a percentage -- `EntityRecord`'s own scale, so a full group is
  /// 255 and nothing has to remember a divide by a hundred.
  std::uint8_t hullGauge = 0;
  std::uint8_t shieldGauge = 0;
};

/*
 * How many rows a client will ask for.
 *
 * The print draws eight and a "8/8" footer that implies scrolling past that.
 * Sixteen is twice what the sheet shows and still a fixed array on the stack;
 * a fleet organised into more groups than this wants a scrolling roster, which
 * is a surface rather than a bigger number.
 */
inline constexpr std::uint32_t MAX_ROSTER_ROWS = 16;

/*
 * One block of the player's ships that are somewhere the scene cannot show
 * them (`station-screen.png`, ADR-017 1; ADR-016 9's location blocks).
 *
 * A ship the scene cannot draw is not always a *docked* one, and that is why
 * this record is not called a docked block. It began as one -- T2 needed the
 * hangar's roster -- and U3b's second and third cases were waiting behind it:
 * a fleet standing on a grid the player is not watching, and a fleet mid-warp
 * that is in no world at all. All three are the same sentence with a different
 * word in it: *this many of yours, over there*.
 *
 * The engine draws a name, a count, a word and maybe a button, and learns
 * nothing else. **It does not know what the word means** -- `stateLabel` is a
 * string the game wrote, not an enum the engine switches on, so "docked",
 * "in warp" and "on grid" are three facts about one game rather than three
 * cases compiled into a library that serves two.
 */
struct LocationBlock
{
  /// What to draw. Points at storage the world view owns, like `RosterRow::name`.
  const char* name = nullptr;

  /// How many of the player's ships are there.
  std::uint16_t shipCount = 0;

  /// What a click hands back to the game -- the anchor the block is about. The
  /// engine echoes it and never reads it.
  std::uint16_t anchor = 0;

  /*
   * What the block's button says, or null for a block with no button.
   *
   * The word is the game's for the same reason every other word on this HUD is:
   * the engine drawing "STATION" would have decided that the place is a station
   * and that the button opens its interior, which are two facts about one game.
   * A client whose game supplies nothing draws the block without a button, and
   * the panel is still a name and a count.
   */
  const char* buttonLabel = nullptr;

  /*
   * What the game calls this place's state -- `DOCKED`, `IN WARP`, `ON GRID`.
   *
   * A string and not a code, unlike `Notice::code`, because nothing keys or
   * coalesces on it: it is drawn and only drawn. The engine picks no colour
   * from it either; a state that should read as urgent says so through the
   * block it is on, not through a word the client has learned to fear.
   */
  const char* stateLabel = nullptr;

  /*
   * True when these ships are the ones already on screen as hulls.
   *
   * The game answers it because the game is what knows; the engine acts on it
   * without learning why. A panel about ships the scene cannot show must not
   * list the fleet the scene *is* showing -- that is one fleet counted twice on
   * one HUD, with nothing to tell the player which count is the lie.
   *
   * It is a flag rather than the client comparing `stateLabel`, because that
   * word is a string the game wrote and comparing it would be the engine
   * reading a vocabulary it is not allowed to have an opinion about.
   */
  bool inScene = false;

  /*
   * Seconds until this block stops being true, or negative when the game will
   * not say (ADR-016 6's `InTransit` row).
   *
   * The one number a fleet in no world can still be given: a crossing is a fact
   * about the transfer bus rather than about a grid, so no snapshot could carry
   * it and the summary family does. Negative for a fleet that is simply
   * somewhere, because "arriving in -1" is not a state and a block should not
   * have to draw one.
   */
  float etaSeconds = -1.0f;
};

/// How many such blocks a client will ask for. One commander's ships can be
/// spread over several places at once, and a fleet spread over more than this
/// wants the hangar screen rather than a longer list.
inline constexpr std::uint32_t MAX_LOCATION_BLOCKS = 8;

/*
 * Something the game wants said to the player (`alerts-and-toasts.png`).
 *
 * Three fields and no meaning attached to any of them. The engine draws the two
 * strings, keys the coalescing on the code, and picks the level and the dwell
 * itself -- because how loud a message is and how long it sits are properties
 * of the surface, while what it says is a property of the game.
 *
 * Both pointers are the world view's storage, valid until the next poll.
 */
struct Notice
{
  /*
   * Opaque. Two notices sharing one are the same message happening again,
   * which is what folds a burst into one row with a count.
   *
   * Wide enough to hold a kind *and* a place: "docked" is one message, but
   * docking at two stations is two things happening, and a game that could only
   * key on the kind would have the second fold into the first.
   */
  std::uint32_t code = 0;

  const char* title = nullptr;
  const char* body = nullptr;
};

/// How many a client will drain in one frame. A game with more to say than this
/// in a sixtieth of a second is talking over itself, and the stack's own
/// backlog is where the rest waits.
inline constexpr std::uint32_t MAX_NOTICES_PER_POLL = 8;

} // namespace Neuron
