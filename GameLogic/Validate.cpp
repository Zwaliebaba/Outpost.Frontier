#include "pch.h"

#include "Validate.h"

#include "Formation.h"

#include <algorithm>
#include <cmath>

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
  // All three are solved as of S10. The check stays rather than becoming a
  // tautology: `formation` arrives as a byte off the wire, so a value outside
  // the enum is reachable from any client and has to be refused rather than
  // switched on. `SolveFormation` treats an unknown one as a Line, which is a
  // safe default only because nothing unknown gets that far.
  switch (_formation)
  {
  case FormationId::Line:
  case FormationId::Wedge:
  case FormationId::Claw:
    return true;
  }
  return false;
}

/*
 * `SolveFormation`'s class lookup, over the view's marks.
 *
 * A linear scan rather than an index: the order names at most 64 ships and the
 * view holds at most the grid's cap, so this is bounded by a number the tick
 * already pays elsewhere, and a sorted-index assumption would be a promise the
 * client's replicated view does not make.
 */
struct MarkLookup
{
  const ValidationView* view = nullptr;

  [[nodiscard]] static HullClass Of(ShipId _shipId, void* _context) noexcept
  {
    const auto* lookup = static_cast<const MarkLookup*>(_context);
    const std::span<const ShipId>& ids = lookup->view->shipIds;
    for (std::size_t index = 0; index < ids.size() && index < lookup->view->shipMarks.size(); ++index)
    {
      if (ids[index] == _shipId)
      {
        return lookup->view->shipMarks[index].hullClass;
      }
    }
    // Unknown to the view. `UnknownShip` is what actually refuses this order;
    // returning the smallest class keeps the solve from inventing a footprint
    // out of a ship nobody has.
    return HullClass::Interceptor;
  }
};

/// The ship's mark, or null when the view carries none for it.
[[nodiscard]] const ShipMark* FindMark(const ValidationView& _view, ShipId _shipId) noexcept
{
  for (std::size_t index = 0; index < _view.shipIds.size() && index < _view.shipMarks.size(); ++index)
  {
    if (_view.shipIds[index] == _shipId)
    {
      return &_view.shipMarks[index];
    }
  }
  return nullptr;
}

} // namespace

float DockRadiusMetres(const ValidationView& _view, const OrderSubmit& _order) noexcept
{
  const auto base = static_cast<float>(DOCK_RADIUS_METRES);
  if (_order.shipCount == 0 || _order.shipCount > MAX_SHIPS_PER_ORDER)
  {
    return base;
  }

  FormationStation stations[MAX_SHIPS_PER_ORDER];
  MarkLookup lookup{&_view};
  const std::span<const ShipId> ships{_order.shipIds, _order.shipCount};
  const std::uint32_t placed =
    SolveFormation(_order.formation, ships, &MarkLookup::Of, &lookup, DirectX::XMFLOAT2{0.0f, 0.0f}, 0.0f,
                   std::span<FormationStation>{stations});
  if (placed == 0)
  {
    return base;
  }

  // Solved about the origin, so the extent is measured about the origin. The
  // arrangement is translation-invariant -- this is a size, not a place.
  const float footprint =
    FormationExtentMetres(std::span<const FormationStation>{stations, placed}, DirectX::XMFLOAT2{0.0f, 0.0f});

  // One largest-class spacing of slack, the same unit the berth scan pads by.
  float margin = 0.0f;
  for (std::uint32_t index = 0; index < placed; ++index)
  {
    margin = std::max(margin, ShipClass(MarkLookup::Of(stations[index].shipId, &lookup)).formationSpacingMetres);
  }

  return std::max(base, footprint + margin);
}

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
  if (_order.kind != OrderKind::Move && _order.kind != OrderKind::Dock)
  {
    return Refuse(OrderReason::UnknownKind);
  }

  // Dock is Replace-only (ADR-017 §2). The queue holds the legs of one movement
  // plan, not a program of verbs: "fly these waypoints then dock" is the
  // client feeding one step at a time, not a program the server runs.
  if (_order.kind == OrderKind::Dock && _order.queueMode != QueueMode::Replace)
  {
    return Refuse(OrderReason::InvalidQueueMode);
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

  // The station the order names has to be this grid's. Before the position
  // checks, because "there is no such station here" is a better answer than
  // "you are too far from it" when both are true.
  if (_order.kind == OrderKind::Dock &&
      (_view.stationAnchor == INVALID_ID || _order.anchor != _view.stationAnchor))
  {
    return Refuse(OrderReason::UnknownStation);
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

  /*
   * Everyone inside the radius (ADR-017 §2, ADR-018 D7). Last, because it is
   * the only check that needs every named ship resolved *and* a solve run, and
   * because the check-order contract puts position checks after ship
   * resolution: a selection holding a dead ship says `UnknownShip`, not "too
   * far", whichever is also true.
   *
   * Compared in **metres, not centimetres**, and the reason is arithmetic
   * rather than taste: the grid's half-extent is 20 km, so a centimetre
   * coordinate reaches 2,000,000 and its square 4e12, well past the range a
   * `float` counts exactly. In metres the same numbers are 20,000 and 4e8,
   * which it does. The quantisation the parity rule cares about already
   * happened on the way in -- both halves are dividing the same integers by
   * the same constant, so they land on the same float.
   */
  if (_order.kind == OrderKind::Dock)
  {
    const float radius = DockRadiusMetres(_view, _order);
    const float stationX = static_cast<float>(_view.stationXCm) * 0.01f;
    const float stationY = static_cast<float>(_view.stationYCm) * 0.01f;
    for (std::uint16_t index = 0; index < _order.shipCount; ++index)
    {
      const ShipMark* mark = FindMark(_view, _order.shipIds[index]);
      if (mark == nullptr)
      {
        // The view has the id but no mark for it, which means the caller built
        // a view without marks and asked a question that needs them. Refusing
        // is the only honest answer: accepting would let a fleet dock from
        // anywhere on a technicality of how the view was assembled.
        return Refuse(OrderReason::NotAtStation);
      }
      const float dx = static_cast<float>(mark->xCm) * 0.01f - stationX;
      const float dy = static_cast<float>(mark->yCm) * 0.01f - stationY;
      if (dx * dx + dy * dy > radius * radius)
      {
        return Refuse(OrderReason::NotAtStation);
      }
    }
  }

  // NotOwned is unreachable in the MVP: there is one player and every ship is
  // theirs. The code exists because ownership is a field, not a redesign, and a
  // reason enum that renumbers when multiplayer lands would renumber the wire.

  return OrderVerdict{true, OrderReason::Accepted};
}

} // namespace Game
