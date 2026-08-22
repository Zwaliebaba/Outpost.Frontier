#pragma once

#include "EconomyDef.h"
#include "Ids.h"

#include <cstdint>

/*
 * The order vocabulary (ADR-005 §1, ADR-004 §7).
 *
 * One kind in the MVP -- Move -- and the whole point of naming a kind at all is
 * that the wire and the validator agree on what a zero means. Everything here
 * is data: no world, no validation, no encoding. `Validate.h` decides whether
 * an order is allowed, `Formation.h` decides where it puts things, and
 * `OrderMessages.h` turns it into bytes. Three files because they are three
 * decisions, and only the first of them needs a world.
 *
 * **Legs are wire-quantised, and that is the parity rule.** A leg carries
 * centimetres and turns/65536, not metres and radians, because ADR-005 §4 says
 * validation consumes wire-quantised values only: the client validates against
 * a replicated view that is already quantised, and the server has to quantise
 * its own state before validating or the two can disagree on float noise. A
 * struct that stored metres would make that mistake available.
 */

namespace Game
{

/*
 * What kind of command this is.
 *
 * Move and Dock have content. The rest are **reserved** in exactly the sense
 * `HullClass`'s Fighter and Cruiser are (ADR-009 §6): nameable, numbered, and
 * never submittable. `ValidateOrder` refuses them with `UnknownKind`, so a
 * reserved kind that somehow reached the wire bounces with a reason rather than
 * being acted on.
 *
 * They are here because the command row draws them (`tactical-hud.png`) and the
 * command wheel will (`puck-and-wheel.png` §3, eight sectors with the illegal
 * ones greyed). A greyed ATTACK button has to be *named* by something, and the
 * engine may not name it -- so the name is here, beside the value it belongs
 * to, rather than as a string in a client that is meant to serve a second game.
 *
 * **`Warp = 4` and `Dock = 5`**, which is the opposite of the order they were
 * built in (ADR-017 §2). The station phase landed before in-system warp and
 * ADR-016 had already published warp's number; renumbering it to close the gap
 * would have made a written-down wire value a lie for the sake of tidiness. So
 * warp kept 4 while it was reserved, and U3a filled it in where it stood.
 *
 * Numbered contiguously from zero and never renumbered: the value crosses the
 * wire in `OrderSubmit`.
 */
enum class OrderKind : std::uint8_t
{
  Move = 0,
  Attack = 1,    // Reserved: no validation, no simulation.
  Stance = 2,    // Reserved.
  Abilities = 3, // Reserved.
  Warp = 4,
  Dock = 5,

