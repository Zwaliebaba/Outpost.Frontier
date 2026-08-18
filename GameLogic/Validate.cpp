#include "pch.h"

#include "Validate.h"

#include <algorithm>

namespace Game
{
namespace
{

[[nodiscard]] OrderVerdict Refuse(OrderReason _reason) noexcept
{
  return OrderVerdict{false, _reason};
}

[[nodiscard]] bool KnownFormation(FormationId _formation) noexcept
{
  // Line only until S10 solves the other two. Naming them in the enum and
  // refusing them here is the honest arrangement: a client that sends Wedge at
  // this build gets `InvalidFormation` rather than a Line it did not ask for.
  return _formation == FormationId::Line;
}

} // namespace

OrderVerdict ValidateOrder(const ValidationView& _view, const OrderSubmit& _order) noexcept
{
  // Ordered cheapest-first, and the order is part of the contract: two builds
  // that check the same things in a different sequence would return different
  // reasons for an order that fails two of them, and the reason is what the
  // player reads. An empty selection with a bad formation says EmptySelection.
  if (_order.shipCount == 0)
  {
    return Refuse(OrderReason::EmptySelection);
  }
  if (_order.shipCount > MAX_SHIPS_PER_ORDER)
  {
    return Refuse(OrderReason::TooManyShips);
  }
  if (_order.kind != OrderKind::Move)
  {
    return Refuse(OrderReason::UnknownKind);
  }
  if (!KnownFormation(_order.formation))
  {
    return Refuse(OrderReason::InvalidFormation);
  }

  // Appending past the leg cap is refused rather than silently dropping the
  // oldest leg: ADR-004 §7 leans on this being cheap and honest, because the
  // strategic map feeds one jump at a time and needs to know when it is full.
  if (_order.queueMode == QueueMode::Append && _view.queuedLegs >= MAX_ORDER_LEGS)
  {
    return Refuse(OrderReason::QueueFull);
  }

  // Centimetres against a centimetre bound. Converting either side to metres
  // here would put a rounding step inside the function whose whole job is to
  // round the same way on both sides of the wire (ADR-005 §4).
  if (std::abs(_order.target.xCm) > PLAY_AREA_HALF_EXTENT_CM || std::abs(_order.target.yCm) > PLAY_AREA_HALF_EXTENT_CM)
  {
    return Refuse(OrderReason::OutOfBounds);
  }

  for (std::uint16_t index = 0; index < _order.shipCount; ++index)
  {
    const ShipId shipId = _order.shipIds[index];
    if (std::find(_view.shipIds.begin(), _view.shipIds.end(), shipId) == _view.shipIds.end())
    {
      // A ship the view does not have. On the client this is a stale selection;
      // on the server it is a ship that died between the click and the
      // datagram. Same reason code either way, which is the point.
      return Refuse(OrderReason::UnknownShip);
    }
  }

  // NotOwned is unreachable in the MVP: there is one player and every ship is
  // theirs. The code exists because ownership is a field, not a redesign, and a
  // reason enum that renumbers when multiplayer lands would renumber the wire.

  return OrderVerdict{true, OrderReason::Accepted};
}

} // namespace Game
