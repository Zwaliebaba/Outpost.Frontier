#pragma once

#include <cstdint>

/*
 * The tick-budget soak (Risk Register R10; ADR-018 D1c and action A4).
 *
 * R10 asks one question -- does the 50 ms tick hold a grid at the corpus cap of
 * 1,024 entities (ADR-018 D4)? -- and the answer decides whether a spatial
 * broadphase for the two O(n^2) tick systems (`Steering`'s avoidance scan and
 * `Separate`'s pair loop) has to land before U2 shapes the universe registry,
 * or is a later tuning decision.
 *
 * It was answered once, by hand, on a cross-build: 1,024 ships cost 10.6 ms
 * mean and 21.9 ms worst, about a fifth of the tick. A number measured once by
 * hand is a number nobody re-takes, and the two things that will move it --
 * U2's world fan-out and whatever the station phase adds to the tick -- are
 * both about to be built. So the soak lives here, in the shipping binary, and
 * runs with the self test on every push: ADR-018 D11 says perf numbers mean
 * **Release**, and A2's Release CI leg is what produces the authoritative one.
 *
 * It measures `World::Tick` and nothing else. The wire half of R10 -- fan-out,
 * datagram scheduling, client apply at 1,024 -- stays untestable until the
 * interest/delta design lands (NET-5), because a world past the full-snapshot
 * cap refuses to serialise at all. This instrument does not pretend otherwise.
 */

namespace Outpost
{

/// ADR-002 §1's tick, and what every rung is reported against. Stated here
/// rather than derived from `World::TICK_SECONDS` on purpose: that constant is
/// the step the simulation *integrates*, and this is the wall clock the tick
/// has to finish inside. They are the same number today and are not the same
/// fact.
inline constexpr double TICK_BUDGET_MS = 50.0;

/// What the self test actually asserts on, and why it is not the budget: a
/// capped grid measured near a fifth of the tick has room for a slow machine
/// but not for a structural regression, and this is where that line is drawn.
/// Raising it is a decision; a build that trips it is not a slow runner.
inline constexpr double TICK_COST_TRIPWIRE_MS = 2.0 * TICK_BUDGET_MS;

/// One population's measurement. `ticksMeasured` is reported rather than fixed
/// because the ladder is wall-clock bounded (see `TickSoak.cpp`): an unoptimised
/// build measures fewer ticks rather than taking a minute over them.
struct TickSoakRung
{
  std::uint32_t shipCount = 0;
  std::uint32_t ticksMeasured = 0;
  double meanMs = 0.0;
  double worstMs = 0.0;
};

/// 41 is the MVP's authored fleet, 1,024 the normative per-grid cap; the two in
/// between are what make the curve readable as quadratic rather than as a pair
/// of unrelated numbers.
inline constexpr std::uint32_t TICK_SOAK_RUNG_COUNT = 4;

struct TickSoakResult
{
  TickSoakRung rungs[TICK_SOAK_RUNG_COUNT] = {};

  /// Every rung spawned exactly the population it asked for. False means the
  /// measurement below is of some other, smaller world and means nothing.
  bool populationsReached = false;

  /// The capped rung's mean, lifted out because it is the number D1c gates on.
  double cappedGridMeanMs = 0.0;
};

/// Runs the ladder, logging one line per rung. Costs a couple of seconds in an
/// optimised build and is wall-clock bounded in an unoptimised one.
[[nodiscard]] TickSoakResult RunTickSoak();

} // namespace Outpost
