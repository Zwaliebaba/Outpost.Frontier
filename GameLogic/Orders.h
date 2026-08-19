#pragma once

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
 * Move is the only one with content. The other three are **reserved** in
 * exactly the sense `HullClass`'s Fighter and Cruiser are (ADR-009 §6):
 * nameable, numbered, and never submittable. `ValidateOrder` refuses anything
 * but Move with `UnknownKind`, so a reserved kind that somehow reached the wire
 * bounces with a reason rather than being acted on.
 *
 * They are here because the command row draws them (`tactical-hud.png`) and the
 * command wheel will (`puck-and-wheel.png` §3, eight sectors with the illegal
 * ones greyed). A greyed ATTACK button has to be *named* by something, and the
 * engine may not name it -- so the name is here, beside the value it belongs
 * to, rather than as a string in a client that is meant to serve a second game.
 *
 * Numbered contiguously from zero and never renumbered: the value crosses the
 * wire in `OrderSubmit`.
 */
enum class OrderKind : std::uint8_t
{
  Move = 0,
  Attack = 1,   // Reserved: no validation, no simulation.
  Stance = 2,   // Reserved.
  Abilities = 3 // Reserved.
};

/// All of them, in the order a command surface should offer them -- the print's
/// own left-to-right. An array for the same reason `FORMATION_IDS` is one.
inline constexpr OrderKind ORDER_KIND_IDS[] = {OrderKind::Move, OrderKind::Attack, OrderKind::Stance,
                                               OrderKind::Abilities};

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

/// False for the three reserved kinds. Asked rather than remembered, the same
/// as `HullClassHasContent` -- a command surface greys what this answers no to.
[[nodiscard]] bool OrderKindHasContent(OrderKind _kind) noexcept;

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
  UnknownKind = 8
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
