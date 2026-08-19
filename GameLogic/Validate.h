#pragma once

#include "Orders.h"
#include "ShipClass.h"

#include <cstdint>
#include <span>

/*
 * Is this order allowed? (ADR-005 §4)
 *
 * A pure function over a quantised view of the world, and the same function on
 * both sides of the wire. The client reaches it through
 * `Neuron::WorldView::PreCheck` and the server through
 * `Neuron::Simulation::ApplyOrderBytes` (ADR-014 §3); neither links the other,
 * and both get the same verdict and the same reason code for the same inputs.
 * That is what ADR-014 calls BounceParity, and it is bought here by there being
 * one implementation rather than by two being kept in step.
 *
 * **Why it takes a list of ids and not a `World`.** The client has no `World` --
 * it has a replicated view, which is quantised positions and ids and nothing
 * else. If this took a `World` the client could not call it, and the parity
 * claim would be a claim about two different functions. So it takes the
 * intersection: which ships exist, and where the operating area is. The server
 * builds that view from its own tables (quantising as it goes, per ADR-005 §4);
 * the client's arrives quantised off the wire.
 *
 * **What it deliberately cannot check.** Staleness. The client's view is a few
 * ticks old by construction (ADR-002 §4), so an order that passes locally can
 * still be refused by the authority -- a ship died, or moved out of bounds, in
 * the interval. ADR-005 §4 calls that designed and accepted: the server's
 * verdict is the only one that counts, and the pre-check exists to make the
 * common refusals instant rather than to be right about all of them.
 */

namespace Game
{

/*
 * What a check needs to know about a ship beyond its id.
 *
 * Quantised centimetres, like everything else validation consumes (ADR-005 §4):
 * the client's arrive that way off the wire and the server quantises its own
 * before validating, so the two cannot disagree on float noise at the edge of a
 * radius.
 */
struct ShipMark
{
  std::int32_t xCm = 0;
  std::int32_t yCm = 0;
  HullClass hullClass = HullClass::Structure;
};

/*
 * The half of the world validation needs, from either side of the wire.
 *
 * Ids always, sorted or not; positions and classes only when the caller has
 * them, because for four slices nothing validation decided depended on where a
 * ship *was* -- only on whether it existed and where it was being sent. Dock is
 * the first check that needs more, and it says so by needing `shipMarks`. When
 * ownership arrives (multiplayer, post-MVP) this is where the owning player id
 * joins it.
 */
struct ValidationView
{
  std::span<const ShipId> shipIds;

  /*
   * Parallel to `shipIds`, and **empty unless the caller has it**: where each
   * ship is and what it is.
   *
   * Move needs neither -- nothing it decides depends on where a ship *is*, only
   * on whether it exists and where it is being sent -- which is why the view
   * went four slices without them. Dock needs both: it is judged on distance
   * from a structure, against a radius derived from the fleet's own solved
   * footprint (ADR-018 D7). Optional rather than required so a caller that has
   * only ids can still validate the orders that only need ids, and so this
   * addition cannot silently invalidate a view built before it existed.
   *
   * One span of a small record rather than two parallel spans of primitives:
   * two could be given different lengths, and there would be no way to notice.
   */
  std::span<const ShipMark> shipMarks;

  /// How many legs the group these ships belong to already holds. Zero for a
  /// replace, and what makes `QueueFull` checkable without the group table.
  std::uint32_t queuedLegs = 0;

  /*
   * The station on this grid, if there is one (ADR-017 §2).
   *
   * A grid is anchored on one thing, so there is at most one station to dock
   * at, and `UnknownStation` is what a Dock naming any other anchor gets.
   * `INVALID_ID` means the grid has no station -- open space, a gate, a
   * planet -- and every Dock there is refused for the same reason.
   */
  AnchorId stationAnchor = INVALID_ID;

  /// Where that station is, in the wire's centimetres, in the grid's local
  /// frame. Meaningless when `stationAnchor` is `INVALID_ID`.
  std::int32_t stationXCm = 0;
  std::int32_t stationYCm = 0;
};

/// The verdict, in GameLogic's own terms. `Neuron::OrderVerdict` carries the
/// same two numbers across the seam; this is the typed version the game reasons
/// with, so a caller cannot compare a reason code against the wrong enum.
struct OrderVerdict
{
  bool accepted = false;
  OrderReason reason = OrderReason::Accepted;

  /// Assigned by the authority when it accepts, and zero everywhere else --
  /// including on an accepted pre-check, because nothing has been given an id
  /// until the server has seen it (ADR-004 §7). `ValidateOrder` never sets it;
  /// only `World::SubmitOrder` does.
  std::uint32_t serverOrderId = 0;
};

/*
 * The operating area, in the wire's centimetres.
 *
 * `World::PLAY_AREA_HALF_EXTENT_METRES` is the same bound in the sim's metres.
 * Stated here in centimetres because that is what a leg carries, and converting
 * the leg to metres to compare would put a rounding step inside the one
 * function that must not round differently on the two sides.
 */
inline constexpr std::int32_t PLAY_AREA_HALF_EXTENT_CM = 20000 * 100;

/*
 * How close a fleet must be to dock (ADR-017 §2), in metres.
 *
 * The *base* radius. ADR-018 D7 made the rule footprint-derived --
 * `max(DOCK_RADIUS_METRES, FormationExtentMetres(order) + margin)` -- because
 * the game's own 41-ship starting fleet spans 19.2 km in a Battleship-paced
 * Line and could not satisfy a flat 5 km however it was flown. So this is the
 * floor the scaled radius is never allowed below, and it is the number the
 * bake's warp-in invariant clears: a fleet that warps to a station is inside
 * the dock radius on arrival, whatever its footprint then widens it to.
 *
 * It lives here rather than in the universe model because it is a validation
 * bound: ADR-018 D9 folds it into the schema text, so retuning it is a
 * compatibility event rather than a table edit.
 */
inline constexpr std::int64_t DOCK_RADIUS_METRES = 5000;

/*
 * The radius the fleet actually has to be inside (ADR-018 D7).
 *
 * `max(DOCK_RADIUS_METRES, footprint + margin)`, where the footprint is the
 * extent of the order's own **solved** formation -- the same `SolveFormation`
 * and `FormationExtentMetres` both halves already share, so parity holds by
 * construction rather than by two implementations being kept in step. Exposed
 * rather than hidden inside `ValidateOrder` because the client draws this
 * circle: a perimeter the player is told to reach has to be the perimeter the
 * server judges.
 *
 * The margin is **one largest-class formation spacing**, which is the same unit
 * the parking scan pads its berths by. It buys the fleet the right to sit one
 * ship-spacing off-centre from the structure rather than dead on it, and it
 * scales with the fleet for the same reason the footprint does: a Battleship
 * line and a flight of interceptors do not mean the same thing by "close".
 *
 * Returns the base radius for a solve that places nothing, so an order with no
 * ships is judged against the flat number rather than against zero.
 */
[[nodiscard]] float DockRadiusMetres(const ValidationView& _view, const OrderSubmit& _order) noexcept;

[[nodiscard]] OrderVerdict ValidateOrder(const ValidationView& _view, const OrderSubmit& _order) noexcept;

} // namespace Game
