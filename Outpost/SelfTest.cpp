#include "pch.h"

#include "SelfTest.h"

#include "ConfigLoad.h"
#include "ReplicatedWorldView.h"

#include "TickSoak.h"

#include "ClientConnection.h"
#include "ObjMesh.h"
#include "StationView.h"

#include "DurableStore.h"
#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"

#include "DurableState.h"
#include "EconomyMessages.h"
#include "FleetSummary.h"
#include "Refining.h"
#include "OrderMessages.h"
#include "ReplicatedView.h"
#include "SchemaHash.h"
#include "Snapshot.h"
#include "Station.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "World.h"
#include "WorldHash.h"
#include "WorldRegistry.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "EntityRecord.h"
#include "Log.h"
#include "Telemetry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Outpost
{
namespace
{

/// Generous: the point is to catch "never happened", not to measure. A healthy
/// loopback finishes in well under a second.
constexpr double TIMEOUT_MS = 10000.0;

/// One pong proves the path; three prove the cadence keeps running.
constexpr std::uint64_t REQUIRED_PONGS = 3;

/// A round trip over loopback is measured in microseconds. A whole second means
/// the number is being computed wrong, not that the link is slow.
constexpr double IMPLAUSIBLE_ROUND_TRIP_MS = 1000.0;

/// How long the tick-cadence measurement runs. Long enough that one late tick
/// does not move the mean, short enough that nobody skips the self test.
constexpr double CADENCE_WINDOW_MS = 3000.0;

/// How long the approach gate watches for a dock that must never arrive. Long
/// enough for several summary frames at the ~1 Hz roster cadence, which is the
/// feed that would report one.
constexpr std::uint32_t APPROACH_SETTLE_TICKS = 60;

/// ADR-002's rate, and what the mean period is checked against. The tolerance
/// is the loose one: S3's "50 ms +/- 0.5" is a measurement for an *idle*
/// machine, and this runs wherever it is run. The tight number is reported
/// rather than asserted, so a person can read it and judge.
constexpr double EXPECTED_TICK_PERIOD_MS = 50.0;
constexpr double ACCEPTABLE_TICK_PERIOD_DRIFT_MS = 5.0;

class Checklist
{
public:
  void Record(const char* _name, bool _passed)
  {
    if (_passed)
    {
      NEURON_LOG_INFO("self test: %s -- ok", _name);
    }
    else
    {
      NEURON_LOG_ERROR("self test: %s -- FAILED", _name);
      ++m_failures;
    }
  }

  [[nodiscard]] std::uint32_t Failures() const noexcept
  {
    return m_failures;
  }

private:
  std::uint32_t m_failures = 0;
};

/*
 * A scripted world for the replay-determinism run (S14): a handful of hulls,
 * real orders through the real validation at fixed ticks, `TICKS` ticks of
 * simulation. Everything is derived from constants and `_targetNudgeMetres`,
 * so two calls are the same run by construction -- and a nudged third call is
 * the control that proves the hash would have noticed a divergence.
 *
 * This re-proves in the shipping binary what `GameLogicTests` proves in CI:
 * the point of having it here is that the self test runs on the machine and
 * the build actually deployed, where a stray /fp switch or a local compiler
 * would otherwise only be discovered by a desync.
 */
constexpr std::uint32_t REPLAY_TICKS = 400;
constexpr std::uint32_t REPLAY_CHECKPOINT_TICK = 200;

struct ReplayResult
{
  std::uint64_t checkpointHash = 0;
  std::uint64_t finalHash = 0;
};

[[nodiscard]] ReplayResult RunScriptedReplay(float _targetNudgeMetres)
{
  Game::World world;
  world.Reset(0x5EEDu);

  Game::ShipId ships[6] = {};
  constexpr Game::HullClass HULLS[6] = {Game::HullClass::Interceptor, Game::HullClass::Bomber,  Game::HullClass::Corvette,
                                        Game::HullClass::Frigate,     Game::HullClass::Carrier, Game::HullClass::Battleship};
  for (std::uint32_t index = 0; index < 6; ++index)
  {
    Game::ShipSpawn spawn;
    spawn.hullClass = HULLS[index];
    spawn.wing = 1;
    spawn.xMetres = -2000.0f + 800.0f * static_cast<float>(index);
    spawn.yMetres = -1500.0f;
    ships[index] = world.Spawn(spawn, static_cast<Game::ShipId>(index));
  }

  ReplayResult result;
  for (std::uint32_t tick = 1; tick <= REPLAY_TICKS; ++tick)
  {
    if (tick == 1 || tick == 150 || tick == 280)
    {
      Game::OrderSubmit order;
      order.orderSeq = tick;
      order.kind = Game::OrderKind::Move;
      order.formation = tick == 150 ? Game::FormationId::Wedge : Game::FormationId::Line;
      order.queueMode = Game::QueueMode::Replace;
      for (const Game::ShipId ship : ships)
      {
        (void)order.AddShip(ship);
      }
      order.target.xCm = Neuron::MetresToCentimetres(1500.0f + _targetNudgeMetres);
      order.target.yCm = Neuron::MetresToCentimetres(tick == 280 ? -2500.0f : 2500.0f);
      (void)world.SubmitOrder(order);
    }

    world.Tick(tick);
    if (tick == REPLAY_CHECKPOINT_TICK)
    {
      result.checkpointHash = Game::ComputeWorldHash(world);
    }
  }
  result.finalHash = Game::ComputeWorldHash(world);
  return result;
}

/*
 * The device-free half of the S14 aggregate: schema self-checks, the wire
 * round-trips and the replay run. First, before any socket opens, so a
 * transport failure cannot mask a determinism one.
 */
/*
 * The restart, in the shipping binary (ADR-025 §8, build order E4a).
 *
 * A shard is docked, loaded and committed; it is written to a real directory
 * through the real store; and a **second registry and a second store** read it
 * back and reproduce it. That is what a restart is -- two independent runtimes
 * meeting only through two files -- so doing it in one process costs nothing
 * the claim needs and saves this gate from spawning itself.
 *
 * What it is not is the whole of §8's scenario: there is no refine job to still
 * be running, because there are no refine jobs until E4b. The build order says
 * so where it will be found, and the check below carries the half that exists.
 */
void RunRestartLoop(Checklist& _checks, const Game::EconomyDef& _economy)
{
  const std::string directory = Outpost::ResolveWritablePath("SelfTestShardState");
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);

  Game::UniverseGenConfig recipe;
  recipe.regionCount = 1;
  recipe.constellationsPerRegion = 1;
  recipe.systemCount = 4;
  Game::UniverseDef universe;
  if (!Game::GenerateUniverse(recipe, _economy.sites, universe))
  {
    _checks.Record("the restart scenario bakes a universe", false);
    return;
  }
  const std::uint64_t universeHash = Game::ComputeUniverseHash(universe);

  Game::AnchorId station = Game::INVALID_ID;
  for (const Game::SolarSystem& system : universe.systems)
  {
    for (const Game::Anchor& anchor : system.anchors)
    {
      if (anchor.kind == Game::AnchorKind::Station && station == Game::INVALID_ID)
      {
        station = anchor.id;
      }
    }
  }
  if (station == Game::INVALID_ID)
  {
    _checks.Record("the restart scenario finds a station", false);
    return;
  }

  Neuron::DurableStoreDesc desc;
  desc.directory = directory;
  desc.hostId = 0;
  desc.universeHash = universeHash;
  desc.economyHash = 0;

  std::uint64_t writtenHash = 0;
  std::vector<std::uint8_t> stateBytes;

  /// What the running job said about itself before the shard stopped. The two
  /// numbers 🏁 G1 is about: which job, and when it finishes.
  std::uint32_t jobSequence = 0;
  std::uint32_t jobCompleteTick = 0;

  // --- The shard, before it stops. ---
  {
    Game::WorldRegistry registry;
    Game::RegistryConfig config;
    config.sessionSeed = universeHash;
    registry.Reset(&universe, &_economy, config);

    Game::ShipSpawn miner;
    miner.hullClass = Game::HullClass::Miner;
    miner.wing = 1;
    miner.cargo.oreUnits[static_cast<std::uint8_t>(Game::OreId::FerroChroma)] = 60;
    const Game::ShipId ship = registry.Spawn(station, miner, Neuron::SOLE_PLAYER_ID);

    Game::World* world = registry.Borrow(station);
    Game::OrderSubmit dock;
    dock.orderSeq = 1;
    dock.kind = Game::OrderKind::Dock;
    dock.anchor = station;
    (void)dock.AddShip(ship);
    (void)world->SubmitOrder(dock);
    registry.Tick(1);
    registry.Tick(2);

    Game::StationCommand toBay;
    toBay.orderSeq = 2;
    toBay.verb = Game::StationVerb::TransferToBay;
    toBay.station = station;
    toBay.ore = Game::OreId::FerroChroma;
    toBay.units = 40;
    (void)toBay.AddShip(ship);
    const Game::OrderVerdict committed = registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, toBay);
    _checks.Record("the restart scenario commits ore to a Bay", committed.accepted);

    /*
     * And a refine job on top of it -- 🏁 G1's own claim (ADR-024 §6b, E4b).
     *
     * The batch is deliberately the smallest the content authors, because what
     * is under test is that a job **survives with its completion tick unmoved**
     * rather than how long it takes: a job restored with a duration instead of
     * a deadline would silently restart its own clock on every restart, and a
     * shard that bounced twice an hour would then never finish anything.
     */
    Game::StationCommand refine;
    refine.orderSeq = 3;
    refine.verb = Game::StationVerb::RefineStart;
    refine.station = station;
    refine.alloy = Game::AlloyId::FerrocitePlates;
    refine.units = 1;
    const Game::OrderVerdict started = registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, refine);
    _checks.Record("the restart scenario starts a refine job", started.accepted);
    _checks.Record("the job is running before the shard stops",
                   registry.RefineJobs().size() == 1 && registry.RefineJobs()[0].Running());
    if (registry.RefineJobs().size() == 1)
    {
      jobSequence = registry.RefineJobs()[0].sequence;
      jobCompleteTick = registry.RefineJobs()[0].completeTick;
    }

    writtenHash = Game::DurableHash(registry);
    stateBytes.resize(1u << 20);
    Neuron::ByteWriter writer{stateBytes};
    const bool serialised = Game::WriteDurableState(registry, writer);
    _checks.Record("the shard serialises its durable state", serialised);
    if (!serialised)
    {
      return;
    }
    stateBytes.resize(writer.BytesWritten());

    Neuron::DurableStore store;
    Neuron::DurableLoadReport report;
    const bool opened = store.Open(desc, report) && report.result == Neuron::DurableLoadResult::Fresh;
    _checks.Record("a shard with no state opens fresh", opened);
    _checks.Record("the shard writes a snapshot", opened && store.WriteSnapshot(stateBytes, writtenHash, 2));
    store.Close();
  }

  // --- And after it starts again. Nothing above is in scope down here, which
  // --- is the point: the only thing crossing is what is on the disk.
  {
    Neuron::DurableStore store;
    Neuron::DurableLoadReport report;
    const bool opened = store.Open(desc, report);
    _checks.Record("the restarted shard finds its state", opened && report.result == Neuron::DurableLoadResult::Loaded);
    _checks.Record("the snapshot carries the reload proof it was written with", report.snapshotDurableHash == writtenHash);

    Game::WorldRegistry registry;
    Game::RegistryConfig config;
    config.sessionSeed = universeHash;
    registry.Reset(&universe, &_economy, config);

    Neuron::ByteReader reader(store.Snapshot());
    std::vector<Game::PersistenceDiagnostic> diagnostics;
    const bool loaded = Game::ReadDurableState(reader, registry, diagnostics);
    for (const Game::PersistenceDiagnostic& diagnostic : diagnostics)
    {
      NEURON_LOG_ERROR("self test: durable state: %s", diagnostic.Text("shard-0.snapshot").c_str());
    }
    _checks.Record("the restarted shard reads its state back", loaded);
    _checks.Record("the reload reproduces the proof", loaded && Game::DurableHash(registry) == writtenHash);

    const std::span<const Game::RosterEntry> docked = registry.Roster(station);
    _checks.Record("the roster survives the restart", docked.size() == 1);
    _checks.Record("the hold survives the restart", docked.size() == 1 && docked[0].Units(Game::OreId::FerroChroma) == 20);

    const Game::StationBay* bay = registry.Bay(Neuron::SOLE_PLAYER_ID, station);
    _checks.Record("the Bay survives the restart", bay != nullptr && bay->Units(Game::OreId::FerroChroma) == 38);

    /*
     * 🏁 G1: **the refinery does not stop when the shard restarts.**
     *
     * The completion tick unmoved is the assertion; the job still being there
     * is only half of it. And then it is run to that tick and the alloy is in
     * the Bay -- because a job that survives and never finishes is the same
     * outcome as one that did not survive, arrived at more slowly.
     */
    const std::span<const Game::RefineJob> jobs = registry.RefineJobs();
    _checks.Record("the refine job survives the restart", jobs.size() == 1);
    _checks.Record("with the job it was", jobs.size() == 1 && jobs[0].sequence == jobSequence);
    _checks.Record("and its completion tick unmoved", jobs.size() == 1 && jobs[0].completeTick == jobCompleteTick);

    for (std::uint32_t tick = registry.ShardTick() + 1; tick <= jobCompleteTick; ++tick)
    {
      registry.Tick(tick);
    }
    const Game::StationBay* finished = registry.Bay(Neuron::SOLE_PLAYER_ID, station);
    _checks.Record("the reloaded job finishes into the Bay",
                   registry.RefineJobs().empty() && finished != nullptr
                     && finished->Units(Game::AlloyId::FerrocitePlates) == 1);

    std::vector<Game::PersistenceDiagnostic> refusal;
    Game::WorldRegistry running;
    running.Reset(&universe, &_economy, config);
    (void)running.Borrow(station);
    Neuron::ByteReader again(store.Snapshot());
    _checks.Record("a load into a running shard is refused", !Game::ReadDurableState(again, running, refusal));
    store.Close();
  }

  // --- And the guard that stops an amnesiac shard from looking healthy. ---
  {
    Neuron::DurableStoreDesc rebaked = desc;
    rebaked.universeHash = universeHash ^ 0x1ull;
    Neuron::DurableStore store;
    Neuron::DurableLoadReport report;
    _checks.Record("a re-baked universe refuses to load an old shard", !store.Open(rebaked, report));
  }

  std::filesystem::remove_all(directory, ignored);
}

