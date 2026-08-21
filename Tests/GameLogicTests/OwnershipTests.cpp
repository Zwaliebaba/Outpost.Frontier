#include "pch.h"

#include "CppUnitTest.h"

#include "DurableState.h"
#include "Station.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "UniverseParse.h"
#include "World.h"
#include "WorldRegistry.h"

#include "Wire.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * U3c-a: a ship belongs to somebody (ADR-018 D5, build order U3c-a).
 *
 * Everything player-keyed in this library already took a `PlayerId` before this
 * slice and then ignored it, because there was one commander and the filter was
 * the identity function. That is the state in which every privacy assertion
 * passes for the wrong reason, so the tests here are deliberately written the
 * only way that can tell the difference: **two commanders, and an assertion
 * about what the second one does NOT get.**
 *
 * A test that says "commander one sees their ship" would have passed for the
 * whole of E2, E3 and E4 against a registry that showed everybody everything.
 */

namespace GameLogicTests
{

namespace
{

[[nodiscard]] const UniverseDef& Universe()
{
  static const UniverseDef universe = []
  {
    static constexpr const char* CANDIDATES[] = {
      "GameData/Universe/Frontier.json",
      "../GameData/Universe/Frontier.json",
      "../../GameData/Universe/Frontier.json",
      "../../../GameData/Universe/Frontier.json",
      "../../../../GameData/Universe/Frontier.json",
    };
    for (const char* path : CANDIDATES)
    {
      std::ifstream file(path, std::ios::binary);
      if (!file)
      {
        continue;
      }
      std::ostringstream text;
      text << file.rdbuf();
      UniverseDef parsed;
      std::vector<UniverseDiagnostic> diagnostics;
      if (ParseUniverse(text.str(), parsed, diagnostics))
      {
        return parsed;
      }
    }
    Assert::Fail(L"the committed Frontier.json was not found from the test working directory");
    return UniverseDef{};
  }();
  return universe;
}

[[nodiscard]] AnchorId FirstOfKind(AnchorKind _kind)
{
  for (const SolarSystem& system : Universe().systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind == _kind)
      {
        return anchor.id;
      }
    }
  }
  return INVALID_ID;
}

/// A second anchor of a kind, because U3c is scoped to **disjoint grids** and
/// half of these tests need two commanders standing in different places.
[[nodiscard]] AnchorId SecondOfKind(AnchorKind _kind)
{
  bool seenOne = false;
  for (const SolarSystem& system : Universe().systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind != _kind)
      {
        continue;
      }
      if (seenOne)
      {
        return anchor.id;
      }
      seenOne = true;
    }
  }
  return INVALID_ID;
}

void MakeRegistry(WorldRegistry& _registry)
{
  RegistryConfig config;
  config.sessionSeed = 0x03CAu;
  _registry.Reset(&Universe(), nullptr, config);
}

[[nodiscard]] ShipSpawn Miner()
{
  ShipSpawn spawn;
  spawn.hullClass = HullClass::Miner;
  spawn.wing = 1;
  return spawn;
}

constexpr Neuron::PlayerId ONE = 1;
constexpr Neuron::PlayerId TWO = 2;

} // namespace

