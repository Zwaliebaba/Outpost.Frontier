#include "pch.h"
#include "CppUnitTest.h"

#include "ShipClass.h"
#include "World.h"

#include "EntityRecord.h"
#include "WorldHash.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

/// One ship of a class, where the contact scenarios need it.
[[nodiscard]] ShipId SpawnShipAt(World& _world, HullClass _hullClass, float _x, float _y, float _headingRadians = 0.0f)
{
  ShipSpawn spawn;
  spawn.hullClass = _hullClass;
  spawn.xMetres = _x;
  spawn.yMetres = _y;
  spawn.headingRadians = _headingRadians;
  return _world.Spawn(spawn);
}

/// Centre distance between two ships, by slot lookup.
[[nodiscard]] float CentreDistance(const World& _world, ShipId _a, ShipId _b)
{
  std::uint32_t slotA = 0;
  std::uint32_t slotB = 0;
  (void)_world.FindSlot(_a, slotA);
  (void)_world.FindSlot(_b, slotB);
  const XMFLOAT2& positionA = _world.Positions()[slotA];
  const XMFLOAT2& positionB = _world.Positions()[slotB];
  return std::hypot(positionB.x - positionA.x, positionB.y - positionA.y);
}

/// The combined contact radius of two classes: closer than this is overlap.
[[nodiscard]] float ContactBetween(HullClass _a, HullClass _b)
{
  return ShipClass(_a).collisionRadiusMetres + ShipClass(_b).collisionRadiusMetres;
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
    Assert::IsTrue(battleship.collisionRadiusMetres > interceptor.collisionRadiusMetres);

    for (const HullClass hullClass : PLAYABLE)
    {
      const ShipClassInfo& info = ShipClass(hullClass);
      Assert::IsTrue(info.maxSpeedMetresPerSec > 0.0f);
      Assert::IsTrue(info.accelMetresPerSecSq > 0.0f);
      Assert::IsTrue(info.turnRateRadiansPerSec > 0.0f);
      Assert::IsTrue(info.pickRadiusMetres > 0.0f);
      Assert::IsTrue(info.formationSpacingMetres > 0.0f);

      /*
       * The contact radius holds two bounds (ADR-015 §1). Below the pick
       * radius, because picking is forgiving on purpose and contact must not
       * be. At most a quarter of the formation spacing, because stations one
       * spacing apart must park hulls with clear water between them -- and a
       * Wedge's arms put neighbours 2*sqrt(2) radii off each other's course,
       * which is also why the avoidance clearance stays under sqrt(2): a wider
       * margin would have a formation in cruise avoiding itself.
       */
      Assert::IsTrue(info.collisionRadiusMetres > 0.0f);
      Assert::IsTrue(info.collisionRadiusMetres < info.pickRadiusMetres, L"contact must be tighter than picking");
      Assert::IsTrue(4.0f * info.collisionRadiusMetres <= info.formationSpacingMetres + 1e-3f,
                     L"stations one spacing apart would park hulls in contact");
    }
    Assert::IsTrue(World::AVOID_CLEARANCE_FACTOR < 1.41421f, L"formation neighbours would sit inside the avoidance corridor");

    const ShipClassInfo& structure = ShipClass(HullClass::Structure);
    Assert::IsTrue(structure.collisionRadiusMetres > 0.0f, L"a station with no footprint is a station ships fly through");
    Assert::IsTrue(structure.collisionRadiusMetres < structure.pickRadiusMetres);
  }
};

/*
 * The contact model (ADR-015): ships no longer fly through each other. Three
 * mechanisms are under test -- braking against the nearest hull in the flown
 * path, deflection around traffic in the wanted course, and the positional
 * separation that resolves whatever those two could not avoid -- but the
 * assertions are about the *property* the three add up to: two hulls never
 * occupy the same place, and everything that could arrive before still does.
 *
 * A slack of half a metre rides on the contact assertions. Separation resolves
 * within a tick anything avoidance-sized, but the instant a crossing is
 * detected the pair can sit fractionally inside contact before the same tick's
 * resolution runs; the slack is for that fraction, not for real overlap.
 */