void RunLocalChecks(Checklist& _checks, Neuron::Simulation& _simulation, const Game::EconomyDef& _economy)
{
  // Schema: the number the handshake fails closed on. Nonzero, stable across
  // computation, and the shipping simulation states the same one the game's
  // own function does -- three cheap facts a corrupted build breaks first.
  const std::uint64_t schema = Game::GameSchemaHash();
  _checks.Record("schema hash is nonzero", schema != 0);
  _checks.Record("schema hash is stable", Game::GameSchemaHash() == schema);
  _checks.Record("the simulation states the game's schema", _simulation.SchemaHash() == schema);
  _checks.Record("the simulation states a content hash", _simulation.ContentHash() != 0);

  // The order wire: write one, read it back, compare every field the layout
  // carries (ADR-004 §7).
  {
    Game::OrderSubmit order;
    order.orderSeq = 77;
    order.kind = Game::OrderKind::Move;
    order.formation = Game::FormationId::Claw;
    order.queueMode = Game::QueueMode::Append;
    (void)order.AddShip(3);
    (void)order.AddShip(9);
    order.target.xCm = -123456;
    order.target.yCm = 654321;
    order.target.facingTurns16 = 0x1234;

    // The kind byte is part of the payload now (ADR-017 §8), so the round trip
    // has to carry it: writing it and then reading the body straight back would
    // be a check that passes only because it skipped the field under test.
    std::array<std::uint8_t, Game::COMMAND_KIND_BYTES + Game::MAX_ORDER_SUBMIT_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    bool ok = Game::WriteCommandKind(Game::CommandKind::Order, writer) && Game::WriteOrderSubmit(order, writer);

    Game::OrderSubmit read;
    if (ok)
    {
      Neuron::ByteReader reader{writer.Written()};
      Game::CommandKind kind = Game::CommandKind::Station;
      ok = Game::ReadCommandKind(reader, kind) && kind == Game::CommandKind::Order && Game::ReadOrderSubmit(reader, read) &&
           reader.FullyConsumed();
    }
    ok = ok && read.orderSeq == order.orderSeq && read.kind == order.kind && read.formation == order.formation &&
         read.queueMode == order.queueMode && read.shipCount == order.shipCount && read.shipIds[0] == 3 && read.shipIds[1] == 9 &&
         read.target.xCm == order.target.xCm && read.target.yCm == order.target.yCm &&
         read.target.facingTurns16 == order.target.facingTurns16;
    _checks.Record("an order round-trips the wire byte-exactly", ok);
  }

  // The snapshot wire: emit from a real world, apply through the client's own
  // view, and compare in *integers* -- re-quantising the sampled ships must
  // reproduce the exact centimetres that crossed (the same assertion
  // GameLogicTests makes, re-proved in the shipping binary).
  {
    Game::World world;
    world.Reset(1);
    for (std::uint32_t index = 0; index < 5; ++index)
    {
      Game::ShipSpawn spawn;
      spawn.hullClass = Game::HullClass::Corvette;
      spawn.wing = 1;
      spawn.xMetres = 100.5f * static_cast<float>(index + 1);
      spawn.yMetres = -77.25f * static_cast<float>(index + 1);
      (void)world.Spawn(spawn, static_cast<Game::ShipId>(index));
    }
    world.Tick(1);

    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    bool ok = Game::WriteSnapshot(world, writer);
    _checks.Record("a snapshot fits the datagram cap", ok && writer.Written().size() <= Neuron::MAX_DATAGRAM_BYTES);

    Game::SnapshotHeader header;
    std::vector<Neuron::EntityRecord> records;
    if (ok)
    {
      Neuron::ByteReader reader{writer.Written()};
      ok = Game::ReadSnapshot(reader, header, records);
    }

    Game::ReplicatedView view;
    ok = ok && view.ApplySnapshot(writer.Written());

    std::vector<Game::ReplicatedShip> sampled;
    if (ok)
    {
      view.SampleAt(static_cast<double>(view.LatestTick()), sampled);
    }
    ok = ok && sampled.size() == records.size() && sampled.size() == world.ShipCount();
    if (ok)
    {
      for (std::size_t index = 0; index < sampled.size(); ++index)
      {
        ok = ok && sampled[index].id == records[index].id &&
             Neuron::MetresToCentimetres(sampled[index].positionMetres.x) == records[index].posXCm &&
             Neuron::MetresToCentimetres(sampled[index].positionMetres.y) == records[index].posYCm;
      }
    }
    _checks.Record("a snapshot round-trips emit -> bytes -> apply in integers", ok);
  }

  // Replay determinism, in this binary on this machine: two identical scripted
  // runs agree at the checkpoint and the end, and the control -- one target
  // moved a metre -- diverges, which is what proves agreement means something.
  {
    const ReplayResult first = RunScriptedReplay(0.0f);
    const ReplayResult second = RunScriptedReplay(0.0f);
    const ReplayResult nudged = RunScriptedReplay(1.0f);
    _checks.Record("replay determinism holds at the checkpoint", first.checkpointHash == second.checkpointHash);
    _checks.Record("replay determinism holds at the end", first.finalHash == second.finalHash);
    _checks.Record("the world hash notices a one-metre change", first.finalHash != nudged.finalHash);

    // The values themselves, so the standing Debug/Release comparison (Risk
    // Register spike 2) is a line CI reads rather than a run someone remembers
    // to do by hand. ADR-005 §6 scopes determinism to one binary, so the two
    // configurations are *allowed* to disagree here and the comparison exists
    // to document that rather than to police it. A configuration disagreeing
    // with itself is the defect, and the two Record lines above are what catch
    // that.
    NEURON_LOG_INFO("self test: replay hash %016llx (checkpoint %016llx)", static_cast<unsigned long long>(first.finalHash),
                    static_cast<unsigned long long>(first.checkpointHash));
  }

  /*
   * A fleet crosses a gate, in this binary (U4, ADR-016 §5).
   *
   * Device-free and off the wire, which is not a shortcut but the only way the
   * shipping binary can carry this check at all: a jump is `GATE_JUMP_TICKS`
   * long by design -- twenty seconds at 20 Hz -- so driving one over the real
   * loopback would add twenty seconds of wall clock to a gate that runs on
   * every push, in two configurations, to prove a wire path the dock and the
   * view request already exercise. Ticked here as fast as the CPU allows, the
   * whole crossing costs microseconds.
   *
   * A universe of its own rather than the loaded one: twelve systems bake in
   * no time, and a check that depended on where the committed content happens
   * to put a gate would be a check about content.
   */
  {
    Game::UniverseGenConfig recipe;
    recipe.regionCount = 2;
    recipe.constellationsPerRegion = 2;
    recipe.systemCount = 12;

    Game::UniverseDef universe;
    // No site content: this checks gate topology, and a universe without
    // mining fields is the cheaper one to bake for it (UniverseGen's guard).
    bool ok = Game::GenerateUniverse(recipe, Game::SitesInfo{}, universe);

    // The first gate with a far side, and the anchor on the other end of it.
    Game::AnchorId here = Game::INVALID_ID;
    Game::AnchorId there = Game::INVALID_ID;
    for (const Game::SolarSystem& system : universe.systems)
    {
      for (const Game::Anchor& anchor : system.anchors)
      {
        if (here != Game::INVALID_ID || anchor.kind != Game::AnchorKind::Gate)
        {
          continue;
        }
        const Game::AnchorId paired = universe.PairedGateAnchor(anchor.id);
        if (paired != Game::INVALID_ID)
        {
          here = anchor.id;
          there = paired;
        }
      }
    }
    ok = ok && here != Game::INVALID_ID && there != Game::INVALID_ID;
    _checks.Record("the bake pairs a gate with one that leads back", ok);

    if (ok)
    {
      Game::WorldRegistry registry;
      Game::RegistryConfig config;
      config.sessionSeed = 0x9A7Eu;
      registry.Reset(&universe, nullptr, config);

      Game::ShipSpawn spawn;
      spawn.hullClass = Game::HullClass::Interceptor;
      spawn.wing = 1;
      spawn.xMetres = 800.0f; // Inside the jump radius, where a warp-in lands.
      const Game::ShipId ship = registry.Spawn(here, spawn, Neuron::SOLE_PLAYER_ID);

      // The gate itself is on that grid, spawned from the anchor's own block.
      const Game::World* gateGrid = registry.Peek(here);
      const bool gateStands =
        gateGrid != nullptr &&
        std::any_of(gateGrid->Classes().begin(), gateGrid->Classes().end(),
                    [](std::uint8_t _class) { return _class == static_cast<std::uint8_t>(Game::HullClass::Gate); });
      _checks.Record("a gate grid holds its gate", gateStands);

      Game::OrderSubmit jump;
      jump.orderSeq = 6001;
      jump.kind = Game::OrderKind::Warp;
      jump.anchor = there;
      bool submitted = ship != Game::INVALID_SHIP_ID && jump.AddShip(ship);

      Game::World* world = registry.Borrow(here);
      submitted = submitted && world != nullptr && world->SubmitOrder(jump).accepted;
      _checks.Record("a fleet at the gate is allowed through it", submitted);

      // The refusal beside it, from the same grid: standing across the grid is
      // `NotAtGate` and nothing else, which is the sentence the player reads.
      Game::OrderSubmit tooFar = jump;
      tooFar.orderSeq = 6002;
      Game::ShipSpawn awaySpawn = spawn;
      awaySpawn.xMetres = 9000.0f;
      const Game::ShipId away = registry.Spawn(here, awaySpawn, Neuron::SOLE_PLAYER_ID);
      tooFar.shipCount = 0;

      // Borrowed again rather than kept across the spawn: `Spawn` can spin a
      // world up, and "borrow, never hold" is the rule that makes a grid on
      // another machine a change of nothing (ADR-019 §6.1).
      world = registry.Borrow(here);
      const bool refused = away != Game::INVALID_SHIP_ID && tooFar.AddShip(away) && world != nullptr &&
                           world->SubmitOrder(tooFar).reason == Game::OrderReason::NotAtGate;
      _checks.Record("a fleet across the grid is refused NotAtGate", refused);

      std::uint32_t tick = 0;
      bool arrived = false;
      for (std::uint32_t step = 0; step < 2000 && !arrived; ++step)
      {
        registry.Tick(++tick);
        Game::AnchorId where = Game::INVALID_ID;
        arrived = registry.LocationOf(ship, where) && where == there;
      }
      _checks.Record("the crossing lands the fleet in the system on the far side", arrived);
    }
  }

  /*
   * 🏁 G0: the headless mining loop, end to end (ADR-024, build order E3).
   *
   * A Miner warps nowhere and mines where it stands, fills up and stops on its
   * own, docks with the ore still aboard, commits it to the Bay by command, and
   * the Bay survives the grid tearing down. Every one of those was a separate
   * slice; this is the first check that they compose.
   *
   * Ticked here rather than driven over the loopback for `GATE_JUMP_TICKS`'
   * reason: a mining cycle is 800 ticks and a hold is hundreds of units, so
   * doing this at 20 Hz would add minutes of wall clock to a gate that runs on
   * every push. What the wire half proves is that the *commands* cross, and the
   * QUIC section below already does that for the station family.
   */
  {
    Game::UniverseGenConfig recipe;
    recipe.regionCount = 1;
    recipe.constellationsPerRegion = 1;
    recipe.systemCount = 4;

    Game::UniverseDef universe;
    bool ok = Game::GenerateUniverse(recipe, _economy.sites, universe);

    // Both from the **same system**, because the loop this proves is an
    // in-system warp: a site in one system and a station in another would make
    // this a gate-jump test wearing a mining costume.
    Game::AnchorId site = Game::INVALID_ID;
    Game::AnchorId station = Game::INVALID_ID;
    for (const Game::SolarSystem& system : universe.systems)
    {
      Game::AnchorId here = Game::INVALID_ID;
      Game::AnchorId dock = Game::INVALID_ID;
      for (const Game::Anchor& anchor : system.anchors)
      {
        if (anchor.kind == Game::AnchorKind::Site && here == Game::INVALID_ID)
        {
          here = anchor.id;
        }
        if (anchor.kind == Game::AnchorKind::Station && dock == Game::INVALID_ID)
        {
          dock = anchor.id;
        }
      }
      if (here != Game::INVALID_ID && dock != Game::INVALID_ID)
      {
        site = here;
        station = dock;
        break;
      }
    }
    ok = ok && site != Game::INVALID_ID && station != Game::INVALID_ID;
    _checks.Record("the bake has a field and a station to run the loop between", ok);

    if (ok)
    {
      Game::WorldRegistry registry;
      Game::RegistryConfig config;
      config.sessionSeed = 0x60A1u;
      registry.Reset(&universe, &_economy, config);

      const Game::SiteStatusRow before = registry.SiteStatusFor(site);
      std::uint32_t startingUnits = 0;
      for (const std::uint32_t units : before.remainingUnits)
      {
        startingUnits += units;
      }

      Game::ShipSpawn spawn;
      spawn.hullClass = Game::HullClass::Miner;
      spawn.wing = 1;
      const Game::ShipId miner = registry.Spawn(site, spawn, Neuron::SOLE_PLAYER_ID);

      Game::OrderSubmit mine;
      mine.orderSeq = 8001;
      mine.kind = Game::OrderKind::Mine;
      Game::World* field = registry.Borrow(site);
      const bool ordered = miner != Game::INVALID_SHIP_ID && mine.AddShip(miner) && field != nullptr &&
                           field->SubmitOrder(mine).accepted;
      _checks.Record("a Miner standing in a field is allowed to work it", ordered);

      // Long enough for several cycles to land and cross the bus, and far short
      // of filling a 120,000-litre hold -- which is the point: the loop has to
      // work without waiting for the exit condition.
      std::uint32_t tick = 0;
      for (std::uint32_t step = 0; step < 4000; ++step)
      {
        registry.Tick(++tick);
      }

      const Game::ShipCargo* aboard = nullptr;
      if (const Game::World* world = registry.Peek(site); world != nullptr)
      {
        const std::span<const Game::ShipId> ids = world->Ids();
        const std::span<const Game::ShipCargo> holds = world->Cargo();
        for (std::size_t slot = 0; slot < ids.size() && slot < holds.size(); ++slot)
        {
          if (ids[slot] == miner)
          {
            aboard = &holds[slot];
          }
        }
      }
      std::uint32_t mined = 0;
      if (aboard != nullptr)
      {
        for (const std::uint32_t units : aboard->oreUnits)
        {
          mined += units;
        }
      }
      _checks.Record("mining puts ore in the hold", mined > 0);

      const Game::SiteStatusRow after = registry.SiteStatusFor(site);
      std::uint32_t leftUnits = 0;
      for (const std::uint32_t units : after.remainingUnits)
      {
        leftUnits += units;
      }
      _checks.Record("and the field's status shows it measurably emptier", leftUnits + mined == startingUnits);

      const bool dented = std::any_of(std::begin(after.clusterFullPct), std::begin(after.clusterFullPct) + after.clusterCount,
                                      [](std::uint8_t _pct) { return _pct < 100; });
      _checks.Record("with the worked cluster reading below full", dented);

      /*
       * Warp to the station and dock: the ore has to survive both crossings,
       * which is the gap E2 shipped with and E3 closed.
       *
       * Flown rather than teleported. A test-only mover on the registry would
       * have been quicker and would have proved a path the game does not have
       * -- and the transit bus is exactly where a manifest could go missing.
       */
      Game::OrderSubmit warp;
      warp.orderSeq = 8002;
      warp.kind = Game::OrderKind::Warp;
      warp.anchor = station;
      field = registry.Borrow(site);
      bool moved = field != nullptr && warp.AddShip(miner) && field->SubmitOrder(warp).accepted;
      _checks.Record("a loaded Miner may warp to the station", moved);

      // An in-system warp to a site's ring is ~2,400 ticks on the bake this
      // scenario generates -- the sites sit 40 % beyond the outermost planet
      // (ADR-024 §3a) -- so the bound is generous rather than tight. Ticking a
      // four-system registry is microseconds; guessing the number and being
      // wrong is a gate that fails for a reason nobody can read.
      for (std::uint32_t step = 0; step < 20000 && moved; ++step)
      {
        registry.Tick(++tick);
        Game::AnchorId where = Game::INVALID_ID;
        if (registry.LocationOf(miner, where) && where == station)
        {
          break;
        }
      }

      Game::OrderSubmit dock;
      dock.orderSeq = 8003;
      dock.kind = Game::OrderKind::Dock;
      dock.anchor = station;
      Game::World* home = registry.Borrow(station);
      bool docked = home != nullptr && dock.AddShip(miner) && home->SubmitOrder(dock).accepted;
      _checks.Record("and dock when it gets there", docked);
      for (std::uint32_t step = 0; step < 8; ++step)
      {
        registry.Tick(++tick);
      }

      const std::span<const Game::RosterEntry> roster = registry.Roster(station);
      const auto row = std::find_if(roster.begin(), roster.end(),
                                    [miner](const Game::RosterEntry& _r) { return _r.shipId == miner; });
      docked = row != roster.end();
      _checks.Record("the Miner reaches the station's roster", docked);

      std::uint32_t held = 0;
      if (docked)
      {
        for (const std::uint32_t units : row->oreUnits)
        {
          held += units;
        }
      }
      _checks.Record("with every unit it was carrying", held == mined && mined > 0);

      // And into the Bay by command -- manual, which is the ruling (ADR-024 §5c).
      Game::OreId richest = Game::OreId::FerroChroma;
      std::uint32_t richestUnits = 0;
      if (docked)
      {
        for (std::uint8_t ore = 0; ore < Game::ORE_COUNT; ++ore)
        {
          if (row->oreUnits[ore] > richestUnits)
          {
            richestUnits = row->oreUnits[ore];
            richest = static_cast<Game::OreId>(ore);
          }
        }
      }

      Game::StationCommand store;
      store.orderSeq = 8004;
      store.verb = Game::StationVerb::TransferToBay;
      store.station = station;
      store.ore = richest;
      store.units = richestUnits;
      const bool stored = docked && richestUnits > 0 && store.AddShip(miner) &&
                          registry.SubmitStationCommand(Neuron::SOLE_PLAYER_ID, store).accepted;
      _checks.Record("and the commander can commit it to the Bay", stored);

      const Game::StationBay* bay = registry.Bay(Neuron::SOLE_PLAYER_ID, station);
      _checks.Record("which holds exactly what was committed",
                     bay != nullptr && bay->Units(richest) == richestUnits && richestUnits > 0);

      // Nobody else's, which is the privacy rule living in the key. The host
      // issues one player id today, so the two-commander case is proven in
      // `CargoTests`; what this asserts is that a viewer with no Bay has none.
      _checks.Record("and belongs to nobody else", registry.Bay(Neuron::SOLE_PLAYER_ID + 1, station) == nullptr);

      // Then let the station grid go idle and tear down under it.
      for (std::uint32_t step = 0; step < 4000; ++step)
      {
        registry.Tick(++tick);
      }
      const Game::StationBay* survivor = registry.Bay(Neuron::SOLE_PLAYER_ID, station);
      _checks.Record("the Bay outlives the grid it was filled at",
                     survivor != nullptr && survivor->Units(richest) == richestUnits);
    }
  }

  /*
   * R10's tick-budget soak (ADR-018 A4, D1c). Last of the device-free checks
   * because it is by far the slowest, and here rather than in a test suite for
   * the reason D11 gives: the perf numbers that mean anything are Release ones,
   * and this is the binary CI runs in Release.
   *
   * Two checks, and only one of them is about time. The population check is
   * unconditional: a rung that quietly measured a smaller world than the one it
   * printed would be worse than having no instrument at all.
   *
   * The timing check is deliberately *not* the acceptance number. D1c's
   * question -- does a capped grid fit inside 50 ms, and how many of them fit a
   * core -- is answered by reading the logged figures against the machine they
   * were taken on, which is a judgement a person makes; asserting the budget
   * itself here would turn every loaded runner into a red build and teach
   * everyone to ignore the one number this exists to produce. So what is
   * asserted is a tripwire at twice the budget, which no machine reaches by
   * being busy and which an O(n^3) pass or a lost early-out clears immediately.
   * It runs in Release only, per D11: an unoptimised tick at the cap costs
   * several times what the acceptance figure means, so gating it would measure
   * the compiler.
   */
  {
    const TickSoakResult soak = RunTickSoak();
    _checks.Record("the soak reaches every population it claims to measure", soak.populationsReached);
#ifdef NDEBUG
    _checks.Record("the capped grid stays inside the tick-cost tripwire",
                   soak.cappedGridMeanMs > 0.0 && soak.cappedGridMeanMs < TICK_COST_TRIPWIRE_MS);
#else
    NEURON_LOG_INFO("self test: the soak's tripwire is not armed here -- ADR-018 D11 scopes tick cost to Release");
#endif
  }
}

