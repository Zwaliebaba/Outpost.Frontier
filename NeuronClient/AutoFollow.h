#pragma once

#include "HudRoster.h"

#include <cstdint>
#include <span>

/*
 * Which grid the client should be watching, given where the player's ships are
 * (ADR-016 §9's auto-follow).
 *
 * A free function over the location blocks rather than a method on `ClientApp`,
 * and the reason is testability rather than tidiness: the decision is the whole
 * of the feature, the surrounding call is three lines of socket, and a rule that
 * lives inside a class with a swap chain in it is a rule nobody can assert
 * against. Everything it needs is already a value.
 *
 * **The policy is one sentence: a player watching a place they have nothing at
 * is watching the wrong place.** Everything else here is that sentence being
 * careful.
 */

namespace Neuron
{

/// No grid to follow. The anchor space is the game's and this is the engine's
/// "none", the way `INVALID_ENTITY_ID` is.
inline constexpr std::uint16_t NO_FOLLOW_TARGET = 0xffffu;

/*
 * The grid to ask for, or `NO_FOLLOW_TARGET` to stay put.
 *
 * `_here` is the grid being watched; `_refused` is a grid the authority has
 * already said no to, or `NO_FOLLOW_TARGET`.
 *
 * Three rules, and each of them is a way of not overruling the player:
 *
 *  - **Anything of theirs here means stay.** A station where their ships are
 *    docked, or a grid where half the fleet stayed behind, is a place they have
 *    a reason to be. Only an empty place follows.
 *  - **A crossing is not a destination.** A fleet mid-warp has an ETA and no
 *    grid to stand on, and `MayView` would rightly refuse a request to watch a
 *    world that does not hold it yet. It becomes a candidate when it lands.
 *  - **Ties break by anchor, not by arrival.** Two grids holding the same count
 *    is a real state, and the camera has to land the same way twice or the
 *    same situation plays differently on two machines.
 */
[[nodiscard]] inline std::uint16_t FollowTarget(std::span<const LocationBlock> _blocks, std::uint16_t _here,
                                                std::uint16_t _refused = NO_FOLLOW_TARGET) noexcept
{
  std::uint16_t best = NO_FOLLOW_TARGET;
  std::uint16_t bestCount = 0;
  for (const LocationBlock& block : _blocks)
  {
    if (block.anchor == _here)
    {
      return NO_FOLLOW_TARGET;
    }
    if (block.etaSeconds >= 0.0f || block.shipCount == 0 || block.anchor == _refused)
    {
      continue;
    }
    if (block.shipCount > bestCount || (block.shipCount == bestCount && block.anchor < best))
    {
      best = block.anchor;
      bestCount = block.shipCount;
    }
  }
  return best;
}

} // namespace Neuron
