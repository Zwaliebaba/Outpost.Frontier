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

} // namespace

const char* StationVerbName(StationVerb _verb) noexcept
{
  switch (_verb)
  {
  case StationVerb::Undock:
    return "Undock";
  case StationVerb::AssignWing:
    return "Assign wing";
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

  return OrderVerdict{true, OrderReason::Accepted};
}

} // namespace Game
