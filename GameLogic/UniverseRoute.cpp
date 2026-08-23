#include "pch.h"

#include "UniverseRoute.h"

#include <algorithm>
#include <deque>

namespace Game
{
namespace
{

/// ASCII lowering, deliberately not `std::tolower`: that one is locale-dependent
/// and the universe's names are content, not text in the player's language.
[[nodiscard]] char Lower(char _c) noexcept
{
  return _c >= 'A' && _c <= 'Z' ? static_cast<char>(_c - 'A' + 'a') : _c;
}

[[nodiscard]] bool ContainsFolded(std::string_view _haystack, std::string_view _needle) noexcept
{
  if (_needle.empty())
  {
    return true;
  }
  if (_needle.size() > _haystack.size())
  {
    return false;
  }
  for (std::size_t start = 0; start + _needle.size() <= _haystack.size(); ++start)
  {
    std::size_t index = 0;
    while (index < _needle.size() && Lower(_haystack[start + index]) == Lower(_needle[index]))
    {
      ++index;
    }
    if (index == _needle.size())
    {
      return true;
    }
  }
  return false;
}

} // namespace

Route SolveRoute(const UniverseDef& _universe, SystemId _from, SystemId _to)
{
  Route route;
  if (_universe.FindSystem(_from) == nullptr || _universe.FindSystem(_to) == nullptr)
  {
    return route;
  }
  if (_from == _to)
  {
    route.systems.push_back(_from);
    return route; // Already there: a route of one system and no jumps.
  }

  /*
   * Breadth-first from the origin, recording where each system was reached
   * from, then walked back. Flat vectors indexed by `SystemId` rather than a
   * map: ids are dense and authored from a table, so this is O(1) per lookup
   * with no iteration order to get wrong (ADR-005 §5's rule about
   * pointer-keyed containers applies to anything whose order is an accident).
   */
  /*
   * Sized by the **largest id**, not by the count.
   *
   * The bake numbers systems densely from one, so the two are the same today --
   * but a hand-authored universe may number sparsely, and a table sized by
   * count would then index out of range on a perfectly legal file. Sizing by
   * the id it has to hold costs a pass over a vector and removes the
   * assumption.
   */
  std::size_t largest = 0;
  for (const SolarSystem& system : _universe.systems)
  {
    largest = std::max<std::size_t>(largest, system.id);
  }
  largest = std::max<std::size_t>(largest, std::max<std::size_t>(_from, _to));

  std::vector<SystemId> cameFrom(largest + 1, INVALID_ID);
  std::vector<bool> seen(largest + 1, false);

  const auto indexOf = [](SystemId _id) -> std::size_t { return static_cast<std::size_t>(_id); };

  std::deque<SystemId> frontier;
  frontier.push_back(_from);
  seen[indexOf(_from)] = true;

  while (!frontier.empty())
  {
    const SystemId here = frontier.front();
    frontier.pop_front();
    if (here == _to)
    {
      break;
    }

    const SolarSystem* system = _universe.FindSystem(here);
    if (system == nullptr)
    {
      continue;
    }

    /*
     * Neighbours in ascending system id, which is what makes two equally short
     * routes the same route everywhere. The gate table's own order is the
     * bake's, and depending on it would make the drawn line a function of how
     * the generator happened to emit gates.
     */
    std::vector<SystemId> neighbours;
    neighbours.reserve(system->gates.size());
    for (const Gate& gate : system->gates)
    {
      neighbours.push_back(gate.toSystem);
    }
    std::sort(neighbours.begin(), neighbours.end());

    for (const SystemId next : neighbours)
    {
      const std::size_t index = indexOf(next);
      if (index >= seen.size() || seen[index])
      {
        continue;
      }
      seen[index] = true;
      cameFrom[index] = here;
      frontier.push_back(next);
    }
  }

  if (!seen[indexOf(_to)])
  {
    return route; // Nothing reaches it. An empty route is the honest answer.
  }

  for (SystemId step = _to; step != INVALID_ID; step = cameFrom[indexOf(step)])
  {
    route.systems.push_back(step);
    if (step == _from)
    {
      break;
    }
  }
  std::reverse(route.systems.begin(), route.systems.end());
  return route;
}

std::vector<SystemId> FindSystems(const UniverseDef& _universe, std::string_view _query, std::size_t _limit)
{
  std::vector<SystemId> found;
  if (_query.empty())
  {
    return found; // An empty box is not a search for everything.
  }

  for (const SolarSystem& system : _universe.systems)
  {
    if (found.size() >= _limit)
    {
      break;
    }
    if (ContainsFolded(system.name, _query))
    {
      found.push_back(system.id);
    }
  }

  // Walked in table order, which is id order, so the list is already sorted --
  // stated rather than sorted again, because a sort here would hide the day the
  // table stops being ordered.
  return found;
}

std::vector<AnchorId> ReachableAnchors(const UniverseDef& _universe, AnchorId _from)
{
  std::vector<AnchorId> reachable;
  const Anchor* here = _universe.FindAnchor(_from);
  if (here == nullptr)
  {
    return reachable;
  }
  const SolarSystem* system = _universe.FindSystem(here->system);
  if (system == nullptr)
  {
    return reachable;
  }

  reachable.reserve(system->anchors.size() + 1);
  for (const Anchor& anchor : system->anchors)
  {
    if (anchor.id != _from)
    {
      reachable.push_back(anchor.id);
    }
  }

  /*
   * And the far side of the gate, if this grid is one.
   *
   * **One id, appended to the same list**, so that `UnknownAnchor` keeps meaning
   * exactly "not from here" and the validator needs no second question to decide
   * whether a destination exists. Which of these is a *jump* is said separately,
   * by `ValidationView::jumpAnchor`, because that changes how the order is
   * judged and not whether the place can be named.
   *
   * One hop and no further: routing across several systems is the client
   * feeding one order per completed hop (ADR-016 §8), and this list is
   * deliberately not the beginning of a server-side planner.
   */
  if (here->kind == AnchorKind::Gate)
  {
    const AnchorId paired = _universe.PairedGateAnchor(_from);
    if (paired != INVALID_ID)
    {
      reachable.push_back(paired);
    }
  }
  return reachable;
}

} // namespace Game
