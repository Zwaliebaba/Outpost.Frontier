#include "pch.h"

#include "IconDensity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Neuron
{
namespace
{

/// Which square of the merge grid a point falls in.
struct MergeCell
{
  std::int32_t x = 0;
  std::int32_t y = 0;

  [[nodiscard]] constexpr bool operator==(const MergeCell& _other) const noexcept
  {
    return x == _other.x && y == _other.y;
  }
};

/*
 * The grid is anchored at the world origin rather than at the camera.
 *
 * A grid that moved with the camera would re-cut the same fleet into different
 * cells as the player panned, so a chip would split and re-form under a finger
 * that had not moved. Anchoring it to the plane means panning slides the chips
 * and zooming re-cuts them, which is the pair a player can learn.
 */
[[nodiscard]] MergeCell CellOf(const DirectX::XMFLOAT2& _point, float _cellMetres) noexcept
{
  // Clamped rather than left to wrap. Positions are bounded by the wire's
  // +-21474 km, so this cannot fire in practice -- but a cell size a whisker
  // above zero would push the quotient past what an int32 holds, and a wrapped
  // index would merge two fleets a continent apart into one chip.
  constexpr float LIMIT = 2.0e9f;
  const float cellX = std::clamp(std::floor(_point.x / _cellMetres), -LIMIT, LIMIT);
  const float cellY = std::clamp(std::floor(_point.y / _cellMetres), -LIMIT, LIMIT);
  return MergeCell{static_cast<std::int32_t>(cellX), static_cast<std::int32_t>(cellY)};
}

/*
 * While a cluster is being accumulated, `centreMetres` holds the running
 * **minimum** and `halfExtentMetres` the running **maximum**; the last pass
 * converts the pair into the centre and half-extent the type promises.
 *
 * Two fields doing two jobs, which is worth the note it costs: the alternative
 * is a parallel scratch array, and the function's whole posture is that it
 * allocates nothing and writes only into the spans it was given.
 */
void Extend(IconCluster& _cluster, const DirectX::XMFLOAT2& _point) noexcept
{
  _cluster.centreMetres.x = std::min(_cluster.centreMetres.x, _point.x);
  _cluster.centreMetres.y = std::min(_cluster.centreMetres.y, _point.y);
  _cluster.halfExtentMetres.x = std::max(_cluster.halfExtentMetres.x, _point.x);
  _cluster.halfExtentMetres.y = std::max(_cluster.halfExtentMetres.y, _point.y);
  ++_cluster.count;
}

/*
 * Which accumulating cluster this candidate belongs to, or `_count` for none.
 *
 * The cluster's cell is recovered from its running minimum rather than stored
 * beside it, and that is exact rather than approximate: every member of a
 * cluster is inside one cell by construction, and the running minimum is built
 * from members' coordinates -- so its x and y are each inside that cell's span
 * even when they came from two different ships.
 */
[[nodiscard]] std::uint32_t FindCluster(std::span<const IconCluster> _clusters, std::uint32_t _count, const MergeCell& _cell,
                                        StandingColour _standing, float _cellMetres) noexcept
{
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    if (_clusters[index].standing == _standing && CellOf(_clusters[index].centreMetres, _cellMetres) == _cell)
    {
      return index;
    }
  }
  return _count;
}

} // namespace

