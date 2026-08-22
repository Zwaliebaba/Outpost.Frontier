#pragma once

#include "Ids.h"
#include "World.h"
#include "WorldRegistry.h"

#include "Wire.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * Who matters to whom, and in what order (ADR-022 §4, U3d-a).
 *
 * The game half of ADR-022's relevance seam, and the split it rests on is
 * ADR-014's applied literally: **the game states policy, the engine applies
 * mechanism.** Relevance is game semantics -- who owns what, what is selected,
 * what a structure is -- while budget is link semantics, and the session host
 * is the only tier that knows a link.
 *
 * So this ranks and **never truncates**. The caller knows the budget; the
 * callee knows the game. A ranked list that dropped a ship would be a ship the
 * engine can never choose to send, which is the one failure a priority scheme
 * must not have -- and it is why the accept tests that the ranking is a total
 * order over the grid rather than merely a good one.
 *
 * Nothing here touches world state, and nothing here is hashed. Ranking is a
 * *read*, performed per viewer per tick outside the tick itself, so it is
 * outside the replay domain entirely (ADR-005 §5) -- which is what lets it
 * depend on a camera without a replay depending on one.
 */

namespace Game
{

/*
 * What one viewer is to one ship (ADR-022 §8b).
 *
 * Viewer-relative, which is why it is computed here and not stored on the
 * world: the same hull is `Own` to one commander and `Neutral` to the next, and
 * a column on `World` could only hold one of those answers. The session host is
 * the tier that knows the viewer, and this is the question it asks.
 *
 * **`Allied` and `Hostile` have no producer yet, and that is recorded rather
 * than hidden.** They are `tactical-icon-system.png` §3's colour channel and
 * ADR-022 §8b's two status bits, so the channel is four-valued on the wire from
 * the day it ships; what decides *which* of the two a foreign commander is, is
 * the combat phase's, and the Plan of Record names that phase as having no ADR
 * and no build order. Until it does, every ship that is not the viewer's own is
 * `Neutral`, which is the honest answer and not a placeholder: with no
 * diplomacy and no PVP flag, nobody is allied with anybody and nobody is at war.
 */
enum class Relationship : std::uint8_t
{
  Own = 0,
  Allied = 1,
  Neutral = 2,
  Hostile = 3
};

/// What this viewer is to that ship. `Neutral` for a ship the registry has no
/// owner for -- an authored occupant, or one it has never heard of -- which is
/// the same answer `OwnerOf` already gives those two cases for the same reason.
[[nodiscard]] Relationship RelationshipOf(const WorldRegistry& _registry, Neuron::PlayerId _viewer, ShipId _shipId) noexcept;

/*
 * One viewer's camera, as the ranking reads it (ADR-022 §4's `InterestQuery`).
 *
 * Everything in it is session state -- which grid, what is selected, where the
 * camera is pointed and how much it shows. None of it reaches `World`, which is
 * ADR-022 §1's rule stated as a data flow: *the sim tier has no viewers.*
 *
 * `tick` is here for the round-robin in §4's tier 2 and for nothing else. It is
 * the shard's tick rather than a counter of this function's own calls, so two
 * viewers of the same grid rotate in step and a ranking is reproducible from
 * the query alone.
 */
struct RelevanceQuery
{
  Neuron::PlayerId viewer = Neuron::INVALID_PLAYER_ID;
  AnchorId grid = INVALID_ID;

  /// What the viewer has selected, as replicated ids. A span, because the
  /// selection already exists in the session and this should not decide where.
  std::span<const ShipId> selection;

  float focusXMetres = 0.0f;
  float focusYMetres = 0.0f;

  /// What the zoom actually shows, as a half-extent in plane metres. Zero shows
  /// nothing, which ranks every unselected foreign ship into tier 2 rather than
  /// refusing the query -- a camera that has not been described yet is not an
  /// error, it is a camera at the origin.
  float viewHalfExtentMetres = 0.0f;

  std::uint32_t tick = 0;
};

/*
 * Ranks the grid's ships for one viewer, most relevant first.
 *
 * `_outRanked` is cleared and refilled with **every** ship on the grid, exactly
 * once each: a total order, no duplicates, no omissions. The tiers are
 * ADR-022 §4's:
 *
 *   tier 0  the viewer's own ships, anything selected, and every structure
 *   tier 1  what is inside the camera's extent, nearest to focus first
 *   tier 2  everything else, nearest first, rotated once per tick
 *
 * Tier 0 is never truncated by the engine (§5a), which is why "own" and
 * "selected" are joined by "structure": there are one or two structures on a
 * grid and they are its landmarks, and a station that flickered out of interest
 * would take the player's sense of where they are with it.
 *
 * **Tier 1 reads as "inside the extent" and not as §4's "visible
 * relationship", and the difference is recorded rather than quietly taken.**
 * §4's rule is a conjunction -- a relationship *and* the extent -- and its
 * first half has no producer until the combat phase gives `Allied` and
 * `Hostile` a meaning, so taken literally the tier would be empty by
 * construction and untestable. What the tier is *for* is stated in the same
 * table: the ships the player is actually looking at get a steady cadence and
 * the rest get a rotated one, and today the extent is the whole of what
 * expresses that. When a hostility model exists the conjunct is added here;
 * the tier does not have to be invented then.
 *
 * `_world` must be the grid `_query.grid` names. The registry is what supplies
 * ownership, which lives on the ship-to-owner index and not on the world.
 */
void RankRelevance(const WorldRegistry& _registry, const World& _world, const RelevanceQuery& _query,
                   std::vector<ShipId>& _outRanked);

} // namespace Game
