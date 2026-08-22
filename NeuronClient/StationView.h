#pragma once

#include <cstdint>

/*
 * What the game hands over for the `Station` surface (ADR-020 §6, ADR-017 §6).
 *
 * The screen-data contract for a full-screen surface, in the shape §6 fixed so
 * that U5's map and this one would not each invent one: a **tab row**, and a
 * **docked roster as group rows plus chip rows**, keyed by station.
 *
 * **Every word here comes across the seam, and that is the whole point.**
 * ADR-020 §6's leak test is explicit that `NeuronClient` must never learn that
 * a tab is called REFIT -- so a tab is a name, a reason and a number, and this
 * file never spells one. The same rule `HudRoster.h` follows for a wing: a
 * group is a name and three numbers, and the word for what a group *is* stays
 * on the side entitled to have an opinion about it.
 *
 * It is also why nothing here is called a hangar. `SurfaceId::Station` is an
 * engine concept -- the client has to know which screen is live -- but HANGAR
 * is one of the words in the tab row, which makes it the game's.
 */

namespace Neuron
{

/*
 * One tab in the station's row.
 *
 * `station-screen.png` §2's "tabs, not tiles": the station's future arrives as
 * siblings in a fixed row rather than as panels inside one screen, so the
 * layout never reshuffles when a service lands. A tab that has no service
 * behind it yet is **drawn and disabled with its reason** rather than hidden --
 * the same treatment the strategic map gives its unbuilt overlays, and the
 * reason `tag` exists.
 */
struct StationTab
{
  /// The word on it. Points at storage the world view owns, like
  /// `RosterRow::name`, and is never null in a tab the game reports.
  const char* name = nullptr;

  /*
   * Why it is disabled, when it is: the print draws FREE against REPAIR and
   * FUTURE against the three that do not exist yet. Null on a live tab.
   *
   * A string rather than a code, like `LocationBlock::stateLabel`, because
   * nothing keys or coalesces on it -- it is drawn and only drawn.
   */
  const char* tag = nullptr;

  /// What a press hands back. Opaque, echoed, never read here.
  std::uint16_t id = 0;

  /// A disabled tab is drawn and refused by the hit test, exactly as a greyed
  /// command is (`CommandButton::enabled`).
  bool enabled = false;
};

/// How many tabs a client will ask for. The print draws seven -- HANGAR, CARGO,
/// REFINERY, REPAIR, REFIT, CONSTRUCT, MARKET -- and this is that plus room,
/// because a service arriving is a tab and nothing else.
inline constexpr std::uint32_t MAX_STATION_TABS = 12;

/*
 * One column of the docked roster.
 *
 * The print groups by wing "because the wing is the unit a player actually
 * undocks", and wing 0 is a real column rather than an afterthought: strays
 * land there, and emptying it is most of what the reorganisation room is for.
 * None of which this file knows -- it holds a name, a tag and two counts.
 */
struct StationGroup
{
  const char* name = nullptr;

  /*
   * The number beside the name -- the print's `· 3`.
   *
   * Drawn rather than interpreted, and it is on the screen at all because the
   * print decided to admit something rather than hide it: names live in the
   * user settings layer, so two clients without shared settings see different
   * words for the same group, and the number is the part they agree on.
   */
  std::uint16_t idTag = 0;

  std::uint16_t chipCount = 0;

  /*
   * How many of them the composer holds.
   *
   * The *game* counts it, for `RosterRow::selectedCount`'s reason: the
   * alternative is the engine matching selected ids against group membership,
   * which is the aggregation it is not supposed to be doing.
   */
  std::uint16_t selectedCount = 0;
};

/// How many columns a client will ask for. Wings are emergent and a player may
/// use any of 1..255, but a screen with more columns than this has stopped
/// being scannable, which is a design problem rather than a bigger number.
inline constexpr std::uint32_t MAX_STATION_GROUPS = 16;

/*
 * One docked ship.
 *
 * A class word, a name and a wing -- "that is the entire vocabulary, and it is
 * why sixty ships fit without a scrollbar" (`station-screen.png` §1). **No
 * gauges, and their absence is a rule rather than an omission:** the roster
 * holds no damage state, which is what makes undocking repair a ship by
 * construction (ADR-017 §1). A hull bar here would be drawing a number that
 * does not exist.
 */
struct StationChip
{
  /*
   * The durable roster id (ADR-018 A11/D6), and 32 bits because a docked ship
   * has no `EntityRecord::id` -- docking despawned it.
   */
  std::uint32_t shipId = 0;

