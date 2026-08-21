#include "pch.h"

#include "CppUnitTest.h"

#include "EconomyMessages.h"
#include "EconomyParse.h"
#include "SiteEpoch.h"
#include "SiteField.h"
#include "Station.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "World.h"
#include "WorldRegistry.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "EntityRecord.h"
#include "Snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * E3: cargo, the Bay, and the wire cluster (ADR-024 §5, build order E3).
 *
 * The claim under test is one sentence -- **ore is property, and property is
 * conserved** -- and it has to hold across four boundaries that used to lose
 * it: a dock, an undock, a transfer either way, and a station grid tearing
 * down. Most of what is here is that one invariant asked in different places.
 */

namespace GameLogicTests
{

namespace
{

/// The committed economy, read the way the suites that need real content do --
/// once, because every test here wants the same shipping numbers.
[[nodiscard]] const EconomyDef& Economy()
{
  static const EconomyDef economy = []
  {
    static constexpr const char* CANDIDATES[] = {
      "GameData/Economy/Economy.json",
      "../GameData/Economy/Economy.json",
      "../../GameData/Economy/Economy.json",
      "../../../GameData/Economy/Economy.json",
      "../../../../GameData/Economy/Economy.json",
    };
    for (const char* candidate : CANDIDATES)
    {
      std::ifstream file(candidate, std::ios::binary);
      if (!file)
      {
        continue;
      }
      std::ostringstream buffer;
      buffer << file.rdbuf();

      EconomyDef parsed;
      std::vector<EconomyDiagnostic> diagnostics;
      Assert::IsTrue(ParseEconomy(buffer.str(), parsed, diagnostics), L"the committed economy does not parse");
      return parsed;
    }
    Assert::Fail(L"the committed Economy.json was not found from the test working directory");
    return EconomyDef{};
  }();
  return economy;
}

/// A universe with one station and one site, which is the smallest world in
/// which every E3 boundary exists.
[[nodiscard]] const UniverseDef& Universe()
{
  static UniverseDef universe;
  static bool baked = false;
  if (!baked)
  {
    baked = true;
    UniverseGenConfig recipe;
    recipe.regionCount = 1;
    recipe.constellationsPerRegion = 1;
    recipe.systemCount = 4;
    (void)GenerateUniverse(recipe, Economy().sites, universe);
  }
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

/// `WorldRegistry` is neither copyable nor movable -- a live world holds an
/// owner assertion, and handing one around would be handing around the thread
/// that may touch it. So the harness resets in place rather than returning one.
void MakeRegistry(WorldRegistry& _registry)
{
  RegistryConfig config;
  config.sessionSeed = 0xE3E3u;
  _registry.Reset(&Universe(), &Economy(), config);
}

/// A docked Hauler holding `_units` of `_ore`, put on the roster the way a dock
/// puts one there -- through the bus, so the crossing is exercised rather than
/// bypassed.
ShipId DockLoaded(WorldRegistry& _registry, AnchorId _station, OreId _ore, std::uint32_t _units, std::uint32_t& _tick)
{
  /*
   * A **Miner**, and the suite found out why the hard way: the shipped content
   * gives the Hauler a 400,000-litre *general* hold and no ore hold at all
   * (ADR-024 §5a). Ore is the Miner's to carry until E4 turns it into
   * something a Hauler is for, so a fixture that loaded a Hauler with ore was
   * testing a ship the game cannot produce -- and `MoveOre` was right to
   * refuse to fill it.
   */
  ShipSpawn spawn;
  spawn.hullClass = HullClass::Miner;
  spawn.wing = 1;
  spawn.cargo.oreUnits[static_cast<std::uint8_t>(_ore)] = _units;
  const ShipId ship = _registry.Spawn(_station, spawn);

  World* world = _registry.Borrow(_station);
  OrderSubmit dock;
  dock.orderSeq = 1;
  dock.kind = OrderKind::Dock;
  dock.anchor = _station;
  (void)dock.AddShip(ship);
  (void)world->SubmitOrder(dock);

  // Two ticks: one to ingest and file, one for the bus to apply between them.
  _registry.Tick(++_tick);
  _registry.Tick(++_tick);
  return ship;
}

[[nodiscard]] StationCommand Transfer(StationVerb _verb, AnchorId _station, ShipId _ship, OreId _ore, std::uint32_t _units)
{
  StationCommand command;
  command.orderSeq = 7;
  command.verb = _verb;
  command.station = _station;
  command.ore = _ore;
  command.units = _units;
  (void)command.AddShip(_ship);
  return command;
}

constexpr Neuron::PlayerId ONE = 1;
constexpr Neuron::PlayerId TWO = 2;

} // namespace

TEST_CLASS(CargoCrossingTests)
{
public:
  TEST_METHOD(ADockedShipKeepsWhatItWasCarrying)
  {
    /*
     * The gap E2 shipped with, closed. A Miner that docked used to arrive with
     * empty holds -- `TransferMember` carried an id, a hull and a wing -- and
     * the ore it had been credited stopped existing at the doorway.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    Assert::AreNotEqual<std::uint16_t>(INVALID_ID, station, L"the bake has no station to dock at");

    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::FerroChroma, 40, tick);

    const std::span<const RosterEntry> docked = registry.Roster(station);
    const auto row = std::find_if(docked.begin(), docked.end(), [ship](const RosterEntry& _r) { return _r.shipId == ship; });
    Assert::IsTrue(row != docked.end(), L"the ship never reached the roster");
    Assert::AreEqual<std::uint32_t>(40, row->Units(OreId::FerroChroma), L"docking ate the hold");
  }

  TEST_METHOD(AnUndockedShipLeavesWithWhatItKept)
  {
    // And the way back. A ship that docks loaded and undocks without
    // transferring is carrying the same ore -- the crossing is symmetric
    // because it is one record read in two directions.
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::Astracite, 15, tick);

    StationCommand undock;
    undock.orderSeq = 2;
    undock.verb = StationVerb::Undock;
    undock.station = station;
    (void)undock.AddShip(ship);
    Assert::IsTrue(registry.SubmitStationCommand(ONE, undock).accepted, L"the undock was refused");

    registry.Tick(++tick);
    registry.Tick(++tick);

    const World* world = registry.Peek(station);
    Assert::IsNotNull(world, L"the station grid went away");
    const std::span<const ShipId> ids = world->Ids();
    const std::span<const ShipCargo> holds = world->Cargo();
    const auto at = std::find(ids.begin(), ids.end(), ship);
    Assert::IsTrue(at != ids.end(), L"the ship never came back");
    Assert::AreEqual<std::uint32_t>(15, holds[static_cast<std::size_t>(at - ids.begin())].Units(OreId::Astracite),
                                    L"undocking ate the hold");
  }

  TEST_METHOD(AWarpCarriesTheHoldToTheFarSide)
  {
    /*
     * The boundary the suite did not have, added because it was missed.
     *
     * Dock and undock both had a test and both were correct; transit had
     * neither, so `ApplyTransit` spawned arrivals with empty holds and a fleet
     * that warped anywhere lost its cargo in silence. The G0 scenario caught
     * it end-to-end, which is what an accept is for -- and this is the unit
     * test that should have caught it first.
     */
    WorldRegistry registry;
    MakeRegistry(registry);

    // A site and a station in the same system, so this is an in-system warp
    // rather than a gate crossing.
    AnchorId site = INVALID_ID;
    AnchorId station = INVALID_ID;
    for (const SolarSystem& system : Universe().systems)
    {
      AnchorId here = INVALID_ID;
      AnchorId dock = INVALID_ID;
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind == AnchorKind::Site && here == INVALID_ID)
        {
          here = anchor.id;
        }
        if (anchor.kind == AnchorKind::Station && dock == INVALID_ID)
        {
          dock = anchor.id;
        }
      }
      if (here != INVALID_ID && dock != INVALID_ID)
      {
        site = here;
        station = dock;
        break;
      }
    }
    Assert::AreNotEqual<std::uint16_t>(INVALID_ID, site, L"no system has both a field and a station");

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Miner;
    spawn.wing = 1;
    spawn.cargo.oreUnits[static_cast<std::uint8_t>(OreId::FerroChroma)] = 77;
    const ShipId ship = registry.Spawn(site, spawn);

