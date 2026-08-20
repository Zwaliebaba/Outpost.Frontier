#pragma once

#include "Ids.h"
#include "Universe.h"

#include <cstdint>

/*
 * Where a mining field is *today* (ADR-024 §3a and §3d, owner ruling R7).
 *
 * The bake authors a site's orbit ring; the bearing on that ring is re-derived
 * every epoch, along with the warp-in vector and the rock layout. So the field
 * re-forms overnight: yesterday's scouting is stale, a camp costs live presence
 * rather than knowledge, and any future free-positioning tool never gets a
 * fixed target.
 *
 * **Ambush deliberately survives this**, because it is the risk pillar the
 * wreck rule depends on. Under anchor-only warp (ADR-016 §3) an attacker
 * reaches today's site exactly the way a miner does -- by clicking it -- so
 * relocation was never what closed that door. What dilutes a camp is the wide
 * `arrivalSpreadRadiusCm` a site bakes, and that is not a function of the
 * epoch at all.
 *
 * **Pure, integer-only, and called by both halves.** The client draws today's
 * field from this; the sim spins today's world up from it. A float here would
 * make two machines disagree about where a system's ore is, which is the
 * failure this whole file is shaped to make impossible.
 */

namespace Game
{

/// Where a site sits, and what its rocks look like, for one epoch.
struct SitePlacement
{
  /// Where the 40 km grid sits on the universe plane this epoch.
  UniversePos origin;

  /// Where a warp arrives, relative to `origin`: outside the field's edge by
  /// the authored standoff, facing in toward the rocks.
  LocalOffsetCm warpInPoint;
  std::uint16_t warpInFacingTurns16 = 0;

  /// The site's `layoutSeed` folded with the epoch. What the client derives
  /// rock positions from and what the sim splits clusters by (ADR-024 §4c) --
  /// the same number on both halves, which is the point of deriving it here
  /// rather than replicating it.
  std::uint32_t layoutSalt = 0;
};

/*
 * Which epoch a shard tick falls in, for one site.
 *
 * **In ticks, not seconds, and the caller converts** -- for the reason
 * `GATE_JUMP_TICKS` is written in ticks (ADR-016 §5): `86400 / 0.05f` is not
 * 1,728,000 on every path that evaluates it, and an epoch that is one tick
 * short on one machine is a field that re-forms while somebody is standing in
 * it. Seconds reach this file already multiplied by an integer tick rate.
 *
 * The **stagger** is why the anchor id is a parameter: without it every field
 * in the shard re-forms on the same tick, which is a visible hitch and a
 * predictable minute to be waiting in. With it the epoch boundary is spread
 * deterministically across the day, and a site's own boundary is still exactly
 * `_epochTicks` apart every time.
 */
[[nodiscard]] std::uint32_t SiteEpochIndex(std::uint32_t _shardTick, AnchorId _anchor, std::uint32_t _epochTicks) noexcept;

/*
 * The placement itself.
 *
 * `_systemCentre` is the star the ring is measured from -- passed rather than
 * looked up, so this stays a function of its arguments and a test can ask it
 * about a system that does not exist.
 *
 * `_warpInStandoffMetres` is the economy's number (`SitesInfo`), passed for the
 * same reason: this file does not read content, it computes with it.
 *
 * Epoch 0 is what the bake writes into the anchor, so
 * `SiteEpochPlacement(centre, anchor, site, 0)` reproduces the committed
 * `origin`, `warpInPoint` and `warpInFacingTurns16` exactly. That is a bake
 * invariant, not a coincidence, and E1b's suite asserts it.
 */
[[nodiscard]] SitePlacement SiteEpochPlacement(const UniversePos& _systemCentre, AnchorId _anchor, const SiteSpec& _site,
                                               std::uint32_t _epochIndex, std::int64_t _warpInStandoffMetres) noexcept;

} // namespace Game