/// The index itself: who owns what, and who owns nothing.
TEST_CLASS(ShipOwnershipTests)
{
public:
  TEST_METHOD(ASpawnedShipBelongsToWhoeverItWasSpawnedFor)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId anchor = FirstOfKind(AnchorKind::Planet);

    const ShipId mine = registry.Spawn(anchor, Miner(), ONE);
    const ShipId theirs = registry.Spawn(anchor, Miner(), TWO);
    Assert::AreNotEqual<std::uint32_t>(INVALID_SHIP_ID, mine);
    Assert::AreNotEqual<std::uint32_t>(INVALID_SHIP_ID, theirs);

    Assert::AreEqual<std::uint32_t>(ONE, registry.OwnerOf(mine), L"a spawned ship lost its owner");
    Assert::AreEqual<std::uint32_t>(TWO, registry.OwnerOf(theirs), L"two commanders spawning on one grid got one owner");
  }

  TEST_METHOD(AuthoredOccupantsBelongToNobody)
  {
    /*
     * The universe's own furniture is not property. If a station belonged to
     * player one, the first commander to connect would own a structure they did
     * not build -- and, through `HasPresence`, could watch every grid in the
     * shard that has one standing on it.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const World* world = registry.Borrow(station);
    Assert::IsNotNull(world, L"the station grid did not spin up");

    bool sawOne = false;
    for (const ShipId occupant : world->Ids())
    {
      sawOne = true;
      Assert::AreEqual<std::uint32_t>(Neuron::INVALID_PLAYER_ID, registry.OwnerOf(occupant),
                                      L"an authored occupant was owned by somebody");
    }
    Assert::IsTrue(sawOne, L"the fixture found no authored occupants to check");
  }

  TEST_METHOD(AShipNobodyHasHeardOfIsNobodys)
  {
    // "Does not exist" and "belongs to nobody" are deliberately the same
    // answer: a filter that had to tell them apart would have a case nobody
    // tested, and both mean "not yours" to every caller that matters.
    WorldRegistry registry;
    MakeRegistry(registry);
    Assert::AreEqual<std::uint32_t>(Neuron::INVALID_PLAYER_ID, registry.OwnerOf(60000));
  }

  TEST_METHOD(ADespawnedShipStopsBeingOwned)
  {
    /*
     * Ownership is cleared with the location rather than left behind, so the
     * invariant stays one sentence: a ship the registry cannot locate is a ship
     * it has no owner for. A stale owner would let a reused id inherit a
     * commander, which is a privacy bug arriving by accident.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId anchor = FirstOfKind(AnchorKind::Planet);
    const ShipId ship = registry.Spawn(anchor, Miner(), ONE);

    Assert::IsTrue(registry.Despawn(anchor, ship), L"the fixture could not despawn its own ship");
    Assert::AreEqual<std::uint32_t>(Neuron::INVALID_PLAYER_ID, registry.OwnerOf(ship),
                                    L"a despawned ship kept its owner");
  }
};

/// The crossing: a ship must not change hands by moving.
TEST_CLASS(OwnershipCrossingTests)
{
public:
  TEST_METHOD(ADockedShipIsStillItsOwners)
  {
    /*
     * The E2-shaped defect, one level up. In transit a ship has left one world
     * and not reached the next, so the owner has to RIDE on the crossing -- E2
     * shipped without the cargo doing that and a Miner arrived empty. An owner
     * arriving empty is quieter and worse: the hangar simply stops being
     * anybody's, and nothing says so.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const ShipId ship = registry.Spawn(station, Miner(), TWO);

    World* world = registry.Borrow(station);
    OrderSubmit dock;
    dock.orderSeq = 1;
    dock.kind = OrderKind::Dock;
    dock.anchor = station;
    (void)dock.AddShip(ship);
    (void)world->SubmitOrder(dock);

    std::uint32_t tick = 0;
    registry.Tick(++tick);
    registry.Tick(++tick);

    const std::span<const RosterEntry> docked = registry.Roster(station);
    Assert::AreEqual<std::size_t>(1, docked.size(), L"the ship did not reach the roster");
    Assert::AreEqual<std::uint32_t>(TWO, docked[0].owner, L"the roster forgot whose ship it holds");
    Assert::AreEqual<std::uint32_t>(TWO, registry.OwnerOf(ship), L"docking changed who owns the ship");
  }

  TEST_METHOD(TheRosterRemembersTwoCommandersSeparately)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);

    const ShipId mine = registry.Spawn(station, Miner(), ONE);
    const ShipId theirs = registry.Spawn(station, Miner(), TWO);

    World* world = registry.Borrow(station);
    OrderSubmit dock;
    dock.orderSeq = 1;
    dock.kind = OrderKind::Dock;
    dock.anchor = station;
    (void)dock.AddShip(mine);
    (void)dock.AddShip(theirs);
    (void)world->SubmitOrder(dock);

    std::uint32_t tick = 0;
    registry.Tick(++tick);
    registry.Tick(++tick);

    // One crossing carrying two commanders' ships is not something the
    // validator produces today -- which is exactly why the owner is per member
    // and not per request. If it ever happens, nobody changes hands.
    Assert::AreEqual<std::size_t>(1, registry.DockedFor(ONE, station).size(), L"commander one's half of the roster");
    Assert::AreEqual<std::size_t>(1, registry.DockedFor(TWO, station).size(), L"commander two's half of the roster");
    Assert::AreEqual<std::uint32_t>(mine, registry.DockedFor(ONE, station)[0].shipId);
    Assert::AreEqual<std::uint32_t>(theirs, registry.DockedFor(TWO, station)[0].shipId);
  }
};

/// The filters: what the second commander does NOT get.
TEST_CLASS(OwnershipPrivacyTests)
{
public:
  TEST_METHOD(SummariesCountOnlyTheViewersShips)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId mine = FirstOfKind(AnchorKind::Planet);
    const AnchorId theirs = SecondOfKind(AnchorKind::Planet);
    Assert::AreNotEqual<std::uint32_t>(mine, theirs, L"the fixture needs two grids");

    (void)registry.Spawn(mine, Miner(), ONE);
    (void)registry.Spawn(mine, Miner(), ONE);
    (void)registry.Spawn(theirs, Miner(), TWO);

    const std::vector<FleetSummary> forOne = registry.Summaries(ONE);
    Assert::AreEqual<std::size_t>(1, forOne.size(), L"commander one saw more than their own grid");
    Assert::AreEqual<std::uint32_t>(mine, forOne[0].anchor);
    Assert::AreEqual<std::uint16_t>(2, forOne[0].shipCount);

    const std::vector<FleetSummary> forTwo = registry.Summaries(TWO);
    Assert::AreEqual<std::size_t>(1, forTwo.size(), L"commander two saw commander one's fleet");
    Assert::AreEqual<std::uint32_t>(theirs, forTwo[0].anchor);
    Assert::AreEqual<std::uint16_t>(1, forTwo[0].shipCount);
  }

  TEST_METHOD(AnAuthoredOccupantIsNobodysFleet)
  {
    /*
     * This used to be a special case -- `ShipCount() - authoredCount`, with a
     * paragraph explaining that a station would otherwise read as a one-ship
     * fleet parked at every station in the universe. It is now a consequence of
     * the general rule instead, and this is the test that says the consequence
     * actually holds.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    Assert::IsNotNull(registry.Borrow(station), L"the station grid did not spin up");

    Assert::IsTrue(registry.Summaries(ONE).empty(), L"the station itself showed up as somebody's fleet");
  }

  TEST_METHOD(PresenceIsPerCommander)
  {
    /*
     * `MayView`'s whole rule, and the one that was a genuine hole: it walked
     * the composition root's scripted patrol list and so answered the same for
     * every viewer. The second commander to connect could watch the first one's
     * grid.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId mine = FirstOfKind(AnchorKind::Planet);
    const AnchorId theirs = SecondOfKind(AnchorKind::Planet);

    (void)registry.Spawn(mine, Miner(), ONE);
    (void)registry.Spawn(theirs, Miner(), TWO);

    Assert::IsTrue(registry.HasPresence(ONE, mine), L"a commander cannot see the grid they are standing on");
    Assert::IsFalse(registry.HasPresence(ONE, theirs), L"a commander could watch another commander's grid");
    Assert::IsTrue(registry.HasPresence(TWO, theirs));
    Assert::IsFalse(registry.HasPresence(TWO, mine));
  }

  TEST_METHOD(DockingCountsAsPresenceForTheOwnerAndNobodyElse)
  {
    // ADR-017 §7 folded docked into the same answer as on-grid, so the filter
    // has to keep that true without also handing the station's grid to whoever
    // else happens to be docked there.
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const ShipId ship = registry.Spawn(station, Miner(), ONE);

    World* world = registry.Borrow(station);
    OrderSubmit dock;
    dock.orderSeq = 1;
    dock.kind = OrderKind::Dock;
    dock.anchor = station;
    (void)dock.AddShip(ship);
    (void)world->SubmitOrder(dock);

    std::uint32_t tick = 0;
    registry.Tick(++tick);
    registry.Tick(++tick);

    Assert::IsTrue(registry.HasPresence(ONE, station), L"a docked commander lost presence at their own station");
    Assert::IsFalse(registry.HasPresence(TWO, station), L"somebody else's dock granted presence");
  }

  TEST_METHOD(ARosterIsFilteredBeforeItReachesAnybody)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const ShipId ship = registry.Spawn(station, Miner(), ONE);

    World* world = registry.Borrow(station);
    OrderSubmit dock;
    dock.orderSeq = 1;
    dock.kind = OrderKind::Dock;
    dock.anchor = station;
    (void)dock.AddShip(ship);
    (void)world->SubmitOrder(dock);

    std::uint32_t tick = 0;
    registry.Tick(++tick);
    registry.Tick(++tick);

    Assert::AreEqual<std::size_t>(1, registry.Roster(station).size(), L"the station's own roster");
    Assert::AreEqual<std::size_t>(1, registry.DockedFor(ONE, station).size(), L"the owner's half");
    Assert::IsTrue(registry.DockedFor(TWO, station).empty(), L"a commander received another's docked ships");
  }

  TEST_METHOD(AnAnonymousViewerGetsNothingAtAll)
  {
    /*
     * `INVALID_PLAYER_ID` is what a zeroed handshake carries, and it must not
     * be a skeleton key. Without the guard it would match every authored
     * occupant in the universe -- so the one identity nobody has to
     * authenticate as would be the one that can watch any grid with a station
     * on it.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const AnchorId planet = FirstOfKind(AnchorKind::Planet);
    (void)registry.Spawn(planet, Miner(), ONE);

    Assert::IsTrue(registry.Summaries(Neuron::INVALID_PLAYER_ID).empty(), L"nobody got a census");
    Assert::IsTrue(registry.DockedFor(Neuron::INVALID_PLAYER_ID, station).empty());
    Assert::IsFalse(registry.HasPresence(Neuron::INVALID_PLAYER_ID, station),
                    L"nobody had presence wherever the universe keeps its furniture");
    Assert::IsFalse(registry.HasPresence(Neuron::INVALID_PLAYER_ID, planet));
  }
};

/// The hash and the format.
TEST_CLASS(OwnershipDurabilityTests)
{
public:
  TEST_METHOD(WhoOwnsAShipReachesTheHash)
  {
    /*
     * Ownership is durable state that outlives the worlds that produced it, on
     * exactly the terms the rosters already hold (ADR-018 D8). A replay that
     * reproduced every ship and forgot who owned them would agree about a
     * universe where nothing belonged to anybody -- silently, because every
     * position, hold and gauge would match.
     */
    WorldRegistry a;
    WorldRegistry b;
    MakeRegistry(a);
    MakeRegistry(b);
    const AnchorId anchor = FirstOfKind(AnchorKind::Planet);

    Assert::AreEqual(a.Hash(), b.Hash(), L"two identical registries disagreed before anything happened");
    (void)a.Spawn(anchor, Miner(), ONE);
    (void)b.Spawn(anchor, Miner(), TWO);
    Assert::AreNotEqual(a.Hash(), b.Hash(), L"whose ship it is does not reach the hash");
  }

  TEST_METHOD(OwnersSurviveTheDurableRoundTrip)
  {
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId mine = FirstOfKind(AnchorKind::Planet);
    const AnchorId theirs = SecondOfKind(AnchorKind::Planet);

    const ShipId shipOne = before.Spawn(mine, Miner(), ONE);
    const ShipId shipTwo = before.Spawn(theirs, Miner(), TWO);

    DurableState state;
    CaptureDurableState(before, state);

    WorldRegistry after;
    MakeRegistry(after);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(after.LoadDurable(state, diagnostics), L"the reload was refused");

    Assert::AreEqual<std::uint32_t>(ONE, after.OwnerOf(shipOne), L"a reloaded ship changed hands");
    Assert::AreEqual<std::uint32_t>(TWO, after.OwnerOf(shipTwo), L"a reloaded ship changed hands");
    Assert::IsTrue(after.HasPresence(ONE, mine));
    Assert::IsFalse(after.HasPresence(ONE, theirs), L"a reload handed one commander another's grid");
  }

  TEST_METHOD(ADockedShipsOwnerSurvivesARestart)
  {
    // The roster is where ownership could go missing across a restart, because
    // a docked ship is on no grid to be asked about.
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const ShipId ship = before.Spawn(station, Miner(), TWO);

    World* world = before.Borrow(station);
    OrderSubmit dock;
    dock.orderSeq = 1;
    dock.kind = OrderKind::Dock;
    dock.anchor = station;
    (void)dock.AddShip(ship);
    (void)world->SubmitOrder(dock);

    std::uint32_t tick = 0;
    before.Tick(++tick);
    before.Tick(++tick);

    DurableState state;
    CaptureDurableState(before, state);

    WorldRegistry after;
    MakeRegistry(after);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(after.LoadDurable(state, diagnostics), L"the reload was refused");

    const std::vector<RosterEntry> theirs = after.DockedFor(TWO, station);
    Assert::AreEqual<std::size_t>(1, theirs.size(), L"a reloaded roster lost its owner");
    Assert::AreEqual<std::uint32_t>(ship, theirs[0].shipId);
    Assert::IsTrue(after.DockedFor(ONE, station).empty(), L"a reload handed a hangar to the wrong commander");
  }

  TEST_METHOD(TheFormatVersionMovedWithTheMeaning)
  {
    /*
     * A version 2 file is not merely missing this field -- it *asserts*
     * something false, that every ship in the shard belongs to player one,
     * because capture wrote `SOLE_PLAYER_ID` unconditionally. Reading one under
     * the new rules would hand the first commander to connect the entire
     * universe, which is the failure a version number exists to prevent.
     */
    Assert::AreEqual<std::uint32_t>(3, DURABLE_FORMAT_VERSION,
                                    L"ownership changed what the format means and the version did not move");
  }
};

} // namespace GameLogicTests