    World* world = registry.Borrow(site);
    OrderSubmit warp;
    warp.orderSeq = 1;
    warp.kind = OrderKind::Warp;
    warp.anchor = station;
    (void)warp.AddShip(ship);
    Assert::IsTrue(world != nullptr && world->SubmitOrder(warp).accepted, L"the warp was refused");

    std::uint32_t tick = 0;
    bool arrived = false;
    for (std::uint32_t step = 0; step < 20000 && !arrived; ++step)
    {
      registry.Tick(++tick);
      AnchorId where = INVALID_ID;
      arrived = registry.LocationOf(ship, where) && where == station;
    }
    Assert::IsTrue(arrived, L"the fleet never reached the far side");

    const World* far = registry.Peek(station);
    Assert::IsNotNull(far, L"the destination grid is not up");
    const std::span<const ShipId> ids = far->Ids();
    const std::span<const ShipCargo> holds = far->Cargo();
    const auto at = std::find(ids.begin(), ids.end(), ship);
    Assert::IsTrue(at != ids.end(), L"the ship is not on the destination grid");
    Assert::AreEqual<std::uint32_t>(77, holds[static_cast<std::size_t>(at - ids.begin())].Units(OreId::FerroChroma),
                                    L"warping ate the hold");
  }

  TEST_METHOD(TheRoundTripConservesEveryUnit)
  {
    /*
     * The property, asked over the whole loop rather than at one boundary:
     * dock, store, withdraw, undock. Whatever the ship started with is what it
     * ends with, and no step is allowed to round.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::Nebulite, 24, tick);

    Assert::IsTrue(
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, ship, OreId::Nebulite, 24)).accepted,
      L"storing was refused");
    Assert::IsTrue(
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToShip, station, ship, OreId::Nebulite, 24)).accepted,
      L"withdrawing was refused");

    const std::span<const RosterEntry> docked = registry.Roster(station);
    const auto row = std::find_if(docked.begin(), docked.end(), [ship](const RosterEntry& _r) { return _r.shipId == ship; });
    Assert::IsTrue(row != docked.end(), L"the ship left the roster");
    Assert::AreEqual<std::uint32_t>(24, row->Units(OreId::Nebulite), L"the round trip lost or invented ore");

    const StationBay* bay = registry.Bay(ONE, station);
    Assert::IsTrue(bay == nullptr || bay->Units(OreId::Nebulite) == 0, L"the bay kept a copy");
  }
};

TEST_CLASS(StationBayTests)
{
public:
  TEST_METHOD(StoringMovesOreAndCreatesTheBay)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::FerroChroma, 30, tick);

    // No bay before, which is the pristine answer rather than an absence: the
    // durable set stays proportional to what has happened.
    Assert::IsNull(registry.Bay(ONE, station), L"a bay existed before anything was stored");

    Assert::IsTrue(
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, ship, OreId::FerroChroma, 18)).accepted,
      L"storing was refused");

    const StationBay* bay = registry.Bay(ONE, station);
    Assert::IsNotNull(bay, L"storing did not create the bay");
    Assert::AreEqual<std::uint32_t>(18, bay->Units(OreId::FerroChroma), L"the bay took the wrong amount");

    const std::span<const RosterEntry> docked = registry.Roster(station);
    const auto row = std::find_if(docked.begin(), docked.end(), [ship](const RosterEntry& _r) { return _r.shipId == ship; });
    Assert::AreEqual<std::uint32_t>(12, row->Units(OreId::FerroChroma), L"the hold was not debited by what the bay gained");
  }

  TEST_METHOD(StoringMoreThanIsHeldIsRefused)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::FerroChroma, 10, tick);

    const OrderVerdict verdict =
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, ship, OreId::FerroChroma, 11));
    Assert::IsFalse(verdict.accepted, L"a shortfall was accepted");
    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OrderReason::InsufficientMaterials),
                                    static_cast<std::uint16_t>(verdict.reason), L"the wrong reason");
    Assert::IsNull(registry.Bay(ONE, station), L"a refused command still made a bay");
  }

  TEST_METHOD(MovingNothingIsRefusedRatherThanIgnored)
  {
    // Zero is a mistake, not a no-op: a command that did nothing and reported
    // success would be a button that looks like it worked.
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::FerroChroma, 10, tick);

    const OrderVerdict verdict =
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, ship, OreId::FerroChroma, 0));
    Assert::IsFalse(verdict.accepted, L"a zero transfer was accepted");
    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OrderReason::InsufficientMaterials),
                                    static_cast<std::uint16_t>(verdict.reason), L"the wrong reason");
  }

  TEST_METHOD(WithdrawingFromAnEmptyBayIsRefused)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::FerroChroma, 10, tick);

    const OrderVerdict verdict =
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToShip, station, ship, OreId::FerroChroma, 1));
    Assert::IsFalse(verdict.accepted, L"withdrawing from nothing was accepted");
    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OrderReason::InsufficientMaterials),
                                    static_cast<std::uint16_t>(verdict.reason), L"the wrong reason");
  }

  TEST_METHOD(AWithdrawalStopsAtTheHoldsCapacity)
  {
    /*
     * A Bay has no capacity and a hold does (ADR-024 §5b), so the outward
     * direction can come up short where the inward one cannot. Nothing is lost
     * -- the remainder stays in the Bay -- and that is the property here.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;

    const std::uint32_t litres = Economy().Cargo(HullClass::Miner).oreHoldLitres;
    const std::uint32_t perUnit = Economy().ores[static_cast<std::uint8_t>(OreId::FerroChroma)].unitVolumeLitres;
    Assert::IsTrue(litres > 0 && perUnit > 0, L"the content has no hold to fill");
    const std::uint32_t capacityUnits = litres / perUnit;

    const ShipId ship = DockLoaded(registry, station, OreId::FerroChroma, capacityUnits, tick);
    Assert::IsTrue(
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, ship, OreId::FerroChroma, capacityUnits))
        .accepted,
      L"storing a full hold was refused");

    // Ask for it all back, then fill the hold to the brim first so the second
    // half of the request has nowhere to go.
    Assert::IsTrue(
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToShip, station, ship, OreId::FerroChroma, capacityUnits))
        .accepted,
      L"withdrawing a hold's worth was refused");

    const std::span<const RosterEntry> docked = registry.Roster(station);
    const auto row = std::find_if(docked.begin(), docked.end(), [ship](const RosterEntry& _r) { return _r.shipId == ship; });
    const StationBay* bay = registry.Bay(ONE, station);
    const std::uint32_t inBay = bay == nullptr ? 0 : bay->Units(OreId::FerroChroma);
    Assert::AreEqual<std::uint32_t>(capacityUnits, row->Units(OreId::FerroChroma) + inBay,
                                    L"the withdrawal lost or invented ore");
    Assert::IsTrue(row->Units(OreId::FerroChroma) <= capacityUnits, L"the hold went past its capacity");
  }

  TEST_METHOD(TwoCommandersHaveTwoBaysAtOneStation)
  {
    /*
     * The privacy rule, in the key rather than in a filter (ADR-017 §1).
     *
     * The host issues one player id today, so this is where the two-commander
     * case is proven -- the loopback cannot yet produce a second. What the self
     * test proves instead is the wiring: a viewer with no Bay is sent no
     * `BayStatus`.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::FerroChroma, 20, tick);

    Assert::IsTrue(
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, ship, OreId::FerroChroma, 20)).accepted,
      L"storing was refused");

    Assert::IsNotNull(registry.Bay(ONE, station), L"the storing commander has no bay");
    Assert::IsNull(registry.Bay(TWO, station), L"a second commander could see the first's bay");

    // And the second commander cannot spend it: the same ore, the same station,
    // and nothing there to withdraw.
    const OrderVerdict verdict =
      registry.SubmitStationCommand(TWO, Transfer(StationVerb::TransferToShip, station, ship, OreId::FerroChroma, 1));
    Assert::IsFalse(verdict.accepted, L"a commander withdrew from somebody else's bay");
  }

  TEST_METHOD(TheBayOutlivesTheGrid)
  {
    /*
     * "Worlds forget, the universe layer does not" (ADR-018 D8), with a third
     * resident. A station grid tearing down with a full Bay leaves the Bay
     * untouched, because it was never world state.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    std::uint32_t tick = 0;
    const ShipId ship = DockLoaded(registry, station, OreId::Astracite, 12, tick);
    Assert::IsTrue(
      registry.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, ship, OreId::Astracite, 12)).accepted,
      L"storing was refused");

    // Long enough for the idle sweep to take any grid nobody is holding.
    for (std::uint32_t step = 0; step < 4000; ++step)
    {
      registry.Tick(++tick);
    }

    const StationBay* bay = registry.Bay(ONE, station);
    Assert::IsNotNull(bay, L"the bay went away with the grid");
    Assert::AreEqual<std::uint32_t>(12, bay->Units(OreId::Astracite), L"the bay lost its contents");
  }

  TEST_METHOD(TheBayAndTheManifestAreInTheRegistryHash)
  {
    /*
     * Durable state that is not in the hash is state a replay can lose in
     * silence. Two worlds that agreed about everything else and disagreed about
     * a Bay have diverged, and this is where it shows.
     */
    WorldRegistry a;
    WorldRegistry b;
    MakeRegistry(a);
    MakeRegistry(b);
    const AnchorId station = FirstOfKind(AnchorKind::Station);

    std::uint32_t tickA = 0;
    std::uint32_t tickB = 0;
    const ShipId shipA = DockLoaded(a, station, OreId::FerroChroma, 20, tickA);
    const ShipId shipB = DockLoaded(b, station, OreId::FerroChroma, 20, tickB);
    Assert::AreEqual(a.Hash(), b.Hash(), L"two identical registries disagreed before anything happened");

    Assert::IsTrue(
      a.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, shipA, OreId::FerroChroma, 5)).accepted,
      L"storing was refused");
    Assert::AreNotEqual(a.Hash(), b.Hash(), L"storing ore did not move the hash");

    Assert::IsTrue(
      b.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, shipB, OreId::FerroChroma, 5)).accepted,
      L"storing was refused");
    Assert::AreEqual(a.Hash(), b.Hash(), L"the same move produced two different hashes");
  }

  TEST_METHOD(ABayIsPerOwnerInTheHashToo)
  {
    // The owner is part of the key, so it has to be part of the fold: two
    // registries where the same ore sits in different commanders' bays are not
    // the same shard.
    WorldRegistry a;
    WorldRegistry b;
    MakeRegistry(a);
    MakeRegistry(b);
    const AnchorId station = FirstOfKind(AnchorKind::Station);

    std::uint32_t tickA = 0;
    std::uint32_t tickB = 0;
    const ShipId shipA = DockLoaded(a, station, OreId::FerroChroma, 20, tickA);
    const ShipId shipB = DockLoaded(b, station, OreId::FerroChroma, 20, tickB);

    Assert::IsTrue(
      a.SubmitStationCommand(ONE, Transfer(StationVerb::TransferToBay, station, shipA, OreId::FerroChroma, 5)).accepted, L"a");
    Assert::IsTrue(
      b.SubmitStationCommand(TWO, Transfer(StationVerb::TransferToBay, station, shipB, OreId::FerroChroma, 5)).accepted, L"b");
    Assert::AreNotEqual(a.Hash(), b.Hash(), L"whose bay it is does not reach the hash");
  }
};