IconMergeResult MergeIcons(std::span<const IconMergeCandidate> _candidates, float _metresPerPixel, const IconDensityTuning& _tuning,
                           std::span<IconCluster> _outClusters, std::span<std::uint32_t> _outSurvivors) noexcept
{
  IconMergeResult result;

  const float cellMetres = _tuning.mergeCellPixels * _metresPerPixel;
  if (!(cellMetres > 0.0f))
  {
    /*
     * No camera scale, so no grid -- and every candidate survives as itself.
     *
     * Drawing them all is the safe direction: the merge is an optimisation of
     * *attention*, and a frame that skips it is busy rather than wrong, where a
     * frame that merged everything into one chip would have thrown the fleet
     * away over a number that had not arrived yet.
     */
    for (std::uint32_t index = 0; index < _candidates.size(); ++index)
    {
      if (result.survivors < _outSurvivors.size())
      {
        _outSurvivors[result.survivors] = index;
        ++result.survivors;
      }
      else
      {
        ++result.dropped;
      }
    }
    return result;
  }

  // --- pass one: how many share each cell ---------------------------------
  for (const IconMergeCandidate& candidate : _candidates)
  {
    if (candidate.alwaysDrawn)
    {
      continue; // §3: a flagged hull is never folded. Counted in pass two.
    }

    const MergeCell cell = CellOf(candidate.planeMetres, cellMetres);
    const std::uint32_t found = FindCluster(_outClusters, result.clusters, cell, candidate.standing, cellMetres);
    if (found < result.clusters)
    {
      Extend(_outClusters[found], candidate.planeMetres);
      continue;
    }

    if (result.clusters >= _outClusters.size())
    {
      // The span was the budget and it is spent. Said rather than swallowed:
      // a merge that silently omitted ships would be the failure §6 is written
      // to prevent, arriving from the direction nobody was watching.
      ++result.dropped;
      continue;
    }

    IconCluster& cluster = _outClusters[result.clusters];
    cluster.standing = candidate.standing;
    cluster.centreMetres = candidate.planeMetres;
    cluster.halfExtentMetres = candidate.planeMetres;
    cluster.count = 1;
    ++result.clusters;
  }

  // --- pass two: who is drawn as themselves -------------------------------
  //
  // Walked in candidate order, so the survivor list comes back in the order the
  // caller handed the entities over and an index can be matched to its entity
  // without a search.
  for (std::uint32_t index = 0; index < _candidates.size(); ++index)
  {
    const IconMergeCandidate& candidate = _candidates[index];

    bool survives = candidate.alwaysDrawn;
    if (!survives)
    {
      const MergeCell cell = CellOf(candidate.planeMetres, cellMetres);
      const std::uint32_t found = FindCluster(_outClusters, result.clusters, cell, candidate.standing, cellMetres);
      // A group of one is not an aggregate. It is drawn as the ship it is,
      // because a chip reading `1` has replaced a glyph with a worse drawing of
      // the same fact. A candidate whose cluster was dropped above is not here
      // either -- it has already been counted as dropped, and inventing a
      // survivor for it would make the report disagree with the screen.
      survives = found < result.clusters && _outClusters[found].count == 1;
    }

    if (!survives)
    {
      continue;
    }

    if (result.survivors < _outSurvivors.size())
    {
      _outSurvivors[result.survivors] = index;
      ++result.survivors;
    }
    else
    {
      ++result.dropped;
    }
  }

  // --- pass three: chips only, and in the shape the type promises ---------
  std::uint32_t kept = 0;
  for (std::uint32_t index = 0; index < result.clusters; ++index)
  {
    const IconCluster& accumulated = _outClusters[index];
    if (accumulated.count < 2)
    {
      continue; // Handed back as a survivor above.
    }

    const DirectX::XMFLOAT2 minimum = accumulated.centreMetres;
    const DirectX::XMFLOAT2 maximum = accumulated.halfExtentMetres;

    IconCluster& chip = _outClusters[kept];
    chip.standing = accumulated.standing;
    chip.count = accumulated.count;
    chip.centreMetres = DirectX::XMFLOAT2{(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f};
    chip.halfExtentMetres = DirectX::XMFLOAT2{(maximum.x - minimum.x) * 0.5f, (maximum.y - minimum.y) * 0.5f};
    ++kept;
  }
  result.clusters = kept;

  return result;
}

const char* IconClusterText(StandingColour _standing, std::uint32_t _count, char* _buffer, std::size_t _bufferBytes) noexcept
{
  if (_buffer == nullptr || _bufferBytes == 0)
  {
    return "";
  }

  _buffer[0] = '\0';
  if (_count == 0)
  {
    // A chip of nothing is a chip that has to be read before it can be ignored.
    // `CountedChipText`'s rule, and the same silence.
    return _buffer;
  }

  /*
   * Spelled as escapes, which is R31's convention and not a style choice: these
   * files carry no BOM and the projects pass no `/utf-8`, so a literal glyph
   * here would be read in the build host's ANSI code page and could reach the
   * screen as two boxes -- which is exactly what U4 found in the warp chip.
   *
   * Every one of the four is in `GlyphAtlas`'s baked set, checked rather than
   * assumed: U+25B2, U+25CF, U+25CB and U+00D7 are all in its marker rows.
   */
  const char* marker = "\xE2\x96\xB2"; // U+25B2, the plate's own own-fleet chip.
  switch (_standing)
  {
  case StandingColour::Own:
    break;
  case StandingColour::Allied:
    marker = "\xE2\x97\x8F"; // U+25CF, a filled disc.
    break;
  case StandingColour::Neutral:
    marker = "\xE2\x97\x8B"; // U+25CB, the same disc hollow: unaligned.
    break;
  case StandingColour::Hostile:
    marker = "\xC3\x97"; // U+00D7, the plate's own hostile chip.
    break;
  }

  const int written = std::snprintf(_buffer, _bufferBytes, "%s %u", marker, _count);
  if (written < 0)
  {
    _buffer[0] = '\0';
  }
  return _buffer;
}

} // namespace Neuron
