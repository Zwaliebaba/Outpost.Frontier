#include "pch.h"
#include "CppUnitTest.h"

#include "ShipClass.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "World.h"
#include "WorldHash.h"
#include "FleetSummary.h"
#include "Formation.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "WorldRegistry.h"

#include "EntityRecord.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
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
  // No site content: the registry's subject is worlds and ids, and a
  // universe without mining fields is the cheaper one to bake for it.
  Assert::IsTrue(GenerateUniverse(config, SitesInfo{}, universe), L"the test universe would not bake");
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
ShipId AddShip(WorldRegistry& _registry, AnchorId _anchor, float _x, float _y, WingId _wing = 1)
{
  ShipSpawn spawn;
  spawn.hullClass = HullClass::Interceptor;
  spawn.wing = _wing;
  spawn.xMetres = _x;
  spawn.yMetres = _y;
  return _registry.Spawn(_anchor, spawn, Neuron::SOLE_PLAYER_ID);
}

/// What wing a docked ship is in, by id rather than by row order -- the roster
/// is the authority's list and nothing promises where a given ship sits in it.
[[nodiscard]] WingId WingOf(const WorldRegistry& _registry, AnchorId _station, ShipId _ship)
{
  for (const RosterEntry& row : _registry.Roster(_station))
  {
    if (row.shipId == _ship)
    {
      return row.wing;
    }
  }
  Assert::Fail(L"that ship is not on this station's roster");
  return INVALID_WING_ID;
}

/// And the same question of a ship that is flying.
[[nodiscard]] WingId WingOnGrid(const World* _world, ShipId _ship)
{
  Assert::IsNotNull(_world);
  for (std::size_t slot = 0; slot < _world->Ids().size(); ++slot)
  {
    if (_world->Ids()[slot] == _ship)
    {
      return _world->Wings()[slot];
    }
  }
  Assert::Fail(L"that ship is not on this grid");
  return INVALID_WING_ID;
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

/// Two anchors of the *same* system, which is what an in-system warp needs.
[[nodiscard]] std::vector<AnchorId> TwoAnchorsInOneSystem(const UniverseDef& _universe)
{
  for (const SolarSystem& system : _universe.systems)
  {
    if (system.anchors.size() >= 2)
    {
      return {system.anchors[0].id, system.anchors[1].id};
    }
  }
  Assert::Fail(L"the test universe has no system with two anchors");
  return {};
}

/// A gate anchor and the anchor on the far side of it -- the pair a jump
/// crosses. Skips gates whose pair the small universe did not place, which the
/// bake does not produce but which a test should not assume away.
[[nodiscard]] std::vector<AnchorId> AGatePair(const UniverseDef& _universe)
{
  for (const SolarSystem& system : _universe.systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind != AnchorKind::Gate)
      {
        continue;
      }
      const AnchorId paired = _universe.PairedGateAnchor(anchor.id);
      if (paired != INVALID_ID)
      {
        return {anchor.id, paired};
      }
    }
  }
  Assert::Fail(L"the test universe has no paired gate");
  return {};
}

/// Submits a warp from the grid the ships are on.
[[nodiscard]] OrderVerdict SubmitWarp(WorldRegistry& _registry, AnchorId _from, AnchorId _to,
                                      std::span<const ShipId> _ships)
{
  World* world = _registry.Borrow(_from);
  Assert::IsNotNull(world);

  OrderSubmit order;
  order.orderSeq = 1;
  order.kind = OrderKind::Warp;
  order.anchor = _to;
  for (const ShipId ship : _ships)
  {
    Assert::IsTrue(order.AddShip(ship));
  }
  return world->SubmitOrder(order);
}

/// Runs the shard until `_predicate` holds or the cap is reached, and reports
/// the tick it stopped at. A cap rather than a while(true): a test that hangs
/// tells you nothing, and a test that stops tells you how long it waited.
std::uint32_t TickUntil(WorldRegistry& _registry, std::uint32_t& _tick, std::uint32_t _cap,
                        const std::function<bool()>& _predicate)
{
  for (std::uint32_t step = 0; step < _cap; ++step)
  {
    _registry.Tick(++_tick);
    if (_predicate())
    {
      return _tick;
    }
  }
  return _tick;
}

