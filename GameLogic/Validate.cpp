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

/// Where a ship sits in the view's parallel arrays, or `npos`. One scan the
/// two lookups below share, because a view with marks and a view with hold
/// room index the same way and two searches could disagree about that.
[[nodiscard]] std::size_t FindShipIndex(const ValidationView& _view, ShipId _shipId) noexcept
{
  for (std::size_t index = 0; index < _view.shipIds.size(); ++index)
  {
    if (_view.shipIds[index] == _shipId)
    {
      return index;
    }
  }
  return static_cast<std::size_t>(-1);
}

/// The ship's mark, or null when the view carries none for it.
[[nodiscard]] const ShipMark* FindMark(const ValidationView& _view, ShipId _shipId) noexcept
{
  const std::size_t index = FindShipIndex(_view, _shipId);
  return index < _view.shipMarks.size() ? &_view.shipMarks[index] : nullptr;
}

/*
 * The smallest volume, in litres, of one unit of anything this filter accepts,
 * or zero when the view cannot say.
 *
 * The *smallest* rather than the filtered ore's own, because an `Any` order
 * takes whatever the cluster is richest in and a hold with room for one
 * Astracite unit and not one Ferro-Chroma unit is a hold that can still mine.
 * Refusing it would ground a Miner that has work left.
 */
