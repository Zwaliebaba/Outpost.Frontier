#pragma once

#include "Ids.h"
#include "Orders.h"
#include "ShipClass.h"
#include "Validate.h"

// `Neuron::PlayerId`: the engine's word for whoever is on the other end of a
// socket, and the Bay's key (ADR-014 §2a -- the engine declares the primitive,
// the game decides what it owns). GameLogic naming it is the same dependency
// direction that already lets this library speak `ByteWriter`.
#include "Wire.h"

#include <cstdint>
#include <span>

/*
 * The station's roster, and the commands over it (ADR-017 §1, §3, §6).
 *
 * Docked ships are not hulls. They are rows -- `(ShipId, class, wing)`, and
 * since E3 what the hold is carrying -- held at the universe layer rather than
 * on a grid. That one decision buys clutter prevention by construction (a
 * docked ship has no position to clutter with), leaves the grid teardown rule
 * untouched, costs no snapshot bytes and no tick time, and makes docked ships
 * untouchable by a combat model that does not exist yet.
 *
 * **The station has a second resident now**: the Bay, a commander's committed
 * ore at this station (ADR-024 §5b). Roster and Bay are the same *kind* of
 * thing -- universe-layer state that outlives the grid and folds into the
 * registry hash -- and they are separate records because they answer different
 * questions: what of mine is parked here, and what of mine is stored here.
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
 * One docked ship. Four fields, and the fourth arrived with an argument.
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

  /*
   * And what it is holding (ADR-024 §5, E3) -- the fourth field, and the
   * amendment ADR-017 §1 named in advance.
   *
   * "Three fields, deliberately" was an argument about *gauges*: a docked ship
   * has no position to clutter with and no damage to repair, and the absence of
   * a damage field is the repair rule. Cargo is not a gauge. Nothing
   * regenerates it, nothing repairs it, and it is the player's property rather
   * than the ship's condition -- so **repair-by-absence is untouched by this**,
   * which is the sentence that lets the row grow without the rule quietly
   * changing meaning.
   *
   * It rides on the roster rather than in a table beside it because a docked
   * ship's hold and its identity go to the same places: the reconnect screen
   * reads both, the hangar screen shows both, and a save file would write both.
   * A parallel structure would be one more thing that can be out of step with
   * the roster and nothing to notice when it was.
   */
  std::uint32_t oreUnits[ORE_COUNT] = {};

  [[nodiscard]] std::uint32_t Units(OreId _ore) const noexcept { return oreUnits[static_cast<std::uint8_t>(_ore)]; }
};

/*
 * A commander's Bay at one station (ADR-024 §5b, E3).
 *
 * The industrial half of a station: what a player has committed to this place,
 * as opposed to what is still sitting in a hold. It is **universe-layer durable
 * state**, on exactly the terms the roster and the site ledgers already hold --
 * it outlives the grid, it folds into the registry hash, and a station tearing
 * down with a full Bay leaves the Bay untouched, because it was never world
 * state to begin with.
 *
 * **Per `(owner, station)` and not per station**, which is the whole of the
 * privacy rule: two commanders docked at one station have two Bays and neither
 * can see the other's. ADR-017 §1 bought that for the roster by sending it per
 * viewer; the Bay needs it in the *key* as well, because unlike a roster a Bay
 * is not even addressable without saying whose.
 *
 * Ore only, today. E4 adds the alloys refining turns it into, and the reason
 * they are not here yet is that a field for something nothing can produce is a
 * field nothing can test.
 */
struct StationBay
{
  Neuron::PlayerId owner = Neuron::INVALID_PLAYER_ID;
  AnchorId station = INVALID_ID;
  std::uint32_t oreUnits[ORE_COUNT] = {};

  [[nodiscard]] std::uint32_t Units(OreId _ore) const noexcept { return oreUnits[static_cast<std::uint8_t>(_ore)]; }
  [[nodiscard]] std::uint32_t TotalUnits() const noexcept;
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