  /*
   * Work a mining field (ADR-024 §4a, build order E2).
   *
   * Appended after `Dock` for the reason warp kept 4 while it was reserved: the
   * numbering is what is on the wire, and tidying it is how a written-down wire
   * value becomes a lie. The economy phase arrived after the station phase, so
   * it takes the next number.
   *
   * Its parameter is an **ore filter** rather than a formation, which is the
   * first time a kind's parameter is not one -- `OrderSubmit` carries both,
   * because a Mine order is still flown in a formation and the escorts in it
   * take stations around the worked cluster.
   */
  Mine = 6
};

/// All of them, in the order a command surface should offer them -- the print's
/// own left-to-right. An array for the same reason `FORMATION_IDS` is one.
inline constexpr OrderKind ORDER_KIND_IDS[] = {OrderKind::Move,  OrderKind::Attack, OrderKind::Stance,
                                               OrderKind::Abilities, OrderKind::Warp, OrderKind::Dock,
                                               OrderKind::Mine};

/// Replace the queue or append to it (ADR-004 §7).
enum class QueueMode : std::uint8_t
{
  Replace = 0,
  Append = 1
};

/*
 * The arrangement a group takes at its destination (ADR-005 §3).
 *
 * The MVP set is Line, Wedge and Claw. S9 solves Line; the other two are
 * S10's, and they are named here rather than added later so that a client
 * sending `Wedge` at an S9 server is refused as `InvalidFormation` instead of
 * silently getting a Line.
 */
enum class FormationId : std::uint8_t
{
  Line = 0,
  Wedge = 1,
  Claw = 2
};

/// All of them, in the order a command surface should offer them. An array
/// rather than a count-and-loop, because "the values of an enum" is not
/// something C++ will hand back and writing `0..FORMATION_COUNT` assumes a
/// contiguity the wire does not promise to keep.
inline constexpr FormationId FORMATION_IDS[] = {FormationId::Line, FormationId::Wedge, FormationId::Claw};

/*
 * The postures the Stance command's parameter names (`tactical-hud.png`'s
 * `STANCE AGGRESSIVE` readout).
 *
 * **Presentation vocabulary only, so far.** The Stance kind is reserved --
 * `ValidateOrder` refuses it and nothing simulates it -- but the HUD's context
 * bar states the chosen posture and the picker offers the list, and both have
 * to get the words from the game (ADR-014 §2b). The values never cross the
 * wire while the kind has no content, which is why they are absent from
 * `GAME_SCHEMA_TEXT`; the day a stance order becomes submittable they join it,
 * beside the validation that makes them mean something.
 *
 * Balanced is zero so a zeroed parameter is the default posture -- the same
 * arrangement `FormationId::Line` has.
 */
enum class StanceId : std::uint8_t
{
  Balanced = 0,
  Aggressive = 1,
  Evasive = 2
};

/// All of them, in the order a command surface should offer them.
inline constexpr StanceId STANCE_IDS[] = {StanceId::Balanced, StanceId::Aggressive, StanceId::Evasive};

/// What to call one on screen. Never null, like `FormationName`.
[[nodiscard]] const char* StanceName(StanceId _stance) noexcept;

/// What to call one on screen. Never null, for the same reason
/// `OrderReasonText` is never null: a label with no text is a control the
/// player cannot name.
[[nodiscard]] const char* FormationName(FormationId _formation) noexcept;

/// And what to call the command itself -- the `MOVE` half of the ghost's
/// `MOVE - CLAW` label (`tactical-hud.png`). Here beside `FormationName`
/// because a label naming one and not the other would be half a sentence, and
/// because the engine is allowed to name neither (ADR-014 §2b).
[[nodiscard]] const char* OrderKindName(OrderKind _kind) noexcept;

/*
 * And what to call what a group of this kind is *doing* once it has stopped
 * travelling -- the `MINING` of `MINING Â· 4M 12S` -- or null for a kind
 * whose whole life is a journey.
 *
 * Separate from `OrderKindName` because they are different words about
 * different moments: `MINE` is the verb a player presses, and `MINING` is the
 * state the fleet is in afterwards. A surface that reused the first for the
 * second would report a command where the player is looking for a condition.
 *
 * **Null is the ordinary answer, and it is what makes the predicate honest.**
 * Every kind but one finishes when its last leg does, so "under way with no leg
 * left" describes only a Mine group -- and that is a game rule rather than an
 * arithmetic one, which is why the client asks for a word here rather than
 * working the state out from two numbers it can already read.
 */
[[nodiscard]] const char* OrderWorkingName(OrderKind _kind) noexcept;

/// False for the three reserved kinds. Asked rather than remembered, the same
/// as `HullClassHasContent` -- a command surface greys what this answers no to.
[[nodiscard]] bool OrderKindHasContent(OrderKind _kind) noexcept;

/*
 * Whether this kind is judged partly on *where the player points*.
 *
 * A Move goes to a place the player chose and a Warp names one reachable anchor
 * out of several, so neither can be decided before the gesture that names it. A
 * Mine works the field the fleet is standing on, and a Dock enters this grid's
 * station -- **"a grid is anchored on one thing, so there is at most one
 * station to dock at"** (`ValidationView::stationAnchor`). For those two there
 * is no place to name, so everything that decides them is on screen already.
 *
 * The predicate exists because that property decides two things at once, and a
 * surface that answered them differently would be incoherent: a kind that names
 * no destination can be **pre-judged** before the gesture, and it can be
 * **issued by the press itself** -- there is nothing a second gesture would add.
 * Answering yes to the first and no to the second is a button that lights up
 * and then waits to be told something the player has nothing left to say.
 *
 * True for the reserved kinds, which is the safe answer for a verb whose rules
 * nobody has written: it keeps them armed-then-gestured rather than firing on a
 * press.
 */
[[nodiscard]] bool OrderKindNamesDestination(OrderKind _kind) noexcept;

/*
 * What a kind's `OrderIntent::parameter` is called, or null when it has none.
 *
 * The print's command row draws `FORMATION` next to `MOVE` and puts a dropdown
 * caret on it: the button is not a command, it is the *name of the thing that
 * command varies by*. `OrderOptions` already reports the values that parameter
 * may take; this is the word for the parameter itself, and without it a client
 * can offer the choice but cannot say what is being chosen.
 */
[[nodiscard]] const char* OrderKindParameterName(OrderKind _kind) noexcept;

/*
 * Why an order was refused, or that it was not (ADR-005 §4).
 *
 * The values cross the wire in `OrderAck` and are rendered as a toast, so they
 * renumber never. `Accepted` is zero so that a zeroed verdict is the honest
 * default only when `accepted` is also true -- the two fields are checked
 * together and neither is sufficient alone.
 */
enum class OrderReason : std::uint16_t
{
  Accepted = 0,
  EmptySelection = 1,
  NotOwned = 2,
  UnknownShip = 3,
  QueueFull = 4,
  OutOfBounds = 5,
  InvalidFormation = 6,
  TooManyShips = 7,
  UnknownKind = 8,

