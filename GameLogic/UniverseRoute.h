#pragma once

#include "Ids.h"
#include "Universe.h"

#include <cstdint>
#include <string_view>
#include <vector>

/*
 * Finding a system, and finding the way there (build order U5, ADR-018 D14).
 *
 * The strategic map is an **engine surface fed neutral data**: the screen is
 * the engine's, the topology crosses the seam once at boot, and the two
 * questions the screen asks that are *not* about drawing -- "which system did
 * you mean" and "how do I get there" -- are pure functions here. That split is
 * D14's, and the reason is the one the whole seam rests on: a search that lived
 * in the client would be a second search the day a route is planned from a
 * command line, and a route solver that lived there could not be replayed.
 *
 * **Pure over the baked universe, and that is the whole interface.** No world,
 * no registry, no tick: a route is a fact about content, so the same call at any
 * moment of any session gives the same answer. It is also why these can be
 * tested against the *committed* universe rather than a fixture.
 *
 * This file is one of the few allowed to name the universe definition -- its
 * job *is* the universe (CI's determinism guard states that as the rule), and
 * nothing here runs in a tick.
 */

namespace Game
{

/*
 * A route: the systems to pass through, starting with the origin.
 *
 * Systems rather than gates, because a jump is identified by where it goes and
 * the planner's job is the sequence of places. Empty when there is no route --
 * which is a real answer in a galaxy whose gate graph is connected by
 * construction only *within* what the bake built, and the honest one to give a
 * SET DESTINATION on a system nothing reaches.
 */
struct Route
{
  std::vector<SystemId> systems;

  /// Jumps, which is one fewer than the systems. Zero for "you are already
  /// there" and for no route at all -- the two are told apart by `systems`
  /// being non-empty.
  [[nodiscard]] std::size_t Jumps() const noexcept { return systems.empty() ? 0 : systems.size() - 1; }
};

/*
 * The fewest jumps from one system to another (U4's planner, U5's route line).
 *
 * Breadth-first, because every jump costs the same: a gate is a gate. When
 * jumps stop being equal -- a security-weighted route, a toll, a blockade --
 * this becomes Dijkstra over the same graph and the signature does not move.
 *
 * **Ties are broken by system id**, which is not an implementation detail: two
 * equally short routes must be the same route on the server and the client, or
 * the line drawn on the map is not the line the fleet will fly.
 */
[[nodiscard]] Route SolveRoute(const UniverseDef& _universe, SystemId _from, SystemId _to);

/*
 * The systems whose name contains `_query`, case-insensitively.
 *
 * For the map's search box. Substring rather than prefix because the names are
 * `ROOT-N` and a player who remembers the number types the number; ordered by
 * system id so the same query gives the same list twice, and capped so a
 * one-letter query cannot ask the screen to draw 2,500 rows.
 */
[[nodiscard]] std::vector<SystemId> FindSystems(const UniverseDef& _universe, std::string_view _query,
                                                std::size_t _limit = 32);

} // namespace Game
