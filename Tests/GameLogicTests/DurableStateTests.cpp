#include "pch.h"

#include "CppUnitTest.h"

#include "DurableState.h"
#include "EconomyParse.h"
#include "Orders.h"
#include "Station.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "World.h"
#include "WorldRegistry.h"

#include "ByteReader.h"
#include "ByteWriter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * E4a: the durable store, GameLogic's half (ADR-025, build order E4a).
 *
 * Two claims, and they are different claims. **The format is honest**: what
 * goes in comes out, and what is malformed is diagnosed rather than guessed at.
 * **The line is where ADR-025 §1 drew it**: identity and location survive a
 * restart and intention does not, which is why a reloaded fleet holds position
 * with an empty queue and why the reload proof is `DurableHash` rather than
 * `WorldRegistry::Hash`.
 */

namespace GameLogicTests
{

namespace
{

/// The committed economy, read once -- the pattern the other suites that need
/// real content use, for the same reason.
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

/*
 * The committed economy with the clock wound in, and a universe with sites in
 * it -- the pair `MiningTests` uses, for the same reason: a cycle is forty
 * seconds and a hold is four hundred units, which is a fine soak and a poor
 * test.
 */
[[nodiscard]] const EconomyDef& FastEconomy()
{
  static const EconomyDef economy = []
  {
    EconomyDef fast = Economy();
    fast.mining.cycleSeconds = 1;
    return fast;
  }();
  return economy;
}

[[nodiscard]] const UniverseDef& SiteUniverse()
{
  static UniverseDef universe;
  static bool baked = false;
  if (!baked)
  {
    baked = true;
    UniverseGenConfig recipe;
    recipe.regionCount = 2;
    recipe.constellationsPerRegion = 2;
    recipe.systemCount = 12;
    (void)GenerateUniverse(recipe, FastEconomy().sites, universe);
  }
  return universe;
}

[[nodiscard]] AnchorId FirstSite()
{
  for (const SolarSystem& system : SiteUniverse().systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind == AnchorKind::Site)
      {
        return anchor.id;
      }
    }
  }
  Assert::Fail(L"the baked universe has no mining sites");
  return INVALID_ID;
}

void MakeSiteRegistry(WorldRegistry& _registry)
{
  RegistryConfig config;
  config.sessionSeed = 0xE4Bu;
  _registry.Reset(&SiteUniverse(), &FastEconomy(), config);
}

void MakeRegistry(WorldRegistry& _registry)
{
  RegistryConfig config;
  config.sessionSeed = 0xE4Au;
  _registry.Reset(&Universe(), &Economy(), config);
}

/// A ship on a grid, loaded, so the round trip has a manifest to carry.
ShipId SpawnLoaded(WorldRegistry& _registry, AnchorId _anchor, HullClass _hull, float _x, float _y, OreId _ore,
                   std::uint32_t _units)
{
  ShipSpawn spawn;
  spawn.hullClass = _hull;
  spawn.wing = 2;
  spawn.xMetres = _x;
  spawn.yMetres = _y;
  spawn.headingRadians = 0.75f;
  spawn.cargo.oreUnits[static_cast<std::uint8_t>(_ore)] = _units;
  const ShipId ship = _registry.Spawn(_anchor, spawn);
  Assert::AreNotEqual<std::uint16_t>(INVALID_SHIP_ID, ship, L"the fixture could not put a ship on a grid");
  return ship;
}

/// Docks a loaded Miner the way a dock actually happens -- through the bus.
ShipId DockLoaded(WorldRegistry& _registry, AnchorId _station, OreId _ore, std::uint32_t _units, std::uint32_t& _tick)
{
  const ShipId ship = SpawnLoaded(_registry, _station, HullClass::Miner, 300.0f, -120.0f, _ore, _units);

  World* world = _registry.Borrow(_station);
  OrderSubmit dock;
  dock.orderSeq = 1;
  dock.kind = OrderKind::Dock;
  dock.anchor = _station;
  (void)dock.AddShip(ship);
  (void)world->SubmitOrder(dock);

  _registry.Tick(++_tick);
  _registry.Tick(++_tick);
  return ship;
}

