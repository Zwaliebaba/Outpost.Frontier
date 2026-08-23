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

  /*
   * And whose it is (ADR-018 D5, U3c-a) -- the fifth field, and the one the
   * privacy rule was always going to need.
   *
   * `StationBay` below says it exactly: ADR-017 §1 bought the roster's privacy
   * by sending it **per viewer**, and the Bay needs the owner in its key as
   * well because a Bay is not addressable without saying whose. A roster IS
   * addressable without one -- it is the station's -- so the owner rides on the
   * row instead, and the filter happens at the send.
   *
   * Which means a docked ship is the one place ownership could quietly go
   * missing. A ship on a grid can be asked of the registry's index; a docked
   * ship is not on any grid, and if the roster did not remember, the answer
   * after a dock would be "nobody" -- so a commander's own hangar would stop
   * being theirs the moment they arrived. It is the same shape as `oreUnits`
   * above, one field along: state that is the player's rather than the ship's,
   * which the crossing has to carry rather than rebuild.
   */
  Neuron::PlayerId owner = Neuron::INVALID_PLAYER_ID;

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

  /*
   * And what refining turned it into (ADR-024 §6, E4b) -- the second half the
   * E3 comment above promised.
   *
   * The same Bay rather than a second ledger beside it, because it is the same
   * statement: what this commander has committed at this station. A refine job
   * takes ore out of one array and puts alloy into the other, and an upgrade
   * project takes alloy out again; splitting them would be two records that had
   * to agree about one place.
   */
  std::uint32_t alloyUnits[ALLOY_COUNT] = {};

  [[nodiscard]] std::uint32_t Units(AlloyId _alloy) const noexcept { return alloyUnits[static_cast<std::uint8_t>(_alloy)]; }

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
   * And the grid, for the one verb that no longer needs a dock (I2, ADR-017
   * §6's 2026-08-23 amendment).
   *
   * **Two spans at one place rather than a second view.** A station's anchor is
   * also a grid's, so `station` and `grid` are usually the same id -- what
   * differs is which set of this commander's ships is being named: the ones on
   * the roster, or the ones flying outside it. An `AssignWing` may name either,
   * because a wing is a control group and a player forms one wherever the ships
   * are; every other verb reads `docked` and nothing else.
   *
   * They are separate rather than concatenated because the difference is a
   * *rule*: `Undock` searching a merged list would accept a ship in space and
   * try to undock it from a station it was never in.
   *
   * `INVALID_ID` and an empty span are "the caller cannot say", the shape
   * `bayUnits` already has and for the same reason -- a client whose snapshot
   * has not arrived pre-refuses, and the authority is what decides.
   */
  AnchorId grid = INVALID_ID;
  std::span<const RosterEntry> onGrid;

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

  /*
   * And the refinery's half of the same view (ADR-024 §6, E4b), on `bayUnits`'
   * terms exactly: what the caller knows, and empty or zero when it cannot say.
   *
   * `refineryTier` is the station's *effective* tier -- authored floor, raised
   * by any completed project, and never above its band's cap. Zero means "the
   * caller does not know", which refuses every recipe as `RecipeLocked`: the
   * designed asymmetry ADR-005 §4 already names, applied to a client that has
   * not yet had a `RefineryStatus`.
   */
  std::uint8_t refineryTier = 0;

  /// How many jobs this commander already has here, running and queued
  /// together. Compared against the tier's slots plus the authored queue depth.
  std::uint32_t refineJobCount = 0;

  /// The tier's slots and the content's queue depth, so the validator does not
  /// need the economy table to answer `RefineryBusy` -- the same reason
  /// `bayUnits` is a span of counts rather than a `StationBay`.
  std::uint32_t refineSlotCount = 0;
  std::uint32_t refineQueueDepth = 0;

  /// What the commanding player has in the Bay here, per alloy. What a
  /// `ProjectContribute` is judged against.
  std::span<const std::uint32_t> bayAlloyUnits;

  /*
   * Is there an open upgrade project here, and how much of `alloy` will it
   * still take?
   *
   * A *remaining* count rather than the project's totals, because the print
   * clamps its preview at what remains (D-P3): the project completes exactly
   * once, so the screen must never offer units it will not take -- and the
   * validator must refuse the ones it would not.
   */
  bool projectOpen = false;
  std::span<const std::uint32_t> projectRemainingUnits;

  /*
   * Whether the batch size and the recipe are ones this station will cook.
   *
   * Two booleans rather than the economy table, for `refineSlotCount`'s
   * reason: the view carries answers, not the data to derive them, so the
   * shared validator stays a pure function over a small struct on both
   * machines.
   */
  bool batchKnown = false;
  bool recipeUnlocked = false;
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
  TransferToShip = 3,

  /*
   * And the three refining adds (ADR-024 §6, E4b): start a job, cancel a
   * queued one, and put alloys into a station's upgrade project.
   *
   * **None of them names a ship, and none of them requires being docked.**
   * That is ADR-024 §6b's "at a docked-or-remote station -- focus never gates
   * command", and it is why the check order below forks: a refine command is
   * about a *Bay*, and a Bay is universe-layer state that a fleet's whereabouts
   * has nothing to do with. Requiring a docked hull to name would be inventing
   * a rule to reuse a code path.
   *
   * `RefineCancel` cancels a **queued** job only, whole, with its inputs
   * returning to the Bay untouched -- a running job cannot be cancelled,
   * because §6b debits at submission and those inputs are spent (owner ruling,
   * 2026-08-21, on D-P3's first open question). Two states, one rule each, and
   * nothing is ever created or destroyed by a cancel.
   */
  RefineStart = 4,
  RefineCancel = 5,
  ProjectContribute = 6
};

