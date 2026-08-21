#include "pch.h"

#include "Station.h"

#include <algorithm>

namespace Game
{
namespace
{

[[nodiscard]] OrderVerdict Refuse(OrderReason _reason) noexcept
{
  return OrderVerdict{false, _reason};
}

/// Same three shapes the order side knows, asked the same way: `formation`
/// arrives as a byte off the wire, so a value outside the enum is reachable
/// from any client and has to be refused rather than switched on.
[[nodiscard]] bool KnownFormation(FormationId _formation) noexcept
{
  switch (_formation)
  {
  case FormationId::Line:
  case FormationId::Wedge:
  case FormationId::Claw:
    return true;
  }
  return false;
}

/// How much of one ore a selection of docked ships is holding, saturating
/// rather than wrapping: 64 ships cannot overflow a u32 with today's holds, and
/// a total that silently wrapped would turn a refusal into an acceptance.
[[nodiscard]] std::uint32_t HeldBySelection(const RosterView& _view, const StationCommand& _command, OreId _ore) noexcept
{
  std::uint64_t total = 0;
  for (std::uint16_t index = 0; index < _command.shipCount; ++index)
  {
    const ShipId shipId = _command.shipIds[index];
    for (const RosterEntry& row : _view.docked)
    {
      if (row.shipId == shipId)
      {
        total += row.Units(_ore);
        break;
      }
    }
  }
  return total > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(total);
}

/// Is this one of the two verbs that moves ore? Asked in three places, so it is
/// a function rather than a repeated pair of comparisons.
[[nodiscard]] bool IsTransferVerb(StationVerb _verb) noexcept
{
  return _verb == StationVerb::TransferToBay || _verb == StationVerb::TransferToShip;
}

} // namespace

std::uint32_t StationBay::TotalUnits() const noexcept
{
  std::uint64_t total = 0;
  for (const std::uint32_t units : oreUnits)
  {
    total += units;
  }
  return total > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(total);
}

const char* StationVerbName(StationVerb _verb) noexcept
{
  switch (_verb)
  {
  case StationVerb::Undock:
    return "Undock";
  case StationVerb::AssignWing:
    return "Assign wing";
  case StationVerb::TransferToBay:
    return "Transfer to bay";
  case StationVerb::TransferToShip:
    return "Transfer to ship";
  }
  // Not a `default` label: a third verb should fail the switch's exhaustiveness
  // warning here rather than appear on a button with no word on it.
  return "unnamed station command";
}

OrderVerdict ValidateStationCommand(const RosterView& _view, const StationCommand& _command) noexcept
{
  if (_command.shipCount == 0)
  {
    return Refuse(OrderReason::EmptySelection);
  }
  if (_command.shipCount > MAX_SHIPS_PER_ORDER)
  {
    // A fuller hangar undocks in waves (ADR-017 §3). Refused rather than
    // truncated, because half a selection is not the fleet the player composed.
    return Refuse(OrderReason::TooManyShips);
  }

  // The formation is Undock's parameter and nothing else's, so it is only
  // checked where it means something -- an AssignWing carrying a garbage
  // formation byte is not a malformed command, it is a field nobody reads.
  if (_command.verb == StationVerb::Undock && !KnownFormation(_command.formation))
  {
    return Refuse(OrderReason::InvalidFormation);
  }

  if (_view.station == INVALID_ID || _command.station != _view.station)
  {
    return Refuse(OrderReason::UnknownStation);
  }

  for (std::uint16_t index = 0; index < _command.shipCount; ++index)
  {
    const ShipId shipId = _command.shipIds[index];
    const bool docked = std::any_of(_view.docked.begin(), _view.docked.end(),
                                    [shipId](const RosterEntry& _row) { return _row.shipId == shipId; });
    if (!docked)
    {
      /*
       * Not on *this* station's roster. On the client that is a stale hangar
       * screen; on the server it is a ship somebody already undocked. Same
       * reason either way, which is the point -- and it is also the reason
       * `AssignWing` is docked-scope for now (ADR-017 §6): the hangar is the
       * reorganisation room, and in-space reassignment can arrive later
       * without new machinery.
       */
      return Refuse(OrderReason::NotDocked);
    }
  }

  /*
   * And last, the quantity (ADR-024 §5c).
   *
   * After the ships resolve, because a shortfall is only meaningful once you
   * know which holds are being counted -- see the check-order note in the
   * header. Zero units is refused here rather than earlier for the same reason:
   * "you asked to move nothing" and "you asked to move more than there is" are
   * the same complaint about the same field, and giving them one reason keeps
   * the enum honest about what it is describing.
   *
   * The two directions ask the same question of different sources. Into the
   * Bay, the selection's holds have to cover it; out of it, the Bay does. The
   * *destination* is not checked at all, and that is a decision rather than an
   * omission: a Bay has no capacity (ADR-024 §5b), and a hold that cannot take
   * the whole amount is E4's problem to shape, once a refinery exists to make
   * partial fills mean something. Until then a `TransferToShip` past the
   * selection's room is refused by the apply path filling what it can and
   * leaving the rest in the Bay -- which is the honest outcome, because nothing
   * is lost either way.
   */
  if (IsTransferVerb(_command.verb))
  {
    const std::uint32_t available = _command.verb == StationVerb::TransferToBay
                                      ? HeldBySelection(_view, _command, _command.ore)
                                      : (_view.bayUnits.size() > static_cast<std::size_t>(_command.ore)
                                           ? _view.bayUnits[static_cast<std::size_t>(_command.ore)]
                                           : 0u);
    if (_command.units == 0 || _command.units > available)
    {
      return Refuse(OrderReason::InsufficientMaterials);
    }
  }

  return OrderVerdict{true, OrderReason::Accepted};
}

} // namespace Game
