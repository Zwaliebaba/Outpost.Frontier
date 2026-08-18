#include "pch.h"

#include "Orders.h"

namespace Game
{

const char* FormationName(FormationId _formation) noexcept
{
  switch (_formation)
  {
  case FormationId::Line:
    return "Line";
  case FormationId::Wedge:
    return "Wedge";
  case FormationId::Claw:
    return "Claw";
  }
  // As with the reason text: not a `default` label, so a new formation fails
  // the switch's exhaustiveness warning here before it reaches a player.
  return "unnamed formation";
}

const char* OrderReasonText(OrderReason _reason) noexcept
{
  switch (_reason)
  {
  case OrderReason::Accepted:
    return "accepted";
  case OrderReason::EmptySelection:
    return "nothing selected";
  case OrderReason::NotOwned:
    return "not yours";
  case OrderReason::UnknownShip:
    return "no such ship";
  case OrderReason::QueueFull:
    return "order queue full";
  case OrderReason::OutOfBounds:
    return "outside the operating area";
  case OrderReason::InvalidFormation:
    return "unknown formation";
  case OrderReason::TooManyShips:
    return "too many ships in one order";
  case OrderReason::UnknownKind:
    return "unknown order";
  }
  // Not a default label: a new enumerator should fail the switch's exhaustive
  // warning first, and only reach here if it crossed the wire from a build that
  // has it and this one does not.
  return "unrecognised reason";
}

} // namespace Game
