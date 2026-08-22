#include "pch.h"

#include "Relevance.h"

#include "ShipClass.h"
#include "World.h"
#include "WorldRegistry.h"

#include <algorithm>

using DirectX::XMFLOAT2;

namespace Game
{

namespace
{

/// Which tier a ship landed in. Ordered so the tier number *is* the sort key --
/// ADR-022 §4's table reads top to bottom and so does this.
enum class Tier : std::uint8_t
{
  Owned = 0,
  InView = 1,
  Elsewhere = 2
};

/*
 * A structure is a landmark, and landmarks are tier 0 (ADR-022 §4).
 *
 * Both immovable classes, not just `Structure`: a gate is the other thing on a
 * grid that a player navigates *by*, and one that blinked out of interest while
 * a route was being flown would be a waypoint that stopped existing.
 */
[[nodiscard]] bool IsLandmark(std::uint8_t _class) noexcept
{
  const auto hull = static_cast<HullClass>(_class);
  return hull == HullClass::Structure || hull == HullClass::Gate;
}

/*
 * One ship's place in the order.
 *
 * `distanceSq` and not a distance: the ranking only ever compares them, and a
 * square root per ship per viewer per tick would be a thousand of them for an
 * answer that sorts identically either way.
 */
struct Ranked
{
  Tier tier = Tier::Elsewhere;
  float distanceSq = 0.0f;
  ShipId shipId = INVALID_SHIP_ID;
};

/*
 * The total order (ADR-022 §4, and the accept's first clause).
 *
 * Tier, then nearest to the camera's focus, then the ship id. The id is not a
 * tie-break of convenience: without it two ships at the same distance would
 * order by whatever `std::sort` did with them, and a ranking that is not a
 * function of its query is a ranking no test can pin down.
 */
[[nodiscard]] bool RanksBefore(const Ranked& _lhs, const Ranked& _rhs) noexcept
{
  if (_lhs.tier != _rhs.tier)
  {
    return _lhs.tier < _rhs.tier;
  }
  if (_lhs.distanceSq != _rhs.distanceSq)
  {
    return _lhs.distanceSq < _rhs.distanceSq;
  }
  return _lhs.shipId < _rhs.shipId;
}

} // namespace

Relationship RelationshipOf(const WorldRegistry& _registry, Neuron::PlayerId _viewer, ShipId _shipId) noexcept
{
  if (_viewer == Neuron::INVALID_PLAYER_ID)
  {
    return Relationship::Neutral; // Nobody's viewer owns nothing.
  }
  return _registry.OwnerOf(_shipId) == _viewer ? Relationship::Own : Relationship::Neutral;
}

std::uint32_t RankRelevance(const WorldRegistry& _registry, const World& _world, const RelevanceQuery& _query,
                            std::vector<ShipId>& _outRanked)
{
  const std::span<const ShipId> ids = _world.Ids();
  const std::span<const std::uint8_t> classes = _world.Classes();
  const std::span<const XMFLOAT2> positions = _world.Positions();

  _outRanked.clear();
  if (ids.empty())
  {
    return 0;
  }

  std::vector<Ranked> ranked;
  ranked.reserve(ids.size());

  for (std::size_t slot = 0; slot < ids.size(); ++slot)
  {
    const ShipId shipId = ids[slot];
    const float deltaX = positions[slot].x - _query.focusXMetres;
    const float deltaY = positions[slot].y - _query.focusYMetres;

    Ranked entry;
    entry.shipId = shipId;
    entry.distanceSq = deltaX * deltaX + deltaY * deltaY;

    /*
     * Tier 0's three clauses, in the order they are cheapest to answer.
     *
     * `selection` is a linear scan rather than a set, and deliberately: it is
     * bounded by what one player can select and the alternative is building a
     * container per viewer per tick to avoid walking a handful of ids.
     */
    const bool selected = std::find(_query.selection.begin(), _query.selection.end(), shipId) != _query.selection.end();
    if (selected || IsLandmark(classes[slot]) || RelationshipOf(_registry, _query.viewer, shipId) == Relationship::Own)
    {
      entry.tier = Tier::Owned;
    }
    else
    {
      /*
       * Inside the camera, as a square rather than a circle.
       *
       * A half-extent describes a box, and the box is what the player can
       * actually see; a radius would rank the corners of their own screen
       * below empty space beside it. The comparison is on the axes rather than
       * on `distanceSq` for exactly that reason.
       */
      const bool inView = std::max(std::abs(deltaX), std::abs(deltaY)) <= _query.viewHalfExtentMetres;
      entry.tier = inView ? Tier::InView : Tier::Elsewhere;
    }

    ranked.push_back(entry);
  }

  std::sort(ranked.begin(), ranked.end(), RanksBefore);

  /*
   * Tier 2's round-robin (ADR-022 §4).
   *
   * Nearest-first alone is a *permanent* ranking: the far tail of a busy grid
   * would sit below the budget line every tick and never update at all, which
   * is the "rather than never" the table is written against. Rotating the tier
   * by the tick turns that ordering into a **cadence** -- the relative order is
   * preserved cyclically, so near ships still lead most of the time, and every
   * ship reaches the front of the tier within `count` ticks and is therefore
   * sent within a bounded number of them.
   *
   * The rotation is by `tick % count` rather than by a stride, because a stride
   * only visits everything when it is coprime with the count, and the count
   * changes whenever a ship spawns. A bound that depends on an arithmetic
   * coincidence is not a bound.
   */
  const auto firstTierTwo = std::find_if(ranked.begin(), ranked.end(),
                                         [](const Ranked& _entry) noexcept { return _entry.tier == Tier::Elsewhere; });
  const auto tierTwoCount = static_cast<std::size_t>(std::distance(firstTierTwo, ranked.end()));
  if (tierTwoCount > 1)
  {
    std::rotate(firstTierTwo, firstTierTwo + static_cast<std::ptrdiff_t>(_query.tick % tierTwoCount), ranked.end());
  }

  _outRanked.reserve(ranked.size());
  std::uint32_t guaranteed = 0;
  for (const Ranked& entry : ranked)
  {
    _outRanked.push_back(entry.shipId);
    if (entry.tier == Tier::Owned)
    {
      ++guaranteed;
    }
  }
  // Tier 0 sorts first, so counting it is counting a prefix -- which is the
  // form the engine needs, because "never truncate inside the first N" is a
  // rule about a list and not about a set it would have to test membership in.
  return guaranteed;
}

} // namespace Game
