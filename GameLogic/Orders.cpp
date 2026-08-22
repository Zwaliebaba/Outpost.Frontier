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
  case OrderKind::Mine:
    return "Mine";
  }
  // Not a `default` label: an eighth kind should fail the switch's exhaustiveness
  // warning here rather than appear on a button as "unnamed order".
  return "unnamed order";
}

const char* OrderWorkingName(OrderKind _kind) noexcept
{
  /*
   * Written as the one exception rather than as a full switch, and the default
   * is the safe direction: a kind with no answer here reads as a journey, which
   * every kind but Mine is. A wrong null costs a label; a wrong word would put
   * a fleet's state on screen for an order that had simply finished.
   *
   * `Mine` is the exception because §4b's cycle is the order: the group arrives,
   * its leg is spent, and the work is what it does for the next several hours
   * (ADR-024 §4a). `LegEtaSeconds` answers for exactly the same case, and the
   * two agreeing is not an accident -- one supplies the number this supplies
   * the word for.
   */
  return _kind == OrderKind::Mine ? "Mining" : nullptr;
}

bool OrderKindHasContent(OrderKind _kind) noexcept
{
  // Move, Warp, Dock and Mine are simulated; the other three are numbered and
  // inert.
  // `ValidateOrder` enforces the same thing from the other side, and the two
  // agreeing is not a coincidence worth relying on -- this answers "may a
  // surface offer it", that answers "may the world act on it", and a kind
  // gaining content has to change both.
  return _kind == OrderKind::Move || _kind == OrderKind::Warp || _kind == OrderKind::Dock ||
         _kind == OrderKind::Mine;
}

bool OrderKindNamesDestination(OrderKind _kind) noexcept
{
  /*
   * Written as the exceptions rather than as the rule, so a kind added without
   * an answer defaults to needing a gesture -- the conservative direction. A
   * new verb wrongly listed here would fire on a press with no target; one
   * wrongly left out only costs a gesture that adds nothing.
   *
   * `Dock` is the correction. It reads like a targeted verb because a player
   * points at a station to issue one, but the *validator* never consults where
   * they pointed: `ValidateOrder` asks whether the named anchor is this grid's,
   * and the grid has exactly one. What the gesture supplied was the anchor,
   * which the grid already knows.
   */
  return _kind != OrderKind::Mine && _kind != OrderKind::Dock;
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

  /*
   * And Mine varies by **ore** (ADR-024 §4a) -- the first kind whose parameter
   * is not a formation, which is why this function exists as a lookup rather
   * than as the constant it could have been for five slices.
   *
   * A Mine order is still flown in a formation and `OrderSubmit` still carries
   * one, because the escorts in a mixed order take stations around the worked
   * cluster. What the command surface *offers a choice of* is the ore, which is
   * the decision the player is actually making at a field.
   */
  case OrderKind::Mine:
    return "Ore Filter";
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
  case OrderReason::NotAtGate:
    return "not at the gate";
  case OrderReason::NotAtSite:
    return "no mining field here";
  case OrderReason::NoMinerInOrder:
    return "no miners in this order";
  case OrderReason::HoldFull:
    return "ore holds are full";
  case OrderReason::InsufficientMaterials:
    return "not enough materials";
  case OrderReason::RefineryBusy:
    return "the refinery is busy";
  case OrderReason::RecipeLocked:
    return "this station cannot make that";
  }
  // Not a default label: a new enumerator should fail the switch's exhaustive
  // warning first, and only reach here if it crossed the wire from a build that
  // has it and this one does not.
  return "unrecognised reason";
}

} // namespace Game