  /*
   * What the commanding player already has in the Bay here, per ore, and
   * **empty when the caller cannot say** -- the shape `ValidationView`'s own
   * optional spans have, for the same reason.
   *
   * A span of three counts rather than a `StationBay`, because what the
   * validator needs is the quantity and not the identity: whose Bay it is has
   * already been decided by whoever built the view, and handing the whole
   * record down would put an owner id inside a function that must return the
   * same verdict on a machine that has no idea who anybody is.
   *
   * Empty reads as an empty Bay, which is the honest default: it is what a
   * client that has not yet received a `BayStatus` knows, and a
   * `TransferToShip` pre-checked against it is refused `InsufficientMaterials`
   * until the summary arrives. Refusing early on stale knowledge is the
   * designed asymmetry ADR-005 §4 already names.
   */
  std::span<const std::uint32_t> bayUnits;
};

/*
 * The verbs (ADR-017 §3, §6; ADR-024 §5c).
 *
 * The first two are about *composition*: which ships leave together, and which
 * wing a ship belongs to. There is no fleet entity to create and no wing table
 * to desync -- a fleet is the set that undocks together, and a wing exists iff
 * a ship carries its number.
 *
 * The second two are about *property*: which ore is the station's to work with
 * and which is still the fleet's to fly away with. Appended, never renumbered
 * -- the value is on the wire and in the schema text.
 */
enum class StationVerb : std::uint8_t
{
  Undock = 0,
  AssignWing = 1,

  /*
   * And the two the economy adds (ADR-024 §5c, E3): ore out of the holds and
   * into the Bay, and ore back out again.
   *
   * **Manual, both directions**, which is the ruling and not an omission. An
   * automatic sweep on docking would be convenient and would take the decision
   * away: committing ore to a station is the moment a haul stops being cargo a
   * player can still fly somewhere else and starts being industry's, and
   * ADR-024 §5c makes that the player's move. The way back is manual for the
   * symmetric reason -- ore in a Bay is not stranded.
   *
   * They are *station commands* rather than orders for the same reason
   * `Undock` is: there are no world ships to name. Both ends of the move are
   * universe-layer state, which also means neither files a transfer -- see
   * `WorldRegistry::SubmitStationCommand`, where they apply on the spot beside
   * `AssignWing` and for the same reason.
   */
  TransferToBay = 2,
  TransferToShip = 3
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

  /*
   * The transfer verbs' pair, and meaningless for the other two -- the
   * arrangement `formation` and `wing` already have.
   *
   * One ore per command rather than three counts, because a transfer is a
   * decision about *one* thing and a player moving two ores has made two
   * decisions. It also keeps the refusal honest: `InsufficientMaterials` names
   * a shortfall, and a command that half-succeeded across three ores could not
   * be described by one reason code.
   *
   * `units` is explicit rather than "all of it", because ADR-024 §5c calls the
   * transfer a commitment: a partial commitment is a real move -- half the haul
   * to industry and half kept for the trip home -- and a verb that could only
   * mean "everything" would take that away. Zero is refused as
   * `InsufficientMaterials` for the same reason an empty selection is refused:
   * a command that does nothing is a mistake, not a no-op.
   */
  OreId ore = OreId::FerroChroma;
  std::uint32_t units = 0;

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
 *   -> `NotDocked` -> `InsufficientMaterials`
 *
 * Selection first (it is the cheapest and the most likely), then the parameter,
 * then the target, then the ships -- target before ship resolution, which is
 * the same shape the order side settled on.
 *
 * **`InsufficientMaterials` is last, after the ships resolve**, and that is the
 * ordering decision E3 had to make rather than inherit. A `TransferToBay` names
 * ships and an amount, and both can be wrong at once; telling a player the
 * station is short of ore when the real problem is that they selected a ship
 * that undocked ten seconds ago would send them to fix the wrong thing. The
 * quantity is the *last* question because it is the only one that cannot be
 * asked until you know which holds are being counted.
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
