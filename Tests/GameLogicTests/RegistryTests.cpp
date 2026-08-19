#include "pch.h"
#include "CppUnitTest.h"

#include "ShipClass.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "World.h"
#include "WorldHash.h"
#include "WorldRegistry.h"

#include "EntityRecord.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
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
/// Through the registry rather than into a borrowed world, because that is the
/// path that also tells the index where the ship went.
ShipId AddShip(WorldRegistry& _registry, AnchorId _anchor, float _x, float _y)
{
  ShipSpawn spawn;
  spawn.hullClass = HullClass::Interceptor;
  spawn.wing = 1;
  spawn.xMetres = _x;
  spawn.yMetres = _y;
  return _registry.Spawn(_anchor, spawn);
}

/// Submits a dock for every ship named, from the grid they are on.
[[nodiscard]] OrderVerdict SubmitDock(WorldRegistry& _registry, AnchorId _anchor, std::span<const ShipId> _ships)
{
  World* world = _registry.Borrow(_anchor);
  Assert::IsNotNull(world);

  OrderSubmit order;
  order.orderSeq = 1;
  order.kind = OrderKind::Dock;
  order.anchor = _anchor;
  for (const ShipId ship : _ships)
  {
    Assert::IsTrue(order.AddShip(ship));
  }
  return world->SubmitOrder(order);
}

/// Docks a fleet and lets the bus land it, so a test that is about *undocking*
/// does not spend a screen getting there.
void DockAndLand(WorldRegistry& _registry, AnchorId _anchor, std::span<const ShipId> _ships, std::uint32_t& _tick)
{
  Assert::IsTrue(SubmitDock(_registry, _anchor, _ships).accepted);
  _registry.Tick(++_tick);
  _registry.Tick(++_tick);
  Assert::AreEqual(_ships.size(), _registry.Roster(_anchor).size());
}

[[nodiscard]] StationCommand Undock(AnchorId _station, std::span<const ShipId> _ships,
                                    FormationId _formation = FormationId::Line)
{
  StationCommand command;
  command.orderSeq = 1;
  command.verb = StationVerb::Undock;
  command.station = _station;
  command.formation = _formation;
  for (const ShipId ship : _ships)
  {
    Assert::IsTrue(command.AddShip(ship));
  }
  return command;
}

} // namespace

