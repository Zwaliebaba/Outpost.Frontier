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

const char* StanceName(StanceId _stance) noexcept
{
  switch (_stance)
  {
  case StanceId::Balanced:
    return "Balanced";
  case StanceId::Aggressive:
    return "Aggressive";
  case StanceId::Evasive:
    return "Evasive";
  }
  // As with the formations: no `default` label, so a new stance fails the
  // exhaustiveness warning here before it reaches a player.
  return "unnamed stance";
}

const char* OrderKindName(OrderKind _kind) noexcept
{
  switch (_kind)
  {
  case OrderKind::Move:
    return "Move";
  case OrderKind::Attack:
    return "Attack";
  case OrderKind::Stance:
    return "Stance";
  case OrderKind::Abilities:
    return "Abilities";
  }
  // Not a `default` label: a fifth kind should fail the switch's exhaustiveness
  // warning here rather than appear on a button as "unnamed order".
  return "unnamed order";
}

bool OrderKindHasContent(OrderKind _kind) noexcept
{
  // Only Move is simulated. `ValidateOrder` enforces the same thing from the
  // other side, and the two agreeing is not a coincidence worth relying on --
  // this answers "may a surface offer it", that answers "may the world act on
  // it", and a kind gaining content has to change both.
  return _kind == OrderKind::Move;
}

const char* OrderKindParameterName(OrderKind _kind) noexcept
{
  switch (_kind)
  {
  case OrderKind::Move:
    return "Formation";
  case OrderKind::Stance:
    return "Stance";
  case OrderKind::Abilities:
    return "Ability";
  case OrderKind::Attack:
    // Attack takes a target, not a parameter. The distinction is the whole
    // reason this returns null rather than an empty string: a button labelled
    // with nothing is still a button.
    return nullptr;
  }
  return nullptr;
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