/// Is this ship standing on that grid?
[[nodiscard]] bool OnGrid(const WorldRegistry& _registry, AnchorId _anchor, ShipId _ship)
{
  const World* world = _registry.Peek(_anchor);
  if (world == nullptr)
  {
    return false;
  }
  const std::span<const ShipId> ids = world->Ids();
  return std::find(ids.begin(), ids.end(), _ship) != ids.end();
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
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId first = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId second = AddShip(registry, station, -200.0f, 0.0f);
    const ShipId fleet[] = {first, second};
    DockAndLand(registry, station, fleet, tick);

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
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
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
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
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
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
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, stations[0], 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, stations[0], fleet, tick);

    const OrderVerdict elsewhere = registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(stations[1], fleet));
    Assert::IsFalse(elsewhere.accepted);
    Assert::IsTrue(elsewhere.reason == OrderReason::NotDocked, L"the station is real; the ship is not on it");

    const ShipId stranger[] = {static_cast<ShipId>(4242)};
    const OrderVerdict unknown = registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(stations[0], stranger));
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
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
    const OrderVerdict again = registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet));
    Assert::IsFalse(again.accepted);
    Assert::IsTrue(again.reason == OrderReason::NotDocked);
  }

  /*
   * --- what docking does to a wing (ADR-017 §3, §6) ------------------------
   *
   * Four tests over one rule, and the rule exists because of a play report the
   * whole stack was individually correct about. Two ships out of one wing and
   * two out of another docked, were composed into one selection, undocked on
   * one command and flew as one fleet -- and still read on the roster as the
   * two wings they came from, because a wing is a number on a ship and nothing
   * on that path had touched it.
   *
   * The rule: **the ships one Dock names become one wing, unless that Dock
   * names a whole wing and nothing else.** The exception is not a special case
   * for tidiness -- without it a refuel round trip renames the fleet that took
   * it, and a commander runs out of call signs in eight docks.
   */

  TEST_METHOD(DockingPartOfAWingSplitsThoseShipsOffIt)
  {
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId first = AddShip(registry, station, 200.0f, 0.0f, 1);
    const ShipId second = AddShip(registry, station, 240.0f, 0.0f, 1);
    const ShipId stayed = AddShip(registry, station, 280.0f, 0.0f, 1);

    const ShipId docking[] = {first, second};
    DockAndLand(registry, station, docking, tick);

    // Two of the three left, so the two that left are not that wing any more.
    const WingId moved = WingOf(registry, station, first);
    Assert::AreEqual<std::uint32_t>(2, moved, L"the lowest number nobody was using");
    Assert::AreEqual<std::uint32_t>(moved, WingOf(registry, station, second), L"and both of them in it");

    // And the one that did not dock is untouched: a split takes the ships that
    // left, not the ones that stayed.
    const World* world = registry.Peek(station);
    Assert::AreEqual<std::uint32_t>(1, WingOnGrid(world, stayed), L"the ship left behind keeps the wing");
  }

  TEST_METHOD(ShipsFromTwoWingsThatDockTogetherBecomeOne)
  {
    /*
     * The player's report, as a test. Nothing here is a *part* of a wing --
     * both wings dock entirely -- and they still form a group, because ships
     * from two wings arriving on one command have visibly stopped flying with
     * whoever they used to. Uniformity is the question, not completeness.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId kilnA = AddShip(registry, station, 200.0f, 0.0f, 1);
    const ShipId kilnB = AddShip(registry, station, 240.0f, 0.0f, 1);
    const ShipId marrowA = AddShip(registry, station, 280.0f, 0.0f, 2);
    const ShipId marrowB = AddShip(registry, station, 320.0f, 0.0f, 2);

    const ShipId docking[] = {kilnA, kilnB, marrowA, marrowB};
    DockAndLand(registry, station, docking, tick);

    const WingId formed = WingOf(registry, station, kilnA);
    Assert::AreEqual<std::uint32_t>(3, formed, L"the lowest number neither wing was using");
    Assert::AreEqual<std::uint32_t>(formed, WingOf(registry, station, kilnB));
    Assert::AreEqual<std::uint32_t>(formed, WingOf(registry, station, marrowA));
    Assert::AreEqual<std::uint32_t>(formed, WingOf(registry, station, marrowB));
  }

  TEST_METHOD(DockingAWholeWingLeavesItsNumberAlone)
  {
    /*
     * The exception, and the reason it is worth the shard-wide count: a wing
     * that docks intact is the same fleet doing an errand, and renaming it
     * would spend a call sign every time somebody refuelled.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId talonA = AddShip(registry, station, 200.0f, 0.0f, 4);
    const ShipId talonB = AddShip(registry, station, 240.0f, 0.0f, 4);
    (void)AddShip(registry, station, 280.0f, 0.0f, 1); // another wing, not docking

    const ShipId docking[] = {talonA, talonB};
    DockAndLand(registry, station, docking, tick);

    Assert::AreEqual<std::uint32_t>(4, WingOf(registry, station, talonA), L"all of it docked, so it is still itself");
    Assert::AreEqual<std::uint32_t>(4, WingOf(registry, station, talonB));
  }

  TEST_METHOD(TheWingTheDockFormedIsTheWingTheyUndockInto)
  {
    /*
     * The last link, and the one the report was actually about: a split that
     * the roster records and the respawn then loses looks exactly like a split
     * that never happened.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId first = AddShip(registry, station, 200.0f, 0.0f, 1);
    const ShipId second = AddShip(registry, station, 240.0f, 0.0f, 1);
    (void)AddShip(registry, station, 280.0f, 0.0f, 1);

    const ShipId docking[] = {first, second};
    DockAndLand(registry, station, docking, tick);
    const WingId formed = WingOf(registry, station, first);

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, docking)).accepted);
    registry.Tick(++tick);

    const World* world = registry.Peek(station);
    Assert::AreEqual<std::uint32_t>(formed, WingOnGrid(world, first), L"back on the grid in the wing the hangar made");
    Assert::AreEqual<std::uint32_t>(formed, WingOnGrid(world, second));
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
    registry.Reset(&universe, nullptr, Config());
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

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, assign).accepted);
    Assert::AreEqual<std::uint32_t>(0, registry.PendingTransferCount(), L"a wing is a number, not a place");
    Assert::AreEqual<std::uint32_t>(7, registry.Roster(station)[0].wing);

    // And it travels with the ship: undocking spawns it into wing 7.
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
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
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    StationCommand empty = Undock(station, {});
    empty.formation = static_cast<FormationId>(200);
    empty.station = 9999;
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, empty).reason == OrderReason::EmptySelection);

    StationCommand badFormation = Undock(station, fleet);
    badFormation.formation = static_cast<FormationId>(200);
    badFormation.station = 9999;
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, badFormation).reason == OrderReason::InvalidFormation);

    StationCommand nowhere = Undock(9999, fleet);
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, nowhere).reason == OrderReason::UnknownStation);

    // And only then does the roster get to answer.
    const ShipId stranger[] = {static_cast<ShipId>(4242)};
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, stranger)).reason == OrderReason::NotDocked);
  }

  TEST_METHOD(AnUndockIntoATornDownGridSpinsOneUp)
  {
    // ADR-016 §4: spawning into a world with no live grid spins one up. An
    // undock is ships arriving, and the door does not care which side it was
    // opened from.
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    registry.Tick(++tick); // Nothing left on the grid, nobody watching: it goes.
    Assert::IsNull(registry.Peek(station));

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
    registry.Tick(++tick);
    Assert::IsNotNull(registry.Peek(station), L"the fleet had somewhere to arrive");
  }

  TEST_METHOD(AnUndockedFleetParksItselfOffTheDoorway)
  {
    /*
     * ADR-017 §4. The moment undocked ships exist the world files a
     * system-issued move to a berth on the parking ring -- a real order group,
     * so the ETA, the drawn lane, the straggler deadline and player override
     * all come free.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
    registry.Tick(++tick);

    const World* world = registry.Peek(station);
    Assert::IsNotNull(world);

    const OrderGroup* parking = nullptr;
    for (const OrderGroup& group : world->Groups())
    {
      if (group.systemIssued)
      {
        parking = &group;
      }
    }
    Assert::IsNotNull(parking, L"nothing parked the fleet");
    Assert::AreEqual<std::uint32_t>(1, parking->memberCount);
    Assert::AreEqual<std::uint32_t>(ship, parking->members[0]);

    const DirectX::XMFLOAT2& berth = parking->legs[parking->legCount - 1].anchorMetres;
    const float distance = std::sqrt(berth.x * berth.x + berth.y * berth.y);
    const bool onARing = std::abs(distance - PARKING_RING_METRES[0]) < 1.0f ||
                         std::abs(distance - PARKING_RING_METRES[1]) < 1.0f;
    Assert::IsTrue(onARing, L"a berth is a point on one of the two rings, not somewhere convenient");

    // Both rings are inside the dock radius, so a parked fleet re-docks without
    // moving first -- which is the reason those two numbers were chosen.
    Assert::IsTrue(distance < static_cast<float>(DOCK_RADIUS_METRES));
  }

  TEST_METHOD(TwoFleetsUndockingTheSameTickParkInDifferentPlaces)
  {
    /*
     * The clause the whole berth design turns on (ADR-017 §4): a candidate is
     * refused when another group's **final-leg anchor** already sits inside its
     * bounding circle. That is what lets two same-tick undocks pick different
     * berths with *no reserved-berth state to store or hash* -- a berth is
     * taken exactly when live positions or live intentions say so.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId first = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId second = AddShip(registry, station, -200.0f, 0.0f);
    const ShipId both[] = {first, second};
    DockAndLand(registry, station, both, tick);

    const ShipId one[] = {first};
    const ShipId two[] = {second};
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, one)).accepted);
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, two)).accepted);
    registry.Tick(++tick);

    const World* world = registry.Peek(station);
    Assert::IsNotNull(world);

    DirectX::XMFLOAT2 berths[2]{};
    std::uint32_t found = 0;
    for (const OrderGroup& group : world->Groups())
    {
      if (group.systemIssued && found < 2)
      {
        berths[found++] = group.legs[group.legCount - 1].anchorMetres;
      }
    }
    Assert::AreEqual<std::uint32_t>(2, found, L"both fleets should have been parked");

    const float dx = berths[0].x - berths[1].x;
    const float dy = berths[0].y - berths[1].y;
    Assert::IsTrue(std::sqrt(dx * dx + dy * dy) > 1.0f, L"two fleets were sent to the same berth");
  }

  TEST_METHOD(AFullRingHoldsAtTheUndockPointRatherThanRefusing)
  {
    /*
     * ADR-017 §4's last sentence, and it is a design position rather than an
     * edge case: **undocking is never refused for clutter**. When all 24
     * candidates are taken the fleet holds at the undock point, protection
     * still ticking, separation keeping it honest.
     *
     * The ring is filled by parking a hull on every candidate -- the same 24
     * points the scan visits, computed here from §4's own description. If the
     * two ever disagree this test fails, which is the point of restating it.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];
    const Anchor* anchor = universe.FindAnchor(station);
    Assert::IsNotNull(anchor);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId ship = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId fleet[] = {ship};
    DockAndLand(registry, station, fleet, tick);

    const float undockX = Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->undockPoint.x));
    const float undockY = Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->undockPoint.y));
    const float baseBearing = std::atan2(undockY, undockX);
    constexpr float BEARING_STEP = DirectX::XM_2PI / static_cast<float>(PARKING_BEARINGS);

    for (const float ring : PARKING_RING_METRES)
    {
      for (std::uint32_t step = 0; step < PARKING_BEARINGS; ++step)
      {
        const std::uint32_t stepsOut = (step + 1) / 2;
        const float side = (step % 2 == 0) ? 1.0f : -1.0f;
        const float bearing = baseBearing + static_cast<float>(stepsOut) * side * BEARING_STEP;
        (void)AddShip(registry, station, std::cos(bearing) * ring, std::sin(bearing) * ring);
      }
    }

    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
    registry.Tick(++tick);

    const World* world = registry.Peek(station);
    Assert::IsNotNull(world);
    for (const OrderGroup& group : world->Groups())
    {
      Assert::IsFalse(group.systemIssued, L"a full ring means hold here, not park anyway");
    }

    // And it is still there, protected, rather than refused out of existence.
    Assert::IsTrue(world->IsProtected(ship, tick));
    bool present = false;
    for (const ShipId id : world->Ids())
    {
      present = present || id == ship;
    }
    Assert::IsTrue(present, L"undocking is never refused for clutter");
  }

  TEST_METHOD(TheEventRecordSaysWhatHappenedWithoutBeingPartOfIt)
  {
    /*
     * ADR-018 D19. One producer behind four designed surfaces -- the toast
     * backlog and its UNREAD count, REVIEW LOSSES, the reconnect away-log, and
     * the strategic feed -- built now, with one consumer, because four
     * consumers of four producers would be four bugs about the same ten events.
     *
     * And it is **outside** the hash, which is the other half of the design: an
     * event describes something the simulation already did, so folding the
     * description in as well would make a replay depend on how talkative the
     * build was.
     */
    const UniverseDef universe = SmallUniverse();
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;
    const ShipId first = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId second = AddShip(registry, station, -200.0f, 0.0f);
    const ShipId fleet[] = {first, second};
    DockAndLand(registry, station, fleet, tick);

    Assert::AreEqual<std::size_t>(1, registry.Events().Entries().size(), L"one line per fleet, not one per hull");
    Assert::IsTrue(registry.Events().Entries()[0].kind == EventKind::Docked);
    Assert::AreEqual<std::uint32_t>(station, registry.Events().Entries()[0].anchor);
    Assert::AreEqual<std::uint32_t>(2, registry.Events().Entries()[0].count);

    StationCommand assign;
    assign.orderSeq = 5;
    assign.verb = StationVerb::AssignWing;
    assign.station = station;
    assign.wing = 4;
    Assert::IsTrue(assign.AddShip(first));
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, assign).accepted);

    const std::uint64_t before = registry.Hash();
    Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(station, fleet)).accepted);
    registry.Tick(++tick);

    const std::span<const EventEntry> entries = registry.Events().Entries();
    Assert::AreEqual<std::size_t>(3, entries.size());
    Assert::IsTrue(entries[1].kind == EventKind::WingAssigned);
    Assert::IsTrue(entries[2].kind == EventKind::Undocked);
    Assert::AreEqual<std::uint32_t>(2, entries[2].count);
    Assert::AreEqual<std::uint32_t>(0, registry.Events().Dropped());

    // The events are not what changed the hash -- the ships and the roster are.
    Assert::AreNotEqual(before, registry.Hash());
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
      registry.Reset(&universe, nullptr, Config());
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
      Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, assign).accepted);

      const ShipId back[] = {a, b};
      Assert::IsTrue(registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, Undock(stations[0], back, FormationId::Wedge)).accepted);
      registry.Tick(++tick);
      registry.Tick(++tick);
      registry.Tick(++tick);
      return registry.Hash();
    };

    Assert::AreEqual(run(), run(), L"the same script has to produce the same universe");
  }
};