TEST_CLASS(StationCommandTests)
{
public:
  TEST_METHOD(AnUndockPutsTheFleetBackOnTheGridAtTheAuthoredPoint)
  {
    /*
     * ADR-017 §3. The selection *is* the fleet -- there is no fleet entity to
     * create -- and it arrives by formation solve at the anchor's authored
     * undock point and facing, which is the warp-arrival pattern run from a
     * new door.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];
    const Anchor* anchor = universe.FindAnchor(station);
    Assert::IsNotNull(anchor);

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId first = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId second = AddShip(registry, station, -200.0f, 0.0f);
    const ShipId fleet[] = {first, second};
    DockAndLand(registry, station, fleet, tick);

    Assert::IsTrue(registry.SubmitStationCommand(Undock(station, fleet)).accepted);
    Assert::AreEqual<std::size_t>(0, registry.Roster(station).size(), L"they left the roster when the record was filed");

    registry.Tick(++tick);

    const World* world = registry.Peek(station);
    Assert::IsNotNull(world);

    // Both are back, with their own ids, near the authored undock point.
    const float undockX = Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->undockPoint.x));
    const float undockY = Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->undockPoint.y));
    std::uint32_t found = 0;
    for (std::size_t slot = 0; slot < world->Ids().size(); ++slot)
    {
      const ShipId id = world->Ids()[slot];
      if (id != first && id != second)
      {
        continue;
      }
      ++found;
      const float dx = world->Positions()[slot].x - undockX;
      const float dy = world->Positions()[slot].y - undockY;
      Assert::IsTrue(std::sqrt(dx * dx + dy * dy) < 1000.0f, L"a two-ship formation solves close to its anchor");
      Assert::AreEqual<std::uint32_t>(255, world->Hulls()[slot], L"the roster held no damage, so nothing came back hurt");
      Assert::AreEqual<std::uint32_t>(255, world->Shields()[slot]);
    }
    Assert::AreEqual<std::uint32_t>(2, found, L"the same two ships, by id");
  }

  TEST_METHOD(AnUndockedShipIsProtectedForFifteenSecondsAndThenIsNot)
  {
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    Assert::IsTrue(registry.SubmitStationCommand(Undock(station, fleet)).accepted);
    registry.Tick(++tick);

    const std::uint32_t arrival = tick;
    const auto window = static_cast<std::uint32_t>(static_cast<float>(UNDOCK_PROTECTION_SECONDS) / World::TICK_SECONDS);

    const World* world = registry.Peek(station);
    Assert::IsTrue(world->IsProtected(ship, arrival), L"protected from its first tick, not its second");
    Assert::IsTrue(world->IsProtected(ship, arrival + window - 1));
    Assert::IsFalse(world->IsProtected(ship, arrival + window), L"and fifteen seconds later it is not");
  }

  TEST_METHOD(ThePlayersOwnCommandEndsProtectionAndASystemOrderDoesNot)
  {
    /*
     * ADR-017 §5, and the reason `systemIssued` exists at all: the parking
     * order arrives in the same tick the fleet does, so if *any* order ended
     * protection the fleet would never have any.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);
    Assert::IsTrue(registry.SubmitStationCommand(Undock(station, fleet)).accepted);
    registry.Tick(++tick);

    World* world = registry.Borrow(station);
    Assert::IsTrue(world->IsProtected(ship, tick));

    OrderSubmit move;
    move.orderSeq = 2;
    Assert::IsTrue(move.AddShip(ship));
    move.target.xCm = Neuron::MetresToCentimetres(1500.0f);

    // The system's own order leaves it alone.
    Assert::IsTrue(world->SubmitSystemOrder(move).accepted);
    registry.Tick(++tick);
    Assert::IsTrue(world->IsProtected(ship, tick), L"the order that parks the fleet must not disarm it");

    // The player's ends it, on ingest.
    move.orderSeq = 3;
    Assert::IsTrue(world->SubmitOrder(move).accepted);
    registry.Tick(++tick);
    Assert::IsFalse(world->IsProtected(ship, tick), L"you cannot shoot from under the station's skirts");
  }

  TEST_METHOD(UndockingSomethingThatIsNotOnThisRosterIsRefused)
  {
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> stations = StationAnchors(universe, 2);

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, stations[0], 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, stations[0], fleet, tick);

    const OrderVerdict elsewhere = registry.SubmitStationCommand(Undock(stations[1], fleet));
    Assert::IsFalse(elsewhere.accepted);
    Assert::IsTrue(elsewhere.reason == OrderReason::NotDocked, L"the station is real; the ship is not on it");

    const ShipId stranger[] = {static_cast<ShipId>(4242)};
    const OrderVerdict unknown = registry.SubmitStationCommand(Undock(stations[0], stranger));
    Assert::IsFalse(unknown.accepted);
    Assert::IsTrue(unknown.reason == OrderReason::NotDocked);
  }

  TEST_METHOD(TheSameShipCannotUndockTwiceInOneTick)
  {
    // The row leaves the roster at *filing*, which is what makes the second
    // command a refusal rather than a race.
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    Assert::IsTrue(registry.SubmitStationCommand(Undock(station, fleet)).accepted);
    const OrderVerdict again = registry.SubmitStationCommand(Undock(station, fleet));
    Assert::IsFalse(again.accepted);
    Assert::IsTrue(again.reason == OrderReason::NotDocked);
  }

  TEST_METHOD(AssignWingRewritesTheRowAndNothingCrosses)
  {
    /*
     * ADR-017 §6: a wing exists iff a ship carries its number. Nothing is
     * created, nothing is destroyed, and nothing crosses the bus -- so it
     * applies on the spot rather than between ticks.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);
    Assert::AreEqual<std::uint32_t>(1, registry.Roster(station)[0].wing);

    StationCommand assign;
    assign.orderSeq = 9;
    assign.verb = StationVerb::AssignWing;
    assign.station = station;
    assign.wing = 7;
    Assert::IsTrue(assign.AddShip(ship));

    Assert::IsTrue(registry.SubmitStationCommand(assign).accepted);
    Assert::AreEqual<std::uint32_t>(0, registry.PendingTransferCount(), L"a wing is a number, not a place");
    Assert::AreEqual<std::uint32_t>(7, registry.Roster(station)[0].wing);

    // And it travels with the ship: undocking spawns it into wing 7.
    Assert::IsTrue(registry.SubmitStationCommand(Undock(station, fleet)).accepted);
    registry.Tick(++tick);
    const World* world = registry.Peek(station);
    for (std::size_t slot = 0; slot < world->Ids().size(); ++slot)
    {
      if (world->Ids()[slot] == ship)
      {
        Assert::AreEqual<std::uint32_t>(7, world->Wings()[slot], L"the wing the hangar assigned is the wing it flies in");
      }
    }
  }

  TEST_METHOD(TheCommandCheckOrderIsTheContract)
  {
    // Same promise the order side makes, for the same reason: a command that
    // breaks two rules has to name the same one on both machines.
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    StationCommand empty = Undock(station, {});
    empty.formation = static_cast<FormationId>(200);
    empty.station = 9999;
    Assert::IsTrue(registry.SubmitStationCommand(empty).reason == OrderReason::EmptySelection);

    StationCommand badFormation = Undock(station, fleet);
    badFormation.formation = static_cast<FormationId>(200);
    badFormation.station = 9999;
    Assert::IsTrue(registry.SubmitStationCommand(badFormation).reason == OrderReason::InvalidFormation);

    StationCommand nowhere = Undock(9999, fleet);
    Assert::IsTrue(registry.SubmitStationCommand(nowhere).reason == OrderReason::UnknownStation);

    // And only then does the roster get to answer.
    const ShipId stranger[] = {static_cast<ShipId>(4242)};
    Assert::IsTrue(registry.SubmitStationCommand(Undock(station, stranger)).reason == OrderReason::NotDocked);
  }

  TEST_METHOD(AnUndockIntoATornDownGridSpinsOneUp)
  {
    // ADR-016 §4: spawning into a world with no live grid spins one up. An
    // undock is ships arriving, and the door does not care which side it was
    // opened from.
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    registry.Tick(++tick); // Nothing left on the grid, nobody watching: it goes.
    Assert::IsNull(registry.Peek(station));

    Assert::IsTrue(registry.SubmitStationCommand(Undock(station, fleet)).accepted);
    registry.Tick(++tick);
    Assert::IsNotNull(registry.Peek(station), L"the fleet had somewhere to arrive");
  }

  TEST_METHOD(AMixedDockUndockScenarioReproducesItselfBitForBit)
  {
    /*
     * T1's acceptance, and the reason the roster and the bus are in the hash: a
     * session replay is the per-grid order logs *plus* the transfer log. Two
     * runs of the same script have to agree about where every ship ended up,
     * which wing it is in, and what is still in flight.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> stations = StationAnchors(universe, 2);

    const auto run = [&universe, &stations]()
    {
      WorldRegistry registry;
      registry.Reset(&universe, Config());
      std::uint32_t tick = 0;

      const ShipId a = AddShip(registry, stations[0], 200.0f, 0.0f);
      const ShipId b = AddShip(registry, stations[0], -200.0f, 300.0f);
      const ShipId c = AddShip(registry, stations[1], 150.0f, -150.0f);

      const ShipId here[] = {a, b};
      const ShipId there[] = {c};
      Assert::IsTrue(SubmitDock(registry, stations[0], here).accepted);
      Assert::IsTrue(SubmitDock(registry, stations[1], there).accepted);
      registry.Tick(++tick);
      registry.Tick(++tick);

      StationCommand assign;
      assign.orderSeq = 4;
      assign.verb = StationVerb::AssignWing;
      assign.station = stations[0];
      assign.wing = 3;
      Assert::IsTrue(assign.AddShip(b));
      Assert::IsTrue(registry.SubmitStationCommand(assign).accepted);

      const ShipId back[] = {a, b};
      Assert::IsTrue(registry.SubmitStationCommand(Undock(stations[0], back, FormationId::Wedge)).accepted);
      registry.Tick(++tick);
      registry.Tick(++tick);
      registry.Tick(++tick);
      return registry.Hash();
    };

    Assert::AreEqual(run(), run(), L"the same script has to produce the same universe");
  }
};

TEST_CLASS(StationRosterTests)
{
public:
  TEST_METHOD(ADockedFleetLeavesTheGridAndArrivesOnTheRosterWithItsIdentityIntact)
  {
    /*
     * ADR-017 §1 and §2 in one scenario: docking removes ships from the world
     * and puts them on a roster that keeps `(ShipId, class, wing)` and nothing
     * else. The identity surviving is the load-bearing part -- every log, order
     * and roster row has to mean the same ship before and after.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());

    const ShipId first = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId second = AddShip(registry, station, -200.0f, 100.0f);
    const std::uint32_t before = registry.Borrow(station)->ShipCount();

    const ShipId fleet[] = {first, second};
    Assert::IsTrue(SubmitDock(registry, station, fleet).accepted, L"both are well inside the dock radius");

    // Tick one files: ingest takes the ships out of the world, and the record
    // is on the bus rather than applied.
    registry.Tick(1);
    Assert::AreEqual(before - 2, registry.Borrow(station)->ShipCount(), L"the fleet left the grid at ingest");
    Assert::AreEqual<std::uint32_t>(1, registry.PendingTransferCount(),
                                    L"one record for the fleet, because a fleet docks together");
    Assert::AreEqual<std::size_t>(0, registry.Roster(station).size(), L"a filed transfer has not happened yet");

    // Tick two applies, between the ticks.
    registry.Tick(2);
    Assert::AreEqual<std::uint32_t>(0, registry.PendingTransferCount());

    const std::span<const RosterEntry> roster = registry.Roster(station);
    Assert::AreEqual<std::size_t>(2, roster.size());
    Assert::AreEqual<std::uint32_t>(first, roster[0].shipId, L"the same ship, not a new one that looks like it");
    Assert::AreEqual<std::uint32_t>(second, roster[1].shipId);
    Assert::IsTrue(roster[0].hullClass == HullClass::Interceptor);
    Assert::AreEqual<std::uint32_t>(1, roster[0].wing, L"the wing travels with the ship");
  }

  TEST_METHOD(ADockedShipIsStillSomewhere)
  {
    // ADR-017 §7: docked counts as presence. A commander whose last fleet
    // docked has not vanished from the universe, and the index that answers
    // "where are my ships" has to keep saying so.
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    const ShipId ship = AddShip(registry, station, 100.0f, 0.0f);

    const ShipId fleet[] = {ship};
    Assert::IsTrue(SubmitDock(registry, station, fleet).accepted);
    registry.Tick(1);
    registry.Tick(2);

    AnchorId where = INVALID_ID;
    Assert::IsTrue(registry.LocationOf(ship, where), L"a docked ship is not nowhere");
    Assert::AreEqual<std::uint32_t>(station, where);
  }

  TEST_METHOD(TheRosterOutlivesTheGridItDockedAt)
  {
    /*
     * The other half of "worlds forget" (ADR-018 D8). A station grid whose last
     * mobile ship has just docked is empty and unwatched, so it tears down --
     * and the roster is untouched, because it was never world state. This is
     * the property that lets 3,356 station grids exist without ticking.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    const ShipId ship = AddShip(registry, station, 100.0f, 0.0f);

    const ShipId fleet[] = {ship};
    Assert::IsTrue(SubmitDock(registry, station, fleet).accepted);
    registry.Tick(1);
    registry.Tick(2);

    Assert::AreEqual<std::size_t>(1, registry.Roster(station).size());

    // Nothing on the grid but its station, and nobody watching: it goes.
    registry.Tick(3);
    Assert::IsNull(registry.Peek(station), L"an empty unwatched grid tears down");
    Assert::AreEqual<std::size_t>(1, registry.Roster(station).size(), L"and the roster does not go with it");
  }

  TEST_METHOD(TheRosterAndTheBusAreInTheReplayDomain)
  {
    /*
     * ADR-017 §9: a session replay is the per-grid order logs *plus* the
     * transfer log. If the roster were outside the hash, two runs that
     * disagreed about where a fleet ended up would still agree they matched.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    const auto run = [&universe, station](std::uint64_t& _afterFiling, std::uint64_t& _afterApply)
    {
      WorldRegistry registry;
      registry.Reset(&universe, Config());
      const ShipId ship = AddShip(registry, station, 100.0f, 0.0f);
      const ShipId fleet[] = {ship};
      Assert::IsTrue(SubmitDock(registry, station, fleet).accepted);
      registry.Tick(1);
      _afterFiling = registry.Hash();
      registry.Tick(2);
      _afterApply = registry.Hash();
    };

    std::uint64_t firstFiled = 0;
    std::uint64_t firstApplied = 0;
    std::uint64_t secondFiled = 0;
    std::uint64_t secondApplied = 0;
    run(firstFiled, firstApplied);
    run(secondFiled, secondApplied);

    Assert::AreEqual(firstFiled, secondFiled, L"same seed, same session, same in-flight bus");
    Assert::AreEqual(firstApplied, secondApplied, L"and the same roster");
    Assert::AreNotEqual(firstFiled, firstApplied, L"a transfer landing is a change to the session");
  }

  TEST_METHOD(TheBusAppliesInItsTotalOrderRatherThanTheOrderItWasCollectedIn)
  {
    /*
     * ADR-018 D17. Two grids file in the same tick; the registry collects them
     * in anchor-id order, which is an implementation detail. What the design
     * fixes is `(applyTick, transferId)`, so this asserts the *ids* come out
     * ascending -- which is the thing a second host would also agree on.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> stations = StationAnchors(universe, 2);

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    const ShipId here = AddShip(registry, stations[0], 100.0f, 0.0f);
    const ShipId there = AddShip(registry, stations[1], 100.0f, 0.0f);

    const ShipId one[] = {here};
    const ShipId two[] = {there};
    Assert::IsTrue(SubmitDock(registry, stations[0], one).accepted);
    Assert::IsTrue(SubmitDock(registry, stations[1], two).accepted);

    registry.Tick(1);
    Assert::AreEqual<std::uint32_t>(2, registry.PendingTransferCount());
    registry.Tick(2);

    // Both landed, each on its own station's roster, and neither on the other's.
    Assert::AreEqual<std::size_t>(1, registry.Roster(stations[0]).size());
    Assert::AreEqual<std::size_t>(1, registry.Roster(stations[1]).size());
    Assert::AreEqual<std::uint32_t>(here, registry.Roster(stations[0])[0].shipId);
    Assert::AreEqual<std::uint32_t>(there, registry.Roster(stations[1])[0].shipId);
  }

  TEST_METHOD(ADockFiledThisTickCannotApplyWithinIt)
  {
    /*
     * The rule the whole bus exists for (ADR-017 §9): transfers apply *between*
     * ticks, so a world's tick can never observe another's mid-flight. If a
     * record filed during tick N could land during tick N, the order two grids
     * ran in would start to matter -- which is exactly the property U2's
     * permuted-tick test protects.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, Config());
    const ShipId ship = AddShip(registry, station, 100.0f, 0.0f);
    const ShipId fleet[] = {ship};
    Assert::IsTrue(SubmitDock(registry, station, fleet).accepted);

    registry.Tick(1);
    Assert::AreEqual<std::size_t>(0, registry.Roster(station).size(),
                                  L"filed during tick 1, so it may not have landed during tick 1");
    Assert::AreEqual<std::uint32_t>(1, registry.PendingTransferCount());
  }
};

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
    Assert::IsTrue(registry.Despawn(station, visitor));
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
