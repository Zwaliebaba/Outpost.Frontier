#include "pch.h"

#include "SelfTest.h"

#include "TickSoak.h"

#include "ClientConnection.h"

#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"

#include "FleetSummary.h"
#include "OrderMessages.h"
#include "ReplicatedView.h"
#include "SchemaHash.h"
#include "Snapshot.h"
#include "Station.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "World.h"
#include "WorldHash.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "EntityRecord.h"
#include "Log.h"
#include "Telemetry.h"

#include <array>
#include <cstdint>
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
void RunLocalChecks(Checklist& _checks, Neuron::Simulation& _simulation)
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

    std::array<std::uint8_t, Game::MAX_ORDER_SUBMIT_BYTES> buffer{};
    Neuron::ByteWriter writer{buffer};
    bool ok = Game::WriteCommandKind(Game::CommandKind::Order, writer) && Game::WriteOrderSubmit(order, writer);

    Game::OrderSubmit read;
    if (ok)
    {
      Neuron::ByteReader reader{writer.Written()};
      ok = Game::ReadOrderSubmit(reader, read);
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
  bool rosterSeen = false;

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
            rosterSeen = true;
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
    return rosterSeen;
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
}

} // namespace

int RunSelfTest(const AppConfig& _config, Neuron::Simulation& _simulation)
{
  NEURON_LOG_INFO("self test: starting (schema, wire round-trips, replay determinism, the tick soak, then the QUIC loopback loop)");

  Checklist checks;

  // The S14 aggregate's device-free half runs first: schema self-check, wire
  // round-trips, the replay-determinism run and R10's tick soak need no socket,
  // so a transport failure cannot mask them -- and on a GPU-less CI runner they
  // are most of what this gate proves.
  RunLocalChecks(checks, _simulation);

  // Port 0 whatever the config says: a self test must not fail because the
  // configured port is already taken by the thing it is testing.
  Neuron::ServerConfig serverConfig;
  serverConfig.port = 0;
  serverConfig.maxSessions = _config.server.maxSessions;

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

        if (!ships.empty())
        {
          // A real order for a real ship, exactly as the client would send it:
          // decoded from the snapshot, validated by the game, acknowledged
          // back with the sequence it went out under (ADR-004 §7).
          Game::OrderSubmit order;
          order.orderSeq = 4242;
          (void)order.AddShip(ships.front().id);
          order.target.xCm = Neuron::MetresToCentimetres(ships.front().positionMetres.x + 100.0f);
          order.target.yCm = Neuron::MetresToCentimetres(ships.front().positionMetres.y);

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