TEST_CLASS(WarpTests)
{
public:
  TEST_METHOD(AWarpSpoolsWhereItStandsAndThenIsSimplyGone)
  {
    /*
     * ADR-016 §5's shape, in one scenario: the fleet holds while it spools --
     * cancellable, visible, still where the player left it -- and then leaves
     * the world entirely until it arrives. Nowhere in between, which is what
     * makes the in-flight bus part of the hash rather than a detail.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    const ShipId ship = AddShip(registry, anchors[0], 300.0f, 0.0f);
    const ShipId fleet[] = {ship};
    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);

    // Spooling: still here, and still standing where it was told to wait.
    registry.Tick(++tick);
    Assert::IsTrue(OnGrid(registry, anchors[0], ship), L"a spooling fleet has not left yet");

    const std::uint32_t left = TickUntil(registry, tick, 2000, [&] { return !OnGrid(registry, anchors[0], ship); });
    Assert::IsTrue(left < 2000, L"the spool never finished");

    AnchorId where = INVALID_ID;
    Assert::IsFalse(registry.LocationOf(ship, where), L"a fleet in transit is nowhere");
    Assert::IsTrue(registry.PendingTransferCount() > 0, L"and its crossing is on the bus");

    const std::uint32_t arrived = TickUntil(registry, tick, 40000, [&] { return OnGrid(registry, anchors[1], ship); });
    Assert::IsTrue(arrived > left, L"the crossing took no time at all");
    Assert::IsTrue(OnGrid(registry, anchors[1], ship), L"the fleet never arrived");
    Assert::IsTrue(registry.LocationOf(ship, where));
    Assert::AreEqual<std::uint32_t>(anchors[1], where);
  }

  TEST_METHOD(TheSlowestMemberSetsThePaceForEverybody)
  {
    /*
     * A fleet arrives together, so it leaves together and travels together --
     * which means the heaviest hull in the selection decides both the spool and
     * the transit. Asserted as a *comparison* rather than against a number, so
     * retuning the class table does not rewrite the test.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    const auto crossingTicks = [&universe, &anchors](HullClass _class)
    {
      WorldRegistry registry;
      registry.Reset(&universe, nullptr, Config());
      std::uint32_t tick = 0;

      ShipSpawn spawn;
      spawn.hullClass = _class;
      spawn.wing = 1;
      spawn.xMetres = 300.0f;
      const ShipId ship = registry.Spawn(anchors[0], spawn, Neuron::SOLE_PLAYER_ID);
      const ShipId fleet[] = {ship};
      Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);
      return TickUntil(registry, tick, 60000, [&] { return OnGrid(registry, anchors[1], ship); });
    };

    const std::uint32_t light = crossingTicks(HullClass::Interceptor);
    const std::uint32_t heavy = crossingTicks(HullClass::Battleship);
    Assert::IsTrue(heavy > light, L"a battleship spools longer and travels slower than an interceptor");
  }

  TEST_METHOD(AReplacingOrderCancelsASpoolThatHasNotFinished)
  {
    /*
     * ADR-016 §8 refuses a program of verbs, so "cancel the warp" is "tell them
     * to do something else" -- and the group table already makes that work. The
     * point of this test is that the machinery *is* the machinery: nothing was
     * built for cancellation.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Battleship; // The longest spool, so there is time to change one's mind.
    spawn.wing = 1;
    spawn.xMetres = 300.0f;
    const ShipId ship = registry.Spawn(anchors[0], spawn, Neuron::SOLE_PLAYER_ID);
    const ShipId fleet[] = {ship};

    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);
    registry.Tick(++tick);

    OrderSubmit move;
    move.orderSeq = 2;
    Assert::IsTrue(move.AddShip(ship));
    move.target.xCm = Neuron::MetresToCentimetres(1200.0f);
    Assert::IsTrue(registry.Borrow(anchors[0])->SubmitOrder(move).accepted);

    // Long enough for the original spool to have finished twice over.
    for (std::uint32_t step = 0; step < 800; ++step)
    {
      registry.Tick(++tick);
    }

    Assert::IsTrue(OnGrid(registry, anchors[0], ship), L"the replaced warp took the fleet anyway");
    Assert::IsFalse(OnGrid(registry, anchors[1], ship));
  }

  TEST_METHOD(WarpingSomewhereThisGridCannotReachIsRefused)
  {
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    const ShipId ship = AddShip(registry, anchors[0], 300.0f, 0.0f);
    const ShipId fleet[] = {ship};

    const OrderVerdict nowhere = SubmitWarp(registry, anchors[0], 60000, fleet);
    Assert::IsFalse(nowhere.accepted);
    Assert::IsTrue(nowhere.reason == OrderReason::UnknownAnchor);

    // And warping to where you already are is not offered either.
    const OrderVerdict itself = SubmitWarp(registry, anchors[0], anchors[0], fleet);
    Assert::IsFalse(itself.accepted);
    Assert::IsTrue(itself.reason == OrderReason::UnknownAnchor);
  }

  TEST_METHOD(ArrivalPutsEveryShipOnItsOwnStationAndNoneInContact)
  {
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);
    const Anchor* destination = universe.FindAnchor(anchors[1]);
    Assert::IsNotNull(destination);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    std::vector<ShipId> fleet;
    for (std::uint32_t index = 0; index < 6; ++index)
    {
      ShipSpawn spawn;
      spawn.hullClass = index % 2 == 0 ? HullClass::Frigate : HullClass::Corvette;
      spawn.wing = 1;
      spawn.xMetres = 200.0f * static_cast<float>(index);
      fleet.push_back(registry.Spawn(anchors[0], spawn, Neuron::SOLE_PLAYER_ID));
    }
    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);
    TickUntil(registry, tick, 60000, [&] { return OnGrid(registry, anchors[1], fleet[0]); });

    const World* world = registry.Peek(anchors[1]);
    Assert::IsNotNull(world);

    const float warpInX = Neuron::CentimetresToMetres(static_cast<std::int32_t>(destination->warpInPoint.x));
    const float warpInY = Neuron::CentimetresToMetres(static_cast<std::int32_t>(destination->warpInPoint.y));

    /*
     * The formation's own reach, plus the room the anchor reserves for arrival
     * contention (ADR-018 D18, N4).
     *
     * The fleet is centred on the authored point *plus this crossing's slot on
     * the arrival ring*, so "near its anchor" is now the authored point plus a
     * radius the anchor itself declares. Written as a sum rather than as a
     * bigger number so it stays the same claim: a six-ship formation lands
     * together and near where it was sent, on a grid 20 km across.
     */
    const float nearEnough = 4000.0f + Neuron::CentimetresToMetres(destination->arrivalSpreadRadiusCm);

    std::uint32_t found = 0;
    for (std::size_t slot = 0; slot < world->Ids().size(); ++slot)
    {
      if (std::find(fleet.begin(), fleet.end(), world->Ids()[slot]) == fleet.end())
      {
        continue;
      }
      ++found;
      const float dx = world->Positions()[slot].x - warpInX;
      const float dy = world->Positions()[slot].y - warpInY;
      Assert::IsTrue(std::sqrt(dx * dx + dy * dy) < nearEnough, L"a six-ship formation solves near its anchor");
    }
    Assert::AreEqual<std::uint32_t>(6, found, L"the whole fleet arrived, and the same ships");

    // No pair in contact, which is what the formation spacing guarantees and
    // ADR-015's separation would otherwise have to fix on arrival.
    for (std::size_t a = 0; a < world->Ids().size(); ++a)
    {
      for (std::size_t b = a + 1; b < world->Ids().size(); ++b)
      {
        const float dx = world->Positions()[a].x - world->Positions()[b].x;
        const float dy = world->Positions()[a].y - world->Positions()[b].y;
        const float contact = ShipClass(static_cast<HullClass>(world->Classes()[a])).collisionRadiusMetres +
                              ShipClass(static_cast<HullClass>(world->Classes()[b])).collisionRadiusMetres;
        Assert::IsTrue(std::sqrt(dx * dx + dy * dy) > contact, L"two arrivals landed on top of each other");
      }
    }
  }

  TEST_METHOD(NoCrossingIsShorterThanTheFloorASecondHostWouldNeed)
  {
    /*
     * ADR-019 §4b. A transit's apply tick is seconds in the future and the
     * destination host has until then to receive the record -- which turns
     * latency into slack instead of a race, but only if the slack is
     * guaranteed. There is one host, so nothing needs this yet, and that is
     * exactly when a timing table gets tuned under a floor without anybody
     * noticing.
     *
     * The nearest pair of anchors in a system is the case that would breach it,
     * so that is the case this asks about.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Interceptor; // The shortest spool and the fastest crossing.
    spawn.wing = 1;
    spawn.xMetres = 300.0f;
    const ShipId ship = registry.Spawn(anchors[0], spawn, Neuron::SOLE_PLAYER_ID);
    const ShipId fleet[] = {ship};
    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);

    const std::uint32_t left = TickUntil(registry, tick, 2000, [&] { return !OnGrid(registry, anchors[0], ship); });
    const std::uint32_t arrived = TickUntil(registry, tick, 60000, [&] { return OnGrid(registry, anchors[1], ship); });

    Assert::IsTrue(arrived - left >= TRANSFER_FLOOR_TICKS,
                   L"a crossing shorter than the floor leaves a second host no time to receive it");
  }

  TEST_METHOD(EveryShipArrivesOnTheStationSolvedForItsOwnId)
  {
    /*
     * `SolveFormation` assigns stations by **ascending ship id** (Formation.h
     * says so, and the client's footprint preview relies on it), which is not
     * the order a transfer record happens to list its members in. Pairing the
     * two by index would put each ship on somebody else's station: a fleet that
     * arrives in the right shape with the wrong ships in it -- and no "did it
     * land near the anchor" or "is anything in contact" test would notice,
     * because both survive a permutation.
     *
     * So this asks the only question that catches it: solve the same formation
     * independently and check each ship is where *its own* station is.
     *
     * **Relative to where the fleet actually landed, since N4** (ADR-018 D18).
     * The arrival point is the authored one plus an offset this crossing's own
     * record decides, so the absolute answer is no longer a thing this test can
     * compute -- and it never wanted to. A permutation survives a translation
     * and dies here either way: the offset is one vector for the whole
     * crossing, so recovering it from one ship and holding the other four to it
     * is the same question asked in the frame the fleet arrived in.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);
    const Anchor* destination = universe.FindAnchor(anchors[1]);
    Assert::IsNotNull(destination);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    // Descending ids in the order the order names them, so index-pairing and
    // id-pairing cannot agree by accident.
    std::vector<ShipId> spawned;
    for (std::uint32_t index = 0; index < 5; ++index)
    {
      ShipSpawn spawn;
      spawn.hullClass = HullClass::Corvette;
      spawn.wing = 1;
      spawn.xMetres = 150.0f * static_cast<float>(index);
      spawned.push_back(registry.Spawn(anchors[0], spawn, Neuron::SOLE_PLAYER_ID));
    }
    std::vector<ShipId> reversed{spawned.rbegin(), spawned.rend()};
    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], reversed).accepted);
    TickUntil(registry, tick, 60000, [&] { return OnGrid(registry, anchors[1], spawned[0]); });

    const World* world = registry.Peek(anchors[1]);
    Assert::IsNotNull(world);

    // The same solve, run here.
    struct Lookup
    {
      static HullClass Of(ShipId, void*) noexcept { return HullClass::Corvette; }
    };
    FormationStation expected[MAX_SHIPS_PER_ORDER];
    const DirectX::XMFLOAT2 arrival{
      Neuron::CentimetresToMetres(static_cast<std::int32_t>(destination->warpInPoint.x)),
      Neuron::CentimetresToMetres(static_cast<std::int32_t>(destination->warpInPoint.y))};
    const std::uint32_t placed =
      SolveFormation(FormationId::Line, std::span<const ShipId>{reversed}, &Lookup::Of, nullptr, arrival,
                     Neuron::HeadingToRadians(destination->warpInFacingTurns16),
                     std::span<FormationStation>{expected});
    Assert::AreEqual<std::uint32_t>(5, placed);

    const auto slotOf = [world](ShipId _ship, std::uint32_t& _outSlot)
    {
      for (std::size_t candidate = 0; candidate < world->Ids().size(); ++candidate)
      {
        if (world->Ids()[candidate] == _ship)
        {
          _outSlot = static_cast<std::uint32_t>(candidate);
          return true;
        }
      }
      return false;
    };

    // The crossing's own offset, recovered from the first station rather than
    // recomputed: what this test is about is which ship stands where, and the
    // vector is the same one for every member of one record.
    std::uint32_t firstSlot = 0;
    Assert::IsTrue(slotOf(expected[0].shipId, firstSlot), L"a ship in the order never arrived");
    const float offsetX = world->Positions()[firstSlot].x - expected[0].positionMetres.x;
    const float offsetY = world->Positions()[firstSlot].y - expected[0].positionMetres.y;
    // The ring's radius is the whole offset, so this is an equality with slack
    // for two independent formation solves rather than a bound with room in it.
    Assert::IsTrue(std::sqrt(offsetX * offsetX + offsetY * offsetY) <
                     Neuron::CentimetresToMetres(destination->arrivalSpreadRadiusCm) + 10.0f,
                   L"the fleet landed outside the room its anchor reserves for arrivals");

    for (std::uint32_t index = 0; index < placed; ++index)
    {
      std::uint32_t slot = 0;
      Assert::IsTrue(slotOf(expected[index].shipId, slot), L"a ship in the order never arrived");

      const float dx = world->Positions()[slot].x - (expected[index].positionMetres.x + offsetX);
      const float dy = world->Positions()[slot].y - (expected[index].positionMetres.y + offsetY);
      Assert::IsTrue(std::sqrt(dx * dx + dy * dy) < 1.0f, L"a ship landed on another ship's station");
    }
  }

  TEST_METHOD(ConcurrentWarpsInBothDirectionsReproduceBitForBit)
  {
    /*
     * U3a's acceptance. Two fleets crossing in opposite directions at the same
     * time is the scenario that breaks a bus whose order is not total: the
     * records interleave, both grids spin up and tear down, and the hash has to
     * agree anyway.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    const auto run = [&universe, &anchors]()
    {
      WorldRegistry registry;
      registry.Reset(&universe, nullptr, Config());
      std::uint32_t tick = 0;

      const ShipId here = AddShip(registry, anchors[0], 300.0f, 0.0f);
      const ShipId there = AddShip(registry, anchors[1], -300.0f, 200.0f);
      const ShipId outbound[] = {here};
      const ShipId inbound[] = {there};

      Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], outbound).accepted);
      Assert::IsTrue(SubmitWarp(registry, anchors[1], anchors[0], inbound).accepted);

      std::vector<std::uint64_t> hashes;
      for (std::uint32_t step = 0; step < 2000; ++step)
      {
        registry.Tick(++tick);
        if (step % 250 == 0)
        {
          hashes.push_back(registry.Hash());
        }
      }
      hashes.push_back(registry.Hash());
      return hashes;
    };

    const std::vector<std::uint64_t> first = run();
    const std::vector<std::uint64_t> second = run();
    Assert::AreEqual(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index)
    {
      Assert::AreEqual(first[index], second[index], L"two runs of one script disagreed mid-crossing");
    }
  }

  /*
   * --- arrival contention (ADR-018 D18, N4) ---------------------------------
   *
   * `Anchor::arrivalSpreadRadiusCm` was baked by U1, parsed, folded into the
   * universe hash -- and read by nothing. Every crossing was placed on the raw
   * `warpInPoint`, so two fleets warping to one hub on one tick were laid on
   * top of each other and pushed apart by ADR-015 separation afterwards: the
   * stacking D18 exists to prevent, resolved by the mechanism that is meant to
   * be its backstop.
   *
   * Two slices each believed the other had it. U1's note said "the rule itself
   * is U3a's"; U3a's note said "Still owed by U3a: nothing."
   */

  /// Where the ship with this id is standing, or false if it is not here.
  [[nodiscard]] static bool PositionOf(const WorldRegistry& _registry, AnchorId _anchor, ShipId _ship, DirectX::XMFLOAT2& _outAt)
  {
    const World* world = _registry.Peek(_anchor);
    if (world == nullptr)
    {
      return false;
    }
    const std::span<const ShipId> ids = world->Ids();
    for (std::size_t slot = 0; slot < ids.size(); ++slot)
    {
      if (ids[slot] == _ship)
      {
        _outAt = world->Positions()[slot];
        return true;
      }
    }
    return false;
  }

  TEST_METHOD(TwoFleetsWarpingToOneAnchorDoNotArriveOnTheSamePoint)
  {
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);
    const Anchor* destination = universe.FindAnchor(anchors[1]);
    Assert::IsNotNull(destination);
    Assert::IsTrue(destination->arrivalSpreadRadiusCm > 0, L"the fixture's destination reserves no room to spread into");

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    // Two fleets, filed separately, so the bus carries two records with two
    // counters -- which is the whole of what the offset is a function of.
    const ShipId first = AddShip(registry, anchors[0], 300.0f, 0.0f, 1);
    const ShipId second = AddShip(registry, anchors[0], -300.0f, 0.0f, 2);
    const ShipId firstFleet[] = {first};
    const ShipId secondFleet[] = {second};
    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], firstFleet).accepted);
    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], secondFleet).accepted);

    DirectX::XMFLOAT2 firstAt{};
    DirectX::XMFLOAT2 secondAt{};
    const std::uint32_t arrived = TickUntil(registry, tick, 40000,
                                            [&]
                                            {
                                              return PositionOf(registry, anchors[1], first, firstAt) &&
                                                     PositionOf(registry, anchors[1], second, secondAt);
                                            });
    Assert::IsTrue(arrived < 40000, L"one of the two crossings never arrived");

    const float warpX = Neuron::CentimetresToMetres(destination->warpInPoint.x);
    const float warpY = Neuron::CentimetresToMetres(destination->warpInPoint.y);
    const float spread = Neuron::CentimetresToMetres(destination->arrivalSpreadRadiusCm);

    const auto distance = [](float _ax, float _ay, float _bx, float _by)
    { return std::sqrt((_ax - _bx) * (_ax - _bx) + (_ay - _by) * (_ay - _by)); };

    /*
     * Apart, and by more than a nudge: consecutive records step most of the way
     * round the ring, so the two are chords apart rather than neighbours. The
     * threshold is deliberately far below what the stride produces (~2.2 km at
     * this radius) -- what is being asserted is that they are not stacked, not
     * that the stride is a particular number.
     */
    Assert::IsTrue(distance(firstAt.x, firstAt.y, secondAt.x, secondAt.y) > 500.0f,
                   L"two fleets arriving at one anchor were placed on top of each other");

    /*
     * And each is on the ring the anchor reserved rather than wherever the
     * arithmetic went. A single-ship fleet solves its formation at the centre,
     * so the slack here is for the solve and not for the offset.
     */
    for (const DirectX::XMFLOAT2& at : {firstAt, secondAt})
    {
      const float fromAuthored = distance(at.x, at.y, warpX, warpY);
      Assert::IsTrue(fromAuthored > 1.0f, L"an arrival was placed on the raw warp-in point, which is what D18 replaces");
      Assert::IsTrue(fromAuthored < spread + 500.0f, L"an arrival wandered outside the room its anchor reserves");
    }
  }

  TEST_METHOD(WhereACrossingArrivesIsAFunctionOfItsRecordAndNotOfChance)
  {
    /*
     * D18's parenthesis, as an assertion: *a function of the transfer record,
     * not randomness*. Two runs of one script have to place the fleet on the
     * same metre, or an arrival is a thing a replay cannot reproduce -- and the
     * registry's own seed is per-world, so an offset drawn from a world's PCG32
     * would have passed every other test in this file and failed this one.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    const auto arrivalPoint = [&universe, &anchors]
    {
      WorldRegistry registry;
      registry.Reset(&universe, nullptr, Config());
      std::uint32_t tick = 0;
      const ShipId ship = AddShip(registry, anchors[0], 300.0f, 0.0f);
      const ShipId fleet[] = {ship};
      Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);

      DirectX::XMFLOAT2 at{};
      const std::uint32_t arrived =
        TickUntil(registry, tick, 40000, [&] { return PositionOf(registry, anchors[1], ship, at); });
      Assert::IsTrue(arrived < 40000, L"the crossing never arrived");
      return at;
    };

    const DirectX::XMFLOAT2 once = arrivalPoint();
    const DirectX::XMFLOAT2 again = arrivalPoint();
    Assert::AreEqual(once.x, again.x, 0.0f, L"two runs of one script arrived at different places");
    Assert::AreEqual(once.y, again.y, 0.0f);
  }
};

/*
 * The gate jump (Build Order U4, ADR-016 §5, §10).
 *
 * A jump is a warp with two differences and no new machinery: it is ordered
 * from the gate rather than from wherever the fleet stands, and it is priced
 * flat rather than by distance. Everything else -- the spool, the bus, the
 * arrival solve, the hash -- is `WarpTests` above, which is the point.
 */
TEST_CLASS(GateJumpTests)
{
public:
  TEST_METHOD(AGateGridSpawnsItsGateAtTheCentreWithItsBakedId)
  {
    /*
     * ADR-016 §10's pattern, and ADR-018 D6a's identity: the structure comes
     * out of the anchor's own block, so a gate grid torn down and recreated
     * holds the same *ship* rather than a new one that looks like it.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> pair = AGatePair(universe);
    const Anchor* anchor = universe.FindAnchor(pair[0]);
    Assert::IsNotNull(anchor);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());

    const World* world = registry.Borrow(pair[0]);
    Assert::IsNotNull(world);
    Assert::AreEqual<std::size_t>(1, world->Ids().size(), L"a gate grid holds its gate and nothing else");
    Assert::AreEqual<std::uint16_t>(static_cast<ShipId>(anchor->occupantIdBase), world->Ids()[0]);
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(HullClass::Gate), world->Classes()[0]);

    // At the grid centre, because the anchor's origin is the gate's universe
    // position -- the grid is anchored on it, the same way a station's is.
    Assert::AreEqual(0.0f, world->Positions()[0].x, 0.001f);
    Assert::AreEqual(0.0f, world->Positions()[0].y, 0.001f);

    // And it never moves, which is what makes it terrain rather than a fleet.
    Assert::AreEqual(0.0f, ShipClass(HullClass::Gate).maxSpeedMetresPerSec);
    Assert::AreEqual(0.0f, ShipClass(HullClass::Gate).warpSpeedMetresPerSec);

    // The grid knows where its gate leads, and no other grid claims to.
    Assert::AreEqual<std::uint16_t>(pair[1], world->JumpAnchor());
    const AnchorId station = StationAnchors(universe, 1)[0];
    const World* stationGrid = registry.Borrow(station);
    Assert::IsNotNull(stationGrid);
    Assert::AreEqual<std::uint16_t>(INVALID_ID, stationGrid->JumpAnchor(),
                                    L"a station grid that offered a jump would be a way out of the system");
  }

  TEST_METHOD(AFleetAtTheGateCrossesToTheSystemOnTheFarSide)
  {
    /*
     * W1's crossing, in one hop: the fleet leaves a grid in one system and
     * arrives on a grid in another, through the same bus a warp uses.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> pair = AGatePair(universe);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    const ShipId ship = AddShip(registry, pair[0], 800.0f, 0.0f); // Inside the jump radius.
    const ShipId fleet[] = {ship};
    Assert::IsTrue(SubmitWarp(registry, pair[0], pair[1], fleet).accepted, L"a fleet at the gate may take it");

    const std::uint32_t left = TickUntil(registry, tick, 2000, [&] { return !OnGrid(registry, pair[0], ship); });
    Assert::IsTrue(left < 2000, L"the spool never finished");

    const std::uint32_t arrived = TickUntil(registry, tick, 4000, [&] { return OnGrid(registry, pair[1], ship); });
    Assert::IsTrue(OnGrid(registry, pair[1], ship), L"the fleet never came out of the gate");

    AnchorId where = INVALID_ID;
    Assert::IsTrue(registry.LocationOf(ship, where));
    Assert::AreEqual<std::uint16_t>(pair[1], where);

    const Anchor* from = universe.FindAnchor(pair[0]);
    const Anchor* to = universe.FindAnchor(pair[1]);
    Assert::IsTrue(from->system != to->system, L"this was not a crossing at all");

    // Flat, and the class table is not consulted for it. Exactly the constant:
    // the record's apply tick is stamped `TransitTicks` after the tick the
    // fleet left on, and the bus applies at the top of that tick.
    Assert::AreEqual<std::uint32_t>(GATE_JUMP_TICKS, arrived - left, L"a jump is priced flat (ADR-016 §5)");
    Assert::IsTrue(GATE_JUMP_TICKS >= TRANSFER_FLOOR_TICKS, L"and still leaves a second host its slack");
  }

  TEST_METHOD(AJumpOrderedFromAcrossTheGridIsRefused)
  {
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> pair = AGatePair(universe);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());

    const ShipId far = AddShip(registry, pair[0], 9000.0f, 0.0f);
    const ShipId fleet[] = {far};

    const OrderVerdict verdict = SubmitWarp(registry, pair[0], pair[1], fleet);
    Assert::IsFalse(verdict.accepted);
    Assert::IsTrue(verdict.reason == OrderReason::NotAtGate);

    // The same fleet, from the same place, may still warp *within* the system:
    // the rule is about crossing a gate and not about where warps start.
    const SolarSystem* system = universe.FindSystem(universe.FindAnchor(pair[0])->system);
    Assert::IsNotNull(system);
    for (const Anchor& anchor : system->anchors)
    {
      if (anchor.id != pair[0])
      {
        Assert::IsTrue(SubmitWarp(registry, pair[0], anchor.id, fleet).accepted,
                       L"an in-system warp does not care where the fleet is standing");
        break;
      }
    }
  }

  TEST_METHOD(TheOnlyWayOutOfASystemIsAGateGrid)
  {
    /*
     * Reachability is per grid, and a gate's far side belongs to the gate's own
     * grid alone. Otherwise a fleet could leave a system from a planet, and the
     * gate would be scenery.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> pair = AGatePair(universe);
    const SystemId here = universe.FindAnchor(pair[0])->system;

    AnchorId nonGate = INVALID_ID;
    for (const Anchor& anchor : universe.FindSystem(here)->anchors)
    {
      if (anchor.kind != AnchorKind::Gate)
      {
        nonGate = anchor.id;
        break;
      }
    }
    Assert::IsTrue(nonGate != INVALID_ID, L"the test system has no anchor that is not a gate");

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    const ShipId ship = AddShip(registry, nonGate, 300.0f, 0.0f);
    const ShipId fleet[] = {ship};

    const OrderVerdict verdict = SubmitWarp(registry, nonGate, pair[1], fleet);
    Assert::IsFalse(verdict.accepted);
    Assert::IsTrue(verdict.reason == OrderReason::UnknownAnchor,
                   L"another system's gate is not somewhere you are far from -- it is not reachable at all");
  }

  TEST_METHOD(TwoGatesAtDifferentMapDistancesCostTheSame)
  {
    /*
     * ADR-016 §5's reason for the flat price: between systems, distance is the
     * strategic map's spacing rather than a journey (ADR-009 §3), so charging
     * for it would charge the player for a number nobody chose to mean
     * anything. Two crossings of very different map lengths, one number.
     */
    const UniverseDef universe = SmallUniverse();

    // Every gate pair in the universe, with the plane distance between them.
    std::vector<std::pair<std::int64_t, std::vector<AnchorId>>> crossings;
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Gate)
        {
          continue;
        }
        const AnchorId paired = universe.PairedGateAnchor(anchor.id);
        const Anchor* far = paired == INVALID_ID ? nullptr : universe.FindAnchor(paired);
        if (far == nullptr)
        {
          continue;
        }
        // Shifted before squaring: raw universe deltas overflow int64 (the
        // same reason the bake shifts, `UniverseGen`'s DistanceSquared).
        const std::int64_t dx = (far->origin.x - anchor.origin.x) >> 20;
        const std::int64_t dy = (far->origin.y - anchor.origin.y) >> 20;
        crossings.emplace_back(dx * dx + dy * dy, std::vector<AnchorId>{anchor.id, paired});
      }
    }
    Assert::IsTrue(crossings.size() >= 2, L"the test universe has too few gates to compare");
    std::sort(crossings.begin(), crossings.end(), [](const auto& _a, const auto& _b) { return _a.first < _b.first; });
    Assert::IsTrue(crossings.back().first > crossings.front().first, L"every gate pair is the same length apart");

    const auto crossingTicks = [&universe](const std::vector<AnchorId>& _pair)
    {
      WorldRegistry registry;
      registry.Reset(&universe, nullptr, Config());
      std::uint32_t tick = 0;
      const ShipId ship = AddShip(registry, _pair[0], 800.0f, 0.0f);
      const ShipId fleet[] = {ship};
      Assert::IsTrue(SubmitWarp(registry, _pair[0], _pair[1], fleet).accepted);
      const std::uint32_t left = TickUntil(registry, tick, 2000, [&] { return !OnGrid(registry, _pair[0], ship); });
      const std::uint32_t arrived = TickUntil(registry, tick, 4000, [&] { return OnGrid(registry, _pair[1], ship); });
      return arrived - left;
    };

    Assert::AreEqual(crossingTicks(crossings.front().second), crossingTicks(crossings.back().second),
                     L"the map's spacing priced a jump");
  }

  TEST_METHOD(ACrossingReproducesItselfBitForBit)
  {
    /*
     * The replay claim, asked of the one path that leaves a system. A jump
     * moves ships between two worlds through the bus, and the bus is in the
     * hash -- so a script that jumped and a re-run of it must agree at every
     * checkpoint, including the ticks where the fleet is in neither world.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> pair = AGatePair(universe);

    const auto run = [&universe, &pair]
    {
      WorldRegistry registry;
      registry.Reset(&universe, nullptr, Config(0x9A7Eu));
      std::uint32_t tick = 0;
      const ShipId ship = AddShip(registry, pair[0], 800.0f, 0.0f);
      const ShipId fleet[] = {ship};
      Assert::IsTrue(SubmitWarp(registry, pair[0], pair[1], fleet).accepted);

      std::vector<std::uint64_t> hashes;
      for (std::uint32_t step = 0; step < 600; ++step)
      {
        registry.Tick(++tick);
        if (step % 20 == 0)
        {
          hashes.push_back(registry.Hash());
        }
      }
      return hashes;
    };

    const std::vector<std::uint64_t> first = run();
    const std::vector<std::uint64_t> second = run();
    Assert::AreEqual(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index)
    {
      Assert::AreEqual(first[index], second[index], L"two runs of one crossing disagreed");
    }
  }
};

TEST_CLASS(FleetSummaryTests)
{
public:
  TEST_METHOD(ASummaryAnswersWhereEverythingIsWithoutLookingAtAnyOfIt)
  {
    /*
     * ADR-016 §6. A fleet is emergent -- your ships sharing a location -- so
     * this is a count grouped by place rather than a record of an entity, and
     * that is why there is nothing to keep in step with the snapshot.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);
    const AnchorId station = StationAnchors(universe, 1)[0];

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    (void)AddShip(registry, anchors[0], 100.0f, 0.0f);
    (void)AddShip(registry, anchors[0], -100.0f, 0.0f);

    const std::vector<FleetSummary> onGrid = registry.Summaries(Neuron::SOLE_PLAYER_ID);
    const auto here = std::find_if(onGrid.begin(), onGrid.end(),
                                   [&](const FleetSummary& _row) { return _row.anchor == anchors[0]; });
    Assert::IsTrue(here != onGrid.end(), L"two ships on a grid should be one row");
    Assert::IsTrue(here->state == FleetState::OnGrid);
    Assert::AreEqual<std::uint32_t>(2, here->shipCount);
    Assert::AreEqual<std::uint32_t>(FLEET_ETA_NONE, here->etaSeconds);

    // A station standing on its own grid is not a fleet.
    const ShipId visitor = AddShip(registry, station, 200.0f, 0.0f);
    const ShipId docking[] = {visitor};
    DockAndLand(registry, station, docking, tick);

    /*
     * By anchor *and* state, because one station anchor can legitimately carry
     * both rows -- ships standing on its grid and ships inside it. The first
     * version of this test looked up by anchor alone and found whichever came
     * first, which is a test that passes for the wrong reason.
     */
    const std::vector<FleetSummary> docked = registry.Summaries(Neuron::SOLE_PLAYER_ID);
    const auto row = std::find_if(docked.begin(), docked.end(), [&](const FleetSummary& _r)
                                  { return _r.anchor == station && _r.state == FleetState::Docked; });
    Assert::IsTrue(row != docked.end(), L"the docked ship should be a row of its own");
    Assert::AreEqual<std::uint32_t>(1, row->shipCount, L"the station itself must not read as a docked fleet");
  }

  TEST_METHOD(AFleetInTransitCarriesTheEtaNoGridCould)
  {
    /*
     * The number U3a owed and could not give: a fleet mid-crossing is in no
     * world, so no grid's order records can carry its ETA. The summary can,
     * because it is a fact about the *bus*.
     */
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    WorldRegistry registry;
    registry.Reset(&universe, nullptr, Config());
    std::uint32_t tick = 0;

    const ShipId ship = AddShip(registry, anchors[0], 300.0f, 0.0f);
    const ShipId fleet[] = {ship};
    Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);
    TickUntil(registry, tick, 2000, [&] { return !OnGrid(registry, anchors[0], ship); });

    const std::vector<FleetSummary> crossing = registry.Summaries(Neuron::SOLE_PLAYER_ID);
    const auto row = std::find_if(crossing.begin(), crossing.end(),
                                  [](const FleetSummary& _r) { return _r.state == FleetState::InTransit; });
    Assert::IsTrue(row != crossing.end(), L"a fleet in transit still has to be reportable");
    Assert::AreEqual<std::uint32_t>(anchors[1], row->anchor,
                                    L"where it is going, because where it is is nowhere");
    Assert::AreEqual<std::uint32_t>(1, row->shipCount);
    Assert::IsTrue(row->etaSeconds != FLEET_ETA_NONE && row->etaSeconds > 0, L"an ETA of none is not an ETA");

    // And it counts down rather than standing still.
    const std::uint16_t first = row->etaSeconds;
    for (std::uint32_t step = 0; step < 200; ++step)
    {
      registry.Tick(++tick);
    }
    const std::vector<FleetSummary> later = registry.Summaries(Neuron::SOLE_PLAYER_ID);
    const auto again = std::find_if(later.begin(), later.end(),
                                    [](const FleetSummary& _r) { return _r.state == FleetState::InTransit; });
    if (again != later.end())
    {
      Assert::IsTrue(again->etaSeconds < first, L"the estimate never moved");
    }
  }

  TEST_METHOD(TheSameShardProducesTheSameSummaryMessageTwice)
  {
    // Ordered by anchor and then state, so a client can diff two of them
    // without sorting and two runs of one script produce the same bytes.
    const UniverseDef universe = SmallUniverse();
    const std::vector<AnchorId> anchors = TwoAnchorsInOneSystem(universe);

    const auto run = [&universe, &anchors]()
    {
      WorldRegistry registry;
      registry.Reset(&universe, nullptr, Config());
      std::uint32_t tick = 0;
      (void)AddShip(registry, anchors[1], 50.0f, 0.0f);
      const ShipId ship = AddShip(registry, anchors[0], 300.0f, 0.0f);
      const ShipId fleet[] = {ship};
      Assert::IsTrue(SubmitWarp(registry, anchors[0], anchors[1], fleet).accepted);
      TickUntil(registry, tick, 2000, [&] { return !OnGrid(registry, anchors[0], ship); });

      std::uint8_t buffer[2048];
      Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
      const std::vector<FleetSummary> rows = registry.Summaries(Neuron::SOLE_PLAYER_ID);
      Assert::IsTrue(WriteFleetSummaries(rows, writer));
      return std::vector<std::uint8_t>{buffer, buffer + writer.BytesWritten()};
    };

    const std::vector<std::uint8_t> first = run();
    const std::vector<std::uint8_t> second = run();
    Assert::IsTrue(first == second, L"two runs of one script produced different summaries");
    Assert::IsTrue(first.size() > FleetSummariesBytes(0), L"the message is empty, so it proves nothing");
  }

  TEST_METHOD(TheSummaryMessageSurvivesTheTripAndRefusesAHostileCount)
  {
    const FleetSummary rows[] = {FleetSummary{7, FleetState::OnGrid, 12, FLEET_ETA_NONE},
                                 FleetSummary{7, FleetState::Docked, 3, FLEET_ETA_NONE},
                                 FleetSummary{9, FleetState::InTransit, 41, 87}};

    std::uint8_t buffer[256];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    Assert::IsTrue(WriteFleetSummaries(rows, writer));
    Assert::AreEqual(FleetSummariesBytes(3), writer.BytesWritten());

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    std::vector<FleetSummary> received;
    Assert::IsTrue(ReadFleetSummaries(reader, received));
    Assert::AreEqual<std::size_t>(3, received.size());
    Assert::AreEqual<std::uint32_t>(9, received[2].anchor);
    Assert::IsTrue(received[2].state == FleetState::InTransit);
    Assert::AreEqual<std::uint32_t>(41, received[2].shipCount);
    Assert::AreEqual<std::uint32_t>(87, received[2].etaSeconds);

    // A count past the cap is refused before a single row is read.
    std::uint8_t hostile[8];
    Neuron::ByteWriter bad{std::span<std::uint8_t>{hostile}};
    bad.WriteUInt16(65535);
    Neuron::ByteReader badReader{std::span<const std::uint8_t>{hostile, bad.BytesWritten()}};
    Assert::IsFalse(ReadFleetSummaries(badReader, received));
  }

  TEST_METHOD(EveryStateIsNamed)
  {
    // A roster block with no word on it is a block the player cannot read.
    for (const FleetState state : {FleetState::OnGrid, FleetState::Docked, FleetState::InTransit})
    {
      Assert::IsNotNull(FleetStateName(state));
      Assert::IsTrue(std::string_view{FleetStateName(state)}.size() > 0);
    }
  }
};