TEST_CLASS(ShipContactTests)
{
public:
  static constexpr float CONTACT_SLACK_METRES = 0.5f;

  TEST_METHOD(TwoShipsCrossingHeadOnPassClearAndBothArrive)
  {
    // The user-visible defect this feature exists to fix: two ships ordered to
    // swap places used to fly through each other at combined cruise speed.
    World world;
    world.Reset(11);
    const ShipId east = SpawnShipAt(world, HullClass::Interceptor, -2000.0f, 0.0f, 0.0f);
    const ShipId west = SpawnShipAt(world, HullClass::Interceptor, 2000.0f, 0.0f, PI);
    const ShipId eastOnly[] = {east};
    const ShipId westOnly[] = {west};

    const float contact = ContactBetween(HullClass::Interceptor, HullClass::Interceptor);
    float closest = std::numeric_limits<float>::max();
    for (std::uint32_t tick = 1; tick <= 1200; ++tick)
    {
      if (tick == 1)
      {
        Assert::IsTrue(world.SubmitOrder(MoveTo(eastOnly, 1, 2000.0f, 0.0f)).accepted);
        Assert::IsTrue(world.SubmitOrder(MoveTo(westOnly, 1, -2000.0f, 0.0f)).accepted);
      }
      world.Tick(tick);
      closest = std::min(closest, CentreDistance(world, east, west));
    }

    Assert::IsTrue(closest >= contact - CONTACT_SLACK_METRES, L"the two ships interpenetrated while crossing");
    Assert::IsTrue(closest < 4000.0f, L"the ships never actually met, so the test proved nothing");

    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(east, slot));
    Assert::IsTrue(DistanceTo(world.Positions()[slot], 2000.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                   L"avoidance cost the eastbound ship its arrival");
    Assert::IsTrue(world.FindSlot(west, slot));
    Assert::IsTrue(DistanceTo(world.Positions()[slot], -2000.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                   L"avoidance cost the westbound ship its arrival");
  }

  TEST_METHOD(AParkedHullStepsOutOfTheLaneAndGoesBackToItsBerth)
  {
    /*
     * Making way (ADR-021), end to end and in one scenario: an idle Frigate sits
     * exactly on the line an Interceptor has been told to fly. The three things
     * the feature owes are all here -- it *leaves* (or nothing was made of it),
     * nothing is ever flown through, and it *comes back* (or the feature has
     * quietly relocated a ship the player never ordered anywhere).
     *
     * The berth is dead centre on purpose. It is the case ADR-015's deflection
     * handles worst -- symmetric, so there is no side the mover naturally
     * prefers -- and the one where a Frigate pointing north has to turn all the
     * way round before it can clear southward, which is the slowest this can
     * possibly go.
     */
    World world;
    world.Reset(11);
    const ShipId mover = SpawnShipAt(world, HullClass::Interceptor, 0.0f, 0.0f);
    const ShipId parked = SpawnShipAt(world, HullClass::Frigate, 1500.0f, 0.0f, PI / 2.0f);
    const ShipId moverOnly[] = {mover};

    const float contact = ContactBetween(HullClass::Interceptor, HullClass::Frigate);
    float closest = std::numeric_limits<float>::max();
    float furthestFromBerth = 0.0f;
    for (std::uint32_t tick = 1; tick <= 1200; ++tick)
    {
      if (tick == 1)
      {
        Assert::IsTrue(world.SubmitOrder(MoveTo(moverOnly, 1, 3000.0f, 0.0f)).accepted);
      }
      world.Tick(tick);
      closest = std::min(closest, CentreDistance(world, mover, parked));

      std::uint32_t parkedSlot = 0;
      Assert::IsTrue(world.FindSlot(parked, parkedSlot));
      furthestFromBerth = std::max(furthestFromBerth, DistanceTo(world.Positions()[parkedSlot], 1500.0f, 0.0f));
    }

    Assert::IsTrue(closest >= contact - CONTACT_SLACK_METRES, L"the mover passed through the parked hull");

    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(mover, slot));
    Assert::IsTrue(DistanceTo(world.Positions()[slot], 3000.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                   L"the mover never reached the target");

    // Room enough to matter: the clearance is what the mover needs to fly past
    // without bending its course, so anything less than most of it is a twitch
    // rather than a lane being cleared.
    const float clearance = World::MAKE_WAY_CLEARANCE_FACTOR * contact;
    Assert::IsTrue(furthestFromBerth >= 0.75f * clearance, L"the parked hull never actually made room");

    // And back again. This is the half a shove cannot fake: the berth is where
    // the player left the ship, and the detour is over the moment the lane is.
    Assert::IsTrue(world.FindSlot(parked, slot));
    Assert::IsTrue(DistanceTo(world.Positions()[slot], 1500.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                   L"the hull that made way never went home");
  }

  TEST_METHOD(MakingWayNeverStartsAJamOfItsOwn)
  {
    /*
     * The failure mode a sidestep invents: a ship that has stepped aside is, to
     * everyone else, a ship on a short journey home -- so idle hulls could take
     * turns clearing lanes for each other's returns and never settle. Six
     * Corvettes parked in a row, one Battleship ordered straight down it.
     *
     * The property is that the row is *quiet* again afterwards: everybody home,
     * nobody still moving, nobody overlapping. A jam would show as any of the
     * three failing, and a mutual-yield loop as the second.
     *
     * Run twice and hashed, because making way is the first thing in the tick
     * that reads one ship's guidance while deciding another ship's course --
     * the shape of coupling that turns an iteration-order slip into a replay
     * divergence (ADR-005 §5).
     */
    const auto run = [](std::uint64_t _seed) -> std::uint64_t
    {
      World world;
      world.Reset(_seed);
      ShipId row[6] = {};
      float berthX[6] = {};
      for (int index = 0; index < 6; ++index)
      {
        berthX[index] = 1000.0f + static_cast<float>(index) * 400.0f;
        row[index] = SpawnShipAt(world, HullClass::Corvette, berthX[index], 0.0f, PI / 2.0f);
      }
      const ShipId mover = SpawnShipAt(world, HullClass::Battleship, -1500.0f, 0.0f);
      const ShipId moverOnly[] = {mover};

      for (std::uint32_t tick = 1; tick <= 4000; ++tick)
      {
        if (tick == 1)
        {
          Assert::IsTrue(world.SubmitOrder(MoveTo(moverOnly, 1, 5000.0f, 0.0f)).accepted);
        }
        world.Tick(tick);
      }

      std::uint32_t slot = 0;
      Assert::IsTrue(world.FindSlot(mover, slot));
      Assert::IsTrue(DistanceTo(world.Positions()[slot], 5000.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                     L"the Battleship never got down the row");

      for (int index = 0; index < 6; ++index)
      {
        Assert::IsTrue(world.FindSlot(row[index], slot));
        Assert::IsTrue(DistanceTo(world.Positions()[slot], berthX[index], 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                       L"a hull that made way never went home");
        Assert::IsTrue(world.SpeedAt(slot) < 1.0f, L"the row never settled");
      }

      const float contact = ContactBetween(HullClass::Corvette, HullClass::Corvette);
      for (std::uint32_t first = 0; first < world.ShipCount(); ++first)
      {
        for (std::uint32_t second = first + 1; second < world.ShipCount(); ++second)
        {
          Assert::IsTrue(DistanceTo(world.Positions()[first], world.Positions()[second].x, world.Positions()[second].y) >=
                           contact - CONTACT_SLACK_METRES,
                         L"the row settled overlapping");
        }
      }
      return ComputeWorldHash(world);
    };

    Assert::AreEqual(run(0x1a4e), run(0x1a4e), L"making way diverged between identical runs");
  }

  TEST_METHOD(AStationIsNeverAskedToMakeWay)
  {
    // Terrain does not step aside (ADR-015 §3): a Structure has no speed to make
    // way with, and a model that asked it to would be a model that lets a fleet
    // relocate a station by flying at it. The mover routes round it as before.
    World world;
    world.Reset(11);
    const ShipId station = SpawnShipAt(world, HullClass::Structure, 0.0f, 0.0f);
    const ShipId mover = SpawnShipAt(world, HullClass::Corvette, -3000.0f, 0.0f);
    const ShipId moverOnly[] = {mover};

    for (std::uint32_t tick = 1; tick <= 2400; ++tick)
    {
      if (tick == 1)
      {
        Assert::IsTrue(world.SubmitOrder(MoveTo(moverOnly, 1, 3000.0f, 0.0f)).accepted);
      }
      world.Tick(tick);

      std::uint32_t stationSlot = 0;
      Assert::IsTrue(world.FindSlot(station, stationSlot));
      Assert::AreEqual(0.0f, world.Positions()[stationSlot].x, 1e-6f, L"the station made way");
      Assert::AreEqual(0.0f, world.Positions()[stationSlot].y, 1e-6f, L"the station made way");
    }

    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(mover, slot));
    Assert::IsTrue(DistanceTo(world.Positions()[slot], 3000.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                   L"the mover never got around the station");
  }

  TEST_METHOD(AHullOnTheDestinationStaysPutRatherThanSurrenderingIt)
  {
    // The one berth making way deliberately does not clear (ADR-021, ADR-015
    // §5). Stepping off a spot someone is arriving at only means stepping back
    // into them once they park, so the mover brakes and parks adjacent instead
    // -- the designed outcome, which stays the designed outcome.
    World world;
    world.Reset(11);
    const ShipId parked = SpawnShipAt(world, HullClass::Corvette, 2000.0f, 0.0f, PI / 2.0f);
    const ShipId mover = SpawnShipAt(world, HullClass::Corvette, 0.0f, 0.0f);
    const ShipId moverOnly[] = {mover};

    for (std::uint32_t tick = 1; tick <= 2000; ++tick)
    {
      if (tick == 1)
      {
        Assert::IsTrue(world.SubmitOrder(MoveTo(moverOnly, 1, 2000.0f, 0.0f)).accepted);
      }
      world.Tick(tick);
    }

    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(parked, slot));
    Assert::IsTrue(DistanceTo(world.Positions()[slot], 2000.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                   L"the hull on the destination gave up a berth it should have kept");
  }

  TEST_METHOD(AStationIsTerrainThatNothingCanBulldoze)
  {
    // A Battleship ordered straight across a Structure: heaviest mover against
    // the one hull that is terrain. The station takes none of the correction,
    // ever -- a fleet cannot relocate a station by parking in it.
    World world;
    world.Reset(11);
    const ShipId station = SpawnShipAt(world, HullClass::Structure, 0.0f, 0.0f);
    const ShipId mover = SpawnShipAt(world, HullClass::Battleship, -3000.0f, 0.0f);
    const ShipId moverOnly[] = {mover};

    const float contact = ContactBetween(HullClass::Battleship, HullClass::Structure);
    float closest = std::numeric_limits<float>::max();
    for (std::uint32_t tick = 1; tick <= 2400; ++tick)
    {
      if (tick == 1)
      {
        Assert::IsTrue(world.SubmitOrder(MoveTo(moverOnly, 1, 3000.0f, 0.0f)).accepted);
      }
      world.Tick(tick);
      closest = std::min(closest, CentreDistance(world, mover, station));

      std::uint32_t stationSlot = 0;
      Assert::IsTrue(world.FindSlot(station, stationSlot));
      Assert::AreEqual(0.0f, world.Positions()[stationSlot].x, 1e-6f, L"the station moved");
      Assert::AreEqual(0.0f, world.Positions()[stationSlot].y, 1e-6f, L"the station moved");
    }

    Assert::IsTrue(closest >= contact - CONTACT_SLACK_METRES, L"the Battleship drove through the station");

    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(mover, slot));
    Assert::IsTrue(DistanceTo(world.Positions()[slot], 3000.0f, 0.0f) <= World::ARRIVAL_TOLERANCE_METRES,
                   L"the Battleship never got around the station");
  }

  TEST_METHOD(OverlappedSpawnsPushApartAndSettleClear)
  {
    // Overlap can be authored, not only flown into. Two holding ships stacked
    // on one point must part -- without either of them ever *moving* in the
    // envelope sense, because separation is positional and leaves speed alone.
    World world;
    world.Reset(11);
    const ShipId a = SpawnShipAt(world, HullClass::Corvette, 0.0f, 0.0f);
    const ShipId b = SpawnShipAt(world, HullClass::Corvette, 0.0f, 0.0f);

    for (std::uint32_t tick = 1; tick <= 40; ++tick)
    {
      world.Tick(tick);
    }

    const float contact = ContactBetween(HullClass::Corvette, HullClass::Corvette);
    Assert::IsTrue(CentreDistance(world, a, b) >= contact - 0.1f, L"stacked spawns stayed inside each other");
    Assert::AreEqual(0.0f, world.SpeedAt(0), 1e-4f, L"separation invented velocity");
    Assert::AreEqual(0.0f, world.SpeedAt(1), 1e-4f, L"separation invented velocity");
  }

  TEST_METHOD(AnOccupiedDestinationEndsParkedAdjacentNotInside)
  {
    /*
     * The one arrival the contact model deliberately costs: a target with a
     * hull already parked on it cannot be reached, so the ship brakes to a stop
     * against the blocker and the leg ends by its own deadline (ADR-005 §2) --
     * the same designed outcome as any other member that cannot arrive. What
     * the model owes here is narrower and testable: no overlap at any point,
     * a stop adjacent to the blocker, and an order that ends rather than wedges.
     */
    World world;
    world.Reset(11);
    const ShipId parked = SpawnShipAt(world, HullClass::Corvette, 2000.0f, 0.0f);
    const ShipId mover = SpawnShipAt(world, HullClass::Corvette, 0.0f, 0.0f);
    const ShipId moverOnly[] = {mover};

    const float contact = ContactBetween(HullClass::Corvette, HullClass::Corvette);
    float closest = std::numeric_limits<float>::max();
    for (std::uint32_t tick = 1; tick <= 2000; ++tick)
    {
      if (tick == 1)
      {
        Assert::IsTrue(world.SubmitOrder(MoveTo(moverOnly, 1, 2000.0f, 0.0f)).accepted);
      }
      world.Tick(tick);
      closest = std::min(closest, CentreDistance(world, mover, parked));
    }

    Assert::IsTrue(closest >= contact - CONTACT_SLACK_METRES, L"the mover pushed into the hull on its target");

    const float finalGap = CentreDistance(world, mover, parked);
    Assert::IsTrue(finalGap <= contact + 2.0f * World::ARRIVAL_TOLERANCE_METRES + CONTACT_SLACK_METRES,
                   L"the mover gave up somewhere short of adjacent");
    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(mover, slot));
    Assert::IsTrue(world.SpeedAt(slot) < 1.0f, L"the mover is still pushing against an unreachable target");
    // Ended by deadline *and long since retired*: a finished group lingers
    // `ORDER_DONE_LINGER_TICKS` and then leaves the table, so two thousand
    // ticks later an empty table is what "ended, not wedged" looks like.
    Assert::IsTrue(world.Groups().empty(), L"a blocked leg must end by deadline and retire, not wedge");
  }

  TEST_METHOD(AFormationStillArrivesOnEveryStationWithContactOn)
  {
    // The property that let the MVP skip avoidance -- stations never overlap --
    // is now the property that lets avoidance coexist with formations: the
    // shuffle to stations crosses paths, but the parked result is contact-free
    // by construction, so every member can still genuinely arrive.
    World world;
    world.Reset(11);
    ShipId ships[5] = {};
    for (int index = 0; index < 5; ++index)
    {
      ships[index] = SpawnShipAt(world, HullClass::Interceptor, static_cast<float>(index) * 50.0f - 100.0f, 0.0f);
    }

    for (std::uint32_t tick = 1; tick <= 700; ++tick)
    {
      if (tick == 1)
      {
        Assert::IsTrue(world.SubmitOrder(MoveTo(ships, 5, 0.0f, 3000.0f, PI / 2.0f)).accepted);
      }
      world.Tick(tick);
    }

    // Finished and retired -- seven hundred ticks is far past the Done linger,
    // so the finished move's absence from the table is the completion proof.
    Assert::IsTrue(world.Groups().empty(), L"the formation never finished its move");
    const float contact = ContactBetween(HullClass::Interceptor, HullClass::Interceptor);
    for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
    {
      const Guidance& guidance = world.Guidances()[slot];
      Assert::IsTrue(DistanceTo(world.Positions()[slot], guidance.targetXMetres, guidance.targetYMetres) <=
                         World::ARRIVAL_TOLERANCE_METRES,
                     L"a member ended somewhere other than its own station");
      for (std::uint32_t other = slot + 1; other < world.ShipCount(); ++other)
      {
        Assert::IsTrue(DistanceTo(world.Positions()[slot], world.Positions()[other].x, world.Positions()[other].y) >=
                           contact - CONTACT_SLACK_METRES,
                       L"two members parked in contact");
      }
    }
  }

  TEST_METHOD(AConvergingCrowdSettlesClearAndReplaysExactly)
  {
    /*
     * Eight ships, eight *independent* orders, one destination: the case the
     * formation solve cannot protect, and the most contact resolution work a
     * player can create with one click per wing. Two properties: the crowd
     * settles without overlap, and -- because separation is the newest and
     * busiest arithmetic in the tick -- the whole scramble replays bit-exactly.
     */
    const auto run = [](std::uint64_t _seed) -> std::uint64_t
    {
      World world;
      world.Reset(_seed);
      ShipId ships[8] = {};
      for (int index = 0; index < 8; ++index)
      {
        const float angle = static_cast<float>(index) * (2.0f * PI / 8.0f);
        ships[index] = SpawnShipAt(world, HullClass::Interceptor, std::cos(angle) * 1500.0f, std::sin(angle) * 1500.0f,
                                   angle + PI);
      }
      for (std::uint32_t tick = 1; tick <= 1500; ++tick)
      {
        if (tick == 1)
        {
          for (const ShipId ship : ships)
          {
            const ShipId one[] = {ship};
            Assert::IsTrue(world.SubmitOrder(MoveTo(one, 1, 0.0f, 0.0f)).accepted);
          }
        }
        world.Tick(tick);
      }

      const float contact = ContactBetween(HullClass::Interceptor, HullClass::Interceptor);
      for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
      {
        Assert::IsTrue(world.SpeedAt(slot) < 1.0f, L"the crowd never settled");
        for (std::uint32_t other = slot + 1; other < world.ShipCount(); ++other)
        {
          Assert::IsTrue(DistanceTo(world.Positions()[slot], world.Positions()[other].x, world.Positions()[other].y) >=
                             contact - CONTACT_SLACK_METRES,
                         L"the crowd settled overlapping");
        }
      }
      return ComputeWorldHash(world);
    };

    Assert::AreEqual(run(0xc0117), run(0xc0117), L"contact resolution diverged between identical runs");
  }

  TEST_METHOD(ContactNeverBreaksTheMovementEnvelope)
  {
    // Separation moves positions, never velocities -- so even mid-scramble, no
    // hull may break its class's speed, turn or acceleration limits. This is
    // the multi-ship twin of NoHullEverExceedsItsOwnLimits, which only ever
    // flew one ship and so could not see contact at all.
    World world;
    world.Reset(11);
    const ShipId ships[4] = {SpawnShipAt(world, HullClass::Interceptor, -1500.0f, 3.0f),
                             SpawnShipAt(world, HullClass::Interceptor, 1500.0f, -3.0f, PI),
                             SpawnShipAt(world, HullClass::Frigate, 0.0f, -1200.0f, PI / 2.0f),
                             SpawnShipAt(world, HullClass::Corvette, 0.0f, 0.0f)};

    std::vector<float> previousHeadings(world.Headings().begin(), world.Headings().end());
    std::vector<float> previousSpeeds;
    for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
    {
      previousSpeeds.push_back(world.SpeedAt(slot));
    }

    for (std::uint32_t tick = 1; tick <= 1000; ++tick)
    {
      if (tick == 1)
      {
        const ShipId a[] = {ships[0]};
        const ShipId b[] = {ships[1]};
        const ShipId c[] = {ships[2]};
        Assert::IsTrue(world.SubmitOrder(MoveTo(a, 1, 1500.0f, 3.0f)).accepted);
        Assert::IsTrue(world.SubmitOrder(MoveTo(b, 1, -1500.0f, -3.0f)).accepted);
        Assert::IsTrue(world.SubmitOrder(MoveTo(c, 1, 0.0f, 1200.0f, PI / 2.0f)).accepted);
      }
      world.Tick(tick);

      for (std::uint32_t slot = 0; slot < world.ShipCount(); ++slot)
      {
        HullClass hullClass = HullClass::Interceptor;
        Assert::IsTrue(TryShipClass(world.Classes()[slot], hullClass));
        const ShipClassInfo& info = ShipClass(hullClass);
        const float speed = world.SpeedAt(slot);
        Assert::IsTrue(speed <= info.maxSpeedMetresPerSec + 1e-3f, L"contact broke a hull's top speed");
        Assert::IsTrue(std::fabs(WrappedDelta(previousHeadings[slot], world.Headings()[slot])) <=
                           info.turnRateRadiansPerSec * World::TICK_SECONDS + 1e-4f,
                       L"contact broke a hull's turn rate");
        Assert::IsTrue(std::fabs(speed - previousSpeeds[slot]) <= info.accelMetresPerSecSq * World::TICK_SECONDS + 1e-3f,
                       L"contact broke a hull's acceleration");
        previousHeadings[slot] = world.Headings()[slot];
        previousSpeeds[slot] = speed;
      }
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
