#pragma once

#include "Ids.h"
#include "Orders.h"
#include "ShipClass.h"
#include "Validate.h"

#include <cstdint>
#include <span>

/*
 * The station's roster, and the commands over it (ADR-017 §1, §3, §6).
 *
 * Docked ships are not hulls. They are rows: `(ShipId, class, wing)` and
 * nothing else, held at the universe layer rather than on a grid. That one
 * decision buys clutter prevention by construction (a docked ship has no
 * position to clutter with), leaves the grid teardown rule untouched, costs no
 * snapshot bytes and no tick time, and makes docked ships untouchable by a
 * combat model that does not exist yet.
 *
 * **The roster keeps no damage state, and that *is* the repair rule.** "Repair
 * on dock" is not a system that runs; it is the absence of a field. When repair
 * is meant to cost time or money, the row grows gauges and a verb, and this
 * sentence is the named hook.
 *
 * **Undocking is not an order on world ships -- there are none to name.** So it
 * is a *station command*: a small family beside `OrderSubmit`, sharing its
 * sequence counter, its ack and its reason enum, validated by one pure function
 * over the replicated roster both halves hold. The parity contract extends
 * rather than forking, which is the whole reason this file has no `World` in it.
 */

namespace Game
{

/*
 * One docked ship. Three fields, deliberately.
 *
 * The id survives docking, and that is load-bearing: undock respawns *this*
 * ship, and every log, order and roster row means the same one before and
 * after.
 */
struct RosterEntry
{
  ShipId shipId = INVALID_SHIP_ID;
  HullClass hullClass = HullClass::Interceptor;
  WingId wing = INVALID_WING_ID;
};

/*
 * What a station command is judged against: which station, and what is on it.
 *
 * The client's copy arrives replicated (ADR-017 §8) and the server's is a span
 * into the registry's own storage. Same shape, same function, same verdict --
 * BounceParity for the station surfaces, bought the same way it was bought for
 * orders.
 */
struct RosterView
{
  AnchorId station = INVALID_ID;
  std::span<const RosterEntry> docked;
};

/*
 * The verbs (ADR-017 §3, §6).
 *
 * Two, and both are about *composition*: which ships leave together, and which
 * wing a ship belongs to. There is no fleet entity to create and no wing table
 * to desync -- a fleet is the set that undocks together, and a wing exists iff
 * a ship carries its number.
 */
enum class StationVerb : std::uint8_t
{
  Undock = 0,
  AssignWing = 1
};

/// What to call one. Never null, for the same reason `OrderReasonText` is not.
[[nodiscard]] const char* StationVerbName(StationVerb _verb) noexcept;

/*
 * A station command as submitted.
 *
 * Fixed storage and the same 64-ship cap as an order, because a fuller hangar
 * undocks in waves and a truncated selection is not the selection. `orderSeq`
 * is the client's own counter -- the *same* counter orders use, so one ack
 * stream serves both and a ghost cannot be confused with a command.
 */
struct StationCommand
{
  std::uint32_t orderSeq = 0;
  StationVerb verb = StationVerb::Undock;

  /// The station this is addressed to. A command for a station the view is not
  /// holding is `UnknownStation`, exactly as a Dock naming another grid's
  /// anchor is.
  AnchorId station = INVALID_ID;

  /// Undock only: the arrangement the fleet takes at the undock point.
  FormationId formation = FormationId::Line;

  /// AssignWing only. Any value: 1..255 are wings, and `INVALID_WING_ID` is
  /// "no wing", which is how the last member of a wing disbands it. There is no
  /// wing table to create or destroy -- a wing exists iff a ship carries its
  /// number (ADR-017 §6).
  WingId wing = INVALID_WING_ID;

  std::uint16_t shipCount = 0;
  ShipId shipIds[MAX_SHIPS_PER_ORDER] = {};

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

/*
 * Is this command allowed? The station half of ADR-005 §4's contract.
 *
 * The check order is the same kind of promise the order side makes, and for the
 * same reason -- the reason is what the player reads, so a command that breaks
 * two rules has to name the same one on both machines:
 *
 *   `EmptySelection` -> `TooManyShips` -> `InvalidFormation` -> `UnknownStation`
 *   -> `NotDocked`
 *
 * Selection first (it is the cheapest and the most likely), then the parameter,
 * then the target, then the ships -- target before ship resolution, which is
 * the same shape the order side settled on.
 */
[[nodiscard]] OrderVerdict ValidateStationCommand(const RosterView& _view, const StationCommand& _command) noexcept;

/*
 * Undock protection (ADR-017 §5), in seconds.
 *
 * Immunity to damage, which is a forward design -- nothing deals damage yet.
 * What it decides *today* is nothing at all about collision: protected ships
 * stay solid, exclude space, brake, deflect and separate exactly as before, so
 * "protected" never means "overlapping" and `Separate` needs no phase flag.
 *
 * Ended early by the player's own command. The system-issued parking order does
 * not end it -- that is what `OrderGroup::systemIssued` is for -- and the early
 * break is the anti-abuse shape combat will want: you cannot shoot from under
 * the station's skirts.
 */
inline constexpr std::uint32_t UNDOCK_PROTECTION_SECONDS = 15;

/*
 * The parking ring (ADR-017 §4).
 *
 * Undocked ships get out of the doorway by themselves, to a **berth**: a
 * candidate anchor on a ring around the station. Two rings, both *inside* the
 * dock radius, so a parked fleet can re-dock without moving first.
 *
 * **Bearings, not arcs.** A fixed arc lies about a capital line whose footprint
 * is kilometres wide: the arc says "this much room" and the fleet needs three
 * times it. A bearing says only *which way*, and how much room that direction
 * has is answered by solving the formation there and looking.
 */
inline constexpr float PARKING_RING_METRES[] = {2500.0f, 4000.0f};

/// Twelve per ring, so 24 candidates in all. Scanned from the bearing of the
/// undock point outward, alternating left and right, inner ring before outer --
/// a fixed order, because "which berth" has to be a function of the world and
/// not of the order two fleets happened to arrive in.
inline constexpr std::uint32_t PARKING_BEARINGS = 12;

} // namespace Game
