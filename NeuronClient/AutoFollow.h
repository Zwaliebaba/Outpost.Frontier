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

/*
 * True when there is no grid worth watching, because everything is crossing
 * (ADR-018 D16, A16).
 *
 * **This exists because `FollowTarget` answers two different questions with one
 * value.** `NO_FOLLOW_TARGET` means "stay put", and staying put is right when
 * the player has ships where they are standing -- and wrong when they have
 * nothing here and nothing anywhere else to go to, which is what a fleet
 * mid-warp leaves behind. D16 rules that second case: *"every fleet in transit
 * -> the map is the view"*. The two look identical from the outside, so the
 * distinction has to be asked for rather than inferred.
 *
 * The test for "crossing" is `etaSeconds`, not the state word. `stateLabel` is
 * a string the game wrote and this library may not have an opinion about it;
 * the ETA is the number `HudRoster.h` already documents as *"negative for a
 * fleet that is simply somewhere"*, so it is the game's own signal, read the
 * way `LocationBlock::inScene` is.
 *
 * **A player with nothing at all is not crossing.** No blocks means no ships --
 * a fresh session before the first summary, or a commander who has lost
 * everything -- and yanking them to the map for it would be the HUD reacting to
 * a frame in which it simply knows nothing yet.
 */
[[nodiscard]] inline bool EveryFleetIsCrossing(std::span<const LocationBlock> _blocks, std::uint16_t _here) noexcept
{
  bool anyCrossing = false;
  for (const LocationBlock& block : _blocks)
  {
    if (block.shipCount == 0)
    {
      continue; // Nothing there to be anywhere.
    }
    if (block.anchor == _here)
    {
      return false; // Something of theirs is here, which is a reason to stay.
    }
    if (block.etaSeconds < 0.0f)
    {
      return false; // A grid to go to, which is `FollowTarget`'s business.
    }
    anyCrossing = true;
  }
  return anyCrossing;
}

} // namespace Neuron