/// The bytes a registry writes, as a vector, because every test here wants to
/// read them back and half of them want to damage them first.
[[nodiscard]] std::vector<std::uint8_t> WriteTo(const WorldRegistry& _registry)
{
  std::vector<std::uint8_t> bytes(1u << 20);
  Neuron::ByteWriter writer(bytes);
  Assert::IsTrue(WriteDurableState(_registry, writer), L"the durable state did not fit a megabyte");
  bytes.resize(writer.BytesWritten());
  return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> WriteTo(const DurableState& _state)
{
  std::vector<std::uint8_t> bytes(1u << 20);
  Neuron::ByteWriter writer(bytes);
  Assert::IsTrue(WriteDurableState(_state, writer), L"the durable state did not fit a megabyte");
  bytes.resize(writer.BytesWritten());
  return bytes;
}

/// One of everything, built by hand: the format's subject, without needing a
/// shard to have produced it.
[[nodiscard]] DurableState OneOfEverything()
{
  DurableState state;
  state.shardTick = 4321;
  state.nextDynamicShipId = 40000;

  DurableShip ship;
  ship.shipId = 501;
  ship.anchor = 12;
  ship.hullClass = HullClass::Miner;
  ship.wing = 3;
  ship.owner = Neuron::SOLE_PLAYER_ID;
  ship.xMetres = -1234.5f;
  ship.yMetres = 987.25f;
  ship.headingRadians = 1.5f;
  ship.oreUnits[0] = 17;
  ship.oreUnits[2] = 4;
  state.ships.push_back(ship);

  DurableRoster roster;
  roster.anchor = 7;
  RosterEntry docked;
  docked.shipId = 502;
  docked.hullClass = HullClass::Hauler;
  docked.wing = 1;
  docked.oreUnits[1] = 250;
  roster.docked.push_back(docked);
  state.rosters.push_back(roster);

  StationBay bay;
  bay.owner = 2;
  bay.station = 7;
  bay.oreUnits[0] = 900;
  state.bays.push_back(bay);

  SiteLedger ledger;
  ledger.anchor = 12;
  ledger.epochIndex = 19;
  ledger.layoutSalt = 0xABCDEF01u;
  ledger.clusterCount = 2;
  ledger.remainingUnits[0][0] = 300;
  ledger.remainingUnits[1][2] = 40;
  state.ledgers.push_back(ledger);

  TransferRecord record;
  record.id.host = 0;
  record.id.counter = 9;
  record.applyTick = 4400;
  record.what.kind = TransferKind::Transit;
  record.what.anchor = 12;
  record.what.formation = FormationId::Wedge;
  TransferMember member;
  member.shipId = 503;
  member.hullClass = HullClass::Interceptor;
  member.wing = 2;
  member.oreUnits[0] = 5;
  Assert::IsTrue(record.what.AddMember(member), L"the fixture could not fill a crossing");
  state.transfers.push_back(record);

  return state;
}

} // namespace