TEST_CLASS(StationWireTests)
{
public:
  TEST_METHOD(ACommandSurvivesTheTripUnchanged)
  {
    StationCommand sent;
    sent.orderSeq = 91;
    sent.verb = StationVerb::Undock;
    sent.station = 4321;
    sent.formation = FormationId::Claw;
    sent.wing = 5;
    for (std::uint16_t index = 0; index < MAX_SHIPS_PER_ORDER; ++index)
    {
      Assert::IsTrue(sent.AddShip(static_cast<ShipId>(1000 + index)));
    }

    std::uint8_t buffer[MAX_STATION_COMMAND_BYTES];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    Assert::IsTrue(WriteStationCommand(sent, writer));
    Assert::AreEqual(StationCommandBytes(MAX_SHIPS_PER_ORDER), writer.BytesWritten(),
                     L"the size function and the writer have to agree, or the decode bound is a guess");

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    StationCommand received;
    Assert::IsTrue(ReadStationCommand(reader, received));

    Assert::AreEqual(sent.orderSeq, received.orderSeq);
    Assert::IsTrue(sent.verb == received.verb);
    Assert::AreEqual<std::uint32_t>(sent.station, received.station);
    Assert::IsTrue(sent.formation == received.formation);
    Assert::AreEqual<std::uint32_t>(sent.wing, received.wing);
    Assert::AreEqual<std::uint32_t>(sent.shipCount, received.shipCount);
    for (std::uint16_t index = 0; index < sent.shipCount; ++index)
    {
      Assert::AreEqual<std::uint32_t>(sent.shipIds[index], received.shipIds[index]);
    }
  }

  TEST_METHOD(AnUnknownVerbDecodesAndIsRefusedRatherThanCalledMalformed)
  {
    /*
     * The order family's rule, applied here: a decoder that rejected a verb it
     * did not recognise would answer "protocol error" where the client is owed
     * a bounce with a reason. Decoding succeeds; validation is what refuses.
     */
    StationCommand sent;
    sent.orderSeq = 1;
    sent.station = 7;
    Assert::IsTrue(sent.AddShip(3));

    std::uint8_t buffer[MAX_STATION_COMMAND_BYTES];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    Assert::IsTrue(WriteStationCommand(sent, writer));
    buffer[4] = 200; // The verb byte.

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    StationCommand received;
    Assert::IsTrue(ReadStationCommand(reader, received), L"a strange verb is still a well-formed message");
    Assert::IsTrue(static_cast<std::uint8_t>(received.verb) == 200);
  }

  TEST_METHOD(AShipIdPastTheOldWindowIsItselfRatherThanSomebodyElse)
  {
    /*
     * **The narrowing this used to assert is gone, and its absence is the
     * claim** (ADR-018 D6, ADR-022 §8a, U3d-b).
     *
     * These messages spoke u32 while the sim's `ShipId` was u16, so a reader
     * had to refuse an id past the window rather than cast it: 65,537 truncated
     * is ship 1, and that is a validated command against the wrong hull. The
     * two widths agree now, so the honest assertion is the opposite one -- an
     * id past the old window arrives **as itself**.
     *
     * Worth keeping rather than deleting, because it is the test that would
     * fail loudly if the widths ever diverged again, which is when the range
     * check has to come back.
     */
    std::uint8_t buffer[MAX_STATION_COMMAND_BYTES];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    writer.WriteUInt32(1);
    writer.WriteUInt8(static_cast<std::uint8_t>(StationVerb::Undock));
    writer.WriteUInt16(7);
    writer.WriteUInt8(static_cast<std::uint8_t>(FormationId::Line));
    writer.WriteUInt8(0);
    writer.WriteUInt8(static_cast<std::uint8_t>(OreId::FerroChroma)); // E3's ore byte.
    writer.WriteUInt32(0);                                            // ...and its unit count.
    writer.WriteUInt8(static_cast<std::uint8_t>(AlloyId::FerrocitePlates)); // E4b's alloy byte.
    writer.WriteUInt32(0);                                                  // ...and its job sequence.
    writer.WriteUInt16(1);
    writer.WriteUInt32(65537);
    Assert::IsTrue(writer.Ok());

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    StationCommand received;
    Assert::IsTrue(ReadStationCommand(reader, received));
    Assert::AreEqual<std::uint32_t>(65537, received.shipIds[0], L"an id past the old u16 window was not itself");
    Assert::AreNotEqual<std::uint32_t>(1, received.shipIds[0], L"and above all, not ship 1");
  }

  TEST_METHOD(ACountPastTheCapIsRefusedBeforeAnythingIsRead)
  {
    std::uint8_t buffer[MAX_STATION_COMMAND_BYTES];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    writer.WriteUInt32(1);
    writer.WriteUInt8(0);
    writer.WriteUInt16(7);
    writer.WriteUInt8(0);
    writer.WriteUInt8(0);
    writer.WriteUInt8(static_cast<std::uint8_t>(OreId::FerroChroma));
    writer.WriteUInt32(0);
    writer.WriteUInt8(static_cast<std::uint8_t>(AlloyId::FerrocitePlates));
    writer.WriteUInt32(0);
    writer.WriteUInt16(65535); // Sixty-five thousand ships in one command.
    Assert::IsTrue(writer.Ok());

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    StationCommand received;
    Assert::IsFalse(ReadStationCommand(reader, received));
  }

  TEST_METHOD(ARosterSurvivesTheTripAndRefusesAClassNobodyCanDraw)
  {
    const RosterEntry docked[] = {RosterEntry{11, HullClass::Battleship, 2},
                                  RosterEntry{12, HullClass::Interceptor, 2},
                                  RosterEntry{13, HullClass::Hauler, 0}};

    std::uint8_t buffer[512];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    Assert::IsTrue(WriteStationRoster(77, docked, writer));
    Assert::AreEqual(StationRosterBytes(3), writer.BytesWritten());

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    AnchorId station = INVALID_ID;
    std::vector<RosterEntry> received;
    Assert::IsTrue(ReadStationRoster(reader, station, received));
    Assert::AreEqual<std::uint32_t>(77, station);
    Assert::AreEqual<std::size_t>(3, received.size());
    Assert::AreEqual<std::uint32_t>(11, received[0].shipId);
    Assert::IsTrue(received[0].hullClass == HullClass::Battleship);
    Assert::AreEqual<std::uint32_t>(2, received[0].wing);

    // A class this build has no row for is refused here, unlike an unknown
    // verb: a roster is an answer rather than a request, so there is no verdict
    // to carry a reason back on, and a row nobody can draw would be drawn wrong.
    buffer[2 + 2 + 4] = 200;
    Neuron::ByteReader bad{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    Assert::IsFalse(ReadStationRoster(bad, station, received));
  }
};

/*
 * The envelope ADR-016 §6's family travels in (ADR-018 A13).
 *
 * One engine wire type carries every member, so the kind byte is the game's and
 * these are the tests for it. What is worth pinning is not that a byte survives
 * a round trip but the two decisions the format rests on: that a frame is
 * **self-delimiting without length prefixes**, because every body already
 * carries its own row count, and that an unrecognised kind is **refused rather
 * than skipped**, because skipping one would hide a schema disagreement the
 * handshake exists to catch.
 */
TEST_CLASS(SummaryFrameTests)
{
public:
  TEST_METHOD(TwoFamilyMembersShareOneFrameAndCanBeToldApart)
  {
    const FleetSummary summaries[] = {FleetSummary{11, FleetState::OnGrid, 5, FLEET_ETA_NONE},
                                      FleetSummary{11, FleetState::Docked, 3, FLEET_ETA_NONE},
                                      FleetSummary{42, FleetState::InTransit, 8, 97}};
    const RosterEntry docked[] = {RosterEntry{101, HullClass::Carrier, 2}, RosterEntry{102, HullClass::Frigate, 2}};

    std::uint8_t buffer[1024];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    Assert::IsTrue(BeginSummaryFrame(2, writer));
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::FleetSummaries, writer));
    Assert::IsTrue(WriteFleetSummaries(summaries, writer));
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::StationRoster, writer));
    Assert::IsTrue(WriteStationRoster(11, docked, writer));

    // The arithmetic a caller has to be able to do before it writes, because it
    // is what decides how many records fit one datagram.
    const std::size_t expected = SUMMARY_FRAME_HEADER_BYTES + SUMMARY_RECORD_HEADER_BYTES + FleetSummariesBytes(3) +
                                 SUMMARY_RECORD_HEADER_BYTES + StationRosterBytes(2);
    Assert::AreEqual(expected, writer.BytesWritten());

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer, writer.BytesWritten()}};
    std::uint8_t records = 0;
    Assert::IsTrue(ReadSummaryFrame(reader, records));
    Assert::AreEqual<std::uint32_t>(2, records);

    SummaryKind kind{};
    std::vector<FleetSummary> readSummaries;
    Assert::IsTrue(ReadSummaryRecord(reader, kind));
    Assert::IsTrue(kind == SummaryKind::FleetSummaries);
    Assert::IsTrue(ReadFleetSummaries(reader, readSummaries));
    Assert::AreEqual<std::size_t>(3, readSummaries.size());
    Assert::AreEqual<std::uint32_t>(97, readSummaries[2].etaSeconds, L"the transit row's ETA");

    AnchorId station = INVALID_ID;
    std::vector<RosterEntry> readDocked;
    Assert::IsTrue(ReadSummaryRecord(reader, kind));
    Assert::IsTrue(kind == SummaryKind::StationRoster);
    Assert::IsTrue(ReadStationRoster(reader, station, readDocked));
    Assert::AreEqual<std::uint32_t>(11, station);
    Assert::AreEqual<std::size_t>(2, readDocked.size());

    // The claim that lets the format drop length prefixes: each body says where
    // it ends, so two of them back to back consume the frame exactly.
    Assert::IsTrue(reader.FullyConsumed(), L"the frame is self-delimiting");
  }

  TEST_METHOD(AnUnknownKindIsRefusedRatherThanSkipped)
  {
    // A frame promising one record, whose kind this build does not define. A
    // decoder that skipped it would turn a version mismatch into a screen that
    // is quietly missing a station.
    const std::uint8_t frame[] = {1, 99};
    Neuron::ByteReader reader{std::span<const std::uint8_t>{frame}};
    std::uint8_t records = 0;
    Assert::IsTrue(ReadSummaryFrame(reader, records));
    Assert::AreEqual<std::uint32_t>(1, records);

    SummaryKind kind{};
    Assert::IsFalse(ReadSummaryRecord(reader, kind));
  }

  TEST_METHOD(ACountPastTheCapIsRefusedBeforeAnyRecordIsTouched)
  {
    // Both directions of the same rule: nothing is written for a count that
    // cannot be honoured, and nothing is read for one that was not.
    std::uint8_t buffer[64];
    Neuron::ByteWriter writer{std::span<std::uint8_t>{buffer}};
    Assert::IsFalse(BeginSummaryFrame(MAX_SUMMARY_RECORDS + 1, writer));
    Assert::AreEqual<std::size_t>(0, writer.BytesWritten());

    const std::uint8_t hostile[] = {200};
    Neuron::ByteReader reader{std::span<const std::uint8_t>{hostile}};
    std::uint8_t records = 0;
    Assert::IsFalse(ReadSummaryFrame(reader, records));
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
    registry.Reset(&universe, nullptr, Config());

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
    registry.Reset(&universe, nullptr, Config());
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
    registry.Reset(&universe, nullptr, Config());
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
      registry.Reset(&universe, nullptr, Config());
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
    registry.Reset(&universe, nullptr, Config());
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
    registry.Reset(&universe, nullptr, Config());
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
    registry.Reset(&universe, nullptr, Config());
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
    registry.Reset(&universe, nullptr, Config());
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
    ticked.Reset(&universe, nullptr, Config());
    ticked.AddViewer(station); // Held alive, or teardown would do this test's job for it.
    for (std::uint32_t tick = 1; tick <= 200; ++tick)
    {
      ticked.Tick(tick);
    }

    WorldRegistry recreated;
    recreated.Reset(&universe, nullptr, Config());
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
    registry.Reset(&universe, nullptr, Config());

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
    unwatched.Reset(&universe, nullptr, Config());
    unwatched.Tick(1);

    WorldRegistry watched;
    watched.Reset(&universe, nullptr, Config());
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
      registry.Reset(&universe, nullptr, Config());
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
      registry.Reset(&universe, nullptr, Config(_seed));
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
    registry.Reset(&universe, nullptr, Config());

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