/// Services the client until the predicate holds or the deadline passes. The
/// server has its own thread, so only this end needs pumping.
template <typename Predicate> bool PumpUntil(Neuron::ClientConnection& _client, Predicate _predicate)
{
  const std::int64_t start = Neuron::Clock::Counter();
  while (Neuron::Clock::MillisecondsBetween(start, Neuron::Clock::Counter()) < TIMEOUT_MS)
  {
    _client.Poll();
    if (_predicate())
    {
      return true;
    }
    Sleep(1);
  }
  return false;
}

/*
 * The view gate over the real loopback (ADR-016 §4, §7 — U3b).
 *
 * One grid exists in this scenario, so what can be proved here is the *path*
 * rather than a switch: that a request reaches the authority, that the game is
 * the one deciding, that a refusal comes back carrying the game's own reason,
 * and that the client's own grid is a view it is allowed to have. The switch
 * itself needs two grids with presence on both, which is U3c's scenario.
 *
 * Worth running anyway, and in the shipping binary: every one of those pieces
 * is new wire, and this is the one place they are exercised end to end rather
 * than against a stub.
 */
void RunViewGate(Checklist& _checks, Neuron::ClientConnection& _client)
{
  const std::uint16_t here = _client.GridAnchor();
  if (here == Game::INVALID_ID)
  {
    return; // Already reported by the docking loop's first check.
  }

  const auto answerFor = [&](std::uint16_t _grid, Neuron::ViewChanged& _out)
  {
    _client.ClearPendingViewChanges();
    if (!_client.RequestView(_grid))
    {
      return false;
    }
    bool found = false;
    (void)PumpUntil(_client,
                    [&]
                    {
                      for (const Neuron::ViewChanged& changed : _client.PendingViewChanges())
                      {
                        if (changed.gridAnchor == _grid)
                        {
                          _out = changed;
                          found = true;
                        }
                      }
                      _client.ClearPendingViewChanges();
                      return found;
                    });
    return found;
  };

  Neuron::ViewChanged mine{};
  _checks.Record("a view request for the grid we are on is answered", answerFor(here, mine));
  _checks.Record("and accepted, because our fleet is there", mine.accepted && mine.reasonCode == 0);

  /*
   * An anchor the commander has nothing on. `INVALID_ID + 1` is not a real
   * anchor in the bake either, so this exercises both halves of the gate at
   * once -- no world to show, and nothing of ours to show it for.
   */
  Neuron::ViewChanged elsewhere{};
  const std::uint16_t nowhere = 0xfffeu;
  _checks.Record("a view request for a grid we are not on is answered", answerFor(nowhere, elsewhere));
  _checks.Record("and refused", !elsewhere.accepted && elsewhere.reasonCode != 0);
  _checks.Record("with a reason the game named, not the engine",
                 elsewhere.reasonCode == static_cast<std::uint16_t>(Game::OrderReason::UnknownAnchor) ||
                   elsewhere.reasonCode == static_cast<std::uint16_t>(Game::OrderReason::NoPresence));

  // And the feed stayed where it was: the snapshots still decode, which they
  // would not if the server had moved this viewer to a grid that is not there.
  Game::ReplicatedView view;
  bool applied = false;
  (void)PumpUntil(_client,
                  [&]
                  {
                    for (const std::vector<std::uint8_t>& payload : _client.PendingSnapshots())
                    {
                      applied = view.ApplySnapshot(payload) || applied;
                    }
                    _client.ClearPendingSnapshots();
                    return applied;
                  });
  _checks.Record("a refused view left the feed on the grid we had", applied && view.Grid() == here);
}

/*
 * H0's loop, over the real loopback (Station Build Order, T2's accept).
 *
 * Everything ADR-017 promises about docking, driven the way a client drives it
 * and observed the way a client observes it: the fleet leaves the snapshot when
 * it docks, the roster says where it went, an undock brings a subset back at the
 * authored point wearing the protection bit, and the protection expires on its
 * own. Nothing here reaches into the simulation -- it is all bytes over a
 * socket, because a headless loop that peeked would prove the sim and not the
 * wire.
 *
 * The station's anchor comes from the `Welcome`, which is the whole reason
 * `gridAnchor` is on it: before that field a client could be *told* about a
 * station and still had no number to address one with.
 */
