#pragma once

#include "Orders.h"

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
 * The half of the world validation needs, from either side of the wire.
 *
 * Ids only, sorted or not. Positions are not here because nothing validation
 * decides depends on where a ship *is* -- only on whether it exists and where
 * it is being sent. When ownership arrives (multiplayer, post-MVP) this is
 * where the owning player id joins it.
 */
struct ValidationView
{
  std::span<const ShipId> shipIds;

  /// How many legs the group these ships belong to already holds. Zero for a
  /// replace, and what makes `QueueFull` checkable without the group table.
  std::uint32_t queuedLegs = 0;
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

[[nodiscard]] OrderVerdict ValidateOrder(const ValidationView& _view, const OrderSubmit& _order) noexcept;

} // namespace Game