TEST_CLASS(DurableFormatTests)
{
public:
  TEST_METHOD(WhatGoesInComesOut)
  {
    const DurableState written = OneOfEverything();
    const std::vector<std::uint8_t> bytes = WriteTo(written);

    Neuron::ByteReader reader(bytes);
    DurableState read;
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(ReadDurableState(reader, read, diagnostics), L"the format could not read what it wrote");
    Assert::AreEqual<std::size_t>(0, diagnostics.size(), L"a clean read reported a problem");
    Assert::IsTrue(reader.FullyConsumed(), L"the reader left bytes behind, so the two halves disagree about a length");

    Assert::AreEqual(DurableHash(written), DurableHash(read), L"the round trip did not reproduce the hash");
    Assert::AreEqual<std::uint32_t>(written.shardTick, read.shardTick);
    Assert::AreEqual<std::uint32_t>(written.nextDynamicShipId, read.nextDynamicShipId);
    Assert::AreEqual<std::size_t>(1, read.ships.size());
    Assert::AreEqual<std::uint32_t>(17, read.ships[0].oreUnits[0], L"a hold did not survive the format");
    Assert::AreEqual<std::uint32_t>(Neuron::SOLE_PLAYER_ID, read.ships[0].owner, L"the reserved owner did not survive");
    Assert::AreEqual<std::size_t>(1, read.rosters.size());
    Assert::AreEqual<std::uint32_t>(250, read.rosters[0].docked[0].oreUnits[1], L"a docked hold did not survive");
    Assert::AreEqual<std::uint32_t>(900, read.bays[0].oreUnits[0], L"a Bay did not survive");
    Assert::AreEqual<std::uint32_t>(0xABCDEF01u, read.ledgers[0].layoutSalt, L"a layout salt did not survive");
    Assert::AreEqual<std::uint16_t>(1, read.transfers[0].what.memberCount, L"a crossing lost its member");
  }

  TEST_METHOD(EveryTruncationDiagnosesRatherThanGuesses)
  {
    /*
     * The `ParseEconomy` posture, applied to bytes: a file that stops early is
     * a diagnostic and never a half-built anything. Every prefix is tried
     * rather than one, because a format is only as honest as its least-guarded
     * length -- and every count in it is written before the things it counts,
     * which is what makes this exhaustive check possible at all.
     */
    const std::vector<std::uint8_t> bytes = WriteTo(OneOfEverything());
    for (std::size_t length = 0; length < bytes.size(); ++length)
    {
      Neuron::ByteReader reader(std::span<const std::uint8_t>(bytes.data(), length));
      DurableState read;
      std::vector<PersistenceDiagnostic> diagnostics;
      Assert::IsFalse(ReadDurableState(reader, read, diagnostics), L"a truncated buffer was accepted");
      Assert::IsTrue(diagnostics.size() >= 1, L"a truncated buffer was refused without saying why");
      Assert::IsTrue(read.ships.empty() && read.rosters.empty() && read.bays.empty() && read.ledgers.empty()
                       && read.transfers.empty(),
                     L"a refused read left something half-built behind");
    }
  }

  TEST_METHOD(SomebodyElsesBytesAreRefused)
  {
    std::vector<std::uint8_t> bytes = WriteTo(OneOfEverything());
    bytes[0] = static_cast<std::uint8_t>(bytes[0] ^ 0xffu);

    Neuron::ByteReader reader(bytes);
    DurableState read;
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsFalse(ReadDurableState(reader, read, diagnostics), L"a foreign file was read as durable state");
    Assert::AreEqual<std::size_t>(1, diagnostics.size());
    Assert::IsTrue(diagnostics[0].message.find("not durable state") != std::string::npos,
                   L"the refusal did not say what it was refusing");
  }

  TEST_METHOD(AnUnknownFormatVersionNamesBothNumbers)
  {
    // ADR-025 §6.1: no silent upgrade path, and the operator's first question
    // is which build wrote it -- so both numbers are in the message or the
    // message is not doing its job.
    std::vector<std::uint8_t> bytes = WriteTo(OneOfEverything());
    bytes[4] = static_cast<std::uint8_t>(DURABLE_FORMAT_VERSION + 1);

    Neuron::ByteReader reader(bytes);
    DurableState read;
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsFalse(ReadDurableState(reader, read, diagnostics), L"an unknown format version was read anyway");
    Assert::AreEqual<std::size_t>(1, diagnostics.size());
    const std::string& message = diagnostics[0].message;
    Assert::IsTrue(message.find(std::to_string(DURABLE_FORMAT_VERSION + 1)) != std::string::npos,
                   L"the refusal did not name the version it found");
    Assert::IsTrue(message.find(std::to_string(DURABLE_FORMAT_VERSION)) != std::string::npos,
                   L"the refusal did not name the version this build knows");
  }

  TEST_METHOD(ACorruptClusterCountIsBoundedBeforeItIndexes)
  {
    /*
     * The one number in this format that is used as an array bound. A ledger
     * claiming twenty clusters against a twelve-cluster field is refused where
     * it is read, not where it is used -- which is the difference between a
     * diagnostic and a buffer overrun.
     */
    DurableState state;
    state.ledgers.push_back(SiteLedger{});
    state.ledgers[0].anchor = 4;
    state.ledgers[0].clusterCount = 2;
    std::vector<std::uint8_t> bytes = WriteTo(state);

    // The cluster count sits before its two clusters, and the transfer count
    // (a u32, zero here) trails the whole ledger block.
    const std::size_t clusterCountAt = bytes.size() - 4u - (2u * ORE_COUNT * 4u) - 1u;
    Assert::AreEqual<std::uint8_t>(2, bytes[clusterCountAt], L"the fixture is looking at the wrong byte");
    bytes[clusterCountAt] = static_cast<std::uint8_t>(MAX_SITE_CLUSTERS + 1);

    Neuron::ByteReader reader(bytes);
    DurableState read;
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsFalse(ReadDurableState(reader, read, diagnostics), L"an impossible cluster count was accepted");
    Assert::IsTrue(diagnostics[0].message.find("clusters") != std::string::npos, L"the refusal did not name the count");
  }

  TEST_METHOD(ALostTailChangesTheHash)
  {
    /*
     * Why every count is folded before the things it counts. The failure this
     * hash exists to catch is *loss*, and a count-free fold is blind to exactly
     * that: drop the last Bay and the remaining bytes hash the same.
     */
    const DurableState full = OneOfEverything();
    DurableState missing = full;
    missing.bays.clear();
    Assert::AreNotEqual(DurableHash(full), DurableHash(missing), L"losing a Bay did not move the reload proof");

    DurableState shortRoster = full;
    shortRoster.rosters[0].docked.clear();
    Assert::AreNotEqual(DurableHash(full), DurableHash(shortRoster), L"losing a docked ship did not move the reload proof");
  }
};