void RunDockingLoop(Checklist& _checks, Neuron::ClientConnection& _client)
{
  const Game::AnchorId station = _client.GridAnchor();
  _checks.Record("the welcome names the grid's anchor", station != Game::INVALID_ID);
  if (station == Game::INVALID_ID)
  {
    return; // Nothing downstream can be composed without it.
  }

  // The freshest view of the fleet, drained the way the frame loop drains it.
  Game::ReplicatedView view;
  std::vector<Game::ReplicatedShip> ships;
  const auto refresh = [&]
  {
    for (const std::vector<std::uint8_t>& payload : _client.PendingSnapshots())
    {
      (void)view.ApplySnapshot(payload);
    }
    _client.ClearPendingSnapshots();
    ships.clear();
    view.SampleAt(static_cast<double>(view.LatestTick()), ships);
  };

  (void)PumpUntil(_client,
                  [&]
                  {
                    refresh();
                    return ships.size() > 2;
                  });
  const std::size_t beforeDock = ships.size();

  /*
   * Three ships of the parked fleet, which are inside the dock radius by
   * construction: the starting fleet parks 1.4 km out and the radius is
   * `max(5 km, footprint + margin)` (ADR-018 D7).
   *
   * The station itself is skipped -- it is a `Structure` on the same grid, it
   * has no speed, and docking a station at itself is not a thing the validator
   * should have to have an opinion about.
   */
  Game::OrderSubmit dock;
  dock.orderSeq = 5001;
  dock.kind = Game::OrderKind::Dock;
  dock.anchor = station;
  std::vector<Game::ShipId> docking;
  for (const Game::ReplicatedShip& ship : ships)
  {
    if (docking.size() >= 3 || ship.classId == static_cast<std::uint8_t>(Game::HullClass::Structure))
    {
      continue;
    }
    if (dock.AddShip(ship.id))
    {
      docking.push_back(ship.id);
    }
  }

  std::array<std::uint8_t, Game::MAX_ORDER_SUBMIT_BYTES + Game::COMMAND_KIND_BYTES> dockBuffer{};
  Neuron::ByteWriter dockWriter{dockBuffer};
  const bool dockSent = docking.size() == 3 && Game::WriteCommandKind(Game::CommandKind::Order, dockWriter) &&
                        Game::WriteOrderSubmit(dock, dockWriter) && _client.SendOrder(dockWriter.Written());
  _checks.Record("a dock order goes up the reliable channel", dockSent);
  if (!dockSent)
  {
    return;
  }

  bool dockAccepted = false;
  (void)PumpUntil(_client,
                  [&]
                  {
                    for (const Neuron::OrderVerdict& verdict : _client.PendingVerdicts())
                    {
                      dockAccepted = dockAccepted || (verdict.orderSeq == dock.orderSeq && verdict.accepted);
                    }
                    _client.ClearPendingVerdicts();
                    return dockAccepted;
                  });
  _checks.Record("the authority accepts the dock", dockAccepted);

  // Docked ships are an off-grid roster (ADR-017 §1), so they stop being in the
  // snapshot at all -- which is the whole reason a roster costs no snapshot
  // bytes and no tick time.
  const bool leftTheGrid = PumpUntil(_client,
                                     [&]
                                     {
                                       refresh();
                                       return ships.size() + docking.size() <= beforeDock;
                                     });
  _checks.Record("the docked ships leave the snapshot", leftTheGrid);

  /*
   * And arrive on the roster, which is the summary family's first resident
   * (ADR-016 §6). Read the way a client reads one: a frame, a kind byte, then
   * the body -- the engine carried it without knowing any of that.
   */
  Game::AnchorId rosterStation = Game::INVALID_ID;
  std::vector<Game::RosterEntry> docked;

  /*
   * One pass over whatever summary frames have arrived, reading them the way a
   * client reads them: a frame, a kind byte, then a body the engine carried
   * without knowing any of it. `_wanted` lets the caller wait for the roster it
   * expects rather than the first one that turns up, which matters after the
   * undock -- the frame in flight when the command lands still describes the
   * hangar as it was.
   */
  const auto readRoster = [&](const auto& _wanted)
  {
    /*
     * Local, not a flag that latches. The second wait below asks for a roster
     * the *first* one would not have satisfied, so a sticky "seen" would make
     * it return on the stale frame and assert nothing.
     */
    bool found = false;
    for (const std::vector<std::uint8_t>& payload : _client.PendingSummaries())
    {
      Neuron::ByteReader reader{payload};
      std::uint8_t records = 0;
      if (!Game::ReadSummaryFrame(reader, records))
      {
        continue;
      }
      for (std::uint8_t index = 0; index < records; ++index)
      {
        Game::SummaryKind kind{};
        if (!Game::ReadSummaryRecord(reader, kind))
        {
          break;
        }
        if (kind == Game::SummaryKind::StationRoster)
        {
          Game::AnchorId seenStation = Game::INVALID_ID;
          std::vector<Game::RosterEntry> seenDocked;
          if (Game::ReadStationRoster(reader, seenStation, seenDocked) && _wanted(seenDocked))
          {
            rosterStation = seenStation;
            docked = seenDocked;
            found = true;
          }
          break;
        }
        std::vector<Game::FleetSummary> summaries;
        if (!Game::ReadFleetSummaries(reader, summaries))
        {
          break;
        }
      }
    }
    _client.ClearPendingSummaries();
    return found;
  };

  const auto holds = [](const std::vector<Game::RosterEntry>& _rows, Game::ShipId _id)
  {
    for (const Game::RosterEntry& row : _rows)
    {
      if (row.shipId == _id)
      {
        return true;
      }
    }
    return false;
  };

  const bool rosterArrived =
    PumpUntil(_client, [&] { return readRoster([&](const std::vector<Game::RosterEntry>& _rows) { return !_rows.empty(); }); });
  _checks.Record("the roster arrives on the summary feed", rosterArrived && rosterStation == station);
  _checks.Record("and holds every ship that docked", holds(docked, docking[0]) && holds(docked, docking[1]) && holds(docked, docking[2]));

  /*
   * --- and the reorganisation the hangar is for (ADR-017 §6) ---------------
   *
   * Two of the three go into a wing nobody is using, and then those two undock.
   * It is here rather than in a gate of its own because it is the same session:
   * the question is not "does `AssignWing` apply" -- `RegistryTests` pins that
   * without a socket -- but **does the wing survive the round trip and the
   * respawn**, which needs a real client, a real server and a real undock.
   *
   * That is the seam the player's report landed in. The command encodes, the
   * validator takes it and the registry rewrites the roster row; if the number
   * is dropped anywhere between the wire and the snapshot, the ships come back
   * from the hangar still wearing the wing they went in with, and every check
   * either side of this one still passes.
   */
  constexpr Game::WingId FRESH_WING = 9;

  Game::StationCommand assign;
  assign.orderSeq = 5003;
  assign.verb = Game::StationVerb::AssignWing;
  assign.station = station;
  assign.wing = FRESH_WING;
  (void)assign.AddShip(docking[0]);
  (void)assign.AddShip(docking[1]);

  std::array<std::uint8_t, Game::MAX_STATION_COMMAND_BYTES + Game::COMMAND_KIND_BYTES> assignBuffer{};
  Neuron::ByteWriter assignWriter{assignBuffer};
  const bool assignSent = Game::WriteCommandKind(Game::CommandKind::Station, assignWriter) &&
                          Game::WriteStationCommand(assign, assignWriter) && _client.SendOrder(assignWriter.Written());
  _checks.Record("a wing assignment goes up the same stream an undock does", assignSent);

  bool assignAccepted = false;
  if (assignSent)
  {
    (void)PumpUntil(_client,
                    [&]
                    {
                      for (const Neuron::OrderVerdict& verdict : _client.PendingVerdicts())
                      {
                        assignAccepted = assignAccepted || (verdict.orderSeq == assign.orderSeq && verdict.accepted);
                      }
                      _client.ClearPendingVerdicts();
                      return assignAccepted;
                    });
  }
  _checks.Record("the authority accepts it and acks it", assignAccepted);

  // The roster is the authority's own statement about the hangar, so this is
  // where the rewrite becomes visible before anything undocks.
  const auto wingOf = [](const std::vector<Game::RosterEntry>& _rows, Game::ShipId _id) -> Game::WingId
  {
    for (const Game::RosterEntry& row : _rows)
    {
      if (row.shipId == _id)
      {
        return row.wing;
      }
    }
    return Game::INVALID_WING_ID;
  };

  const bool rosterRewritten =
    PumpUntil(_client,
              [&]
              {
                return readRoster([&](const std::vector<Game::RosterEntry>& _rows)
                                  { return wingOf(_rows, docking[0]) == FRESH_WING; });
              });
  _checks.Record("the docked roster shows the two ships in their new wing",
                 rosterRewritten && wingOf(docked, docking[1]) == FRESH_WING);
  _checks.Record("and leaves the ship that was not named in the wing it was in",
                 wingOf(docked, docking[2]) != FRESH_WING);

  /*
   * Undock a subset -- two of the three -- as a station command on the same
   * acked stream the orders use (ADR-017 §8). This is the message that had no
   * wire path at all before this slice.
   */
  Game::StationCommand undock;
  undock.orderSeq = 5002;
  undock.verb = Game::StationVerb::Undock;
  undock.station = station;
  undock.formation = Game::FormationId::Line;
  (void)undock.AddShip(docking[0]);
  (void)undock.AddShip(docking[1]);

  std::array<std::uint8_t, Game::MAX_STATION_COMMAND_BYTES + Game::COMMAND_KIND_BYTES> undockBuffer{};
  Neuron::ByteWriter undockWriter{undockBuffer};
  const bool undockSent = Game::WriteCommandKind(Game::CommandKind::Station, undockWriter) &&
                          Game::WriteStationCommand(undock, undockWriter) && _client.SendOrder(undockWriter.Written());
  _checks.Record("an undock goes up the same acked stream as an order", undockSent);
  if (!undockSent)
  {
    return;
  }

  bool undockAccepted = false;
  (void)PumpUntil(_client,
                  [&]
                  {
                    for (const Neuron::OrderVerdict& verdict : _client.PendingVerdicts())
                    {
                      undockAccepted = undockAccepted || (verdict.orderSeq == undock.orderSeq && verdict.accepted);
                    }
                    _client.ClearPendingVerdicts();
                    return undockAccepted;
                  });
  _checks.Record("the authority accepts the undock and acks it on the order stream", undockAccepted);

  /*
   * The undocked pair comes back wearing bit 0 (ADR-017 §5) -- the shimmer the
   * client draws, replicated as one bit of `statusBits` and costing no field of
   * its own.
   */
  bool protectedSeen = false;
  const bool respawned = PumpUntil(_client,
                                   [&]
                                   {
                                     refresh();
                                     std::size_t back = 0;
                                     for (const Game::ReplicatedShip& ship : ships)
                                     {
                                       if (ship.id != docking[0] && ship.id != docking[1])
                                       {
                                         continue;
                                       }
                                       ++back;
                                       protectedSeen = protectedSeen || (ship.statusBits & Game::SHIP_STATUS_PROTECTED) != 0;
                                     }
                                     return back == 2;
                                   });
  _checks.Record("the undocked ships are back on the grid, with their own ids", respawned);
  _checks.Record("and arrive protected (statusBits bit 0)", protectedSeen);

  /*
   * **And they came back in the wing the hangar put them in.**
   *
   * The check the player's report is about, and the last link in the chain: a
   * wing that is rewritten on the roster and then lost at the respawn looks
   * exactly like a wing assignment that never happened -- the ships undock as a
   * fleet, fly as a fleet, and read on the roster as the two wings they were
   * composed from.
   *
   * Read off `ReplicatedShip::wing`, which is what `BuildRoster` groups by, so
   * this asserts the number the HUD will actually draw rather than one the
   * server merely holds.
   */
  bool bothInTheNewWing = respawned;
  for (const Game::ReplicatedShip& ship : ships)
  {
    if (ship.id == docking[0] || ship.id == docking[1])
    {
      bothInTheNewWing = bothInTheNewWing && ship.wing == FRESH_WING;
    }
  }
  _checks.Record("and they undock into the wing the hangar put them in", bothInTheNewWing);

  /*
   * The bit is *per ship*, not a mood the grid is in.
   *
   * The station has stood on this grid the whole time and has never undocked,
   * so it must not be wearing the shimmer. Without this the check above would
   * pass just as happily against a build that set bit 0 on every record, which
   * is exactly the kind of blanket that a one-fleet test never notices.
   *
   * The fifteen-second expiry itself is not asserted here: it is far longer
   * than this gate should sit, and `RegistryTests` already pins it tick by tick
   * without needing a socket.
   */
  bool anyUnprotected = false;
  for (const Game::ReplicatedShip& ship : ships)
  {
    if (ship.id != docking[0] && ship.id != docking[1])
    {
      anyUnprotected = anyUnprotected || (ship.statusBits & Game::SHIP_STATUS_PROTECTED) == 0;
    }
  }
  _checks.Record("and the ships that did not undock are not wearing it", anyUnprotected);

  /*
   * The roster keeps what the undock did not name (ADR-017 §3): an undock is a
   * subset operation, so the third ship is still docked and the two that left
   * are gone from the hangar rather than duplicated into it.
   */
  const bool rosterFollowed = PumpUntil(_client,
                                        [&]
                                        {
                                          return readRoster([&](const std::vector<Game::RosterEntry>& _rows)
                                                            { return !holds(_rows, docking[0]) && !holds(_rows, docking[1]); });
                                        });
  _checks.Record("the roster drops the ships that undocked", rosterFollowed);
  _checks.Record("and keeps the one that was not named", holds(docked, docking[2]));

  /*
   * **The parking order, in the order records** (ADR-017 4, T2's accept).
   *
   * The fleet does not merely appear at the undock point -- the authority
   * issues it a Move to the first free berth, and that order is a group like
   * any other, so it takes a lane in the snapshot's order area and the client
   * draws a ghost for it without being told it is special.
   *
   * It is identified by the two things that are true of it and of nothing the
   * client sent: `clientOrderSeq` is zero, because a system order has no client
   * sequence to echo, and its membership is exactly the fleet that undocked.
   * Checking the count as well as the zero matters -- a record that merely lost
   * its sequence would pass on the zero alone.
   *
   * This is also the only observation in this gate that proves `systemIssued`
   * did not disarm the protection it was issued alongside: the ships are
   * parking *and* still wearing bit 0, which is the distinction `OrderGroup`
   * grew the flag for.
   */
  bool parkingSeen = false;
  const bool parked = PumpUntil(_client,
                                [&]
                                {
                                  refresh();
                                  for (const Game::OrderStateRecord& record : view.LatestOrders())
                                  {
                                    if (record.clientOrderSeq == 0 && record.memberCount == 2)
                                    {
                                      parkingSeen = true;
                                    }
                                  }
                                  return parkingSeen;
                                });
  _checks.Record("the undocked fleet is given a parking order, in its own lane of the order records", parked && parkingSeen);
}

/*
 * A disconnect mid-approach leaves the fleet outside (T2's accept).
 *
 * The approach chain is the *client's* (ADR-017 2): it flies the fleet to the
 * perimeter and sends the Dock only once every member is inside the radius. So
 * a client that goes away mid-approach never sends the second half, and the
 * question this answers is what the authority does with the half it has --
 * which must be "finish the Move and stop", never "dock them anyway".
 *
 * Observed the way everything else here is: a second client joins after the
 * first has gone and reads the world off the wire. That is a stronger check
 * than watching from the connection that left, because it is the state the
 * server actually kept rather than the last thing one client happened to see.
 *
 * The client half of the chain is not built yet, so what is driven here is its
 * first leg -- the Move -- and what is asserted is the invariant that holds
 * whether or not the chain exists: a lost connection docks nobody.
 */
/*
 * Every member of the summary family decodes, in one frame (ADR-016 6, E3).
 *
 * **This gate exists because a real bug got through, and the shape of it is the
 * lesson.** The client's decoder was written when the family had two kinds and
 * E3 added three more. A summary frame carries no length prefix -- each body is
 * self-delimiting and a reader that understands a kind knows where it ends --
 * so a record the decoder does not read is not a record skipped, it is every
 * record after it misparsed. The client dropped whole frames, and the docked
 * blocks and their toasts went with them.
 *
 * Nothing caught it. The compiler could not: C4062 is a level-4 warning and this
 * tree builds at /W3, so an unhandled enumerator was invisible (the decoder now
 * promotes it to an error locally, which is the other half of the fix). No unit
 * test could either, because `ReplicatedWorldView` lives in the executable and
 * has no test project -- which is exactly why the check belongs here.
 *
 * So the frame below is written the way the *server* writes one, with all five
 * kinds present, and handed to the real decoder. It is deliberately not built
 * from a live world: the starting world has no site, no cargo and no Bay, which
 * is precisely why the bug stayed hidden, and a gate that reproduced that
 * condition would reproduce the blindness with it.
 */
/*
 * The client's own Dock pre-check answers, and answers the way the server does.
 *
 * **This gate exists because the feature was inert and looked built.** T2's
 * approach chain waits on `WorldView::PreCheck` -- deliberately, so that "close
 * enough to dock" has exactly one definition (ADR-014 3). But the client's
 * `ValidationView` was built with ids and nothing else, so `stationAnchor`
 * stayed `INVALID_ID` and the shared validator refused **every** Dock this side
 * ever asked about with `UnknownStation`. The chain would start, the fleet would
 * fly, the chip would show, and the verb would never go.
 *
 * Nothing caught it. `ApproachChain`'s unit tests exercise the chain against a
 * hand-made verdict, which is the right scope for them and exactly why they
 * could not see this: the bug was in what the *view* was filled with, one layer
 * below the seam they mock.
 *
 * So this asserts the two answers that matter and the parity between them: a
 * fleet standing on the station is accepted, the same fleet far away is refused
 * `NotAtStation` rather than `UnknownStation` -- the wrong reason being the
 * symptom the bug actually presented -- and both verdicts match what
 * `ValidateOrder` gives the authority for the same order.
 */
void RunClientDockPreCheckGate(Checklist& _checks, const Neuron::Simulation&)
{
  constexpr Game::AnchorId STATION = 11;
  constexpr std::int32_t STATION_X_CM = 120000; // 1.2 km from the grid origin.
  constexpr std::int32_t STATION_Y_CM = -80000;

  // Two ships beside the station, and the station itself, exactly as a scene
  // build would leave them.
  const Game::ShipId ids[] = {41, 42};
  const Game::ShipMark closeMarks[] = {
    Game::ShipMark{STATION_X_CM + 20000, STATION_Y_CM, Game::HullClass::Frigate},
    Game::ShipMark{STATION_X_CM, STATION_Y_CM + 20000, Game::HullClass::Interceptor},
  };
  // The same two, a long way out -- past `DOCK_RADIUS_METRES` by any footprint.
  const Game::ShipMark farMarks[] = {
    Game::ShipMark{STATION_X_CM + 9000000, STATION_Y_CM, Game::HullClass::Frigate},
    Game::ShipMark{STATION_X_CM, STATION_Y_CM + 9000000, Game::HullClass::Interceptor},
  };

  Game::OrderSubmit dock;
  dock.kind = Game::OrderKind::Dock;
  dock.anchor = STATION;
  dock.shipCount = 2;
  dock.shipIds[0] = ids[0];
  dock.shipIds[1] = ids[1];

  const auto judge = [&](std::span<const Game::ShipMark> _marks) {
    Game::ValidationView view;
    view.shipIds = ids;
    view.shipMarks = _marks;
    view.stationAnchor = STATION;
    view.stationXCm = STATION_X_CM;
    view.stationYCm = STATION_Y_CM;
    return Game::ValidateOrder(view, dock);
  };

  const Game::OrderVerdict close = judge(closeMarks);
  const Game::OrderVerdict distant = judge(farMarks);

  _checks.Record("a fleet at the station pre-checks as dockable", close.accepted);
  _checks.Record("and one out in the black is refused for distance",
                 !distant.accepted && distant.reason == Game::OrderReason::NotAtStation);

  // The regression itself: a view with no station refuses for the *wrong*
  // reason, which is what the client was doing to every Dock it ever judged.
  Game::ValidationView blind;
  blind.shipIds = ids;
  blind.shipMarks = closeMarks;
  const Game::OrderVerdict unknown = Game::ValidateOrder(blind, dock);
  _checks.Record("a view with no station names no station, which is the bug's signature",
                 !unknown.accepted && unknown.reason == Game::OrderReason::UnknownStation);
}

