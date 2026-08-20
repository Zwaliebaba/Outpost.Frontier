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
  case OrderKind::Warp:
    return "Warp";
  case OrderKind::Dock:
    return "Dock";
  }
  // Not a `default` label: a seventh kind should fail the switch's exhaustiveness
  // warning here rather than appear on a button as "unnamed order".
  return "unnamed order";
}

bool OrderKindHasContent(OrderKind _kind) noexcept
{
  // Move, Warp and Dock are simulated; the other three are numbered and inert.
  // `ValidateOrder` enforces the same thing from the other side, and the two
  // agreeing is not a coincidence worth relying on -- this answers "may a
  // surface offer it", that answers "may the world act on it", and a kind
  // gaining content has to change both.
  return _kind == OrderKind::Move || _kind == OrderKind::Warp || _kind == OrderKind::Dock;
}

const char* OrderKindParameterName(OrderKind _kind) noexcept
{
  switch (_kind)
  {
  // Dock varies by formation exactly as Move does, and not decoratively: the
  // radius it is judged against is derived from the *solved* formation
  // (ADR-018 D7), so the dropdown changes how close the fleet has to be.
  case OrderKind::Move:
  case OrderKind::Dock:
    return "Formation";
  case OrderKind::Stance:
    return "Stance";
  case OrderKind::Abilities:
    return "Ability";

  // Attack takes a *target* rather than a parameter, and null rather than an
  // empty string says so: a button labelled with nothing is still a button.
  case OrderKind::Attack:
    return nullptr;
  case OrderKind::Warp:
    // Warp varies by formation too: the fleet arrives in one, solved at the
    // anchor's authored warp-in point.
    return "Formation";
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
  case OrderReason::UnknownStation:
    return "no such station here";
  case OrderReason::NotAtStation:
    return "too far from the station";
  case OrderReason::NotDocked:
    return "not docked here";
  case OrderReason::InvalidQueueMode:
    return "cannot be queued";
  case OrderReason::CombatEngaged:
    return "in combat";
  case OrderReason::UnknownAnchor:
    return "no route to there";
  case OrderReason::NoPresence:
    return "nothing of yours is there";
  }
  // Not a default label: a new enumerator should fail the switch's exhaustive
  // warning first, and only reach here if it crossed the wire from a build that
  // has it and this one does not.
  return "unrecognised reason";
}

} // namespace Game
