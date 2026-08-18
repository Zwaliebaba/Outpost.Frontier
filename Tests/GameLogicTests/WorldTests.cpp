#include "pch.h"
#include "CppUnitTest.h"

#include "ShipClass.h"
#include "World.h"

#include "EntityRecord.h"
#include "WorldHash.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;
using namespace DirectX;

/*
 * The simulation (ADR-005), tested the way it is meant to be: construct a
 * world, feed it orders, tick it, hash it. No device, no clock, no filesystem
 * and no fixtures -- which is the whole return on keeping GameLogic free of
 * everything but NeuronCore.
 *
 * Two things are being asserted here and they are different in kind. The
 * **replay** suite asserts an exact property: the same build, the same seed and
 * the same order log produce bit-identical state, forever. The **envelope**
 * suite asserts bounds a hull may never exceed, without pinning the balance
 * numbers -- so retuning a class is a table edit rather than a test rewrite.
 */

namespace GameLogicTests
{
namespace
{

constexpr float PI = 3.14159265358979323846f;

[[nodiscard]] float WrappedDelta(float _from, float _to) noexcept
{
  float delta = _to - _from;
  while (delta > PI)
  {
    delta -= 2.0f * PI;
  }
  while (delta < -PI)
  {
    delta += 2.0f * PI;
  }
  return delta;
}

[[nodiscard]] float DistanceTo(const XMFLOAT2& _position, float _x, float _y) noexcept
{
  return std::hypot(_x - _position.x, _y - _position.y);
}

/// One ship of a class, at the origin, pointing east.
[[nodiscard]] ShipId SpawnOne(World& _world, HullClass _hullClass, float _headingRadians = 0.0f)
{
  ShipSpawn spawn;
  spawn.hullClass = _hullClass;
  spawn.headingRadians = _headingRadians;
  return _world.Spawn(spawn);
}

/*
 * A move order for these ships, quantised the way the wire quantises it.
 *
 * Metres in, centimetres out, because that is what an `OrderSubmit` carries and
 * what `ValidateOrder` reads (ADR-005 §4). A test that built a leg in metres
 * would be exercising a path the game does not have.
 */
[[nodiscard]] OrderSubmit MoveTo(const ShipId* _ships, std::uint32_t _count, float _x, float _y, float _facing = 0.0f)
{
  OrderSubmit order;
  order.orderSeq = 1;
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    (void)order.AddShip(_ships[index]);
  }
  order.target.xCm = Neuron::MetresToCentimetres(_x);
  order.target.yCm = Neuron::MetresToCentimetres(_y);
  order.target.facingTurns16 = Neuron::RadiansToHeading(_facing);
  return order;
}

/// The classes with content, which are the only ones that can be spawned.
const HullClass PLAYABLE[] = {HullClass::Interceptor, HullClass::Bomber, HullClass::Corvette, HullClass::Frigate,
                              HullClass::Battleship,  HullClass::Carrier, HullClass::Hauler,   HullClass::Miner};

/*
 * Runs a fixed scenario and returns a hash every twenty ticks.
 *
 * Deliberately a plain function of its inputs with no state outside it: if this
 * captured anything -- a clock, a counter, a static -- the replay suite would
 * be testing the harness rather than the world.
 */
[[nodiscard]] std::vector<std::uint64_t> RunScenario(std::uint64_t _seed, std::uint32_t _tickCount, bool _divergeAtHalf = false)
{
  World world;
  world.Reset(_seed);

  std::vector<ShipId> ships;
  for (int i = 0; i < 16; ++i)
  {
    ShipSpawn spawn;
    spawn.hullClass = PLAYABLE[i % static_cast<int>(std::size(PLAYABLE))];
    spawn.wing = static_cast<WingId>(1 + (i % 3));
    spawn.xMetres = static_cast<float>(i * 211 - 1600);
    spawn.yMetres = static_cast<float>(i * -137 + 900);
    spawn.headingRadians = static_cast<float>(i) * 0.41f;
    const ShipId id = world.Spawn(spawn);
    if (id != INVALID_SHIP_ID)
    {
      ships.push_back(id);
    }
  }

  std::vector<std::uint64_t> checkpoints;
  for (std::uint32_t tick = 1; tick <= _tickCount; ++tick)
  {
    // A new order every hundred ticks, so the log exercises re-targeting
    // mid-flight rather than one long uninterrupted approach.
    const bool issue = (tick % 100) == 1;
    OrderSubmit move = MoveTo(ships.data(), static_cast<std::uint32_t>(ships.size()),
                               ((tick / 200) % 2) != 0 ? -6000.0f : 6000.0f, ((tick / 300) % 2) != 0 ? 4000.0f : -4000.0f,
                               static_cast<float>(tick % 7) * 0.4f);
    if (_divergeAtHalf && tick == _tickCount / 2)
    {
      move.target.xCm += 100; // One metre, once. It must still show up.
    }

    if (issue || (_divergeAtHalf && tick == _tickCount / 2))
    {
      (void)world.SubmitOrder(move);
    }
    world.Tick(tick);
    if (tick % 20 == 0)
    {
      checkpoints.push_back(ComputeWorldHash(world));
    }
  }
  return checkpoints;
}

} // namespace