  /*
   * The station phase's five (ADR-017 §8). `CombatEngaged` is **reserved**:
   * numbered and named now, returned by nothing, because the reconnect print
   * already promises a combat flag that refuses docking and a reason enum that
   * renumbered when combat arrived would renumber the wire.
   *
   * U3a's warp reasons append *after* these -- the cost of the station phase
   * landing first, written down so the numbering surprises nobody.
   */
  UnknownStation = 9,
  NotAtStation = 10,
  NotDocked = 11,
  InvalidQueueMode = 12,
  CombatEngaged = 13, // Reserved: nothing returns this until combat exists.

  /// The anchor a `Warp` named is not one this grid can reach (U3a). After the
  /// station phase's five, which is the cost of that phase landing first and
  /// was written down when it did.
  UnknownAnchor = 14,

  /*
   * The commander has nothing at the grid they asked to watch (ADR-016 §7,
   * U3b's view gate).
   *
   * Its own value rather than a reused one, because the reason is the sentence
   * the player reads: `NotAtStation` would tell someone asking to look at a
   * system that their fleet is in the wrong place to dock, which is a different
   * complaint about a different action. A refusal that names the wrong problem
   * costs more than an enumerator does.
   */
  NoPresence = 15,

  /*
   * A jump was ordered from too far off the gate (ADR-016 §5, U4).
   *
   * Distinct from `UnknownAnchor`, which is what naming a destination this grid
   * cannot reach at all gets: the pair are the same two answers `UnknownStation`
   * and `NotAtStation` already give for docking -- "there is no such place from
   * here" and "there is, and you are not at it" -- and a player who is told the
   * wrong one of those flies the wrong correction.
   *
   * Only a *jump* can earn it. An in-system warp is ordered from wherever the
   * fleet stands, because §5's spool is what it pays instead of proximity; it
   * is crossing a gate that requires standing at one.
   */
  NotAtGate = 16,

  /*
   * The economy phase's three (ADR-024 §8, build order E2). Appended after the
   * warp pair, which is the cost of that phase landing first and is written
   * down here so the numbering surprises nobody.
   *
   * `NotAtSite` is `NotAtStation` one noun along: the grid you are on has no
   * field to work. Getting to one is the client's job -- the MINE context
   * action on a site feeds a Warp and then a Mine, the way the DOCKING chip
   * already does -- so this is what a Mine ordered anywhere else gets.
   */
  NotAtSite = 17,

  /*
   * Nothing in the order can mine.
   *
   * Its own reason rather than `EmptySelection`, because the selection is not
   * empty: a commander who dragged a box over their escort wing and hit MINE
   * has made a specific mistake, and "you have no Miners here" is the sentence
   * that fixes it. Mixed groups are legal and intended (ADR-024 §4a) -- this
   * refuses only a group with no Miner at all.
   */
  NoMinerInOrder = 18,

  /*
   * Every Miner in the order is already full.
   *
   * Refused rather than accepted-and-immediately-done, because a fleet that
   * cannot mine has nothing to do at a field and an order that completed on its
   * first tick would read as an order that worked. The fix is a station, not a
   * retry.
   */
  HoldFull = 19,