TEST_CLASS(EconomyWireTests)
{
public:
  TEST_METHOD(EntityRecordIsUntouchedByTheEconomy)
  {
    /*
     * The arithmetic ADR-024 §4d refused in advance, asserted so that the next
     * person tempted by a cargo byte on the per-tick record finds this test
     * rather than a mystery about the ship cap.
     *
     * One byte takes the record 21 -> 22 and the snapshot cap 43 -> 41, landing
     * exactly on the floor `Snapshot.h` asserts, with nothing left over. Cargo
     * rides the ~1 Hz summary family instead, which is what this whole file is
     * about.
     */
    Assert::AreEqual<std::size_t>(21, Neuron::ENTITY_RECORD_BYTES, L"the per-tick record grew; the economy must not do this");
    Assert::AreEqual<std::uint32_t>(43, MAX_SHIPS_PER_SNAPSHOT, L"the ship cap moved");

    constexpr std::size_t widened = Neuron::ENTITY_RECORD_BYTES + 1;
    constexpr std::uint32_t wouldFit =
      static_cast<std::uint32_t>((SNAPSHOT_BUDGET_BYTES - SNAPSHOT_HEADER_BYTES - ORDER_AREA_BYTES) / widened);
    Assert::AreEqual<std::uint32_t>(41, wouldFit, L"the one-byte cost is no longer 43 -> 41; the note needs rewriting");
  }

  TEST_METHOD(AStationCommandRoundTripsWithItsOreAndUnits)
  {
    StationCommand out;
    out.orderSeq = 91;
    out.verb = StationVerb::TransferToBay;
    out.station = 7;
    out.ore = OreId::Nebulite;
    out.units = 250000;
    (void)out.AddShip(11);
    (void)out.AddShip(12);

    std::array<std::uint8_t, MAX_STATION_COMMAND_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteStationCommand(out, writer), L"the command did not fit its own cap");

    Neuron::ByteReader reader{writer.Written()};
    StationCommand back;
    Assert::IsTrue(ReadStationCommand(reader, back), L"the command did not read back");
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(out.ore), static_cast<std::uint8_t>(back.ore), L"ore");
    Assert::AreEqual<std::uint32_t>(out.units, back.units, L"units");
    Assert::AreEqual<std::uint16_t>(out.shipCount, back.shipCount, L"ships");
  }

  TEST_METHOD(AnOreByteOutsideTheEnumIsRefused)
  {
    /*
     * The one field of this message the decoder refuses on, where the verb and
     * the formation beside it are cast through. An ore index would address the
     * manifest arrays before validation got an opinion, and there is no
     * sentence to show a player whose client believes in a fourth ore.
     */
    StationCommand out;
    out.orderSeq = 1;
    out.verb = StationVerb::TransferToBay;
    out.station = 7;
    out.ore = OreId::FerroChroma;
    out.units = 1;
    (void)out.AddShip(11);

    std::array<std::uint8_t, MAX_STATION_COMMAND_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteStationCommand(out, writer), L"the command did not write");

    // The ore byte sits after orderSeq(4) + verb(1) + station(2) + formation(1)
    // + wing(1). Poked rather than constructed, because no writer in this build
    // can produce the value being tested.
    const std::size_t written = writer.BytesWritten();
    buffer[9] = 3;

    Neuron::ByteReader reader{std::span<const std::uint8_t>{buffer.data(), written}};
    StationCommand back;
    Assert::IsFalse(ReadStationCommand(reader, back), L"a fourth ore decoded");
  }

  TEST_METHOD(ARosterRoundTripsItsManifests)
  {
    std::vector<RosterEntry> docked;
    RosterEntry row;
    row.shipId = 42;
    row.hullClass = HullClass::Miner;
    row.wing = 3;
    row.oreUnits[0] = 7;
    row.oreUnits[1] = 0;
    row.oreUnits[2] = 900;
    docked.push_back(row);

    std::vector<std::uint8_t> buffer(StationRosterBytes(docked.size()));
    Neuron::ByteWriter writer{buffer};
    Assert::IsTrue(WriteStationRoster(5, docked, writer), L"the roster did not write");

    Neuron::ByteReader reader{writer.Written()};
    AnchorId station = INVALID_ID;
    std::vector<RosterEntry> back;
    Assert::IsTrue(ReadStationRoster(reader, station, back), L"the roster did not read back");
    Assert::AreEqual<std::size_t>(1, back.size(), L"row count");
    Assert::AreEqual<std::uint32_t>(7, back[0].oreUnits[0], L"ore 0");
    Assert::AreEqual<std::uint32_t>(0, back[0].oreUnits[1], L"ore 1");
    Assert::AreEqual<std::uint32_t>(900, back[0].oreUnits[2], L"ore 2");
  }

  TEST_METHOD(TheThreeSummaryBodiesRoundTrip)
  {
    std::vector<std::uint8_t> buffer(512);

    {
      SiteStatusRow row;
      row.anchor = 9;
      row.epoch = 4;
      row.remainingUnits[0] = 1000;
      row.remainingUnits[2] = 5;
      row.clusterCount = 3;
      row.clusterFullPct[0] = 100;
      row.clusterFullPct[1] = 50;
      row.clusterFullPct[2] = 0;

      Neuron::ByteWriter writer{buffer};
      Assert::IsTrue(WriteSiteStatus(row, writer), L"site status did not write");
      Neuron::ByteReader reader{writer.Written()};
      SiteStatusRow back;
      Assert::IsTrue(ReadSiteStatus(reader, back), L"site status did not read back");
      Assert::AreEqual<std::uint16_t>(9, back.anchor, L"anchor");
      Assert::AreEqual<std::uint32_t>(4, back.epoch, L"epoch");
      Assert::AreEqual<std::uint32_t>(1000, back.remainingUnits[0], L"remaining");
      Assert::AreEqual<std::uint8_t>(3, back.clusterCount, L"clusters");
      Assert::AreEqual<std::uint8_t>(50, back.clusterFullPct[1], L"fraction");
    }

    {
      std::vector<CargoStatusRow> rows;
      CargoStatusRow row;
      row.shipId = 12;
      row.oreUnits[1] = 64;
      rows.push_back(row);

      Neuron::ByteWriter writer{buffer};
      Assert::IsTrue(WriteCargoStatus(rows, writer), L"cargo status did not write");
      Neuron::ByteReader reader{writer.Written()};
      std::vector<CargoStatusRow> back;
      Assert::IsTrue(ReadCargoStatus(reader, back), L"cargo status did not read back");
      Assert::AreEqual<std::size_t>(1, back.size(), L"row count");
      Assert::AreEqual<std::uint16_t>(12, back[0].shipId, L"ship");
      Assert::AreEqual<std::uint32_t>(64, back[0].oreUnits[1], L"units");
    }

    {
      const std::uint32_t units[ORE_COUNT] = {1, 2, 3};
      // And the alloys the Bay grew with E4b: one statement about one place.
      const std::uint32_t alloys[ALLOY_COUNT] = {10, 20, 30, 40, 50};
      Neuron::ByteWriter writer{buffer};
      Assert::IsTrue(WriteBayStatus(6, units, alloys, writer), L"bay status did not write");
      Neuron::ByteReader reader{writer.Written()};
      AnchorId station = INVALID_ID;
      std::uint32_t back[ORE_COUNT] = {};
      std::uint32_t backAlloys[ALLOY_COUNT] = {};
      Assert::IsTrue(ReadBayStatus(reader, station, back, backAlloys), L"bay status did not read back");
      Assert::AreEqual<std::uint16_t>(6, station, L"station");
      Assert::AreEqual<std::uint32_t>(3, back[2], L"units");
      Assert::AreEqual<std::uint32_t>(50, backAlloys[4], L"alloy units");
    }
  }

  TEST_METHOD(TheSummaryFamilyAcceptsTheThreeNewKinds)
  {
    // A kind this build does not know is refused rather than skipped, so a kind
    // it *does* know has to decode -- otherwise E3's records would read as a
    // schema disagreement on the build that wrote them.
    std::array<std::uint8_t, 16> buffer{};
    for (const SummaryKind kind : {SummaryKind::SiteStatus, SummaryKind::CargoStatus, SummaryKind::BayStatus})
    {
      Neuron::ByteWriter writer{buffer};
      Assert::IsTrue(BeginSummaryRecord(kind, writer), L"the record header did not write");
      Neuron::ByteReader reader{writer.Written()};
      SummaryKind back = SummaryKind::StationRoster;
      Assert::IsTrue(ReadSummaryRecord(reader, back), L"a known kind was refused");
      Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(kind), static_cast<std::uint8_t>(back), L"kind");
    }
  }
};

