#pragma once

#include "UiDrawList.h"

#include <cstdint>
#include <span>

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

  /*
   * How full this group's carrying capacity is, on the same 0-255 scale
   * (ADR-024 §5c).
   *
   * **The game sums it, and that is not a convenience.** The engine has no way
   * to: what a unit of each ore displaces and how much a hull can hold are
   * authored content, so a client that added them up would be a second copy of
   * the balance file drifting from the first. It also would not know which
   * hulls carry anything at all.
   *
   * Meaningless unless `carriesCargo`. Zero on a group whose holds are empty is
   * a real answer; zero on a group that cannot carry is not an answer at all,
   * which is why the two are separate fields rather than one sentinel.
   */
  std::uint8_t cargoGauge = 0;

  /*
   * Whether this group has anything that carries.
   *
   * A wing of interceptors has no hold, so a cargo readout on its row would be
   * a gauge for a thing that cannot happen -- the same reason the roster's
   * health strips draw only when a gauge is below full. Combat wings get no
   * cargo ink at all.
   */
  bool carriesCargo = false;
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

/// How many bars the field readout can hold. The game's cluster cap; a number
/// the engine is given rather than one it decides, like every other bound here.
inline constexpr std::uint32_t MAX_FIELD_BARS = 12;

/*
 * How much is left of the place the fleet is standing in (ADR-024 §3d).
 *
 * A row of thin bars and, sometimes, a word. The engine draws a percentage per
 * bar and never learns that the place is a mining field, that the bars are
 * rocks, or that they empty as ships work them -- a game on these libraries
 * could mean a salvage wreck, a quarry or a well with the same shape.
 *
 * **The staleness is the game's to decide, and the word is the game's to
 * write.** Whether a status describes the *current* state of the place depends
 * on a schedule that lives in authored content; a client comparing timestamps
 * would be a client that had learned the schedule. So it is handed a label or
 * nothing, and draws what it is handed.
 */
struct FieldReadout
{
  /// 0-100 per bar, saturating rather than wrapping: a bar reading 3% for an
  /// untouched cluster is worse than one reading 100%.
  std::uint8_t fullPct[MAX_FIELD_BARS] = {};
  std::uint32_t barCount = 0;

  /// The game's word for "this describes a state that has since been replaced",
  /// or null when the readout is current.
  const char* staleLabel = nullptr;
};

/*
 * Sizes for the roster column, at scale 1.0 and read off `tactical-hud.png`.
 *
 * One tuning for both of the column's lists, because they are one column: the
 * wing chips and the ELSEWHERE blocks under them share a left edge, a padding
 * and a bottom, and two tables that agreed by hand would be two tables that
 * eventually did not.
 */
struct RosterColumnTuning
{
  /// One wing per chip: a name, a count and the gauge strips.
  float chipHeight = 34.0f;
  float chipGap = 6.0f;

  /// The `FLEET ROSTER` and `ELSEWHERE` labels, each one line of small text.
  float headingHeight = 18.0f;

  /// A block: one line of name and count, one of state and ETA, and the
  /// button's strip beneath them. Read off what the draw used to compute in
  /// place (`chipHeight + 36`), so moving the layout out moved nothing on
  /// screen.
  float blockHeight = 70.0f;
  float blockGap = 6.0f;

  /// The button along the bottom of a block. `station-screen.png` is what it
  /// opens; the word on it is the game's.
  float buttonHeight = 16.0f;

  float padding = 8.0f;
};

/*
 * Where the `_row`-th wing chip goes.
 *
 * A function of the index rather than a running total, so the column's vertical
 * arithmetic exists once. It used to be accumulated inside the draw, which was
 * fine while nothing in the column was pressable -- and stopped being fine the
 * moment T3 made the block button below it the way into the hangar
 * (ADR-020 §5.1).
 *
 * The caller still decides whether the chip *fits*: a rect past the column's
 * bottom is returned rather than clamped, because clamping would stack every
 * overflowing row on the last visible one.
 */
[[nodiscard]] UiRect RosterChipRect(const UiRect& _column, float _scale, const RosterColumnTuning& _tuning,
                                    std::uint32_t _row) noexcept;

/// How many of `_rowCount` chips fit in the column. `Tactical`'s overflow rule
/// is drop (ADR-020 §7), and this is where the dropping is counted.
[[nodiscard]] std::uint32_t RosterChipsThatFit(const UiRect& _column, std::uint32_t _rowCount, float _scale,
                                               const RosterColumnTuning& _tuning) noexcept;

/*
 * Where the ELSEWHERE blocks begin, under `_rowCount` wing chips.
 *
 * The heading sits one `headingHeight` above the value returned, and is drawn
 * only when a block actually fitted -- "no ships anywhere but here" is not a
 * place at all, and a heading over nothing says it is.
 */
/*
 * Which roster chip a press landed on, or `_rowCount` when none.
 *
 * The same rect the draw takes from `RosterChipRect`, and bounded by the same
 * `RosterChipsThatFit` -- a press must not land on a row the column had no room
 * to draw, which is the one way a list that clips can lie about what is under a
 * finger (ADR-020 §5.1).
 */
[[nodiscard]] std::uint32_t HitRosterChip(const UiRect& _column, std::uint32_t _rowCount, float _scale,
                                          const RosterColumnTuning& _tuning, float _x, float _y) noexcept;

[[nodiscard]] float RosterBlocksTop(const UiRect& _column, std::uint32_t _rowCount, float _scale,
                                    const RosterColumnTuning& _tuning) noexcept;

/*
 * One laid-out block: where it goes, and where its button is.
 *
 * `block` indexes back into the caller's own array rather than copying the
 * block, because the strings in one are the world view's storage and a layout
 * that held them would be a second lifetime to reason about.
 */
struct LocationBlockLayout
{
  UiRect panel;

  /// The button's rect, meaningful only when `hasButton`. A block whose game
  /// supplied no word gets no button, and the panel is still a name and a
  /// count.
  UiRect button;
  bool hasButton = false;

  /// Which entry of the caller's span this is.
  std::uint32_t block = 0;

  /// The place this block is about, echoed back to the game and never read
  /// here -- `LocationBlock::anchor`, carried so a press has an answer without
  /// a second lookup.
  std::uint16_t anchor = 0;
};

/*
 * Lays the blocks out, top-down from `_top` inside `_column`.
 *
 * **Blocks already on screen as hulls are skipped**, which is why this returns
 * a count rather than filling one slot per input: the game reports every place
 * the player has ships, including the grid being watched, and listing that one
 * would be one fleet counted twice on one HUD with nothing to say which count
 * is the lie.
 *
 * A block that would run past the column's bottom is dropped rather than shrunk
 * -- `Tactical`'s declared overflow rule (ADR-020 §7) -- so the count returned
 * is what fitted.
 */
[[nodiscard]] std::uint32_t BuildLocationBlockColumn(std::span<const LocationBlock> _blocks, const UiRect& _column,
                                                     float _top, float _scale, const RosterColumnTuning& _tuning,
                                                     std::span<LocationBlockLayout> _outLayouts);

/*
 * Which block's button a press at these pixels landed on, or null.
 *
 * Only the button, not the panel: the whole block is not a target, because a
 * player reading the count is not asking to be taken somewhere.
 */
[[nodiscard]] const LocationBlockLayout* HitLocationBlockButton(std::span<const LocationBlockLayout> _layouts, float _x,
                                                                float _y) noexcept;

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