  /*
   * Reserved for the refining slices (ADR-024 §8, build order E3/E4).
   *
   * Numbered now and returned by nothing, the way `CombatEngaged` is: the wire
   * cluster E3 lands is one fail-closed bump, and a reason enum that renumbered
   * when refining arrived would renumber the wire for a feature that was always
   * planned.
   */
  InsufficientMaterials = 20, // Reserved: E3's Bay transfers.
  RefineryBusy = 21,          // Reserved: E4's job slots.
  RecipeLocked = 22           // Reserved: E4's tier caps.
};

/// Human text for a reason, for logs and for the toast the client raises. Never
/// localised here -- this is the diagnostic string, not the UI one.
[[nodiscard]] const char* OrderReasonText(OrderReason _reason) noexcept;

/*
 * One waypoint: where to go and which way to face on arrival.
 *
 * Quantised, per the file comment. `facingTurns16` is the same encoding
 * `EntityRecord::headingTurns16` uses, so a leg and a ship heading are
 * comparable without a conversion that could round differently.
 */
struct OrderLeg
{
  std::int32_t xCm = 0;
  std::int32_t yCm = 0;
  std::uint16_t facingTurns16 = 0;
};

/// How many legs a group may hold (ADR-005 §1: "up to 4 legs -- the wire cap is
/// the sim cap"). An append past this is `QueueFull`, which is the honest
/// answer rather than dropping the oldest.
inline constexpr std::uint32_t MAX_ORDER_LEGS = 4;

/*
 * How many ships one order may name.
 *
 * Not a wire limit -- a datagram holds hundreds of ids -- but a game one. It
 * matches `Neuron::MAX_ORDER_PREVIEW_MARKS`, because an order whose footprint
 * the client cannot draw is an order the player cannot see the result of, and
 * refusing it as `TooManyShips` is better than accepting one whose preview
 * silently truncated.
 */
inline constexpr std::uint32_t MAX_SHIPS_PER_ORDER = 64;

/*
 * A command as submitted, before anything has decided whether it is allowed.
 *
 * Fixed storage rather than a vector: this is decoded from a datagram on the
 * server's tick thread, and an allocation per order is an allocation the
 * simulation does not need to make. `MAX_SHIPS_PER_ORDER` ids is 128 bytes.
 *
 * `orderSeq` is the client's own monotonic counter, echoed in `OrderAck` and in
 * the snapshot header so the client can close the loop even when an ack is lost
 * (ADR-004 §6, §7).
 */
struct OrderSubmit
{
  std::uint32_t orderSeq = 0;
  OrderKind kind = OrderKind::Move;
  FormationId formation = FormationId::Line;
  QueueMode queueMode = QueueMode::Replace;

  std::uint16_t shipCount = 0;
  ShipId shipIds[MAX_SHIPS_PER_ORDER] = {};

  OrderLeg target;

  /*
   * The place this order names, when it names one rather than pointing at the
   * plane: the station for `Dock`, and the destination for `Warp` when U3a
   * fills it in. `INVALID_ID` for a Move, which points at `target` instead.
   *
   * An anchor id rather than a station id, because ADR-016 §3 made the anchor
   * the thing a command can address -- a warp order carries one number and
   * nothing else -- and a Dock that named a `StationId` would need the system
   * to disambiguate it. One field for both verbs, so the client's "act on that
   * structure" gesture fills the same slot whichever verb it resolves to.
   */
  AnchorId anchor = INVALID_ID;

  /*
   * Which ore a `Mine` is after (ADR-024 §4a). `Any` for every other kind,
   * which is also what a zeroed field means.
   *
   * A second parameter beside `formation` rather than a reuse of it, and the
   * reason is that a Mine order needs both: the Miners work a cluster and the
   * escorts take formation stations around them. Smuggling one into the other
   * is the mistake `PendingOrder`'s own comment records -- a mode is not a leg
   * index, and a filter is not a formation.
   */
  OreFilter oreFilter = OreFilter::Any;

  /// Appends an id, or reports the order is full. Returning false rather than
  /// dropping keeps a truncated selection from reading as the whole one.
  [[nodiscard]] bool AddShip(ShipId _shipId) noexcept
  {
    if (shipCount >= MAX_SHIPS_PER_ORDER)
    {
      return false;
    }
    shipIds[shipCount] = _shipId;
    ++shipCount;
    return true;
  }
};

/// What a group is doing, reported in the snapshot so a ghost can be promoted
/// (ADR-005 §1, ADR-004 §6).
enum class OrderState : std::uint8_t
{
  Underway = 0,
  Arriving = 1,
  Done = 2
};

} // namespace Game