[[nodiscard]] std::uint32_t SmallestUnitLitres(const ValidationView& _view, OreFilter _filter) noexcept
{
  std::uint32_t smallest = 0;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    const std::uint32_t litres = _view.oreUnitLitres[ore];
    if (litres == 0 || !OreFilterMatches(_filter, static_cast<OreId>(ore)))
    {
      continue;
    }
    if (smallest == 0 || litres < smallest)
    {
      smallest = litres;
    }
  }
  return smallest;
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
  if (!OrderKindHasContent(_order.kind))
  {
    return Refuse(OrderReason::UnknownKind);
  }

  /*
   * Dock and Warp are Replace-only (ADR-017 §2, ADR-016 §8).
   *
   * The queue holds the legs of one movement plan, not a program of verbs.
   * "Fly these waypoints then dock" and "...then warp" are the client feeding
   * one step at a time, which is a different thing from the server running a
   * program -- and the difference matters because a queued verb would have to
   * be re-validated at a moment nobody is watching.
   */
  if (_order.kind != OrderKind::Move && _order.queueMode != QueueMode::Replace)
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

  // And the anchor a warp names has to be one this grid can reach. Beside the
  // station check rather than after the ship checks, because both answer the
  // same question -- is the place you named a place? -- and the sequence should
  // group them.
  if (_order.kind == OrderKind::Warp &&
      std::find(_view.reachableAnchors.begin(), _view.reachableAnchors.end(), _order.anchor) ==
        _view.reachableAnchors.end())
  {
    return Refuse(OrderReason::UnknownAnchor);
  }

  /*
   * And a Mine has to be ordered *at* a field (ADR-024 §4a). Beside the two
   * above it, because it is the same question with a third noun -- "is the
   * place you named a place?" -- and grouping them is what keeps the answers
   * comparable. A Mine names no destination: the field a wing works is the one
   * it is standing in, so this asks about the grid rather than about the order.
   */
  if (_order.kind == OrderKind::Mine && _view.siteAnchor == INVALID_ID)
  {
    return Refuse(OrderReason::NotAtSite);
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
   * Something in the order has to be able to mine, and to have room for what it
   * mines (ADR-024 §4a).
   *
   * After every named ship is resolved and before the positional checks, which
   * is where the check-order contract puts a question about the *selection*: a
   * commander who selected a dead ship is told that first, and one whose wing
   * is simply the wrong wing is told that before being told how far away it is.
   *
   * Mixed groups are legal and intended -- the escorts take formation stations
   * around the worked cluster -- so this refuses only an order with no Miner in
   * it at all, and only one whose every Miner is already full.
   */
  if (_order.kind == OrderKind::Mine)
  {
    bool anyMiner = false;
    for (std::uint16_t index = 0; index < _order.shipCount && !anyMiner; ++index)
    {
      const ShipMark* mark = FindMark(_view, _order.shipIds[index]);
      // A view with ids and no marks cannot prove there is a Miner here, and
      // saying so is the only honest verdict -- the same refusal the dock check
      // makes when it is asked a positional question it has no marks for.
      anyMiner = mark != nullptr && mark->hullClass == HullClass::Miner;
    }
    if (!anyMiner)
    {
      return Refuse(OrderReason::NoMinerInOrder);
    }

    /*
     * The hold check is **skipped by a caller that cannot answer it**, which is
     * the client until E3 replicates cargo.
     *
     * Not a refusal, because a pre-check that bounced every Mine order for want
     * of a number the client is not sent yet would make the feature unusable
     * from the surface that offers it. The authority still applies it, and a
     * client whose order is refused there learns it from the ack -- which is
     * the designed asymmetry, not a hole.
     */
    const std::uint32_t smallest = SmallestUnitLitres(_view, _order.oreFilter);
    if (smallest > 0 && !_view.oreHoldFreeLitres.empty())
    {
      bool anyRoom = false;
      for (std::uint16_t index = 0; index < _order.shipCount && !anyRoom; ++index)
      {
        const std::size_t slot = FindShipIndex(_view, _order.shipIds[index]);
        if (slot >= _view.shipMarks.size() || slot >= _view.oreHoldFreeLitres.size())
        {
          continue;
        }
        anyRoom = _view.shipMarks[slot].hullClass == HullClass::Miner &&
                  _view.oreHoldFreeLitres[slot] >= smallest;
      }
      if (!anyRoom)
      {
        return Refuse(OrderReason::HoldFull);
      }
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

  /*
   * And everyone at the gate, for the one destination that is a jump
   * (ADR-016 §5, U4).
   *
   * The same shape as the dock check immediately above it -- last, in metres,
   * every named ship resolved first -- because it answers the same kind of
   * question and a player who fails both should be told the same one twice.
   * What differs is the radius: `JUMP_RADIUS_METRES` is flat where the dock
   * radius scales with the fleet, and its own comment says why.
   *
   * `jumpAnchor` is `INVALID_ID` on every grid that is not a gate's, so this
   * costs one comparison on the path everything else takes. An in-system warp
   * never reaches it, which is the rule: warp is ordered from where you stand,
   * and only crossing a gate requires standing at one.
   */
  if (_order.kind == OrderKind::Warp && _view.jumpAnchor != INVALID_ID && _order.anchor == _view.jumpAnchor)
  {
    constexpr auto RADIUS = static_cast<float>(JUMP_RADIUS_METRES);
    const float gateX = static_cast<float>(_view.gateXCm) * 0.01f;
    const float gateY = static_cast<float>(_view.gateYCm) * 0.01f;
    for (std::uint16_t index = 0; index < _order.shipCount; ++index)
    {
      const ShipMark* mark = FindMark(_view, _order.shipIds[index]);
      if (mark == nullptr)
      {
        // A view with ids and no marks cannot answer a positional question, and
        // saying so is the only honest verdict -- the same refusal the dock
        // check makes, for the same reason.
        return Refuse(OrderReason::NotAtGate);
      }
      const float dx = static_cast<float>(mark->xCm) * 0.01f - gateX;
      const float dy = static_cast<float>(mark->yCm) * 0.01f - gateY;
      if (dx * dx + dy * dy > RADIUS * RADIUS)
      {
        return Refuse(OrderReason::NotAtGate);
      }
    }
  }

  /*
   * `NotOwned` is not refused HERE, and that is the rule rather than an omission
   * (ADR-018 D2, U3c-b).
   *
   * It used to say it was unreachable -- one player, every ship theirs -- and
   * that ownership was a field rather than a redesign. The field exists now and
   * the refusal is real, but it cannot live in this function: a world knows only
   * its own ships and must never learn who owns one, because a player identity
   * inside the deterministic SoA would put accounts in the replay domain.
   *
   * So the composition root checks it, where the registry's owner index and the
   * wire are both visible, and the refusal travels back through the engine as a
   * number it does not read (ADR-014 §3). A validator that also checked would be
   * a second place to keep the rule, and the two would eventually disagree.
   */

  return OrderVerdict{true, OrderReason::Accepted};
}

} // namespace Game