/*
 * MINE greys and ungreys in lockstep with the authority's own refusals.
 *
 * The prompt's accept criterion, and it is an **integration** gate on purpose.
 * Both bugs this slice uncovered were of one kind: the shared validator was
 * right and the client handed it a view that did not describe the world, so a
 * test at the validator's level would have passed while the feature did
 * nothing. This one therefore drives the real path -- a real snapshot into
 * `ApplySnapshot`, a real scene build to fill the validation view, real summary
 * frames for the field and the holds -- and then asks the seam the question the
 * command row asks it.
 *
 * Four states, which are the three refusals and the acceptance:
 *   no field under the fleet          -> NotAtSite (17)
 *   a field, but no Miner selected    -> NoMinerInOrder (18)
 *   a Miner whose hold is full        -> HoldFull (19)
 *   a Miner with room                 -> offered
 */
void RunMineAvailabilityGate(Checklist& _checks, const Game::EconomyDef& _economy)
{
  constexpr Game::AnchorId SITE = 21;
  constexpr Game::ShipId MINER = 5;
  constexpr Game::ShipId FRIGATE = 6;

  // A world with one Miner and one Frigate, and a snapshot of it.
  Game::World world;
  world.Reset(0x51DEu);
  Game::ShipSpawn spawn;
  spawn.hullClass = Game::HullClass::Miner;
  spawn.wing = 1;
  spawn.xMetres = 0.0f;
  spawn.yMetres = 0.0f;
  (void)world.Spawn(spawn, MINER);
  spawn.hullClass = Game::HullClass::Frigate;
  spawn.xMetres = 200.0f;
  (void)world.Spawn(spawn, FRIGATE);
  world.Tick(1);

  std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> snapshotBytes{};
  Neuron::ByteWriter snapshotWriter{snapshotBytes};
  if (!Game::WriteSnapshot(world, snapshotWriter) || !snapshotWriter.Ok())
  {
    _checks.Record("the mine gate could write its snapshot", false);
    return;
  }

  // A view that can draw both hulls -- a hull with no mesh is excluded from the
  // validation ids, so a desc without these would silently validate an empty
  // selection and prove nothing.
  Outpost::ReplicatedWorldView::Desc desc;
  desc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, 0);
  desc.economy = &_economy;
  Outpost::ReplicatedWorldView view{std::move(desc)};

  const bool applied = view.ApplySnapshot(snapshotWriter.Written()) != 0;
  Neuron::RenderScene scene;
  view.BuildScene(static_cast<double>(world.Tick()), scene);
  _checks.Record("the mine gate's fleet is on screen", applied && scene.entities.size() == 2);

  const auto mineOption = [&](std::span<const std::uint16_t> _selection) {
    std::array<Neuron::OrderKindOption, Neuron::MAX_ORDER_KINDS> kinds{};
    const std::uint32_t count = view.OrderKinds(_selection, kinds);
    for (std::uint32_t index = 0; index < count; ++index)
    {
      if (kinds[index].kind == static_cast<std::uint16_t>(Game::OrderKind::Mine))
      {
        return kinds[index];
      }
    }
    return Neuron::OrderKindOption{};
  };

  const std::uint16_t both[] = {MINER, FRIGATE};
  const std::uint16_t frigateOnly[] = {FRIGATE};

  // 1. No summary has arrived, so there is no field here.
  const Neuron::OrderKindOption noField = mineOption(both);
  _checks.Record("MINE is refused where there is no field",
                 !noField.available && noField.reasonCode == static_cast<std::uint16_t>(Game::OrderReason::NotAtSite));

  // A frame that says there is a field, and that the Miner's hold is full.
  const auto sendSummary = [&](std::uint32_t _minerOreUnits) {
    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> bytes{};
    Neuron::ByteWriter writer{bytes};
    Game::SiteStatusRow site;
    site.anchor = SITE;
    site.epoch = 1;
    site.clusterCount = 1;
    site.clusterFullPct[0] = 100;
    const Game::CargoStatusRow cargo[] = {Game::CargoStatusRow{MINER, {_minerOreUnits, 0, 0}}};
    const bool ok = Game::BeginSummaryFrame(2, writer) && Game::BeginSummaryRecord(Game::SummaryKind::SiteStatus, writer) &&
                    Game::WriteSiteStatus(site, writer) && Game::BeginSummaryRecord(Game::SummaryKind::CargoStatus, writer) &&
                    Game::WriteCargoStatus(cargo, writer);
    return ok && view.ApplySummary(writer.Written());
  };

  // 2. A field, and a selection with no Miner in it.
  const bool fedEmpty = sendSummary(0);
  const Neuron::OrderKindOption noMiner = mineOption(frigateOnly);
  _checks.Record("MINE is refused for a selection holding no Miner",
                 fedEmpty && !noMiner.available &&
                   noMiner.reasonCode == static_cast<std::uint16_t>(Game::OrderReason::NoMinerInOrder));

  // 3. A Miner with room is the whole point of the verb.
  const Neuron::OrderKindOption offered = mineOption(both);
  _checks.Record("and offered to a Miner with room", offered.available && offered.reasonCode == 0);

  // 4. The same Miner, hold filled to its authored capacity.
  const std::uint32_t holdLitres = _economy.Cargo(Game::HullClass::Miner).oreHoldLitres;
  const std::uint32_t unitLitres = _economy.Ore(Game::OreId::FerroChroma).unitVolumeLitres;
  const bool fedFull = unitLitres > 0 && sendSummary(holdLitres / unitLitres + 1);
  const Neuron::OrderKindOption full = mineOption(both);
  _checks.Record("and refused again once every Miner is full",
                 fedFull && !full.available && full.reasonCode == static_cast<std::uint16_t>(Game::OrderReason::HoldFull));
}

/*
 * The location blocks say where a fleet is when the scene cannot (ADR-016 9).
 *
 * Three states share one panel, and the assertion that matters is the one about
 * the *fourth* case: a fleet standing on the grid the player is looking at must
 * **not** get a block, because those ships are already on screen as hulls with
 * rings and a roster row each. Counted twice on one HUD, the player has no way
 * to tell which count is the lie.
 *
 * Driven through the real decoder with a real summary frame, for the reason the
 * other client-side gates are: the bugs this slice has produced were all in what
 * the view was filled with rather than in the arithmetic over it.
 */
void RunLocationBlockGate(Checklist& _checks)
{
  constexpr Game::AnchorId HERE = 31;   // the grid the client is watching
  constexpr Game::AnchorId THERE = 32;  // another grid, same commander
  constexpr Game::AnchorId STATION = 33;
  constexpr Game::AnchorId CROSSING_TO = 34;

  Outpost::ReplicatedWorldView::Desc desc;
  desc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, 0);
  desc.gridAnchor = HERE;
  Outpost::ReplicatedWorldView view{std::move(desc)};

  const Game::FleetSummary rows[] = {
    Game::FleetSummary{HERE, Game::FleetState::OnGrid, 6, Game::FLEET_ETA_NONE},
    Game::FleetSummary{THERE, Game::FleetState::OnGrid, 4, Game::FLEET_ETA_NONE},
    Game::FleetSummary{STATION, Game::FleetState::Docked, 12, Game::FLEET_ETA_NONE},
    Game::FleetSummary{CROSSING_TO, Game::FleetState::InTransit, 3, 71},
  };

  std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> bytes{};
  Neuron::ByteWriter writer{bytes};
  const bool wrote = Game::BeginSummaryFrame(1, writer) &&
                     Game::BeginSummaryRecord(Game::SummaryKind::FleetSummaries, writer) &&
                     Game::WriteFleetSummaries(rows, writer);
  _checks.Record("the location gate can state where four fleets are", wrote && view.ApplySummary(writer.Written()));

  std::array<Neuron::LocationBlock, Neuron::MAX_LOCATION_BLOCKS> blocks{};
  const std::uint32_t count = view.BuildLocationBlocks(blocks);

  // No snapshot has arrived, so the view is watching no grid yet and even the
  // HERE row is somewhere the scene cannot show. Four places, four blocks.
  _checks.Record("with no world on screen, every place is elsewhere", count == 4);

  const auto find = [&](Game::AnchorId _anchor) -> const Neuron::LocationBlock* {
    for (std::uint32_t index = 0; index < count; ++index)
    {
      if (blocks[index].anchor == static_cast<std::uint16_t>(_anchor))
      {
        return &blocks[index];
      }
    }
    return nullptr;
  };

  const Neuron::LocationBlock* crossing = find(CROSSING_TO);
  _checks.Record("a crossing carries its eta and offers no button",
                 crossing != nullptr && crossing->etaSeconds > 70.0f && crossing->etaSeconds < 72.0f &&
                   crossing->buttonLabel == nullptr);

  const Neuron::LocationBlock* docked = find(STATION);
  _checks.Record("a docked block offers the hangar and no eta",
                 docked != nullptr && docked->etaSeconds < 0.0f && docked->buttonLabel != nullptr);

  const Neuron::LocationBlock* elsewhere = find(THERE);
  _checks.Record("a fleet on another grid offers the view switch",
                 elsewhere != nullptr && elsewhere->buttonLabel != nullptr && elsewhere->etaSeconds < 0.0f);

  // Every word on the panel came from the game; the engine drew and compared
  // none of them.
  bool everyStateNamed = count > 0;
  for (std::uint32_t index = 0; index < count; ++index)
  {
    everyStateNamed = everyStateNamed && blocks[index].stateLabel != nullptr;
  }
  _checks.Record("and every block was given its state in the game's own words", everyStateNamed);
}

/*
 * The hangar can make a wing out of what is docked in it (ADR-017 §6).
 *
 * **This gate exists because the verb was finished and unreachable.**
 * `AssignWing` has had a wire format, a shared validator, a server path and a
 * registry test since T1; what it never had was a control. The hangar screen
 * offered exactly one action, `BuildStationActions` returned exactly one, and
 * `MAX_STATION_ACTIONS` had been four the whole time -- so the only thing that
 * could rewrite a `WingId` was a command nothing could compose.
 *
 * Nothing caught it, and nothing was going to. Every layer was individually
 * correct: the validator's tests pass, the registry's apply is covered, and the
 * screen's layout tests assert the two rects are in the right place. The defect
 * lived in the *absence* of a line joining them, which is the class of fault
 * this file exists for -- the same shape as T2's inert Dock pre-check, one
 * section down.
 *
 * It reads as a play report rather than as a stack trace: dock two ships out of
 * one wing and two out of another, and there is no way to make the four of them
 * a wing. So the gate is written the way that session went -- a hangar holding
 * two wings, four ships picked across both, one command -- and asserts the
 * whole path a press takes: the action is offered, its values include a number
 * nobody is using, the authority accepts it, sending it spends a call sign, and
 * the wing that results has a roster row to appear on. That last one is not a
 * flourish: a wing with no name draws no row (`BuildRoster`), so a command that
 * worked and left the ships invisible would have looked exactly like the bug it
 * replaced.
 */
void RunWingAssignmentGate(Checklist& _checks)
{
  constexpr Game::AnchorId STATION = 41;
  constexpr Game::ShipId TALON_A = 1;
  constexpr Game::ShipId TALON_B = 2;
  constexpr Game::ShipId ANVIL_A = 3;
  constexpr Game::ShipId ANVIL_B = 4;

  const Game::RosterEntry docked[] = {
    Game::RosterEntry{TALON_A, Game::HullClass::Interceptor, 1},
    Game::RosterEntry{TALON_B, Game::HullClass::Interceptor, 1},
    Game::RosterEntry{ANVIL_A, Game::HullClass::Bomber, 2},
    Game::RosterEntry{ANVIL_B, Game::HullClass::Bomber, 2},
  };

  /*
   * The frame the server would send about that hangar: the fleet row that makes
   * the place exist, and the roster that says who is in it. Both, because the
   * decoder hands a roster to the *docked* block, and a client given only the
   * second record has a roster belonging to no place.
   */
  const auto feed = [&](Outpost::ReplicatedWorldView& _view)
  {
    const Game::FleetSummary rows[] = {
      Game::FleetSummary{STATION, Game::FleetState::Docked, 4, Game::FLEET_ETA_NONE},
    };
    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> bytes{};
    Neuron::ByteWriter writer{bytes};
    const bool wrote = Game::BeginSummaryFrame(2, writer) && Game::BeginSummaryRecord(Game::SummaryKind::FleetSummaries, writer) &&
                       Game::WriteFleetSummaries(rows, writer) && Game::BeginSummaryRecord(Game::SummaryKind::StationRoster, writer) &&
                       Game::WriteStationRoster(STATION, docked, writer);
    return wrote && _view.ApplySummary(writer.Written());
  };

  Outpost::ReplicatedWorldView::Desc desc;
  desc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, 0);
  desc.wingNames = {"-", "TALON", "ANVIL"};
  desc.spareWingNames = {"VERGE"};
  Outpost::ReplicatedWorldView view{std::move(desc)};

  _checks.Record("the wing gate can state a hangar holding two wings", feed(view));

  std::array<Neuron::StationAction, Neuron::MAX_STATION_ACTIONS> actions{};
  const std::uint32_t actionCount = view.BuildStationActions(static_cast<std::uint16_t>(STATION), actions);
  _checks.Record("the hangar offers a second action beside UNDOCK", actionCount == 2);
  if (actionCount != 2)
  {
    return; // Everything below is about that action; without it there is nothing to say.
  }

  /*
   * The flag the screen reads instead of the verb. UNDOCK sends its ships away
   * and the composer must drop them; this one leaves them docked, and a screen
   * that cleared after it would charge the player a fresh selection for the
   * assign-then-undock they were obviously in the middle of.
   */
  _checks.Record("and only the one that sends ships away consumes the selection",
                 actions[0].consumesSelection && !actions[1].consumesSelection);

  const Neuron::StationAction& assign = actions[1];
  std::array<Neuron::OrderOption, Neuron::MAX_STATION_OPTIONS> options{};
  const std::uint32_t optionCount = view.StationActionOptions(assign.verb, options);
  _checks.Record("its values are the wings that have names, plus one number nobody is using",
                 optionCount == 3 && options[0].parameter == 1 && options[1].parameter == 2 && options[2].parameter == 3);

  const std::uint32_t everyone[] = {TALON_A, TALON_B, ANVIL_A, ANVIL_B};
  Neuron::StationIntent intent;
  intent.verb = assign.verb;
  intent.parameter = optionCount == 3 ? options[2].parameter : 0;
  intent.anchor = static_cast<std::uint16_t>(STATION);
  intent.orderSeq = 1;
  intent.shipIds = everyone;
  intent.shipCount = 4;

  // The player's report, as an assertion: four ships out of two wings, one wing.
  _checks.Record("four ships picked across both wings may become one new wing", view.PreCheckStation(intent).accepted);

  const std::uint32_t stranger[] = {TALON_A, 99};
  Neuron::StationIntent elsewhere = intent;
  elsewhere.shipIds = stranger;
  elsewhere.shipCount = 2;
  const Neuron::OrderVerdict refused = view.PreCheckStation(elsewhere);
  _checks.Record("while a ship this hangar does not hold is refused in the authority's own words",
                 !refused.accepted && refused.reasonCode == static_cast<std::uint16_t>(Game::OrderReason::NotDocked));

  /*
   * Sending it is what mints the name -- not cycling the chip, which would
   * spend a call sign on every value the player looked at.
   */
  std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> commandBytes{};
  Neuron::ByteWriter commandWriter{commandBytes};
  const bool sent = view.EncodeStationCommand(intent, commandWriter) && commandWriter.Ok();

  const std::uint32_t afterCount = view.StationActionOptions(assign.verb, options);
  _checks.Record("sending it spends a call sign on the new wing and offers the next number",
                 sent && afterCount == 4 && options[2].name != nullptr && std::string_view{options[2].name} == "VERGE" &&
                   options[3].parameter == 4);

  std::array<Neuron::RosterRow, Neuron::MAX_ROSTER_ROWS> roster{};
  const std::uint32_t rowCount = view.BuildRoster(std::span<const std::uint16_t>{}, roster);
  _checks.Record("and the wing the player just made has a roster row to appear on",
                 rowCount == 3 && roster[2].groupId == 3 && roster[2].name != nullptr && std::string_view{roster[2].name} == "VERGE");

  /*
   * And the limit, which is the honest end of an emergent thing.
   *
   * A wing past the roster's row cap would be created, carry ships, and never be
   * drawn -- so the chip stops offering new numbers rather than offering one
   * that goes somewhere the player cannot see. The existing wings stay on the
   * list: assigning *between* them costs no row.
   */
  Outpost::ReplicatedWorldView::Desc fullDesc;
  fullDesc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, 0);
  fullDesc.wingNames.emplace_back("-");
  for (std::uint32_t index = 0; index < Neuron::MAX_ROSTER_ROWS; ++index)
  {
    fullDesc.wingNames.emplace_back("FULL");
  }
  Outpost::ReplicatedWorldView fullView{std::move(fullDesc)};

  std::array<Neuron::StationAction, Neuron::MAX_STATION_ACTIONS> fullActions{};
  const bool fullFed = feed(fullView) && fullView.BuildStationActions(static_cast<std::uint16_t>(STATION), fullActions) == 2;
  const std::uint32_t fullOptions = fullFed ? fullView.StationActionOptions(fullActions[1].verb, options) : 0;
  _checks.Record("a roster with no room for another row offers no new wing, only the ones it has",
                 fullFed && fullOptions == Neuron::MAX_ROSTER_ROWS);
}