TEST_CLASS(ReplayDeterminismTests)
{
public:
  TEST_METHOD(TheSameLogRunTwiceIsBitIdentical)
  {
    // ADR-005 §5's whole promise, and the reason a desync is debuggable at all.
    // A thousand ticks is fifty seconds of simulated time, which is long enough
    // for float error to accumulate anywhere it is going to.
    const std::vector<std::uint64_t> first = RunScenario(0x5eed, 1000);
    const std::vector<std::uint64_t> second = RunScenario(0x5eed, 1000);

    Assert::AreEqual<std::size_t>(50, first.size(), L"a checkpoint every twenty ticks");
    Assert::AreEqual(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
    {
      Assert::AreEqual(first[i], second[i], L"the two runs diverged");
    }
  }

  TEST_METHOD(AOneMetreChangeInOneOrderChangesTheHash)
  {
    // The other half of the claim. A hash that never changed would pass the
    // test above while proving nothing, so this is the control: a single order
    // moved by one metre, once, halfway through, and every later checkpoint
    // has to notice.
    const std::vector<std::uint64_t> baseline = RunScenario(0x5eed, 1000);
    const std::vector<std::uint64_t> nudged = RunScenario(0x5eed, 1000, true);

    Assert::AreEqual(baseline.size(), nudged.size());
    Assert::IsTrue(baseline.back() != nudged.back(), L"a changed order left no trace in the final state");

    std::size_t firstDifference = baseline.size();
    for (std::size_t i = 0; i < baseline.size(); ++i)
    {
      if (baseline[i] != nudged[i])
      {
        firstDifference = i;
        break;
      }
    }
    Assert::IsTrue(firstDifference < baseline.size(), L"no checkpoint differed at all");
    Assert::IsTrue(firstDifference >= 24, L"the runs differed before the order that was supposed to change");
  }

  TEST_METHOD(TheSeedIsPartOfTheState)
  {
    // Nothing in S6's movement draws from the RNG, so this asserts something
    // narrow and worth having: the seed reaches world state and the hash covers
    // it. The day something starts consuming randomness, replay already holds.
    const std::vector<std::uint64_t> a = RunScenario(1, 200);
    const std::vector<std::uint64_t> b = RunScenario(2, 200);
    Assert::IsTrue(a.back() != b.back(), L"the seed is not in the hash");

    World world;
    world.Reset(1);
    const std::uint64_t before = world.Random().State();
    world.Reset(1);
    Assert::AreEqual(before, world.Random().State(), L"reseeding is not reproducible");
  }

  TEST_METHOD(TheReplicatedHashIgnoresWhatAClientCannotSee)
  {
    /*
     * Two worlds identical on the wire but differing in intent.
     *
     * A `Structure` is what makes the case constructible: it is a ship-table
     * entry with zero speed and zero turn rate, so it takes an order and then
     * does not move. Position, velocity and heading stay bit-identical across
     * the two worlds while their guidance differs -- which is exactly the state
     * the replicated hash must not distinguish and the full hash must.
     *
     * Any mobile hull would move during the tick that delivers the order, so
     * the two would differ everywhere and the test would prove nothing.
     */
    World a;
    World b;
    a.Reset(7);
    b.Reset(7);

    const ShipId stationA = SpawnOne(a, HullClass::Structure);
    const ShipId stationB = SpawnOne(b, HullClass::Structure);
    Assert::AreEqual<ShipId>(stationA, stationB);

    const ShipId onlyA[] = {stationA};
    const ShipId onlyB[] = {stationB};
    const OrderSubmit moveA = MoveTo(onlyA, 1, 3000.0f, 0.0f);
    const OrderSubmit moveB = MoveTo(onlyB, 1, -3000.0f, 0.0f);

    for (std::uint32_t tick = 1; tick <= 50; ++tick)
    {
      if (tick == 1)
      {
        (void)a.SubmitOrder(moveA);
      }
      a.Tick(tick);
      if (tick == 1)
      {
        (void)b.SubmitOrder(moveB);
      }
      b.Tick(tick);
    }

    Assert::AreEqual(ComputeReplicatedHash(a), ComputeReplicatedHash(b), L"nothing a client can see has changed");
    Assert::IsTrue(ComputeWorldHash(a) != ComputeWorldHash(b), L"the full hash missed a difference in guidance");
  }
};

TEST_CLASS(MovementEnvelopeTests)
{
public:
  TEST_METHOD(NoHullEverExceedsItsOwnLimits)
  {
    // The bounds, not the balance. Every playable class flies four approach
    // geometries and no tick may break its class's speed, turn rate or
    // acceleration -- so retuning the table is a table edit, not a test rewrite.
    const float targets[][3] = {{5000.0f, 0.0f, 0.0f},     // straight ahead
                                {0.0f, 5000.0f, PI},       // ninety degrees off
                                {-4000.0f, 0.0f, 0.0f},    // dead astern
                                {-2500.0f, -2500.0f, 1.0f}};

    for (const HullClass hullClass : PLAYABLE)
    {
      const ShipClassInfo& info = ShipClass(hullClass);
      for (const auto& target : targets)
      {
        World world;
        world.Reset(3);
        const ShipId ship = SpawnOne(world, hullClass);
        const ShipId ships[] = {ship};
        const OrderSubmit move = MoveTo(ships, 1, target[0], target[1], target[2]);

        float previousHeading = world.Headings()[0];
        float previousSpeed = 0.0f;
        for (std::uint32_t tick = 1; tick <= 2500; ++tick)
        {
          if (tick == 1)
          {
            (void)world.SubmitOrder(move);
          }
          world.Tick(tick);

          const float speed = world.SpeedAt(0);
          const float heading = world.Headings()[0];

          Assert::IsTrue(speed <= info.maxSpeedMetresPerSec + 1e-3f, L"a hull exceeded its top speed");
          Assert::IsTrue(std::fabs(WrappedDelta(previousHeading, heading)) <= info.turnRateRadiansPerSec * World::TICK_SECONDS + 1e-4f,
                         L"a hull turned faster than its turn rate");
          Assert::IsTrue(std::fabs(speed - previousSpeed) <= info.accelMetresPerSecSq * World::TICK_SECONDS + 1e-3f,
                         L"a hull changed speed faster than its acceleration");

          previousHeading = heading;
          previousSpeed = speed;
        }
      }
    }
  }

  TEST_METHOD(EveryHullArrivesAndStops)
  {
    for (const HullClass hullClass : PLAYABLE)
    {
      World world;
      world.Reset(3);
      const ShipId ship = SpawnOne(world, hullClass);
      const ShipId ships[] = {ship};
      const OrderSubmit move = MoveTo(ships, 1, -3000.0f, 2000.0f, 2.0f);

      for (std::uint32_t tick = 1; tick <= 4000; ++tick)
      {
        if (tick == 1)
        {
          (void)world.SubmitOrder(move);
        }
        world.Tick(tick);
      }

      const float finalDistance = DistanceTo(world.Positions()[0], -3000.0f, 2000.0f);
      Assert::IsTrue(finalDistance <= World::ARRIVAL_TOLERANCE_METRES,
                     L"a hull did not reach its target inside the arrival tolerance");
      Assert::IsTrue(world.SpeedAt(0) < 1.0f, L"a hull arrived and kept going");
      Assert::AreEqual(0.0f, WrappedDelta(2.0f, world.Headings()[0]), 1e-2f, L"a hull arrived facing the wrong way");
    }
  }

  TEST_METHOD(OvershootStaysInsideTheArrivalRing)
  {
    // The braking profile reaches zero at the edge of the ring rather than at
    // its centre, so a ship should never sail through and come back.
    for (const HullClass hullClass : PLAYABLE)
    {
      World world;
      world.Reset(3);
      const ShipId ship = SpawnOne(world, hullClass);
      const ShipId ships[] = {ship};
      const OrderSubmit move = MoveTo(ships, 1, 6000.0f, 0.0f);

      bool arrived = false;
      float worstAfterArrival = 0.0f;
      for (std::uint32_t tick = 1; tick <= 4000; ++tick)
      {
        if (tick == 1)
        {
          (void)world.SubmitOrder(move);
        }
        world.Tick(tick);
        const float distance = DistanceTo(world.Positions()[0], 6000.0f, 0.0f);
        if (distance <= World::ARRIVAL_TOLERANCE_METRES)
        {
          arrived = true;
        }
        if (arrived)
        {
          worstAfterArrival = std::max(worstAfterArrival, distance);
        }
      }

      Assert::IsTrue(arrived, L"a hull never reached its target");
      Assert::IsTrue(worstAfterArrival <= World::ARRIVAL_TOLERANCE_METRES + 0.5f, L"a hull overshot the arrival ring");
    }
  }

  TEST_METHOD(AShipAtSpeedNeverOrbitsAPointBehindIt)
  {
    /*
     * The regression test for a real defect. A hull's turn radius at cruise is
     * speed / turnRate -- 477 m for a Battleship -- so a target sixty metres
     * astern is deep inside the circle it can turn on. Without the turn limit
     * in Steering the ship kept its speed, swept past, and came round again:
     * three full laps and a hundred seconds of simulated time to travel sixty
     * metres.
     *
     * The measurement is the accumulated bearing from the target, which is what
     * "orbit" actually means -- distance alone cannot tell circling from a wide
     * approach.
     */
    for (const HullClass hullClass : PLAYABLE)
    {
      const ShipClassInfo& info = ShipClass(hullClass);

      World world;
      world.Reset(5);
      const ShipId ship = SpawnOne(world, hullClass);
      const ShipId ships[] = {ship};

      // Get up to cruise heading east.
      const OrderSubmit run = MoveTo(ships, 1, 15000.0f, 0.0f);
      std::uint32_t tick = 1;
      for (; tick <= 900; ++tick)
      {
        if (tick == 1)
        {
          (void)world.SubmitOrder(run);
        }
        world.Tick(tick);
      }
      Assert::IsTrue(world.SpeedAt(0) > info.maxSpeedMetresPerSec * 0.9f, L"the hull never reached cruise");

      // Now reverse onto a point just behind, well inside the turn circle.
      const XMFLOAT2 at = world.Positions()[0];
      const float targetX = at.x - 60.0f;
      const float targetY = at.y;
      const OrderSubmit back = MoveTo(ships, 1, targetX, targetY);

      float accumulatedBearing = 0.0f;
      float previousBearing = std::atan2(at.y - targetY, at.x - targetX);
      bool arrived = false;
      for (std::uint32_t step = 0; step < 4000 && !arrived; ++step, ++tick)
      {
        if (step == 0)
        {
          (void)world.SubmitOrder(back);
        }
        world.Tick(tick);
        const XMFLOAT2 position = world.Positions()[0];
        const float bearing = std::atan2(position.y - targetY, position.x - targetX);
        accumulatedBearing += WrappedDelta(previousBearing, bearing);
        previousBearing = bearing;
        arrived = DistanceTo(position, targetX, targetY) <= World::ARRIVAL_TOLERANCE_METRES;
      }

      Assert::IsTrue(arrived, L"a hull never reached a point sixty metres behind it");
      Assert::IsTrue(std::fabs(accumulatedBearing) < 2.0f * PI,
                     L"a hull circled its target instead of turning onto it");
    }
  }

  TEST_METHOD(BiggerHullsAreSlowerAndLessAgile)
  {
    // The table's shape, which balance may not invert without saying so: an
    // Interceptor out-runs and out-turns a Battleship, and every playable hull
    // has a usable envelope.
    const ShipClassInfo& interceptor = ShipClass(HullClass::Interceptor);
    const ShipClassInfo& battleship = ShipClass(HullClass::Battleship);

    Assert::IsTrue(interceptor.maxSpeedMetresPerSec > battleship.maxSpeedMetresPerSec);
    Assert::IsTrue(interceptor.turnRateRadiansPerSec > battleship.turnRateRadiansPerSec);
    Assert::IsTrue(interceptor.accelMetresPerSecSq > battleship.accelMetresPerSecSq);
    Assert::IsTrue(battleship.formationSpacingMetres > interceptor.formationSpacingMetres);

    for (const HullClass hullClass : PLAYABLE)
    {
      const ShipClassInfo& info = ShipClass(hullClass);
      Assert::IsTrue(info.maxSpeedMetresPerSec > 0.0f);
      Assert::IsTrue(info.accelMetresPerSecSq > 0.0f);
      Assert::IsTrue(info.turnRateRadiansPerSec > 0.0f);
      Assert::IsTrue(info.pickRadiusMetres > 0.0f);
      Assert::IsTrue(info.formationSpacingMetres > 0.0f);
    }
  }
};

TEST_CLASS(WorldTableTests)
{
public:
  TEST_METHOD(ReservedClassesAreNamedAndNeverSpawnable)
  {
    // Fighter and Cruiser hold their wire values with no content behind them
    // (ADR-009 §6). Naming them has to work; spawning them must not.
    Assert::IsFalse(HullClassHasContent(HullClass::Fighter));
    Assert::IsFalse(HullClassHasContent(HullClass::Cruiser));
    Assert::IsFalse(HullClassName(HullClass::Fighter).empty());
    Assert::IsFalse(HullClassName(HullClass::Cruiser).empty());

    World world;
    world.Reset(1);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Fighter;
    Assert::AreEqual<ShipId>(INVALID_SHIP_ID, world.Spawn(spawn));
    spawn.hullClass = HullClass::Cruiser;
    Assert::AreEqual<ShipId>(INVALID_SHIP_ID, world.Spawn(spawn));
    Assert::AreEqual<std::uint32_t>(0, world.ShipCount());
  }

  TEST_METHOD(AClassFromTheWireIsCheckedRatherThanTrusted)
  {
    HullClass parsed = HullClass::Structure;
    Assert::IsTrue(TryShipClass(0, parsed));
    Assert::IsTrue(parsed == HullClass::Interceptor);
    Assert::IsTrue(TryShipClass(HULL_CLASS_COUNT - 1, parsed));
    Assert::IsTrue(parsed == HullClass::Structure);
    Assert::IsFalse(TryShipClass(HULL_CLASS_COUNT, parsed), L"an unknown class must be refused, not clamped");
    Assert::IsFalse(TryShipClass(255, parsed));
  }

  TEST_METHOD(IdsSurviveTheSwapAndPopThatKeepsArraysDense)
  {
    // Removal moves the last ship into the freed slot, so a stale slot index
    // would silently address a different ship. Ids are the identity; slots are
    // not, and this is the test that keeps that true.
    World world;
    world.Reset(1);
    const ShipId first = SpawnOne(world, HullClass::Interceptor);
    const ShipId second = SpawnOne(world, HullClass::Corvette);
    const ShipId third = SpawnOne(world, HullClass::Carrier);
    Assert::AreEqual<std::uint32_t>(3, world.ShipCount());

    Assert::IsTrue(world.Despawn(second));
    Assert::AreEqual<std::uint32_t>(2, world.ShipCount());
    Assert::IsFalse(world.Despawn(second), L"despawning twice should report that it is gone");

    std::uint32_t slot = 0;
    Assert::IsFalse(world.FindSlot(second, slot));
    Assert::IsTrue(world.FindSlot(first, slot));
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(HullClass::Interceptor), world.Classes()[slot]);
    Assert::IsTrue(world.FindSlot(third, slot));
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(HullClass::Carrier), world.Classes()[slot],
                                   L"the moved ship's id points at someone else");
  }

  TEST_METHOD(ANewShipHoldsWhereItWasPutRatherThanSteeringToTheOrigin)
  {
    World world;
    world.Reset(1);
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Frigate;
    spawn.xMetres = 1234.0f;
    spawn.yMetres = -567.0f;
    spawn.headingRadians = 1.0f;
    const ShipId ship = world.Spawn(spawn);
    Assert::IsTrue(ship != INVALID_SHIP_ID);

    for (std::uint32_t tick = 1; tick <= 100; ++tick)
    {
      world.Tick(tick);
    }

    Assert::AreEqual(1234.0f, world.Positions()[0].x, 1e-3f);
    Assert::AreEqual(-567.0f, world.Positions()[0].y, 1e-3f);
    Assert::AreEqual(0.0f, world.SpeedAt(0), 1e-4f);
  }

  TEST_METHOD(HoldStopsAShipWhereItIs)
  {
    World world;
    world.Reset(1);
    const ShipId ship = SpawnOne(world, HullClass::Corvette);
    const ShipId ships[] = {ship};

    const OrderSubmit go = MoveTo(ships, 1, 8000.0f, 0.0f);
    std::uint32_t tick = 1;
    for (; tick <= 200; ++tick)
    {
      if (tick == 1)
      {
        (void)world.SubmitOrder(go);
      }
      world.Tick(tick);
    }
    Assert::IsTrue(world.SpeedAt(0) > 50.0f, L"the ship never got moving");

    // A hold is a Move to where the ship already is. ADR-004 §7's wire carries
    // one order kind, so "stop" is a destination rather than a verb, and the
    // ship brakes to it under the same arrival logic as any other move.
    const OrderSubmit stop = MoveTo(ships, 1, world.Positions()[0].x, world.Positions()[0].y);
    for (std::uint32_t step = 0; step < 400; ++step, ++tick)
    {
      if (step == 0)
      {
        (void)world.SubmitOrder(stop);
      }
      world.Tick(tick);
    }

    Assert::AreEqual(0.0f, world.SpeedAt(0), 1e-3f, L"a held ship is still moving");
  }

  TEST_METHOD(AnAbsurdTargetIsRefusedRatherThanClamped)
  {
    /*
     * This asserted clamping before S9, because before S9 nothing validated.
     * `ValidateOrder` refuses an out-of-bounds leg now, and refusing is the
     * better answer: a clamped order is one the player did not give, arriving
     * with no explanation, at the edge of the map.
     *
     * The value matters. 1e9 metres is 1e11 centimetres, well past `int32`, and
     * casting a float that far out of range is undefined behaviour --
     * `MetresToCentimetres` saturates for exactly this reason, so the number
     * validation sees is enormous rather than wrapped into something small
     * enough to accept.
     */
    World world;
    world.Reset(1);
    const ShipId ship = SpawnOne(world, HullClass::Interceptor);
    const ShipId ships[] = {ship};
    const OrderSubmit absurd = MoveTo(ships, 1, 1.0e9f, -1.0e9f);

    Assert::IsTrue(absurd.target.xCm > PLAY_AREA_HALF_EXTENT_CM, L"saturation should leave it far outside, not wrapped");
    Assert::IsTrue(absurd.target.yCm < -PLAY_AREA_HALF_EXTENT_CM);

    const OrderVerdict verdict = world.SubmitOrder(absurd);
    Assert::IsFalse(verdict.accepted);
    Assert::IsTrue(verdict.reason == OrderReason::OutOfBounds);

    world.Tick(1);
    Assert::IsTrue(world.Guidances()[0].mode == GuidanceMode::Hold, L"a refused order must move nothing");
  }

  TEST_METHOD(TheEdgeOfTheOperatingAreaIsStillInsideIt)
  {
    // The bound is inclusive, and a test says so rather than leaving the next
    // reader to work it out from a comparison operator.
    World world;
    world.Reset(1);
    const ShipId ship = SpawnOne(world, HullClass::Interceptor);
    const ShipId ships[] = {ship};

    const OrderSubmit onTheEdge = MoveTo(ships, 1, World::PLAY_AREA_HALF_EXTENT_METRES, 0.0f);
    Assert::IsTrue(world.SubmitOrder(onTheEdge).accepted);

    OrderSubmit oneCentimetreOut = onTheEdge;
    oneCentimetreOut.target.xCm += 1;
    Assert::IsTrue(world.SubmitOrder(oneCentimetreOut).reason == OrderReason::OutOfBounds);
  }

  TEST_METHOD(AnOrderNamingAMissingShipIsRefusedWhole)
  {
    /*
     * Also a change of answer at S9. The order used to be applied to whatever
     * ships existed; it is refused entirely now, with `UnknownShip`.
     *
     * Refusing the whole thing is what makes the client's pre-check worth
     * having: a partial application would succeed on the server and fail
     * locally in a way no reason code describes, and the player would see some
     * of their fleet move.
     */
    World world;
    world.Reset(1);
    const ShipId ship = SpawnOne(world, HullClass::Bomber);
    const ShipId ships[] = {ship, static_cast<ShipId>(9999)};
    const OrderSubmit move = MoveTo(ships, 2, 1000.0f, 0.0f);

    const OrderVerdict verdict = world.SubmitOrder(move);
    Assert::IsFalse(verdict.accepted);
    Assert::IsTrue(verdict.reason == OrderReason::UnknownShip);

    world.Tick(1);
    Assert::IsTrue(world.Guidances()[0].mode == GuidanceMode::Hold, L"no part of a refused order may be applied");
  }

  TEST_METHOD(AStationIsAShipThatNeverMoves)
  {
    // Structure is in the ship table with zero speed on purpose: one movement
    // path rather than two, and a station that can be selected and replicated
    // by the same code as everything else.
    World world;
    world.Reset(1);
    const ShipId station = SpawnOne(world, HullClass::Structure);
    Assert::IsTrue(station != INVALID_SHIP_ID);

    const ShipId ships[] = {station};
    const OrderSubmit move = MoveTo(ships, 1, 5000.0f, 5000.0f);
    for (std::uint32_t tick = 1; tick <= 500; ++tick)
    {
      if (tick == 1)
      {
        (void)world.SubmitOrder(move);
      }
      world.Tick(tick);
    }

    Assert::AreEqual(0.0f, world.Positions()[0].x, 1e-4f);
    Assert::AreEqual(0.0f, world.Positions()[0].y, 1e-4f);
    Assert::AreEqual(0.0f, world.SpeedAt(0), 1e-6f);
  }
};

} // namespace GameLogicTests
