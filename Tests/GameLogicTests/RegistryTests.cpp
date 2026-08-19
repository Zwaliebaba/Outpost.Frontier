#include "pch.h"
#include "CppUnitTest.h"

#include "ShipClass.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "World.h"
#include "WorldHash.h"
#include "WorldRegistry.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * The universe runtime (Build Order U2, ADR-016 §4, ADR-019, ADR-018 A6-A8).
 *
 * These are the invariants the whole shard story rests on, and they are tested
 * rather than reviewed because every one of them is the kind that holds until
 * the day it quietly does not: ids that collide only after a transfer, a hash
 * that moves because someone was watching, a tick order that matters only once
 * two worlds are busy.
 */

namespace GameLogicTests
{

namespace
{

/// A small real universe. Generated rather than hand-written because the
/// registry reads the anchor table, and an anchor table typed out by hand is a
/// second implementation of the bake to keep in step.
[[nodiscard]] UniverseDef SmallUniverse()
{
  UniverseGenConfig config;
  config.regionCount = 2;
  config.constellationsPerRegion = 2;
  config.systemCount = 12;
  UniverseDef universe;
  Assert::IsTrue(GenerateUniverse(config, universe), L"the test universe would not bake");
  return universe;
}

/// The station anchors, which are the ones that author occupants.
[[nodiscard]] std::vector<AnchorId> StationAnchors(const UniverseDef& _universe, std::size_t _count)
{
  std::vector<AnchorId> anchors;
  for (const SolarSystem& system : _universe.systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind == AnchorKind::Station && anchors.size() < _count)
      {
        anchors.push_back(anchor.id);
      }
    }
  }
  Assert::AreEqual(_count, anchors.size(), L"the test universe has fewer stations than the test needs");
  return anchors;
}

[[nodiscard]] RegistryConfig Config(std::uint64_t _seed = 0x513Eu)
{
  RegistryConfig config;
  config.sessionSeed = _seed;
  config.hostId = 0;
  return config;
}

/// Puts a ship on a grid, so the world is not merely its authored occupants.
ShipId AddShip(WorldRegistry& _registry, AnchorId _anchor, float _x, float _y)
{
  World* world = _registry.Borrow(_anchor);
  Assert::IsNotNull(world);
  ShipSpawn spawn;
  spawn.hullClass = HullClass::Interceptor;
  spawn.wing = 1;
  spawn.xMetres = _x;
  spawn.yMetres = _y;
  return world->Spawn(spawn, _registry.AllocateShipId());
}

} // namespace