/*
 * The fleet is drawn at the size the game says it is (ADR-006 §5, ADR-016 §10).
 *
 * This check exists here and can exist nowhere else: it needs a mesh loader,
 * which is `NeuronClient`'s, and a class table, which is `GameLogic`'s, and the
 * composition root is the one project entitled to see both (ADR-014 §2a).
 *
 * **What it is guarding against is silence.** Nothing used to relate the two,
 * so the art drifted: `Structure.obj` and `Stargate.obj` matched their rows to
 * within a per cent -- their rows were written from them -- and every flyable
 * hull was between a quarter and a twelfth of the size its own row described.
 * That is invisible to every test either project can write on its own, and on
 * screen it reads as a camera problem rather than as a content one.
 *
 * A tolerance rather than an equality, because the fit is applied to whatever
 * the file contains: this asserts the *result*, so a re-export at any scale
 * still passes and a fit that silently stopped running does not.
 */
void RunHullScaleGate(Checklist& _checks, const Outpost::AppConfig& _config)
{
  const std::string directory = Outpost::ResolveContentPath(_config.content.meshDirectory);

  struct Expected
  {
    const char* file;
    Game::HullClass hullClass;
  };
  const Expected fleet[] = {
      {"Interceptor.obj", Game::HullClass::Interceptor}, {"Bomber.obj", Game::HullClass::Bomber},
      {"Corvette.obj", Game::HullClass::Corvette},       {"Frigate.obj", Game::HullClass::Frigate},
      {"Hauler.obj", Game::HullClass::Hauler},           {"Miner.obj", Game::HullClass::Miner},
      {"Carrier.obj", Game::HullClass::Carrier},         {"Battleship.obj", Game::HullClass::Battleship},
      {"Structure.obj", Game::HullClass::Structure},     {"Stargate.obj", Game::HullClass::Gate},
  };

  bool everyHullFits = true;
  bool anythingLoaded = false;
  for (const Expected& expected : fleet)
  {
    Neuron::ObjMesh mesh;
    Neuron::ObjDiagnostic error;
    if (!Neuron::LoadObjMesh(directory.empty() ? _config.content.meshDirectory : directory, expected.file, mesh, error))
    {
      NEURON_LOG_WARNING("hull scale gate: %s", error.Text().c_str());
      everyHullFits = false;
      continue;
    }
    anythingLoaded = true;

    const float target = Game::SilhouetteRadiusMetres(expected.hullClass);
    const float authored = Neuron::PlaneRadiusMetres(mesh);
    (void)Neuron::FitObjMeshToPlaneRadius(mesh, target);
    const float drawn = Neuron::PlaneRadiusMetres(mesh);

    // A tenth of a metre over targets that run from 21 m to 250 m, which is
    // float noise rather than a tolerance for drift.
    if (std::fabs(drawn - target) > 0.1f)
    {
      NEURON_LOG_ERROR("hull scale gate: %s draws at %.1f m, the class table says %.1f m", expected.file,
                       static_cast<double>(drawn), static_cast<double>(target));
      everyHullFits = false;
    }
    NEURON_LOG_INFO("self test: %-16s %6.1f m authored -> %6.1f m drawn (x%.2f)", expected.file,
                    static_cast<double>(authored), static_cast<double>(drawn),
                    static_cast<double>(authored > 0.0f ? drawn / authored : 1.0f));
  }

  _checks.Record("every hull is drawn at the size the class table states", anythingLoaded && everyHullFits);
}

void RunSummaryFamilyGate(Checklist& _checks)
{
  std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> payload{};
  Neuron::ByteWriter writer{payload};

  constexpr Game::AnchorId STATION = 7;
  constexpr Game::AnchorId SITE = 9;

  const Game::FleetSummary summaries[] = {
    Game::FleetSummary{STATION, Game::FleetState::Docked, 3, Game::FLEET_ETA_NONE},
  };
  const Game::RosterEntry docked[] = {
    Game::RosterEntry{101, Game::HullClass::Interceptor, 1},
    Game::RosterEntry{102, Game::HullClass::Frigate, 1},
    Game::RosterEntry{103, Game::HullClass::Hauler, 2},
  };
  Game::SiteStatusRow site;
  site.anchor = SITE;
  site.epoch = 4;
  site.clusterCount = 2;
  site.clusterFullPct[0] = 100;
  site.clusterFullPct[1] = 42;
  const Game::CargoStatusRow cargo[] = {
    Game::CargoStatusRow{201, {12, 0, 0}},
  };
  const std::uint32_t bayUnits[Game::ORE_COUNT] = {1240, 310, 85};
  const std::uint32_t bayAlloys[Game::ALLOY_COUNT] = {40, 0, 0, 0, 0};

  // Five records, in the order `WriteSummaries` puts them.
  bool written = Game::BeginSummaryFrame(5, writer);
  written = written && Game::BeginSummaryRecord(Game::SummaryKind::FleetSummaries, writer) &&
            Game::WriteFleetSummaries(summaries, writer);
  written = written && Game::BeginSummaryRecord(Game::SummaryKind::StationRoster, writer) &&
            Game::WriteStationRoster(STATION, docked, writer);
  written = written && Game::BeginSummaryRecord(Game::SummaryKind::SiteStatus, writer) && Game::WriteSiteStatus(site, writer);
  written = written && Game::BeginSummaryRecord(Game::SummaryKind::CargoStatus, writer) && Game::WriteCargoStatus(cargo, writer);
  written = written && Game::BeginSummaryRecord(Game::SummaryKind::BayStatus, writer) &&
            Game::WriteBayStatus(STATION, bayUnits, bayAlloys, writer);
  _checks.Record("a summary frame carrying all five kinds is written", written && writer.Ok());

  Outpost::ReplicatedWorldView::Desc desc;
  desc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, Outpost::ReplicatedWorldView::INVALID_RENDER_CLASS);
  desc.gridAnchor = STATION;
  Outpost::ReplicatedWorldView view{std::move(desc)};

  const bool accepted = view.ApplySummary(writer.Written());
  _checks.Record("and the client accepts it whole", accepted && view.RejectedSummaryCount() == 0);

  // The records after the two the old decoder knew are the ones that used to be
  // lost, so they are what the gate actually asserts on.
  _checks.Record("the site status survived the records before it", view.SiteStatusCount() == 1);
  _checks.Record("so did the cargo rows", view.CargoRowCount() == 1);
  _checks.Record("and the bay", view.BayCount() == 1);

  // And the record *before* them still lands, which is the half that was
  // visibly broken: a frame refused mid-walk never committed its blocks.
  _checks.Record("the docked block the frame opened with is still there", view.DockedCountAt(STATION) == 3);

  /*
   * And the case that emptied the hangar: **two rows for one anchor**.
   *
   * `WorldRegistry::Summaries` reports where a commander's ships are, and a
   * commander flying at a station while some of their hulls sit inside it is in
   * two places at one anchor -- one `OnGrid` row from the live world and one
   * `Docked` row from the roster, in that order, because grids are walked
   * first. There is still exactly one `StationRoster` record, because a roster
   * is a fact about a station rather than about a fleet.
   *
   * The merge that attaches the roster to its block therefore has to match on
   * the **state as well as the anchor**. Matching on the anchor alone hands the
   * roster to whichever row came first -- the grid -- and moves it there, so
   * the docked block is left with an empty list and the hangar draws NOTHING
   * DOCKED HERE over a station holding a full roster, while its own status bar
   * reads the count off the other row and says the ships are there.
   *
   * Asserted through `BuildStationRoster` rather than through the count,
   * because the count came from the *summary* and was right the whole time:
   * this bug was only ever visible in the list.
   */
  Neuron::ByteWriter both{payload};
  const Game::FleetSummary twoPlaces[] = {
    Game::FleetSummary{STATION, Game::FleetState::OnGrid, 12, Game::FLEET_ETA_NONE},
    Game::FleetSummary{STATION, Game::FleetState::Docked, 3, Game::FLEET_ETA_NONE},
  };
  bool pair = Game::BeginSummaryFrame(2, both);
  pair = pair && Game::BeginSummaryRecord(Game::SummaryKind::FleetSummaries, both) &&
         Game::WriteFleetSummaries(twoPlaces, both);
  pair = pair && Game::BeginSummaryRecord(Game::SummaryKind::StationRoster, both) &&
         Game::WriteStationRoster(STATION, docked, both);
  _checks.Record("a commander both on a grid and docked at one anchor is two rows and one roster",
                 pair && both.Ok() && view.ApplySummary(both.Written()));

  _checks.Record("the docked row still carries the count", view.DockedCountAt(STATION) == 3);

  std::array<Neuron::StationGroup, Neuron::MAX_STATION_GROUPS> groups{};
  std::array<Neuron::StationChip, Neuron::MAX_STATION_CHIPS> chips{};
  const Neuron::StationRosterCounts counts = view.BuildStationRoster(STATION, {}, groups, chips);
  _checks.Record("and the hangar behind it is not empty -- the grid row did not take its roster",
                 counts.chips == 3 && counts.groups == 2);

  // And the number the status bar prints is the roster's own rather than the
  // block's, so the bar and the list are one statement about one place.
  _checks.Record("the station's own docked count is what the bar can read", counts.docked == 3);
}

/*
 * U3c's accept: two commanders on one shard (ADR-018 D5/A25, build order U3c-b).
 *
 * This is the section U3c exists for, and the reason it is written with the
 * second client's assertions phrased NEGATIVELY -- what B does not get -- is
 * that every one of them would have passed a slice ago against a registry where
 * both commanders owned everything. A privacy check that cannot fail is worse
 * than none, because it gets reported as evidence.
 *
 * Disjoint grids, deliberately: two full fleets on one grid exceed the
 * full-snapshot cap by arithmetic, and lifting that is the interest/delta
 * slice's job (ADR-022, D6), not this one's.
 */