/// Does this verb name ships? The fork the check order turns on, written once
/// so the validator and the registry cannot disagree about which family a verb
/// is in.
[[nodiscard]] constexpr bool NamesShips(StationVerb _verb) noexcept
{
  return _verb == StationVerb::Undock || _verb == StationVerb::AssignWing || _verb == StationVerb::TransferToBay
         || _verb == StationVerb::TransferToShip;
}

/*
 * And does it require those ships to be *docked*?
 *
 * Everything that names ships did until I2, which is why this was one question
 * and is now two. `AssignWing` is the exception (ADR-017 §6's 2026-08-23
 * amendment): a wing is a number a ship carries rather than a place it is, so
 * reassigning one in space asks nothing of a station. The other three move
 * hulls or holds across a station's threshold and are meaningless without it.
 *
 * Split out rather than spelled at each site for `NamesShips`' own reason: the
 * validator and the registry both branch on it, and a rule written twice is a
 * rule that can be changed once.
 */
[[nodiscard]] constexpr bool RequiresDock(StationVerb _verb) noexcept
{
  return NamesShips(_verb) && _verb != StationVerb::AssignWing;
}

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

  /*
   * The refining verbs' pair, and meaningless for the other four -- the
   * arrangement `formation`, `wing` and `ore` already have.
   *
   * `alloy` is what a `RefineStart` cooks and what a `ProjectContribute` puts
   * in. `units` serves both as the **batch size** and as the **contribution**,
   * which is the same field doing the same job in both: an explicit quantity
   * the player chose. A batch size the content does not author is refused
   * rather than rounded, because a silently-adjusted batch is a different job
   * from the one the form priced.
   */
  AlloyId alloy = AlloyId::FerrocitePlates;

  /*
   * `RefineCancel` only: which of your jobs, by the sequence `RefineryStatus`
   * reports.
   *
   * Its own field rather than `units` wearing a third hat. A sequence is an
   * *identity* and the other two are *quantities*, and a field that meant both
   * would eventually cancel job 50 because somebody meant a batch of 50.
   */
  std::uint32_t sequence = 0;

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
 * **`NotDocked` reads `UnknownShip` for `AssignWing`** (I2): the position in the
 * order is the same and so is the question -- "is this ship here" -- but the
 * sentence has to match the verb. "Not docked here" is the wrong complaint to a
 * player who selected ships in space, and "no such ship" is true wherever the
 * ships were. Keyed on the verb rather than on which of the view's two lists
 * came up empty, because the verb is a byte both machines have and the view is
 * built differently on each.
 *
 * **The refining verbs take a different order, because they name no ships**
 * (ADR-024 §6b, E4b):
 *
 *   `UnknownStation` -> `RecipeLocked` -> `RefineryBusy` -> `InsufficientMaterials`
 *
 * Same shape and same reasoning -- target, then the parameter, then the
 * quantity last -- with the three ship checks absent because there is nothing
 * to check. A refine command is about a Bay, and being docked has nothing to do
 * with one. `RecipeLocked` precedes `RefineryBusy` because a locked recipe is a
 * fact about the *station* and a full queue is a fact about *you*: telling a
 * player their queue is full when the real answer is that this station will
 * never cook Nova-Steel would send them to wait instead of to fly.
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
