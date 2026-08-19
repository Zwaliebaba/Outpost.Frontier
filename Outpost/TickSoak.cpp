#include "pch.h"

#include "TickSoak.h"

#include "Orders.h"
#include "ShipClass.h"
#include "World.h"

#include "Clock.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Outpost
{
namespace
{

/// The ladder. 41 is the authored starting fleet, so the first rung is the game
/// as it actually ships; 1,024 is ADR-018 D4's normative per-grid cap, so the
/// last rung is the number the interest/delta design must deliver a view of.
constexpr std::uint32_t RUNG_SHIPS[TICK_SOAK_RUNG_COUNT] = {41, 256, 512, 1024};

/*
 * How long a rung runs.
 *
 * Two ticks are flown and thrown away first: the first touch of the world's
 * tables is page faults and cold cache, and reporting that as the worst tick
 * would make the headline number about the allocator rather than about the
 * simulation.
 *
 * After that the rung runs to `MEASURED_TICKS` unless `RUNG_BUDGET_MS` runs out
 * first, with `MIN_TICKS` as the floor so a mean is always a mean of something.
 * The budget is what keeps this affordable in an unoptimised build, where the
 * capped rung costs an order of magnitude more per tick than the Release number
 * D11 says the acceptance figures mean. The tick count is reported beside every
 * mean so a short run is visible rather than implied.
 */
constexpr std::uint32_t WARMUP_TICKS = 2;
constexpr std::uint32_t MEASURED_TICKS = 200;
constexpr std::uint32_t MIN_TICKS = 20;
constexpr double RUNG_BUDGET_MS = 1500.0;

/*
 * The layout: a square lattice, every ship ordered to the middle of it.
 *
 * This is the converging-crowd worst case that the hand-run measurement used,
 * and it is worst in the way that matters. Both O(n^2) systems cost their pair
 * loop whatever the geometry -- `Separate` walks every pair four times a tick
 * and `Steering` scans every other ship for each ship *under guidance* -- so
 * what the layout has to guarantee is that no ship ever stops being guided and
 * quietly drops out of the expensive half. Flying the whole lattice at its own
 * centre does that: nothing arrives inside the measured window, and the crowd
 * thickens throughout it.
 *
 * The spacing clears the largest mobile hull's contact diameter (a Battleship's
 * 120 m radius, `ShipClass.cpp`), so the spawn itself is never overlapped --
 * `Separate` would otherwise spend the first ticks unpicking an authored pile,
 * which is a different measurement. At the capped rung the lattice is 32 x 32,
 * 9.3 km across, comfortably inside the 40 km grid.
 */
constexpr float LATTICE_SPACING_METRES = 300.0f;

/// The spawnable mobile classes, cycled so the mix exercises the area-weighted
/// separation split rather than one radius. `Structure` is left out on purpose:
/// it is terrain to `Separate` and never steers, so a lattice of stations would
/// measure the cheap path. The two reserved ids (ADR-009 §6) are not spawnable.
constexpr std::uint32_t SOAK_HULL_COUNT = 8;
constexpr Game::HullClass SOAK_HULLS[SOAK_HULL_COUNT] = {Game::HullClass::Interceptor, Game::HullClass::Bomber,
                                                        Game::HullClass::Corvette,    Game::HullClass::Frigate,
                                                        Game::HullClass::Carrier,     Game::HullClass::Battleship,
                                                        Game::HullClass::Hauler,      Game::HullClass::Miner};

/// Fills `_world` with `_shipCount` ships on the lattice and returns their ids
/// in spawn order.
std::vector<Game::ShipId> SpawnLattice(Game::World& _world, std::uint32_t _shipCount)
{
  const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(_shipCount))));
  const float extent = static_cast<float>(side - 1) * LATTICE_SPACING_METRES;
  const float origin = -0.5f * extent;

  std::vector<Game::ShipId> ships;
  ships.reserve(_shipCount);
  for (std::uint32_t index = 0; index < _shipCount; ++index)
  {
    Game::ShipSpawn spawn;
    spawn.hullClass = SOAK_HULLS[index % SOAK_HULL_COUNT];
    spawn.wing = static_cast<Game::WingId>(1 + index / Game::MAX_SHIPS_PER_ORDER);
    spawn.xMetres = origin + static_cast<float>(index % side) * LATTICE_SPACING_METRES;
    spawn.yMetres = origin + static_cast<float>(index / side) * LATTICE_SPACING_METRES;
    // Sequential from zero: the soak is one world and owns its whole population,
    // so it is its own allocator (ADR-018 D6a moved that job off `World`).
    const Game::ShipId id = _world.Spawn(spawn, static_cast<Game::ShipId>(index));
    if (id != Game::INVALID_SHIP_ID)
    {
      ships.push_back(id);
    }
  }
  return ships;
}