TEST_CLASS(DurableReloadTests)
{
public:
  TEST_METHOD(AShardReloadsIntoTheSameDurableHash)
  {
    /*
     * The claim the whole slice exists for, and the accept's first line:
     * rosters, Bays, manifests and ships both docked and in space, written,
     * read into a second registry, and the two proofs agree.
     */
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const AnchorId planet = FirstOfKind(AnchorKind::Planet);
    Assert::AreNotEqual<std::uint16_t>(INVALID_ID, station, L"the bake has no station");
    Assert::AreNotEqual<std::uint16_t>(INVALID_ID, planet, L"the bake has no planet");

    std::uint32_t tick = 0;
    const ShipId docked = DockLoaded(before, station, OreId::FerroChroma, 40, tick);
    (void)SpawnLoaded(before, planet, HullClass::Miner, 512.0f, -256.0f, OreId::Astracite, 11);
    (void)SpawnLoaded(before, planet, HullClass::Interceptor, -90.0f, 40.0f, OreId::FerroChroma, 0);

    StationCommand toBay;
    toBay.orderSeq = 5;
    toBay.verb = StationVerb::TransferToBay;
    toBay.station = station;
    toBay.ore = OreId::FerroChroma;
    toBay.units = 25;
    Assert::IsTrue(toBay.AddShip(docked), L"the fixture could not name the ship it just docked");
    const OrderVerdict verdict = before.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, toBay);
    Assert::IsTrue(verdict.accepted, L"the fixture's transfer to the Bay was refused");

    const std::vector<std::uint8_t> bytes = WriteTo(before);

    WorldRegistry after;
    MakeRegistry(after);
    Neuron::ByteReader reader(bytes);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(ReadDurableState(reader, after, diagnostics), L"the shard would not reload");
    Assert::AreEqual<std::size_t>(0, diagnostics.size(), L"a clean reload reported a problem");

    Assert::AreEqual(DurableHash(before), DurableHash(after), L"the reload did not reproduce what it claims");
    Assert::AreEqual<std::uint32_t>(before.ShardTick(), after.ShardTick(), L"the shard tick did not survive");
    Assert::AreEqual<std::size_t>(before.Roster(station).size(), after.Roster(station).size(), L"the roster did not survive");
    Assert::IsNotNull(after.Bay(Neuron::SOLE_PLAYER_ID, station), L"the Bay did not survive the restart");
    Assert::AreEqual<std::uint32_t>(25, after.Bay(Neuron::SOLE_PLAYER_ID, station)->Units(OreId::FerroChroma),
                                    L"the Bay came back with the wrong ore in it");
  }

  TEST_METHOD(AReloadedFleetHoldsPositionWithAnEmptyQueue)
  {
    /*
     * ADR-025 §1's line, as a test rather than a sentence. The ship is where it
     * was; the order it was flying is gone, because intention is the player's
     * to restate and a shard that guessed would be flying somebody's fleet
     * somewhere they did not ask for after a restart they did not see.
     */
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId planet = FirstOfKind(AnchorKind::Planet);
    const ShipId ship = SpawnLoaded(before, planet, HullClass::Miner, 640.0f, 128.0f, OreId::Nebulite, 7);

    World* world = before.Borrow(planet);
    OrderSubmit move;
    move.orderSeq = 3;
    move.kind = OrderKind::Move;
    move.target.xCm = 500000;
    move.target.yCm = 500000;
    Assert::IsTrue(move.AddShip(ship), L"the fixture could not name its own ship");
    Assert::IsTrue(world->SubmitOrder(move).accepted, L"the fixture's move order was refused");

    std::uint32_t slot = 0;
    Assert::IsTrue(world->FindSlot(ship, slot), L"the ship is not on the grid it was spawned on");
    const float x = world->Positions()[slot].x;
    const float y = world->Positions()[slot].y;

    const std::vector<std::uint8_t> bytes = WriteTo(before);

    WorldRegistry after;
    MakeRegistry(after);
    Neuron::ByteReader reader(bytes);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(ReadDurableState(reader, after, diagnostics), L"the shard would not reload");

    const World* reloaded = after.Peek(planet);
    Assert::IsNotNull(reloaded, L"the grid a fleet was standing on did not come back");
    std::uint32_t reloadedSlot = 0;
    Assert::IsTrue(reloaded->FindSlot(ship, reloadedSlot), L"the ship did not come back");
    Assert::AreEqual(x, reloaded->Positions()[reloadedSlot].x, 0.0f, L"the ship moved across the restart");
    Assert::AreEqual(y, reloaded->Positions()[reloadedSlot].y, 0.0f, L"the ship moved across the restart");
    Assert::AreEqual<std::uint32_t>(7, reloaded->Cargo()[reloadedSlot].Units(OreId::Nebulite), L"the hold did not survive");
    Assert::AreEqual<std::uint32_t>(0, reloaded->PendingOrderCount(), L"an intention survived the restart");
    Assert::AreEqual<std::size_t>(0, reloaded->Groups().size(), L"an order group survived the restart");
    Assert::AreEqual<std::uint32_t>(0, reloaded->ProtectedUntil()[reloadedSlot],
                                    L"undock protection survived a restart, and fifteen seconds do not");
  }

  TEST_METHOD(TheReplayHashAndTheReloadProofAnswerDifferentQuestions)
  {
    /*
     * ADR-025 §1a, tested rather than asserted in prose. An accepted order
     * moves `WorldRegistry::Hash()` -- it is world state and the replay domain
     * folds it -- and does not move `DurableHash()`, because a queue is
     * intention. If these two ever agreed, the reload check would be measuring
     * the wrong thing and every reload would fail it for the right reason.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId planet = FirstOfKind(AnchorKind::Planet);
    const ShipId ship = SpawnLoaded(registry, planet, HullClass::Miner, 100.0f, 100.0f, OreId::FerroChroma, 0);

    const std::uint64_t replayBefore = registry.Hash();
    const std::uint64_t durableBefore = DurableHash(registry);

    World* world = registry.Borrow(planet);
    OrderSubmit move;
    move.orderSeq = 11;
    move.kind = OrderKind::Move;
    move.target.xCm = 200000;
    move.target.yCm = 0;
    Assert::IsTrue(move.AddShip(ship), L"the fixture could not name its own ship");
    Assert::IsTrue(world->SubmitOrder(move).accepted, L"the fixture's move order was refused");

    Assert::AreNotEqual(replayBefore, registry.Hash(), L"an accepted order left the replay hash unmoved");
    Assert::AreEqual(durableBefore, DurableHash(registry), L"an intention moved the reload proof");
  }

  TEST_METHOD(TheShipIdMarkSurvivesAndNeverGoesBackwards)
  {
    /*
     * ADR-025 §1a's first trap. A restarted shard that re-issued ids would hand
     * a Bay row, a transfer record and a client's selection the wrong ship, and
     * every one of those lookups would succeed -- which is why a restore that
     * would lower the mark is a refusal and not a clamp.
     */
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId planet = FirstOfKind(AnchorKind::Planet);
    for (int index = 0; index < 5; ++index)
    {
      (void)SpawnLoaded(before, planet, HullClass::Interceptor, 10.0f * index, 0.0f, OreId::FerroChroma, 0);
    }
    const std::uint32_t mark = before.NextDynamicShipId();

    DurableState state;
    CaptureDurableState(before, state);
    Assert::AreEqual<std::uint32_t>(mark, state.nextDynamicShipId, L"the mark was not captured");

    WorldRegistry after;
    MakeRegistry(after);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(after.LoadDurable(state, diagnostics), L"the shard would not reload");
    Assert::AreEqual<std::uint32_t>(mark, after.NextDynamicShipId(), L"the mark did not survive the restart");

    // And a new ship gets an id past everything the reload restored, which is
    // the property the mark exists to provide rather than the number itself.
    const ShipId fresh = after.Spawn(planet, ShipSpawn{});
    Assert::AreNotEqual<std::uint16_t>(INVALID_SHIP_ID, fresh, L"the reloaded shard could not spawn");
    for (const DurableShip& ship : state.ships)
    {
      Assert::AreNotEqual<std::uint16_t>(ship.shipId, fresh, L"a restarted shard reissued a live ship's id");
    }

    /*
     * And the refusal. A fresh registry's mark is already at
     * `DYNAMIC_SHIP_ID_BASE` -- the floor the authored ids stop below -- so a
     * snapshot claiming the allocator sits under it is claiming the shard once
     * issued ids that belong to the bake. Clamping that up would be the trap
     * this rule exists to prevent, wearing a repair's clothes.
     */
    DurableState lowered = state;
    lowered.nextDynamicShipId = DYNAMIC_SHIP_ID_BASE - 1;
    WorldRegistry third;
    MakeRegistry(third);
    std::vector<PersistenceDiagnostic> refusal;
    Assert::IsFalse(third.LoadDurable(lowered, refusal), L"a mark that goes backwards was accepted");
    Assert::IsTrue(refusal.size() >= 1, L"the refusal said nothing");
    Assert::AreEqual<std::uint32_t>(DYNAMIC_SHIP_ID_BASE, third.NextDynamicShipId(),
                                    L"a refused load moved the mark anyway");
  }

  TEST_METHOD(AuthoredOccupantsAreNotInTheSaveFile)
  {
    /*
     * A station's structure is content, not property. Spin-up rebuilds it with
     * the id the bake derived from the anchor (ADR-018 D6a), so writing it down
     * would put content in a save file -- and the save file would then argue
     * with the next re-bake about a ship neither of them owns.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const World* world = registry.Borrow(station);
    Assert::IsTrue(world->ShipCount() > 0, L"a station grid spun up with nothing on it");

    DurableState state;
    CaptureDurableState(registry, state);
    Assert::AreEqual<std::size_t>(0, state.ships.size(), L"an authored occupant was written to the save file");

    WorldRegistry after;
    MakeRegistry(after);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(after.LoadDurable(state, diagnostics), L"an empty shard would not reload");
    const World* reloaded = after.Borrow(station);
    Assert::AreEqual<std::uint32_t>(world->ShipCount(), reloaded->ShipCount(),
                                    L"the station came back with a different number of structures");
  }

  TEST_METHOD(TwoCommandersBaysBothSurvive)
  {
    // The privacy rule lives in the key, so the format has to carry the key.
    // Two Bays at one station is the case a per-station format would silently
    // merge, and merging two commanders' property is the worst bug this file
    // could have.
    DurableState state;
    StationBay first;
    first.owner = 1;
    first.station = 40;
    first.oreUnits[0] = 10;
    StationBay second;
    second.owner = 2;
    second.station = 40;
    second.oreUnits[0] = 20;
    state.bays.push_back(first);
    state.bays.push_back(second);
    state.nextDynamicShipId = DYNAMIC_SHIP_ID_BASE;

    WorldRegistry registry;
    MakeRegistry(registry);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(registry.LoadDurable(state, diagnostics), L"two Bays at one station would not load");
    Assert::IsNotNull(registry.Bay(1, 40), L"the first commander's Bay is missing");
    Assert::IsNotNull(registry.Bay(2, 40), L"the second commander's Bay is missing");
    Assert::AreEqual<std::uint32_t>(10, registry.Bay(1, 40)->Units(OreId::FerroChroma));
    Assert::AreEqual<std::uint32_t>(20, registry.Bay(2, 40)->Units(OreId::FerroChroma));
  }

  TEST_METHOD(ACrossingInFlightSurvivesAndTheCounterResumesPastIt)
  {
    /*
     * A fleet three seconds into a warp is somewhere, and the record is the
     * only thing that knows where. The counter matters as much as the record:
     * one that restarted at zero would stamp a new crossing with an id a
     * pending crossing already holds, and the bus's total order (ADR-018 D17)
     * would have two records claiming one place in it.
     */
    WorldRegistry before;
    MakeRegistry(before);
    const AnchorId station = FirstOfKind(AnchorKind::Station);
    const ShipId ship = SpawnLoaded(before, station, HullClass::Miner, 200.0f, 0.0f, OreId::FerroChroma, 3);

    World* world = before.Borrow(station);
    OrderSubmit dock;
    dock.orderSeq = 1;
    dock.kind = OrderKind::Dock;
    dock.anchor = station;
    (void)dock.AddShip(ship);
    (void)world->SubmitOrder(dock);

    std::uint32_t tick = 0;
    before.Tick(++tick); // Ingested and filed, and not yet applied.
    Assert::AreEqual<std::uint32_t>(1, before.PendingTransferCount(), L"the fixture never got a crossing onto the bus");
    const std::uint32_t counter = before.PendingTransfers()[0].id.counter;

    const std::vector<std::uint8_t> bytes = WriteTo(before);

    WorldRegistry after;
    MakeRegistry(after);
    Neuron::ByteReader reader(bytes);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(ReadDurableState(reader, after, diagnostics), L"a shard with a crossing in flight would not reload");
    Assert::AreEqual<std::uint32_t>(1, after.PendingTransferCount(), L"the crossing did not survive the restart");
    Assert::AreEqual<std::uint32_t>(counter, after.PendingTransfers()[0].id.counter, L"the crossing came back as a different record");
    Assert::AreEqual(DurableHash(before), DurableHash(after), L"a crossing in flight broke the reload proof");
  }

  TEST_METHOD(AWorkedFieldsLedgerSurvivesTheRestart)
  {
    /*
     * "Worlds forget, ledgers do not" -- extended by one word. A field that has
     * been mined carries a ledger at the universe layer that outlives the grid;
     * this is the first slice in which it also outlives the *process*, which is
     * what makes the sentence a promise rather than a scoping note.
     */
    WorldRegistry before;
    MakeSiteRegistry(before);
    const AnchorId site = FirstSite();

    ShipSpawn miner;
    miner.hullClass = HullClass::Miner;
    miner.wing = 1;
    const ShipId ship = before.Spawn(site, miner);
    Assert::AreNotEqual<std::uint16_t>(INVALID_SHIP_ID, ship, L"the fixture could not put a Miner on a field");

    World* world = before.Borrow(site);
    OrderSubmit mine;
    mine.orderSeq = 1;
    mine.kind = OrderKind::Mine;
    mine.oreFilter = OreFilter::Any;
    Assert::IsTrue(mine.AddShip(ship), L"the fixture could not name its own Miner");
    Assert::IsTrue(world->SubmitOrder(mine).accepted, L"the fixture's Mine order was refused");

    std::uint32_t tick = 0;
    for (std::uint32_t step = 0; step < 20000 && before.Ledger(site) == nullptr; ++step)
    {
      before.Tick(++tick);
    }
    const SiteLedger* ledger = before.Ledger(site);
    Assert::IsNotNull(ledger, L"the Miner never landed a cycle on the ledger");
    const std::uint32_t remaining = ledger->TotalRemaining();
    const std::uint32_t epoch = ledger->epochIndex;

    const std::vector<std::uint8_t> bytes = WriteTo(before);

    WorldRegistry after;
    MakeSiteRegistry(after);
    Neuron::ByteReader reader(bytes);
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsTrue(ReadDurableState(reader, after, diagnostics), L"a shard with a worked field would not reload");

    const SiteLedger* reloaded = after.Ledger(site);
    Assert::IsNotNull(reloaded, L"the ledger did not survive the restart, so the field refilled itself for free");
    Assert::AreEqual<std::uint32_t>(remaining, reloaded->TotalRemaining(), L"the field came back with a different amount in it");
    Assert::AreEqual<std::uint32_t>(epoch, reloaded->epochIndex, L"the ledger came back against a different epoch");
    Assert::AreEqual(DurableHash(before), DurableHash(after), L"a worked field broke the reload proof");
  }

  TEST_METHOD(ALoadIntoARunningRegistryIsRefused)
  {
    // There is no answer to what merging a save file into a running shard would
    // mean that is better than declining to have one, and a refusal at boot is
    // visible where a silent merge is visible weeks later as a duplicated fleet.
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId planet = FirstOfKind(AnchorKind::Planet);
    (void)SpawnLoaded(registry, planet, HullClass::Miner, 0.0f, 0.0f, OreId::FerroChroma, 0);

    DurableState state;
    std::vector<PersistenceDiagnostic> diagnostics;
    Assert::IsFalse(registry.LoadDurable(state, diagnostics), L"a running shard accepted a save file");
    Assert::IsTrue(diagnostics.size() >= 1, L"the refusal said nothing");
  }
};

} // namespace GameLogicTests