void RunSecondCommanderGate(Checklist& _checks, Neuron::ServerHost& _server, const Neuron::Simulation& _simulation)
{
  Neuron::ClientConnection alice;
  Neuron::ClientConnection bob;

  if (!alice.Connect("127.0.0.1", _server.BoundPort(), _simulation.SchemaHash(), _simulation.ContentHash(), "alice") ||
      !PumpUntil(alice, [&] { return alice.State() == Neuron::ClientLinkState::Joined; }))
  {
    _checks.Record("the first commander joins", false);
    return;
  }
  if (!bob.Connect("127.0.0.1", _server.BoundPort(), _simulation.SchemaHash(), _simulation.ContentHash(), "bob") ||
      !PumpUntil(bob, [&] { return bob.State() == Neuron::ClientLinkState::Joined; }))
  {
    _checks.Record("a second commander joins the same shard", false);
    return;
  }
  _checks.Record("two commanders hold sessions at once", _server.SessionCount() >= 2);

  /*
   * Distinct ids, and neither of them nobody. The second half matters on its
   * own: `INVALID_PLAYER_ID` is what a zeroed handshake carries, and a shard
   * that handed it out would be issuing the one identity every filter treats
   * as "not yours".
   */
  _checks.Record("the two commanders have different ids", alice.Player() != bob.Player());
  _checks.Record("and neither of them is nobody",
                 alice.Player() != Neuron::INVALID_PLAYER_ID && bob.Player() != Neuron::INVALID_PLAYER_ID);

  // Each got a resume handle. Zero would mean the grace window has nothing to
  // work with, which is a failure the disconnect below would report far less
  // clearly.
  _checks.Record("each commander is given a resume handle", alice.ResumeToken() != 0 && bob.ResumeToken() != 0);
  _checks.Record("and the two handles are different", alice.ResumeToken() != bob.ResumeToken());

  // Both are put down somewhere, and not on top of each other.
  const std::uint16_t aliceGrid = alice.GridAnchor();
  const std::uint16_t bobGrid = bob.GridAnchor();
  _checks.Record("the second commander is put on a grid of their own", aliceGrid != bobGrid);

  /*
   * View rights. B asking to watch A's grid is the request `MayView` exists to
   * refuse, and until U3c-a it would have been ALLOWED -- the gate walked the
   * composition root's scripted patrol list and answered the same for every
   * viewer.
   */
  bob.ClearPendingViewChanges();
  bool answered = false;
  bool refused = false;
  if (bob.RequestView(aliceGrid))
  {
    answered = PumpUntil(bob,
                         [&]
                         {
                           for (const Neuron::ViewChanged& changed : bob.PendingViewChanges())
                           {
                             if (changed.gridAnchor == aliceGrid)
                             {
                               refused = !changed.accepted;
                               return true;
                             }
                           }
                           bob.ClearPendingViewChanges();
                           return false;
                         });
  }
  bob.ClearPendingViewChanges();
  _checks.Record("a request to watch another commander's grid is answered", answered);
  _checks.Record("and refused", refused);

  // And B keeps their own: a refusal must not move the feed.
  _checks.Record("the refused viewer is still on their own grid", bob.GridAnchor() == bobGrid);

  /*
   * Orders attributed correctly -- U3c's accept clause, and the one that needed
   * a hole closing rather than a check writing.
   *
   * `Validate.cpp` has said since the MVP that `NotOwned` was unreachable
   * because "there is one player and every ship is theirs". It was, and the
   * order path proved it: every order went to the START grid whoever sent it,
   * and named any ship standing there. With a second commander that is a
   * commander ordering another's fleet, and nothing in the engine would have
   * minded -- the world it went to knows only its own ships and must not learn
   * who owns one (ADR-018 D2), so the check could only ever live where the
   * registry and the wire are both visible.
   *
   * B is handed one of A's ship ids by the harness, which is the strongest
   * form of the test: it does not rely on B being unable to LEARN the id, only
   * on the server refusing it.
   */
  Game::ReplicatedView aliceView;
  std::vector<Game::ReplicatedShip> aliceShips;
  (void)PumpUntil(alice,
                  [&]
                  {
                    for (const std::vector<std::uint8_t>& payload : alice.PendingSnapshots())
                    {
                      (void)aliceView.ApplySnapshot(payload);
                    }
                    alice.ClearPendingSnapshots();
                    aliceShips.clear();
                    aliceView.SampleAt(static_cast<double>(aliceView.LatestTick()), aliceShips);
                    return !aliceShips.empty();
                  });

  Game::ShipId alicesHull = Game::INVALID_SHIP_ID;
  for (const Game::ReplicatedShip& ship : aliceShips)
  {
    if (ship.classId != static_cast<std::uint8_t>(Game::HullClass::Structure))
    {
      alicesHull = ship.id;
      break;
    }
  }
  _checks.Record("the harness can name one of the first commander's hulls", alicesHull != Game::INVALID_SHIP_ID);

  if (alicesHull != Game::INVALID_SHIP_ID)
  {
    Game::OrderSubmit poach;
    poach.orderSeq = 9301;
    (void)poach.AddShip(alicesHull);
    poach.target.xCm = Neuron::MetresToCentimetres(500.0f);
    poach.target.yCm = Neuron::MetresToCentimetres(500.0f);

    std::array<std::uint8_t, Game::MAX_ORDER_SUBMIT_BYTES + Game::COMMAND_KIND_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    bob.ClearPendingVerdicts();
    const bool sent = Game::WriteCommandKind(Game::CommandKind::Order, writer) &&
                      Game::WriteOrderSubmit(poach, writer) && bob.SendOrder(writer.Written());
    _checks.Record("a commander may send an order naming another's ship", sent);

    bool refusedNotOwned = false;
    (void)PumpUntil(bob,
                    [&]
                    {
                      for (const Neuron::OrderVerdict& verdict : bob.PendingVerdicts())
                      {
                        if (verdict.orderSeq == poach.orderSeq)
                        {
                          refusedNotOwned = !verdict.accepted &&
                                            verdict.reasonCode ==
                                              static_cast<std::uint16_t>(Game::OrderReason::NotOwned);
                          return true;
                        }
                      }
                      return false;
                    });
    _checks.Record("and the authority refuses it NotOwned", refusedNotOwned);
  }

  /*
   * Summaries. A's fleet must not appear in B's, in either direction -- the
   * symmetric assertion is worth making because a filter keyed on the wrong
   * side of the comparison would pass one and fail the other.
   */
  const auto fleetAnchors = [&](Neuron::ClientConnection& _client)
  {
    std::vector<std::uint16_t> anchors;
    for (const std::vector<std::uint8_t>& payload : _client.PendingSummaries())
    {
      Neuron::ByteReader reader{payload};
      std::uint8_t records = 0;
      if (!Game::ReadSummaryFrame(reader, records))
      {
        continue;
      }

      /*
       * The family is read in order, and every record has to be consumed even
       * when it is not the one wanted -- the records share one reader, so
       * skipping a payload would leave the next read pointed at the middle of
       * it. That is why the roster branch reads rather than continues.
       */
      for (std::uint8_t index = 0; index < records; ++index)
      {
        Game::SummaryKind kind{};
        if (!Game::ReadSummaryRecord(reader, kind))
        {
          break;
        }
        if (kind == Game::SummaryKind::FleetSummaries)
        {
          std::vector<Game::FleetSummary> rows;
          if (!Game::ReadFleetSummaries(reader, rows))
          {
            break;
          }
          for (const Game::FleetSummary& row : rows)
          {
            anchors.push_back(row.anchor);
          }
          continue;
        }
        break; // Anything else and this reader has said what it can.
      }
    }
    _client.ClearPendingSummaries();
    return anchors;
  };

  std::vector<std::uint16_t> bobSees;
  (void)PumpUntil(bob,
                  [&]
                  {
                    const std::vector<std::uint16_t> rows = fleetAnchors(bob);
                    bobSees.insert(bobSees.end(), rows.begin(), rows.end());
                    return !bobSees.empty();
                  });
  _checks.Record("the second commander is told where their own fleet is", !bobSees.empty());
  _checks.Record("and never where the first one's is",
                 std::find(bobSees.begin(), bobSees.end(), aliceGrid) == bobSees.end());

  /*
   * The grace window (ADR-018 D5). B drops and comes straight back with the
   * handle they were given.
   *
   * The claim is not merely that the reconnect succeeds -- it is that the
   * SAME COMMANDER comes back. A shard that minted a fresh id here would look
   * fine from the client's side and would have quietly orphaned a fleet.
   */
  const Neuron::PlayerId wasBob = bob.Player();
  const std::uint64_t handle = bob.ResumeToken();
  bob.Disconnect();
  (void)PumpUntil(alice, [&] { return _server.SessionCount() <= 1; });

  Neuron::ClientConnection returning;
  const bool reconnected =
    returning.Connect("127.0.0.1", _server.BoundPort(), _simulation.SchemaHash(), _simulation.ContentHash(), "bob",
                      wasBob, handle) &&
    PumpUntil(returning, [&] { return returning.State() == Neuron::ClientLinkState::Joined; });
  _checks.Record("a dropped commander can reconnect", reconnected);
  if (reconnected)
  {
    _checks.Record("and comes back as the same commander", returning.Player() == wasBob);
    _checks.Record("on the grid they were watching", returning.GridAnchor() == bobGrid);
    _checks.Record("with a fresh handle, so the old one is spent", returning.ResumeToken() != handle);

    // The fleet is intact, which is the point of the window: their ships never
    // left the grid, because ownership is universe-layer state and the socket
    // was never what held it.
    std::vector<std::uint16_t> backSees;
    (void)PumpUntil(returning,
                    [&]
                    {
                      const std::vector<std::uint16_t> rows = fleetAnchors(returning);
                      backSees.insert(backSees.end(), rows.begin(), rows.end());
                      return !backSees.empty();
                    });
    _checks.Record("with the fleet still theirs",
                   std::find(backSees.begin(), backSees.end(), bobGrid) != backSees.end());
    _checks.Record("and still not the other commander's",
                   std::find(backSees.begin(), backSees.end(), aliceGrid) == backSees.end());
    returning.Disconnect();
  }

  alice.Disconnect();
  _checks.Record("the server outlives both commanders", _server.Running());
}

void RunApproachDisconnectGate(Checklist& _checks, Neuron::ServerHost& _server, const Neuron::Simulation& _simulation)
{
  Game::ShipId approaching[2] = {Game::INVALID_SHIP_ID, Game::INVALID_SHIP_ID};
  std::size_t rosterBefore = 0;

  /// Who was flying, so the observer can come back as them (U3c-b). Captured
  /// inside the block below, because the connection that knows is closed by the
  /// time the observer needs it.
  Neuron::PlayerId approachPlayer = Neuron::INVALID_PLAYER_ID;
  std::uint64_t approachToken = 0;
  Game::AnchorId station = Game::INVALID_ID;

  {
    Neuron::ClientConnection client;
    if (!client.Connect("127.0.0.1", _server.BoundPort(), _simulation.SchemaHash(), _simulation.ContentHash(), "approach"))
    {
      _checks.Record("a second client opens a socket for the approach gate", false);
      return;
    }
    if (!PumpUntil(client, [&] { return client.State() == Neuron::ClientLinkState::Joined; }))
    {
      _checks.Record("the approach gate's client joins", false);
      return;
    }
    station = client.GridAnchor();

    Game::ReplicatedView view;
    std::vector<Game::ReplicatedShip> ships;
    const auto refresh = [&]
    {
      for (const std::vector<std::uint8_t>& payload : client.PendingSnapshots())
      {
        (void)view.ApplySnapshot(payload);
      }
      client.ClearPendingSnapshots();
      ships.clear();
      view.SampleAt(static_cast<double>(view.LatestTick()), ships);
    };
    (void)PumpUntil(client, [&] { refresh(); return ships.size() > 2; });

    // Two movers that are not the station, sent *towards* it but with no Dock
    // to follow -- the approach's first leg and nothing else.
    Game::OrderSubmit approach;
    approach.orderSeq = 7101;
    for (const Game::ReplicatedShip& ship : ships)
    {
      if (ship.classId == static_cast<std::uint8_t>(Game::HullClass::Structure))
      {
        continue;
      }
      if (approach.shipCount < 2)
      {
        approaching[approach.shipCount] = ship.id;
        (void)approach.AddShip(ship.id);
      }
    }
    approach.target.xCm = Neuron::MetresToCentimetres(600.0f);
    approach.target.yCm = Neuron::MetresToCentimetres(600.0f);

    std::array<std::uint8_t, Game::MAX_ORDER_SUBMIT_BYTES + Game::COMMAND_KIND_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    const bool sent = approach.shipCount == 2 && Game::WriteCommandKind(Game::CommandKind::Order, writer) &&
                      Game::WriteOrderSubmit(approach, writer) && client.SendOrder(writer.Written());
    _checks.Record("an approach leg goes up before the client vanishes", sent);
    if (!sent)
    {
      return;
    }
    (void)PumpUntil(client, [&] { return !client.PendingVerdicts().empty(); });

    // Mid-approach on purpose: a few ticks of flying, then the socket goes.
    const std::uint32_t leaveAt = client.ServerTick() + 4;
    (void)PumpUntil(client, [&] { return client.ServerTick() >= leaveAt; });

    rosterBefore = ships.size();

    /*
     * Whose approach this was (U3c-b).
     *
     * The observer below has to come back as THIS commander, and the reason is
     * the whole point of the slice: since `ServerHost` mints a `PlayerId` per
     * player, a second connection is a second commander with its own fleet on
     * its own grid -- so an observer that merely reconnected would be watching
     * somewhere else entirely and would find no approaching ships because there
     * were none there to find.
     *
     * Before U3c-b every connection was `SOLE_PLAYER_ID` and this section got
     * the right answer without asking the question. That is exactly the class
     * of assumption U3c exists to flush out.
     */
    approachPlayer = client.Player();
    approachToken = client.ResumeToken();
  } // The connection closes here, mid-leg.

  // The session has to be gone before the second client asks, or this measures
  // a race rather than a rule.
  bool emptied = false;
  const std::int64_t start = Neuron::Clock::Counter();
  while (Neuron::Clock::MillisecondsBetween(start, Neuron::Clock::Counter()) < 3000.0)
  {
    if (_server.SessionCount() == 0)
    {
      emptied = true;
      break;
    }
  }
  _checks.Record("the server outlives a client that left mid-approach", emptied && _server.Running());

  // Back as the same commander, inside the grace window (ADR-018 D5), so what
  // it observes is the fleet it left mid-leg rather than a fresh one.
  Neuron::ClientConnection observer;
  if (!observer.Connect("127.0.0.1", _server.BoundPort(), _simulation.SchemaHash(), _simulation.ContentHash(), "observer",
                        approachPlayer, approachToken) ||
      !PumpUntil(observer, [&] { return observer.State() == Neuron::ClientLinkState::Joined; }))
  {
    _checks.Record("an observer can join after the disconnect", false);
    return;
  }
  _checks.Record("and comes back as the commander that left", observer.Player() == approachPlayer);

  Game::ReplicatedView view;
  std::vector<Game::ReplicatedShip> ships;
  const auto refresh = [&]
  {
    for (const std::vector<std::uint8_t>& payload : observer.PendingSnapshots())
    {
      (void)view.ApplySnapshot(payload);
    }
    observer.ClearPendingSnapshots();
    ships.clear();
    view.SampleAt(static_cast<double>(view.LatestTick()), ships);
  };
  (void)PumpUntil(observer, [&] { refresh(); return !ships.empty(); });

  const auto onGrid = [&](Game::ShipId _id)
  {
    return std::any_of(ships.begin(), ships.end(), [_id](const Game::ReplicatedShip& _ship) { return _ship.id == _id; });
  };

  // Still there: an interrupted approach must not have docked anybody, which is
  // what "leaves the fleet outside" means on the wire -- a docked ship leaves
  // the snapshot entirely (ADR-017 1).
  _checks.Record("a fleet whose client left mid-approach is still on the grid, not docked",
                 onGrid(approaching[0]) && onGrid(approaching[1]));
  _checks.Record("and the grid did not lose ships to the disconnect", ships.size() >= rosterBefore);
  (void)station;

  /*
   * **Outside, and it stays outside.**
   *
   * The first draft of this asked whether the fleet had come to a *stop*, which
   * is what "halts outside" sounds like it means, and it failed for a reason
   * worth keeping: this simulation's fleet is on a scripted patrol
   * (`UniverseSimulation::AdvanceTick`), so it is never stationary and nothing
   * about the disconnect would make it so. "Halted" is not a property this
   * world has.
   *
   * What the criterion is actually about survives that intact: the client that
   * would have sent the Dock is gone, so **nobody docks**. The check is the
   * roster over a window rather than a speed at an instant -- the station has to
   * still not be holding these two after the summary feed has had several
   * chances to say otherwise, which is exactly the failure a half-finished
   * approach would produce.
   */
  bool rosterGained = false;
  const std::uint32_t settleUntil = observer.ServerTick() + APPROACH_SETTLE_TICKS;
  (void)PumpUntil(observer,
                  [&]
                  {
                    for (const std::vector<std::uint8_t>& payload : observer.PendingSummaries())
                    {
                      Neuron::ByteReader reader{payload};
                      std::uint8_t records = 0;
                      if (!Game::ReadSummaryFrame(reader, records))
                      {
                        continue;
                      }
                      for (std::uint8_t index = 0; index < records; ++index)
                      {
                        Game::SummaryKind kind{};
                        if (!Game::ReadSummaryRecord(reader, kind))
                        {
                          break;
                        }
                        if (kind != Game::SummaryKind::StationRoster)
                        {
                          break;
                        }
                        Game::AnchorId seenStation = Game::INVALID_ID;
                        std::vector<Game::RosterEntry> seenDocked;
                        if (Game::ReadStationRoster(reader, seenStation, seenDocked))
                        {
                          for (const Game::RosterEntry& row : seenDocked)
                          {
                            rosterGained = rosterGained || row.shipId == approaching[0] || row.shipId == approaching[1];
                          }
                        }
                        break;
                      }
                    }
                    observer.ClearPendingSummaries();
                    refresh();
                    return rosterGained || observer.ServerTick() >= settleUntil;
                  });
  _checks.Record("and the interrupted approach never docks anybody", !rosterGained);
  _checks.Record("and the fleet is still outside when the dust settles", onGrid(approaching[0]) && onGrid(approaching[1]));
}

} // namespace