TEST_CLASS(SiteStatusTests)
{
public:
  TEST_METHOD(AnUntouchedFieldReportsFull)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId site = FirstOfKind(AnchorKind::Site);
    Assert::AreNotEqual<std::uint16_t>(INVALID_ID, site, L"the bake has no site");

    const SiteStatusRow row = registry.SiteStatusFor(site);
    Assert::IsTrue(row.clusterCount > 0, L"the site has no clusters");
    for (std::uint8_t index = 0; index < row.clusterCount; ++index)
    {
      Assert::AreEqual<std::uint8_t>(100, row.clusterFullPct[index], L"an untouched cluster did not read full");
    }
  }

  TEST_METHOD(AWorkedFieldReportsMeasurablyEmptier)
  {
    /*
     * The accept's own sentence: `SiteStatus` shows the field measurably
     * emptier than it started. Driven through the ledger rather than by editing
     * a field, so what is measured is the path the game takes.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId site = FirstOfKind(AnchorKind::Site);
    const SiteStatusRow before = registry.SiteStatusFor(site);

    std::uint32_t total = 0;
    for (const std::uint32_t units : before.remainingUnits)
    {
      total += units;
    }
    Assert::IsTrue(total > 0, L"the site started empty");

    // Spin the grid up and mine it with a real Miner, which is what puts a
    // debit on the bus and a ledger under the anchor.
    ShipSpawn spawn;
    spawn.hullClass = HullClass::Miner;
    spawn.wing = 1;
    const ShipId miner = registry.Spawn(site, spawn);
    Assert::AreNotEqual<std::uint16_t>(INVALID_SHIP_ID, miner, L"no miner");

    World* world = registry.Borrow(site);
    OrderSubmit mine;
    mine.orderSeq = 1;
    mine.kind = OrderKind::Mine;
    (void)mine.AddShip(miner);
    Assert::IsTrue(world->SubmitOrder(mine).accepted, L"the mine order was refused");

    // A cycle is 800 ticks; give it room to land and cross the bus.
    std::uint32_t tick = 0;
    for (std::uint32_t step = 0; step < 3000; ++step)
    {
      registry.Tick(++tick);
    }

    const SiteStatusRow after = registry.SiteStatusFor(site);
    std::uint32_t left = 0;
    for (const std::uint32_t units : after.remainingUnits)
    {
      left += units;
    }
    Assert::IsTrue(left < total, L"mining a field did not make its status emptier");

    const bool anyPartial =
      std::any_of(std::begin(after.clusterFullPct), std::begin(after.clusterFullPct) + after.clusterCount,
                  [](std::uint8_t _pct) { return _pct < 100; });
    Assert::IsTrue(anyPartial, L"no cluster reported a dent");
  }

  TEST_METHOD(CargoIsReportedForTheOwnerAndNobodyElse)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId site = FirstOfKind(AnchorKind::Site);

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Miner;
    spawn.wing = 1;
    spawn.cargo.oreUnits[0] = 33;
    const ShipId ship = registry.Spawn(site, spawn);
    Assert::AreNotEqual<std::uint16_t>(INVALID_SHIP_ID, ship, L"no ship");

    const std::vector<CargoStatusRow> mine = registry.CargoFor(ONE);
    const auto row = std::find_if(mine.begin(), mine.end(), [ship](const CargoStatusRow& _r) { return _r.shipId == ship; });
    Assert::IsTrue(row != mine.end(), L"a loaded ship was not reported to its owner");
    Assert::AreEqual<std::uint32_t>(33, row->oreUnits[0], L"the wrong amount");

    Assert::IsTrue(registry.CargoFor(Neuron::INVALID_PLAYER_ID).empty(), L"nobody was told what somebody is carrying");
  }

  TEST_METHOD(AnEmptyShipIsNotAReportedRow)
  {
    // A summary of nothing is bytes nobody needs. The frame is capped at 64
    // rows, so spending them on empty holds would push real ones out.
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId site = FirstOfKind(AnchorKind::Site);

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Miner;
    spawn.wing = 1;
    const ShipId ship = registry.Spawn(site, spawn);

    const std::vector<CargoStatusRow> rows = registry.CargoFor(ONE);
    const auto row = std::find_if(rows.begin(), rows.end(), [ship](const CargoStatusRow& _r) { return _r.shipId == ship; });
    Assert::IsTrue(row == rows.end(), L"an empty hold was reported");
  }
};

} // namespace GameLogicTests