  const char* name = nullptr;

  /// The game's word for what it is. Drawn beside the name, never compared.
  const char* className = nullptr;

  /// Which `StationGroup` it belongs to, by index into the groups the same
  /// call filled. An index rather than the wing number, so the screen can walk
  /// columns without a lookup and never has to learn what a wing is.
  std::uint16_t group = 0;

  bool selected = false;
};

/*
 * How many chips a client will ask for.
 *
 * Twice what the print draws, and past what one roster message can carry: a
 * `StationRoster` is bounded by the datagram, which puts it near sixty ships.
 * A hangar itself is uncapped -- stations dock unlimited ships, and the print
 * refuses to draw a capacity meter for a scarcity nobody has designed.
 */
inline constexpr std::uint32_t MAX_STATION_CHIPS = 128;

/// What one `BuildStationRoster` filled in. Counts rather than a return value,
/// because the call fills two spans and a caller needs both.
struct StationRosterCounts
{
  std::uint32_t groups = 0;
  std::uint32_t chips = 0;

  /*
   * How many ships are docked there, before any of this side's caps.
   *
   * **Not the sum of the columns, and not `chips`**, which is the whole reason
   * it is here. A ship in a wing past the column cap is skipped before it is
   * counted anywhere, and a ship past the chip cap is counted in its column but
   * not emitted -- so both of the numbers a caller could add up understate a
   * hangar that overflowed, in two different ways.
   *
   * A status bar saying how many ships are at a station must not be one of
   * those: it is a statement about the *station*, not about what fitted on the
   * screen. The screen says how much of it it could draw by other means.
   */
  std::uint32_t docked = 0;
};

/*
 * The composer's primary action, as the game states it.
 *
 * `OrderKindOption`'s shape, moved from a row to a screen and for the identical
 * reason: **UNDOCK is a verb the client sends and may not name.** The word on
 * the button, the number that goes on the wire, and the word for whatever the
 * verb's parameter means all come from the side entitled to have an opinion
 * about them -- which is the same rule that keeps REFIT out of `StationTab`.
 *
 * **No `available` and no reason here**, unlike `OrderKindOption`, and the
 * absence is the design: whether UNDOCK is live depends on *this frame's*
 * selection, and `PreCheckStation` already answers that from
 * `ValidateStationCommand` -- the authority's own function. A second answer
 * built here would be a second opinion, which is exactly what bounce parity
 * forbids.
 */
struct StationAction
{
  /// The word on the button. Never null in an action the game reports.
  const char* name = nullptr;

  /// What the verb's parameter is called -- the print's `FORMATION`. Null for a
  /// verb that takes none, the way `OrderKindOption::parameterName` is.
  const char* parameterName = nullptr;

  /// What goes in `StationIntent::verb`. Opaque, echoed, never read here.
  std::uint16_t verb = 0;

  /*
   * Whether pressing this takes its ships out of the composer.
   *
   * The game answers it, and the engine acts on it without learning why --
   * `LocationBlock::inScene`'s arrangement exactly. It exists because the two
   * verbs on this screen differ on it and the client may not tell them apart:
   * one sends ships away, so a composer still naming them would build a second
   * command against a roster row that has gone; the other rewrites a number on
   * ships that stay docked, and clearing after it would make reorganising a
   * wing cost a fresh selection every time.
   *
   * `Reconcile` would eventually drop the departed ids anyway -- but the roster
   * arrives at about 1 Hz, and "eventually" is a whole second in which a second
   * press bounces.
   */
  bool consumesSelection = false;
};

/// How many actions a composer will ask for. Two today -- the print's primary
/// and its secondary -- and the room is for the verbs ADR-024 adds, which
/// arrive as siblings rather than as a second panel.
inline constexpr std::uint32_t MAX_STATION_ACTIONS = 4;

/*
 * How many values one station verb's parameter may take.
 *
 * `MAX_ORDER_OPTIONS` is eight because that is the command wheel's sector count
 * -- an argument about *that* surface, and this is not it. A wing number is not
 * a short authored list the way a formation is: wings are emergent, a player
 * may use any of 1..255, and the composer's parameter is a chip in a
 * full-screen panel rather than a sector on a wheel. So the bound is this
 * screen's own, and it is the roster's row cap plus the values that are not
 * existing wings -- past `MAX_ROSTER_ROWS` a wing has no row to appear on, so
 * offering it would be offering a wing the player could not then see.
 */
inline constexpr std::uint32_t MAX_STATION_OPTIONS = 20;

} // namespace Neuron