/// Orders every ship at the lattice centre, in batches of `MAX_SHIPS_PER_ORDER`
/// -- the game's own cap, so this is a player-reachable amount of contention
/// rather than a synthetic one. Returns false if any batch was refused, which
/// would leave part of the population unguided and the measurement wrong.
bool OrderEveryoneInward(Game::World& _world, const std::vector<Game::ShipId>& _ships)
{
  std::uint32_t orderSeq = 0;
  for (std::size_t first = 0; first < _ships.size(); first += Game::MAX_SHIPS_PER_ORDER)
  {
    const std::size_t last = std::min(first + Game::MAX_SHIPS_PER_ORDER, _ships.size());

    Game::OrderSubmit order;
    order.orderSeq = ++orderSeq;
    order.kind = Game::OrderKind::Move;
    order.formation = Game::FormationId::Line;
    order.queueMode = Game::QueueMode::Replace;
    for (std::size_t index = first; index < last; ++index)
    {
      if (!order.AddShip(_ships[index]))
      {
        return false;
      }
    }
    order.target.xCm = 0;
    order.target.yCm = 0;

    if (!_world.SubmitOrder(order).accepted)
    {
      return false;
    }
  }
  return true;
}

TickSoakRung MeasureRung(std::uint32_t _shipCount, bool& _outPopulationReached)
{
  TickSoakRung rung;
  rung.shipCount = _shipCount;

  Game::World world;
  world.Reset(0x50A4u);

  const std::vector<Game::ShipId> ships = SpawnLattice(world, _shipCount);
  _outPopulationReached = ships.size() == _shipCount && world.ShipCount() == _shipCount && OrderEveryoneInward(world, ships);

  std::uint32_t tick = 0;
  for (; tick < WARMUP_TICKS; ++tick)
  {
    world.Tick(tick + 1);
  }

  const std::int64_t rungStart = Neuron::Clock::Counter();
  double totalMs = 0.0;
  for (std::uint32_t measured = 0; measured < MEASURED_TICKS; ++measured)
  {
    const std::int64_t before = Neuron::Clock::Counter();
    world.Tick(++tick);
    const double elapsedMs = Neuron::Clock::MillisecondsBetween(before, Neuron::Clock::Counter());

    totalMs += elapsedMs;
    rung.worstMs = std::max(rung.worstMs, elapsedMs);
    ++rung.ticksMeasured;

    if (rung.ticksMeasured >= MIN_TICKS && Neuron::Clock::MillisecondsBetween(rungStart, Neuron::Clock::Counter()) >= RUNG_BUDGET_MS)
    {
      break;
    }
  }

  rung.meanMs = rung.ticksMeasured > 0 ? totalMs / static_cast<double>(rung.ticksMeasured) : 0.0;
  return rung;
}

} // namespace

TickSoakResult RunTickSoak()
{
  TickSoakResult result;
  result.populationsReached = true;

  for (std::uint32_t index = 0; index < TICK_SOAK_RUNG_COUNT; ++index)
  {
    bool populationReached = false;
    result.rungs[index] = MeasureRung(RUNG_SHIPS[index], populationReached);
    result.populationsReached = result.populationsReached && populationReached;

    const TickSoakRung& rung = result.rungs[index];
    NEURON_LOG_INFO("self test: soak %u ships -- %u ticks, mean %.3f ms, worst %.3f ms, %.1f %% of the %.0f ms tick", rung.shipCount,
                    rung.ticksMeasured, rung.meanMs, rung.worstMs, 100.0 * rung.meanMs / TICK_BUDGET_MS, TICK_BUDGET_MS);
  }

  result.cappedGridMeanMs = result.rungs[TICK_SOAK_RUNG_COUNT - 1].meanMs;

  // The figure D1c actually asks for. The tick budget is per *shard*, not per
  // grid -- "the 50 ms tick must hold the shard's target of concurrent live
  // grids at up to the per-grid cap" -- so what a capped grid costs is only
  // half the answer, and how many of them fit one core is the other half. It is
  // printed rather than asserted because the shard's target grid count is a
  // deployment number, not a compiled-in one.
  if (result.cappedGridMeanMs > 0.0)
  {
    NEURON_LOG_INFO("self test: soak headroom -- %.1f capped grids fit one core at 20 Hz", TICK_BUDGET_MS / result.cappedGridMeanMs);
  }

  return result;
}

} // namespace Outpost