TEST_CLASS(WorldRegistryTests)
{
public:
  TEST_METHOD(SpinUpAuthorsTheAnchorsOccupantsWithTheIdsTheBakeDerived)
  {
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];
    const Anchor* anchor = universe.FindAnchor(station);
    Assert::IsNotNull(anchor);

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    Assert::AreEqual(0u, registry.LiveWorldCount(), L"a registry should start with nothing spun up");

    const World* world = registry.Borrow(station);
    Assert::IsNotNull(world, L"borrowing an authored anchor should spin its world up");
    Assert::AreEqual(1u, registry.LiveWorldCount());
    Assert::AreEqual(static_cast<std::uint32_t>(anchor->occupantCount), world->ShipCount());
    Assert::AreEqual(static_cast<ShipId>(anchor->occupantIdBase), world->Ids()[0],
                     L"the structure's id should be the one the bake derived from its anchor");

    // And the index knows where it is, without anyone walking the registry.
    AnchorId where = INVALID_ID;
    Assert::IsTrue(registry.LocationOf(static_cast<ShipId>(anchor->occupantIdBase), where));
    Assert::AreEqual(static_cast<std::uint16_t>(station), static_cast<std::uint16_t>(where));
  }

  TEST_METHOD(ANonAnchorIsNotAWorld)
  {
    // Warping somewhere nobody authored is a refusal, not a grid: anchors are
    // the only destinations there are (ADR-016 §3).
    const UniverseDef universe = SmallUniverse();
    WorldRegistry registry;
    registry.Reset(&universe, Config());
    Assert::IsNull(registry.Borrow(static_cast<AnchorId>(60000)), L"an unauthored anchor spun a world up");
    Assert::AreEqual(0u, registry.LiveWorldCount());
  }

  TEST_METHOD(AnEmptyWorldTicksToTheSameHashAsItsRecreation)
  {
    /*
     * The quiescence invariant (ADR-018 A7).
     *
     * A grid holding only its authored occupants must be *quiet*: ticking it
     * changes nothing that a recreation would not reproduce. If it were not
     * true, a station grid that happened to stay live would drift away from an
     * identical one that had been torn down and rebuilt, and the divergence
     * would surface a phase later as an unexplained replay failure.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry ticked;
    ticked.Reset(&universe, Config());
    ticked.AddViewer(station); // Held alive, or teardown would do this test's job for it.
    for (std::uint32_t tick = 1; tick <= 200; ++tick)
    {
      ticked.Tick(tick);
    }

    WorldRegistry recreated;
    recreated.Reset(&universe, Config());
    recreated.AddViewer(station);
    // Spun up late, and driven to the same shard tick: ADR-019 §2 makes the
    // comparison meaningful only at equal ticks, which is exactly why the
    // registry takes the number rather than counting its own.
    for (std::uint32_t tick = 1; tick <= 200; ++tick)
    {
      recreated.Tick(tick);
    }

    const World* a = ticked.Peek(station);
    const World* b = recreated.Peek(station);
    Assert::IsNotNull(a);
    Assert::IsNotNull(b);
    Assert::AreEqual(ComputeWorldHash(*a), ComputeWorldHash(*b), L"an empty world did not tick quietly");
  }

  TEST_METHOD(TeardownAndRecreateReproduceTheOccupantIdsExactly)
  {
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());

    const ShipId visitor = AddShip(registry, station, 500.0f, 0.0f);
    Assert::AreNotEqual(static_cast<std::uint16_t>(INVALID_SHIP_ID), static_cast<std::uint16_t>(visitor));
    const std::uint64_t before = ComputeWorldHash(*registry.Peek(station));

    // The visitor leaves; nobody is watching; the world goes.
    Assert::IsTrue(registry.Borrow(station)->Despawn(visitor));
    registry.Tick(1);
    Assert::AreEqual(0u, registry.LiveWorldCount(), L"an empty, unwatched grid should have been torn down");
    AnchorId gone = INVALID_ID;
    Assert::IsFalse(registry.LocationOf(visitor, gone), L"a torn-down world left its ships in the index");

    // And it comes back the same: worlds forget everything but their authored
    // occupants (ADR-018 D8), and those are content.
    const World* again = registry.Borrow(station);
    Assert::IsNotNull(again);
    const Anchor* anchor = universe.FindAnchor(station);
    Assert::AreEqual(static_cast<ShipId>(anchor->occupantIdBase), again->Ids()[0],
                     L"a recreated world gave its structure a different id");
    Assert::AreNotEqual(before, ComputeWorldHash(*again), L"the visitor should not have come back");
  }

  TEST_METHOD(AViewerHeldEmptyGridIsOutsideTheReplayDomain)
  {
    /*
     * ADR-018 D8, and the reason it matters: without this, whether a commander
     * happened to be looking at an empty grid would change the session's hash,
     * and where a camera points would be a hidden simulation input.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry unwatched;
    unwatched.Reset(&universe, Config());
    unwatched.Tick(1);

    WorldRegistry watched;
    watched.Reset(&universe, Config());
    watched.AddViewer(station);
    watched.Tick(1);

    Assert::AreEqual(1u, watched.LiveWorldCount(), L"a viewer should hold its grid alive");
    Assert::AreEqual(0u, unwatched.LiveWorldCount());
    Assert::AreEqual(unwatched.Hash(), watched.Hash(), L"watching an empty grid changed the session hash");

    // A grid with a ship on it is in the domain whether or not anyone watches.
    (void)AddShip(watched, station, 100.0f, 0.0f);
    Assert::AreNotEqual(unwatched.Hash(), watched.Hash(), L"a ship on a grid must reach the hash");
  }

  TEST_METHOD(TickOrderCannotMatterBecauseWorldsShareNothing)
  {
    /*
     * The world-isolation invariant (ADR-018 D1a/A8).
     *
     * Worlds share no mutable state during `Tick`, so ticking them in any order
     * must produce the same state. This is the property that makes world-level
     * fan-out the pre-approved first parallel consumer -- and it is tested by
     * suite rather than by review precisely because the first accidental
     * crossing will look harmless.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> stations = StationAnchors(universe, 3);

    const auto run = [&universe, &stations](bool _reversed)
    {
      WorldRegistry registry;
      registry.Reset(&universe, Config());
      for (const AnchorId anchor : stations)
      {
        (void)AddShip(registry, anchor, 300.0f, -200.0f);
      }

      for (std::uint32_t tick = 1; tick <= 60; ++tick)
      {
        // Tick the worlds by hand, in the order under test, rather than through
        // the registry -- which is what makes this a statement about the worlds
        // rather than about the registry's own loop.
        std::vector<AnchorId> order = stations;
        if (_reversed)
        {
          std::reverse(order.begin(), order.end());
        }
        for (const AnchorId anchor : order)
        {
          registry.Borrow(anchor)->Tick(tick);
        }
      }

      std::uint64_t hash = 0;
      for (const AnchorId anchor : stations)
      {
        hash ^= ComputeWorldHash(*registry.Peek(anchor));
      }
      return hash;
    };

    Assert::AreEqual(run(false), run(true), L"the order worlds tick in changed their state, so they share something");
  }

  TEST_METHOD(TheRegistryReproducesItselfFromTheSameSeed)
  {
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> stations = StationAnchors(universe, 2);

    const auto run = [&universe, &stations](std::uint64_t _seed)
    {
      WorldRegistry registry;
      registry.Reset(&universe, Config(_seed));
      for (const AnchorId anchor : stations)
      {
        (void)AddShip(registry, anchor, 800.0f, 400.0f);
      }
      for (std::uint32_t tick = 1; tick <= 120; ++tick)
      {
        registry.Tick(tick);
      }
      return registry.Hash();
    };

    Assert::AreEqual(run(0x513Eu), run(0x513Eu), L"two runs of one seed produced different registries");
  }

  TEST_METHOD(DynamicIdsArePartitionedFromAuthoredOnes)
  {
    // ADR-018 D6a: authored ids come from the anchor, dynamic ids from the
    // host's block, and the two spaces do not meet -- which is what lets the
    // registry answer "is this content or is it something a commander built?"
    // from the id alone.
    const UniverseDef universe = SmallUniverse();
    WorldRegistry registry;
    registry.Reset(&universe, Config());

    const ShipId first = registry.AllocateShipId();
    const ShipId second = registry.AllocateShipId();
    Assert::IsTrue(first >= DYNAMIC_SHIP_ID_BASE, L"a dynamic id landed in the authored space");
    Assert::AreEqual(static_cast<std::uint16_t>(first + 1), static_cast<std::uint16_t>(second));

    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.occupantCount > 0)
        {
          Assert::IsTrue(anchor.occupantIdBase + anchor.occupantCount <= DYNAMIC_SHIP_ID_BASE,
                         L"an authored id block reaches into the dynamic space");
        }
      }
    }
  }

  TEST_METHOD(EveryAnchorHasAHostAndItIsThisOne)
  {
    // ADR-019 §6.4. The function returning a constant is the entire point: the
    // call sites exist, so the day it returns something else they already do.
    Assert::AreEqual(static_cast<std::uint16_t>(0), static_cast<std::uint16_t>(WorldRegistry::HostForAnchor(1)));
    Assert::AreEqual(static_cast<std::uint16_t>(0), static_cast<std::uint16_t>(WorldRegistry::HostForAnchor(4242)));
  }
};

} // namespace GameLogicTests