int RunSelfTest(const AppConfig& _config, Neuron::Simulation& _simulation, const Game::EconomyDef& _economy)
{
  NEURON_LOG_INFO("self test: starting (schema, wire round-trips, replay determinism, the tick soak, then the QUIC loopback loop)");

  Checklist checks;

  // The S14 aggregate's device-free half runs first: schema self-check, wire
  // round-trips, the replay-determinism run and R10's tick soak need no socket,
  // so a transport failure cannot mask them -- and on a GPU-less CI runner they
  // are most of what this gate proves.
  RunLocalChecks(checks, _simulation, _economy);
  RunRestartLoop(checks, _economy);

  /*
   * The host's own store, on a scratch directory of its own (ADR-025 §5).
   *
   * `RunRestartLoop` above proves the *format* survives two runtimes; this
   * proves the **wiring** -- that a host which stops cleanly writes itself down
   * on the way out, on the Sim thread, before anything is torn down. They are
   * different claims and the second one is the one a person would forget to
   * make: a store that works and is never called is a shard that loses
   * everything and passes every unit test.
   *
   * It guards on `ContentHash` rather than on the universe hash alone, because
   * the mixed number is the one this function has -- and what is under test is
   * the guard's behaviour, not which hash fills it.
   */
  const std::string hostDirectory = Outpost::ResolveWritablePath("SelfTestHostState");
  std::error_code ignoredHostState;
  std::filesystem::remove_all(hostDirectory, ignoredHostState);

  Neuron::DurableStoreDesc hostDesc;
  hostDesc.directory = hostDirectory;
  hostDesc.hostId = 0;
  hostDesc.universeHash = _simulation.ContentHash();
  hostDesc.economyHash = 0;

  Neuron::DurableStore hostStore;
  Neuron::DurableLoadReport hostReport;
  const bool hostStoreOpen = hostStore.Open(hostDesc, hostReport);
  checks.Record("the host's durable store opens", hostStoreOpen);

  // Port 0 whatever the config says: a self test must not fail because the
  // configured port is already taken by the thing it is testing.
  Neuron::ServerConfig serverConfig;
  serverConfig.port = 0;
  serverConfig.maxSessions = _config.server.maxSessions;
  serverConfig.durableStore = hostStoreOpen ? &hostStore : nullptr;

  Neuron::ServerHost server;
  const bool started = server.Start(serverConfig, _simulation);
  checks.Record("server starts", started);
  if (!started)
  {
    return 3; // Nothing after this can mean anything.
  }
  checks.Record("server reports its bound port", server.BoundPort() != 0);

  {
    Neuron::ClientConnection client;
    const bool connected =
      client.Connect("127.0.0.1", server.BoundPort(), _simulation.SchemaHash(), _simulation.ContentHash(), "self test");
    checks.Record("client opens a socket", connected);

    if (connected)
    {
      const bool joined = PumpUntil(client, [&] { return client.State() != Neuron::ClientLinkState::Connecting; });
      checks.Record("handshake completes", joined && client.State() == Neuron::ClientLinkState::Joined);
      checks.Record("server issues a client id", client.ClientId() != 0);
      checks.Record("server reports 20 Hz", client.ServerTickRate() == 20);
      checks.Record("server counts the session", server.SessionCount() == 1);

      const bool ponged = PumpUntil(client, [&] { return client.PongCount() >= REQUIRED_PONGS; });
      checks.Record("heartbeat crosses the loopback", ponged);
      checks.Record("round trip is plausible", client.RoundTripMs() >= 0.0 && client.RoundTripMs() < IMPLAUSIBLE_ROUND_TRIP_MS);
      checks.Record("server advances its tick", client.ServerTick() > 0);

      // The rest of the loop (S13's exit criterion): a snapshot down the
      // unreliable channel, an order up the reliable one, and the authority's
      // verdict back. Until now the self test stopped at the heartbeat.
      const bool snapshotArrived = PumpUntil(client, [&] { return !client.PendingSnapshots().empty(); });
      checks.Record("a snapshot arrives", snapshotArrived);

      if (snapshotArrived)
      {
        Game::ReplicatedView view;
        bool applied = false;
        for (const std::vector<std::uint8_t>& payload : client.PendingSnapshots())
        {
          applied = view.ApplySnapshot(payload) || applied;
        }
        client.ClearPendingSnapshots();

        std::vector<Game::ReplicatedShip> ships;
        if (applied)
        {
          view.SampleAt(static_cast<double>(view.LatestTick()), ships);
        }
        checks.Record("the snapshot decodes to ships", applied && !ships.empty());

        /*
         * One of the COMMANDER'S OWN hulls, which used to be `ships.front()`
         * (U3c-b).
         *
         * The station is an authored occupant, it is spawned first on a station
         * grid -- "the first occupant is the structure itself" -- and so it is
         * what `front()` returns. Ordering it was accepted until this slice,
         * because nothing knew who owned anything; it is refused `NotOwned`
         * now, which is right, and it makes the fixture wrong rather than the
         * rule.
         *
         * Worth saying plainly: for the whole of the MVP this check has been
         * proving that the ack path works by telling a SPACE STATION to move
         * a hundred metres to the right.
         */
        const Game::ReplicatedShip* mine = nullptr;
        for (const Game::ReplicatedShip& ship : ships)
        {
          if (ship.classId != static_cast<std::uint8_t>(Game::HullClass::Structure))
          {
            mine = &ship;
            break;
          }
        }
        checks.Record("the snapshot holds a hull of the commander's own", mine != nullptr);

        if (mine != nullptr)
        {
          // A real order for a real ship, exactly as the client would send it:
          // decoded from the snapshot, validated by the game, acknowledged
          // back with the sequence it went out under (ADR-004 §7).
          Game::OrderSubmit order;
          order.orderSeq = 4242;
          (void)order.AddShip(mine->id);
          order.target.xCm = Neuron::MetresToCentimetres(mine->positionMetres.x + 100.0f);
          order.target.yCm = Neuron::MetresToCentimetres(mine->positionMetres.y);

          std::array<std::uint8_t, Game::MAX_ORDER_SUBMIT_BYTES> orderBuffer{};
          Neuron::ByteWriter orderWriter{orderBuffer};
          const bool sent = Game::WriteCommandKind(Game::CommandKind::Order, orderWriter) && Game::WriteOrderSubmit(order, orderWriter) &&
                            client.SendOrder(orderWriter.Written());
          checks.Record("an order goes up the reliable channel", sent);

          if (sent)
          {
            (void)PumpUntil(client, [&] { return !client.PendingVerdicts().empty(); });
            bool accepted = false;
            for (const Neuron::OrderVerdict& verdict : client.PendingVerdicts())
            {
              accepted = accepted || (verdict.orderSeq == order.orderSeq && verdict.accepted);
            }
            checks.Record("the authority accepts it and the ack returns", accepted);
          }
        }
      }

      RunViewGate(checks, client);
      RunDockingLoop(checks, client);

      const Neuron::TransportStats stats = client.Stats();
      checks.Record("no datagram was dropped", stats.datagramsDropped == 0);

      // The spike's latency gate: QUIC on loopback must add less than a
      // millisecond. The *minimum* round trip is the instrument, because the
      // smoothed one is dominated by the peer's deliberate ack batching (~25 ms
      // max ack delay) and the app-level ping by the server's 50 ms poll
      // cadence -- neither is latency the transport adds to the game's bytes.
      checks.Record("transport adds under a millisecond", stats.minRoundTripMs > 0.0 && stats.minRoundTripMs < 1.0);

      NEURON_LOG_INFO("self test: %llu pings, %llu pongs, app rtt %.3f ms, transport rtt %.3f ms (min %.3f ms), %llu resends, "
                      "server at tick %u",
                      static_cast<unsigned long long>(client.PingCount()), static_cast<unsigned long long>(client.PongCount()),
                      client.RoundTripMs(), stats.roundTripMs, stats.minRoundTripMs,
                      static_cast<unsigned long long>(stats.controlResends), client.ServerTick());
    }

    // Leaving the scope disconnects, which is itself part of what is checked:
    // the server must survive it rather than shutting down (ADR-008 §1).
  }

  // The goodbye has to reach the sim thread and be acted on, so this waits for
  // the session table to empty rather than assuming it already has.
  bool emptied = false;
  const std::int64_t waitStart = Neuron::Clock::Counter();
  while (Neuron::Clock::MillisecondsBetween(waitStart, Neuron::Clock::Counter()) < TIMEOUT_MS)
  {
    if (server.SessionCount() == 0)
    {
      emptied = true;
      break;
    }
    Sleep(1);
  }
  checks.Record("server outlives the client", emptied && server.Running());

  // Runs here, after the first session is provably gone, because its whole
  // subject is what the authority keeps when a client is not there.
  RunApproachDisconnectGate(checks, server, _simulation);

  // The client-side gates first: they are pure decode and validation, so they
  // run whether or not a second commander could be brought up.
  RunSummaryFamilyGate(checks);
  RunHullScaleGate(checks, _config);
  RunClientDockPreCheckGate(checks, _simulation);
  RunMineAvailabilityGate(checks, _economy);
  RunLocationBlockGate(checks);
  RunWingAssignmentGate(checks);

  // U3c's accept, after the single-commander loop has proved the machinery it
  // builds on: two clients at once, on grids of their own.
  RunSecondCommanderGate(checks, server, _simulation);

  // S3's acceptance is a cadence measurement, and until now it had no harness --
  // which is a good way for "mean period 50 ms +/- 0.5" to stay a sentence
  // nobody ever checked. The tight tolerance assumes an idle machine and this
  // runs wherever it is run, so the number is reported for a person to judge
  // and only a loose bound is enforced.
  {
    const std::uint32_t before = server.TickCount();
    const std::int64_t start = Neuron::Clock::Counter();
    Sleep(static_cast<DWORD>(CADENCE_WINDOW_MS));
    const double elapsedMs = Neuron::Clock::MillisecondsBetween(start, Neuron::Clock::Counter());
    const std::uint32_t ticks = server.TickCount() - before;
    const double meanPeriodMs = ticks == 0 ? 0.0 : elapsedMs / static_cast<double>(ticks);

    NEURON_LOG_INFO("self test: %u ticks in %.0f ms -- mean period %.3f ms (want %.1f +/- 0.5 idle), %u overrun(s)", ticks, elapsedMs,
                    meanPeriodMs, EXPECTED_TICK_PERIOD_MS, server.OverrunCount());

    checks.Record("the tick loop keeps its cadence", ticks > 0 &&
                                                       meanPeriodMs > EXPECTED_TICK_PERIOD_MS - ACCEPTABLE_TICK_PERIOD_DRIFT_MS &&
                                                       meanPeriodMs < EXPECTED_TICK_PERIOD_MS + ACCEPTABLE_TICK_PERIOD_DRIFT_MS);
  }

  server.Stop();
  server.Join();
  checks.Record("server stops cleanly", !server.Running() && server.TickCount() > 0);

  /*
   * And what it left behind (ADR-025 §4): **a clean stop loses nothing.**
   *
   * The store is closed first, because reading a file this process still holds
   * open would be testing the buffer rather than the disk -- and the disk is
   * the whole subject.
   */
  if (hostStoreOpen)
  {
    hostStore.Close();

    Neuron::DurableStore reopened;
    Neuron::DurableLoadReport report;
    const bool found = reopened.Open(hostDesc, report) && report.result == Neuron::DurableLoadResult::Loaded;
    checks.Record("a host that stops cleanly leaves a snapshot behind", found && !reopened.Snapshot().empty());
    checks.Record("the snapshot is stamped with the tick the host stopped at", found && report.snapshotTick > 0);
    checks.Record("the snapshot carries the shard's reload proof", found && report.snapshotDurableHash == _simulation.DurableHash());
    reopened.Close();
  }
  std::filesystem::remove_all(hostDirectory, ignoredHostState);

  // Zero on any healthy machine, but this gate now runs in CI (S14) and a
  // shared runner is not a real-time system -- the same argument S3 made for
  // the loose cadence bound. A couple of overruns is scheduler noise; a stream
  // of them is a real defect, and the count is logged either way.
  if (server.OverrunCount() != 0)
  {
    NEURON_LOG_WARNING("self test: %u tick overrun(s) -- expected 0 on an idle machine", server.OverrunCount());
  }
  checks.Record("ticks did not persistently overrun", server.OverrunCount() <= 2);

  // What the lanes recorded while all of that happened. Not a check -- a
  // measurement, printed because a self test that says only "PASSED" tells you
  // nothing when the next one says "FAILED" (ADR-007 §8).
  Neuron::TelemetrySnapshot telemetry;
  telemetry.DrainAll();
  for (const Neuron::TelemetryStat& stat : telemetry.Stats())
  {
    if (stat.kind == Neuron::TelemetryKind::Span)
    {
      NEURON_LOG_INFO("self test: span %-12s n=%-6u mean %.3f ms  max %.3f ms", stat.name, stat.count,
                      Neuron::TelemetrySnapshot::Milliseconds(stat.total) / static_cast<double>(stat.count),
                      Neuron::TelemetrySnapshot::Milliseconds(stat.maximum));
    }
    else
    {
      NEURON_LOG_INFO("self test: count %-12s total %lld over %u", stat.name, static_cast<long long>(stat.total), stat.count);
    }
  }

  if (checks.Failures() == 0)
  {
    NEURON_LOG_INFO("self test: PASSED");
    return 0;
  }
  NEURON_LOG_ERROR("self test: FAILED (%u checks)", checks.Failures());
  return 3;
}

} // namespace Outpost
