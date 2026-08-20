#include "pch.h"
#include "CppUnitTest.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"
#include "QuicTransport.h"
#include "Wire.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The server is driven here through NeuronCore alone -- a raw transport and the
 * semantics-free wire messages. No GameLogic and no NeuronClient appear, which
 * is the point: if these tests can join a session, the engine really is
 * game-agnostic (ADR-014).
 */

namespace NeuronServerTests
{
namespace
{

/// A simulation that only counts, so a test can assert the loop ran without
/// caring what a world is.
class CountingSimulation final : public Simulation
{
public:
  void AdvanceTick(std::uint32_t _tick) override
  {
    m_lastTick = _tick;
    ++m_ticks;
  }

  /*
   * Writes a tiny recognisable payload so a test can prove the datagram
   * carried the simulation's bytes and not the engine's idea of them. The
   * last accepted order rides along, which is what "state in the next
   * snapshot" means for a simulation with no world.
   *
   * The viewer is written into the payload rather than ignored: it is what
   * lets a test prove the *sender* knew which client it was serialising for
   * (ADR-018 A13), which is a claim no amount of identical bytes can make.
   *
   * `RefuseSnapshots` makes every write fail the way a grid past the datagram
   * cap does -- the loud refusal ADR-022 §6 keeps until the delta slice.
   */
  [[nodiscard]] bool WriteSnapshot(PlayerId _viewer, std::uint16_t _grid, std::uint32_t _tick, ByteWriter& _writer) override
  {
    m_lastViewer.store(_viewer, std::memory_order_relaxed);
    m_lastGrid.store(_grid, std::memory_order_relaxed);
    if (m_refuseSnapshots.load(std::memory_order_relaxed))
    {
      return false;
    }
    _writer.WriteUInt32(_tick);
    _writer.WriteUInt32(SNAPSHOT_MARKER);
    _writer.WriteUInt32(m_lastAcceptedSeq.load(std::memory_order_relaxed));
    _writer.WriteUInt32(_viewer);
    _writer.WriteUInt32(_grid);
    ++m_snapshotsWritten;
    return _writer.Ok();
  }

  /*
   * One grid is viewable and everything else is not, which is the smallest
   * shape that can tell an enforced gate from an ignored one. The refusal
   * carries a number of this test's own choosing, exactly as the order
   * refusals do -- the engine passes it through without knowing what it says.
   */
  static constexpr std::uint16_t VIEWABLE_GRID = 8317;
  static constexpr std::uint16_t FORBIDDEN_GRID = 999;
  static constexpr std::uint16_t NO_PRESENCE_REASON = 4343;

  [[nodiscard]] std::uint16_t MayView(PlayerId, std::uint16_t _grid) override
  {
    ++m_viewChecks;
    return _grid == VIEWABLE_GRID ? std::uint16_t{0} : NO_PRESENCE_REASON;
  }

  [[nodiscard]] std::uint16_t LastGrid() const noexcept { return m_lastGrid.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t ViewChecks() const noexcept { return m_viewChecks.load(std::memory_order_relaxed); }

  /*
   * A summary frame carrying nothing but the viewer it was written for.
   *
   * The engine never learns what is inside one (ADR-014 §5) -- a roster and a
   * fleet summary are the game's, under the game's hash -- so a test at this
   * level can put anything in it, and the useful thing to put in it is the
   * question this suite can actually answer: *which client was this written
   * for, and which client received it.*
   */
  [[nodiscard]] bool WriteSummaries(PlayerId _viewer, std::uint32_t, ByteWriter& _writer) override
  {
    ++m_summariesWritten;
    _writer.WriteUInt32(SUMMARY_MARKER);
    _writer.WriteUInt32(_viewer);
    return _writer.Ok();
  }

  static constexpr std::uint32_t SUMMARY_MARKER = 0xd0cca5e5u;
  [[nodiscard]] std::uint32_t SummariesWritten() const noexcept { return m_summariesWritten.load(std::memory_order_relaxed); }

  /// Makes the simulation behave like a grid that outgrew one datagram.
  void RefuseSnapshots(bool _refuse) noexcept { m_refuseSnapshots.store(_refuse, std::memory_order_relaxed); }
  [[nodiscard]] PlayerId LastViewer() const noexcept { return m_lastViewer.load(std::memory_order_relaxed); }

  static constexpr std::uint32_t SNAPSHOT_MARKER = 0xfeedbeefu;
  [[nodiscard]] std::uint32_t SnapshotsWritten() const noexcept { return m_snapshotsWritten.load(std::memory_order_relaxed); }

  /*
   * An order format this test invented, which is exactly the point.
   *
   * The engine hands over bytes it never read (ADR-004 ruling 4), so a
   * simulation is free to mean anything by them -- and a simulation that means
   * something no game does is the strongest way to show the engine is not
   * quietly assuming a layout. A payload is a marker and a sequence; anything
   * else is refused with a reason code of this test's own choosing, and the
   * engine passes that number through without knowing what it says.
   */
  static constexpr std::uint32_t ORDER_MARKER = 0xc0defaceu;
  static constexpr std::uint16_t REFUSED_REASON = 4242;

  [[nodiscard]] OrderVerdict ApplyOrderBytes(std::uint32_t _clientId, std::span<const std::uint8_t> _payload) override
  {
    ++m_ordersSeen;
    m_lastOrderClientId = _clientId;

    ByteReader reader{_payload};
    const std::uint32_t marker = reader.ReadUInt32();
    const std::uint32_t sequence = reader.ReadUInt32();

    OrderVerdict verdict;
    verdict.orderSeq = reader.Ok() ? sequence : 0;
    if (!reader.Ok() || marker != ORDER_MARKER)
    {
      verdict.reasonCode = REFUSED_REASON;
      return verdict;
    }

    verdict.accepted = true;
    verdict.serverOrderId = ++m_nextOrderId;
    m_lastAcceptedSeq.store(sequence, std::memory_order_relaxed);
    return verdict;
  }

  [[nodiscard]] std::uint32_t OrdersSeen() const noexcept { return m_ordersSeen.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t LastOrderClientId() const noexcept { return m_lastOrderClientId.load(std::memory_order_relaxed); }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0xfeedfacecafebeefull; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0x0123456789abcdefull; }

  /// A world with a far, negative anchor: the plane is signed and full-width
  /// (ADR-009 §1), so a narrowed field folds here instead of in a session.
  /// The display strings ride along so the test proves they arrive verbatim,
  /// and `gridAnchor` — which grid, as opposed to where it is — rides with them.
  [[nodiscard]] WorldMeta World() const override
  {
    return WorldMeta{42, VIEWABLE_GRID, -4200000000ll, 1750000000ll, "Testfall-9", "Proving Grounds 0.0", "SEC -1.0"};
  }

  [[nodiscard]] std::uint32_t Ticks() const noexcept { return m_ticks.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t LastTick() const noexcept { return m_lastTick.load(std::memory_order_relaxed); }

private:
  std::atomic<std::uint32_t> m_ticks{0};
  std::atomic<std::uint32_t> m_snapshotsWritten{0};
  std::atomic<std::uint32_t> m_lastTick{0};
  std::atomic<std::uint32_t> m_ordersSeen{0};
  std::atomic<std::uint32_t> m_lastOrderClientId{0};
  std::atomic<std::uint32_t> m_lastAcceptedSeq{0};
  std::atomic<std::uint32_t> m_nextOrderId{0};
  std::atomic<std::uint32_t> m_summariesWritten{0};
  std::atomic<PlayerId> m_lastViewer{INVALID_PLAYER_ID};
  std::atomic<std::uint16_t> m_lastGrid{0};
  std::atomic<std::uint32_t> m_viewChecks{0};
  std::atomic<bool> m_refuseSnapshots{false};
};

template <typename Predicate>
bool WaitUntil(QuicTransport& _transport, Predicate _predicate, double _timeoutMs = 5000.0)
{
  const std::int64_t start = Clock::Counter();
  while (Clock::MillisecondsBetween(start, Clock::Counter()) < _timeoutMs)
  {
    _transport.Poll();
    if (_predicate())
    {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

} // namespace

TEST_CLASS(ServerHostLifecycleTests)
{
public:
  TEST_METHOD(StartsListeningBeforeItReturns)
  {
    // A caller must be able to connect the moment Start returns (ADR-008 §1).
    NullSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;

    Assert::IsTrue(host.Start(config, simulation));
    Assert::IsTrue(host.Running());
    Assert::IsTrue(host.BoundPort() != 0, L"an ephemeral port was not reported");

    host.Stop();
    host.Join();
  }

  TEST_METHOD(SurvivesRepeatedStartStopCycles)
  {
    // A hundred cycles, per S3's acceptance: enough that a leaked socket, a
    // leaked thread or a handle left open shows up as a failure to bind rather
    // than as something noticed weeks later.
    NullSimulation simulation;
    for (int i = 0; i < 100; ++i)
    {
      ServerHost host;
      ServerConfig config;
      config.port = 0;
      Assert::IsTrue(host.Start(config, simulation));
      host.Stop();
      host.Join();
    }
  }

  TEST_METHOD(StopIsIdempotentAndJoinWithoutStartIsSafe)
  {
    ServerHost host;
    host.Stop();
    host.Join(); // Never started: must not fault or block.

    NullSimulation simulation;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));
    host.Stop();
    host.Stop();
    host.Join();
  }

  TEST_METHOD(AdvancesTheSimulationAtRoughlyTwentyHertz)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Every counter is read *after* the thread has stopped. Sampling one while
    // the loop is still running and comparing it to another read after the join
    // compares two different moments: CI caught exactly that, 10 against 11.
    host.Stop();
    host.Join();

    const std::uint32_t ticks = simulation.Ticks();

    // 500 ms at 20 Hz is ten ticks, plus the one or two the loop finishes while
    // it notices the stop. The bounds are loose because a shared CI runner is
    // not a real-time system; what is checked is that the loop ticks at all,
    // does not sprint, and reports the same count the simulation actually saw.
    Assert::IsTrue(ticks >= 5, L"the tick loop ran far too slowly");
    Assert::IsTrue(ticks <= 40, L"the tick loop ran far too fast");
    Assert::AreEqual(ticks, host.TickCount());
    Assert::AreEqual(simulation.LastTick(), host.TickCount());
  }
};

TEST_CLASS(ServerHandshakeTests)
{
public:
  TEST_METHOD(AcceptsAMatchingClientAndAnswersPings)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    Welcome welcome;
    bool joined = false;
    const bool answered = WaitUntil(client,
                                    [&]
                                    {
                                      TransportEvent event;
                                      while (client.NextEvent(event))
                                      {
                                        if (event.type != TransportEvent::Type::Message)
                                        {
                                          continue;
                                        }
                                        ByteReader reader{event.payload};
                                        if (ReadWireType(reader) == WireType::Welcome && Read(reader, welcome))
                                        {
                                          joined = true;
                                        }
                                      }
                                      return joined;
                                    });

    Assert::IsTrue(answered, L"the server never sent Welcome");
    Assert::IsTrue(welcome.clientId != 0);
    Assert::AreEqual<std::uint16_t>(20, welcome.tickRate);
    Assert::AreEqual(simulation.SchemaHash(), welcome.schemaHash);
    Assert::AreEqual(simulation.ContentHash(), welcome.contentHash);
    Assert::AreEqual<std::uint32_t>(1, host.SessionCount());

    // The world reaches the client from the simulation, untouched by the engine
    // in between (ADR-009 §8). Without it a client in another process has no
    // frame to place a replicated position in.
    const WorldMeta world = simulation.World();
    Assert::AreEqual<std::uint16_t>(world.worldId, welcome.worldId);
    Assert::AreEqual<std::uint16_t>(world.gridAnchor, welcome.gridAnchor,
                                    L"which grid, as opposed to where it is -- the number a Dock names");
    Assert::AreEqual(world.anchorX, welcome.anchorX);
    Assert::AreEqual(world.anchorY, welcome.anchorY);
    Assert::AreEqual(world.worldName, welcome.worldName);
    Assert::AreEqual(world.worldDetail, welcome.worldDetail);
    Assert::AreEqual(world.worldBadge, welcome.worldBadge);

    // The heartbeat: ping out on the unreliable channel, pong back with the
    // timestamp untouched. This is what milestone M0 is.
    const std::uint64_t stamp = 0x00c0ffee0000ull;
    ByteWriter pingWriter{buffer};
    WriteWireType(pingWriter, WireType::Ping);
    Write(pingWriter, Ping{stamp});
    Assert::IsTrue(client.Send(link, TransportChannel::State, pingWriter.Written()));

    Pong pong;
    bool ponged = false;
    const bool returned = WaitUntil(client,
                                    [&]
                                    {
                                      TransportEvent event;
                                      while (client.NextEvent(event))
                                      {
                                        if (event.type != TransportEvent::Type::Message)
                                        {
                                          continue;
                                        }
                                        ByteReader reader{event.payload};
                                        if (ReadWireType(reader) == WireType::Pong && Read(reader, pong))
                                        {
                                          ponged = true;
                                        }
                                      }
                                      return ponged;
                                    });

    Assert::IsTrue(returned, L"the heartbeat never came back");
    Assert::AreEqual(stamp, pong.clientSendMicroseconds);
    Assert::IsTrue(pong.serverTick > 0, L"the server answered without having ticked");

    host.Stop();
    host.Join();
  }

  /*
   * The loop closes: a joined client is sent the simulation's own bytes, on the
   * unreliable channel, without having asked (ADR-004 §6).
   *
   * This is the only place a snapshot is watched crossing a real socket. The
   * GameLogic suite proves the encoding round-trips and the client suite proves
   * the clock behaves; the server's fan-out sits between them and nothing else
   * touches it. The payload is checked for the simulation's marker rather than
   * merely for length, because the failure worth catching is the engine sending
   * something of its own devising -- a snapshot it framed but did not get from
   * the game would still be the right size.
   */
  TEST_METHOD(SendsTheSimulationsOwnBytesToAJoinedClient)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    // Nothing is sent to a room of nobody, so the join has to land first.
    bool joined = false;
    PlayerId welcomedPlayer = INVALID_PLAYER_ID;
    std::uint32_t snapshotTick = 0;
    std::uint32_t marker = 0;
    std::uint32_t snapshotViewer = INVALID_PLAYER_ID;
    std::uint32_t snapshotsSeen = 0;

    const bool sawSnapshots = WaitUntil(client,
                                        [&]
                                        {
                                          TransportEvent event;
                                          while (client.NextEvent(event))
                                          {
                                            if (event.type != TransportEvent::Type::Message)
                                            {
                                              continue;
                                            }
                                            ByteReader reader{event.payload};
                                            const WireType type = ReadWireType(reader);
                                            if (type == WireType::Welcome)
                                            {
                                              Welcome welcome;
                                              if (Read(reader, welcome))
                                              {
                                                welcomedPlayer = welcome.playerId;
                                              }
                                              joined = true;
                                            }
                                            else if (type == WireType::Snapshot)
                                            {
                                              // Recorded only once the whole
                                              // payload has been read, so a
                                              // truncated datagram cannot
                                              // overwrite a good one.
                                              const std::uint32_t tick = reader.ReadUInt32();
                                              const std::uint32_t payloadMarker = reader.ReadUInt32();
                                              (void)reader.ReadUInt32(); // the last accepted order
                                              const std::uint32_t viewer = reader.ReadUInt32();
                                              if (reader.Ok())
                                              {
                                                snapshotTick = tick;
                                                marker = payloadMarker;
                                                snapshotViewer = viewer;
                                                ++snapshotsSeen;
                                              }
                                            }
                                          }
                                          // Two, so this cannot pass on a single
                                          // snapshot that happened to be sent.
                                          return joined && snapshotsSeen >= 2;
                                        });

    Assert::IsTrue(sawSnapshots, L"the server never sent a snapshot to a joined client");
    Assert::AreEqual(CountingSimulation::SNAPSHOT_MARKER, marker, L"the payload was not the simulation's");
    Assert::IsTrue(snapshotTick > 0, L"a snapshot was sent for a tick the world never ran");
    Assert::IsTrue(snapshotTick <= host.TickCount(), L"a snapshot ran ahead of the tick counter");
    Assert::IsTrue(simulation.SnapshotsWritten() >= snapshotsSeen, L"more snapshots arrived than were written");
    Assert::AreEqual<std::uint32_t>(0, host.SnapshotFailureCount());

    /*
     * The per-client claim, and the only assertion here that a broadcast sender
     * could not also satisfy (ADR-018 A13, ADR-022 §1).
     *
     * The simulation wrote the viewer it was asked to serialise for into the
     * payload, so this compares the player the handshake named with the player
     * the snapshot was made for. With identical bytes going to one client there
     * is no other way to tell a per-client sender from a broadcast one -- which
     * is exactly why the seam had to grow the viewer before the day two clients
     * are owed different worlds, not on it.
     */
    Assert::IsTrue(welcomedPlayer != INVALID_PLAYER_ID, L"the Welcome named no player");
    Assert::AreEqual(welcomedPlayer, snapshotViewer, L"the snapshot was serialised for a different viewer than joined");
    Assert::AreEqual(welcomedPlayer, simulation.LastViewer(), L"the simulation was asked for no particular viewer");

    host.Stop();
    host.Join();
  }

  /*
   * The summary family reaches its viewer, and reaches it at its own cadence
   * (ADR-016 §6, ADR-017 §1, ADR-018 A13).
   *
   * ADR-017 §1's privacy rule -- other commanders cannot see what is docked at
   * a station -- is a *wire* fact, and the wire is the only place it can be
   * checked. It is checked here rather than left to U3c's two clients because
   * on the broadcast sender this replaced, the rule was satisfied by accident:
   * with one client, "everyone gets it" and "its owner gets it" are the same
   * observation, and the day they stop being the same is the day a leak ships.
   * What this test can prove today is the half that does not need a second
   * client -- that the frame was *written for* the player who joined, and
   * arrived on that player's connection -- and that is exactly the half that
   * had no mechanism before A13.
   *
   * The cadence is the other claim. A summary is not a snapshot: 20 Hz for the
   * grid you are watching, about 1 Hz for everywhere else, which is what makes
   * showing a fleet you are *not* watching affordable at all.
   */
  TEST_METHOD(ASummaryFrameIsWrittenForItsViewerAndArrivesAtItsOwnCadence)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    PlayerId welcomedPlayer = INVALID_PLAYER_ID;
    std::uint32_t snapshotsSeen = 0;
    std::uint32_t summariesSeen = 0;
    std::uint32_t summaryViewer = INVALID_PLAYER_ID;
    std::uint32_t summaryMarker = 0;

    const bool sawSummaries = WaitUntil(client,
                                        [&]
                                        {
                                          TransportEvent event;
                                          while (client.NextEvent(event))
                                          {
                                            if (event.type != TransportEvent::Type::Message)
                                            {
                                              continue;
                                            }
                                            ByteReader reader{event.payload};
                                            const WireType type = ReadWireType(reader);
                                            if (type == WireType::Welcome)
                                            {
                                              Welcome welcome;
                                              if (Read(reader, welcome))
                                              {
                                                welcomedPlayer = welcome.playerId;
                                              }
                                              joined = true;
                                            }
                                            else if (type == WireType::Snapshot)
                                            {
                                              ++snapshotsSeen;
                                            }
                                            else if (type == WireType::Summary)
                                            {
                                              const std::uint32_t marker = reader.ReadUInt32();
                                              const std::uint32_t viewer = reader.ReadUInt32();
                                              if (reader.Ok())
                                              {
                                                summaryMarker = marker;
                                                summaryViewer = viewer;
                                                ++summariesSeen;
                                              }
                                            }
                                          }
                                          // Two, so a single frame that happened
                                          // to be sent cannot pass this.
                                          return joined && summariesSeen >= 2;
                                        });

    Assert::IsTrue(sawSummaries, L"no summary frame reached a joined client");
    Assert::AreEqual(CountingSimulation::SUMMARY_MARKER, summaryMarker, L"the payload was not the simulation's");
    Assert::IsTrue(welcomedPlayer != INVALID_PLAYER_ID, L"the Welcome named no player");
    Assert::AreEqual(welcomedPlayer, summaryViewer, L"the frame was written for a player other than the one it reached");

    // The cadence, stated as the relationship rather than as a count of ticks:
    // by the time two frames have arrived, many more snapshots have. Asserting
    // the ratio rather than an exact number keeps this from failing on a slow
    // runner, which is the failure mode a timing test earns its keep by not
    // having.
    Assert::IsTrue(snapshotsSeen > summariesSeen * 4,
                   L"summaries arrived at anything like the snapshot rate; the ~1 Hz cadence is not being applied");

    host.Stop();
    host.Join();
  }

  /*
   * A view request is answered, gated by the game, and enforced by the engine
   * (ADR-016 §4, §7 — U3b).
   *
   * The three claims worth separating, because a broken gate can satisfy two of
   * them: that the *game* is asked (`ViewChecks` climbs), that a refusal is
   * carried back with the game's own reason code intact, and that a refused
   * request **leaves the feed where it was** rather than half-moving it. The
   * last is the one a naive implementation gets wrong — setting the view first
   * and validating after leaves a client watching a grid it was just told it
   * could not have.
   */
  TEST_METHOD(AViewRequestIsGatedByTheGameAndEnforcedByTheEngine)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    std::vector<ViewChanged> answers;
    std::uint32_t snapshotGrid = 0;
    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          joined = true;
        }
        else if (type == WireType::ViewChanged)
        {
          ViewChanged changed;
          if (Read(reader, changed))
          {
            answers.push_back(changed);
          }
        }
        else if (type == WireType::Snapshot)
        {
          (void)reader.ReadUInt32(); // tick
          (void)reader.ReadUInt32(); // marker
          (void)reader.ReadUInt32(); // last accepted order
          (void)reader.ReadUInt32(); // viewer
          const std::uint32_t grid = reader.ReadUInt32();
          if (reader.Ok())
          {
            snapshotGrid = grid;
          }
        }
      }
    };

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return joined && snapshotGrid != 0; }));
    Assert::AreEqual<std::uint32_t>(CountingSimulation::VIEWABLE_GRID, snapshotGrid,
                                    L"a session opens on the simulation's own grid");

    // A grid the game refuses.
    ByteWriter refuseWriter{buffer};
    WriteWireType(refuseWriter, WireType::ViewRequest);
    Write(refuseWriter, ViewRequest{CountingSimulation::FORBIDDEN_GRID});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, refuseWriter.Written()));

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return !answers.empty(); }));
    Assert::IsFalse(answers[0].accepted, L"the game refused and the engine agreed");
    Assert::AreEqual<std::uint16_t>(CountingSimulation::FORBIDDEN_GRID, answers[0].gridAnchor,
                                    L"the grid is echoed, so a client with two in flight knows which was refused");
    Assert::AreEqual<std::uint16_t>(CountingSimulation::NO_PRESENCE_REASON, answers[0].reasonCode,
                                    L"the game's own reason came back unread");
    Assert::IsTrue(simulation.ViewChecks() > 0, L"the game was actually asked");

    // And the feed did not move. Several more snapshots have to arrive for this
    // to mean anything, so wait for them rather than sampling once.
    snapshotGrid = 0;
    Assert::IsTrue(WaitUntil(client, [&] { drain(); return snapshotGrid != 0; }));
    Assert::AreEqual<std::uint32_t>(CountingSimulation::VIEWABLE_GRID, snapshotGrid,
                                    L"a refused request moved the feed anyway");

    // The grid it is allowed to have is accepted, and the same request path works.
    answers.clear();
    ByteWriter allowWriter{buffer};
    WriteWireType(allowWriter, WireType::ViewRequest);
    Write(allowWriter, ViewRequest{CountingSimulation::VIEWABLE_GRID});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, allowWriter.Written()));

    Assert::IsTrue(WaitUntil(client, [&] { drain(); return !answers.empty(); }));
    Assert::IsTrue(answers[0].accepted, L"the grid the game allows was refused");
    Assert::AreEqual<std::uint16_t>(0, answers[0].reasonCode, L"an accepted view carries no reason");

    host.Stop();
    host.Join();
  }

  /*
   * A grid that will not fit one datagram refuses **loudly**, and recovers
   * (ADR-018 A13, ADR-022 §6).
   *
   * The designed behaviour until the interest/delta slice lands is exactly
   * this: nothing is sent, the tick is counted, and a line names the client.
   * The two halves are both worth pinning. *Nothing sent*, because a truncated
   * snapshot is worse than a missing one -- the client reads the absent ships
   * as despawned and resurrects them next tick. *Counted*, because a server
   * that has quietly stopped replicating looks from the outside exactly like a
   * world where nothing is moving, and `SnapshotFailureCount` is what the debug
   * strip reads to tell those apart.
   *
   * It is driven from the refusal outwards rather than by building a fleet past
   * the cap, because the cap is GameLogic's arithmetic and this suite has no
   * GameLogic in it (ADR-014). `SnapshotTests` owns the other side: that a
   * world of `MAX_SHIPS_PER_SNAPSHOT + 5` ships is what makes the game refuse.
   */
  TEST_METHOD(AGridPastTheDatagramCapRefusesLoudlyAndRecovers)
  {
    CountingSimulation simulation;
    simulation.RefuseSnapshots(true);

    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    bool joined = false;
    std::uint32_t snapshotsSeen = 0;
    const auto drain = [&]
    {
      TransportEvent event;
      while (client.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          joined = true;
        }
        else if (type == WireType::Snapshot)
        {
          ++snapshotsSeen;
        }
      }
    };

    // Several refused ticks, so this cannot pass on one that merely had not
    // happened yet.
    const bool refused = WaitUntil(client,
                                   [&]
                                   {
                                     drain();
                                     return joined && host.SnapshotFailureCount() >= 3;
                                   });

    Assert::IsTrue(refused, L"a simulation refusing every snapshot was never counted as failing");
    Assert::AreEqual<std::uint32_t>(0, snapshotsSeen, L"a refused snapshot still put bytes on the wire");

    // And it is per tick, not a latch: the moment the grid fits again the feed
    // resumes without the session being rebuilt.
    const std::uint32_t failuresWhileRefusing = host.SnapshotFailureCount();
    simulation.RefuseSnapshots(false);

    const bool recovered = WaitUntil(client,
                                     [&]
                                     {
                                       drain();
                                       return snapshotsSeen >= 2;
                                     });

    Assert::IsTrue(recovered, L"the feed never resumed once the simulation could write again");
    Assert::IsTrue(host.SnapshotFailureCount() >= failuresWhileRefusing, L"the failure count went backwards");

    host.Stop();
    host.Join();
  }

  /*
   * Two clients, two serialisations (ADR-022 §1, ADR-018 A13).
   *
   * The rule this pins is not "snapshots arrive" -- the test above covers that
   * -- but *where the bytes are made*. A broadcast-shaped host serialises once
   * per tick and sends the result twice, so the count of snapshots the
   * simulation was asked to write would sit at about half the count the two
   * clients received. Per client, writes can never be fewer than sends.
   *
   * It is written now, before anything culls, because this is the assertion
   * that stops the shape regressing: ADR-017 §1's private roster and ADR-022's
   * per-viewer delta both rest on it, and neither is here yet to notice.
   */
  TEST_METHOD(EverySessionIsServedItsOwnSerialisation)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport first;
    QuicTransport second;
    const ConnectionId firstLink = first.Connect("127.0.0.1", host.BoundPort());
    const ConnectionId secondLink = second.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(firstLink != INVALID_CONNECTION, L"the first client could not open a link");
    Assert::IsTrue(secondLink != INVALID_CONNECTION, L"the second client could not open a link");

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(first.Send(firstLink, TransportChannel::Control, writer.Written()));
    Assert::IsTrue(second.Send(secondLink, TransportChannel::Control, writer.Written()));

    bool firstJoined = false;
    bool secondJoined = false;
    std::uint32_t firstSnapshots = 0;
    std::uint32_t secondSnapshots = 0;

    const auto drain = [](QuicTransport& _transport, bool& _joined, std::uint32_t& _snapshots)
    {
      TransportEvent event;
      while (_transport.NextEvent(event))
      {
        if (event.type != TransportEvent::Type::Message)
        {
          continue;
        }
        ByteReader reader{event.payload};
        const WireType type = ReadWireType(reader);
        if (type == WireType::Welcome)
        {
          _joined = true;
        }
        else if (type == WireType::Snapshot)
        {
          ++_snapshots;
        }
      }
    };

    // Three each, so the comparison below has enough ticks in it to separate
    // "one serialisation sent twice" from "two serialisations".
    constexpr std::uint32_t ENOUGH = 3;
    const std::int64_t start = Clock::Counter();
    while (Clock::MillisecondsBetween(start, Clock::Counter()) < 5000.0)
    {
      first.Poll();
      second.Poll();
      drain(first, firstJoined, firstSnapshots);
      drain(second, secondJoined, secondSnapshots);
      if (firstJoined && secondJoined && firstSnapshots >= ENOUGH && secondSnapshots >= ENOUGH)
      {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    Assert::IsTrue(firstJoined && secondJoined, L"both clients did not join");
    Assert::IsTrue(firstSnapshots >= ENOUGH && secondSnapshots >= ENOUGH, L"both clients were not served snapshots");
    Assert::IsTrue(simulation.SnapshotsWritten() >= firstSnapshots + secondSnapshots,
                   L"more snapshots arrived than were serialised, so one serialisation was sent to more than one client");
    Assert::AreEqual<PlayerId>(SOLE_PLAYER_ID, simulation.LastViewer(), L"the simulation was not told who the snapshot was for");
    Assert::AreEqual<std::uint32_t>(0, host.SnapshotFailureCount());

    host.Stop();
    host.Join();
  }

  /*
   * Submit, ack, and see it in the next snapshot (Build Order S9's acceptance).
   *
   * The whole loop over a real socket, and every part of it engine-side: the
   * order format belongs to `CountingSimulation` and the engine never parses
   * it, the verdict comes back out of the game and the engine echoes it, and
   * the sequence in the ack is the one the simulation read -- because the
   * engine could not have read it (ADR-004 ruling 4).
   */
  TEST_METHOD(AnOrderIsAckedAndThenAppearsInTheSnapshot)
  {
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter hello{buffer};
    WriteWireType(hello, WireType::Hello);
    Write(hello, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, hello.Written()));

    bool joined = false;
    Assert::IsTrue(WaitUntil(client,
                             [&]
                             {
                               TransportEvent event;
                               while (client.NextEvent(event))
                               {
                                 if (event.type != TransportEvent::Type::Message)
                                 {
                                   continue;
                                 }
                                 ByteReader reader{event.payload};
                                 Welcome welcome;
                                 if (ReadWireType(reader) == WireType::Welcome && Read(reader, welcome))
                                 {
                                   joined = true;
                                 }
                               }
                               return joined;
                             }),
                   L"the server never sent Welcome");

    constexpr std::uint32_t ORDER_SEQ = 31337;
    ByteWriter order{buffer};
    WriteWireType(order, WireType::OrderSubmit);
    order.WriteUInt32(CountingSimulation::ORDER_MARKER);
    order.WriteUInt32(ORDER_SEQ);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, order.Written()));

    OrderAck ack;
    bool acked = false;
    std::uint32_t seenSeq = 0;
    bool seenInSnapshot = false;

    const bool closed = WaitUntil(client,
                                  [&]
                                  {
                                    TransportEvent event;
                                    while (client.NextEvent(event))
                                    {
                                      if (event.type != TransportEvent::Type::Message)
                                      {
                                        continue;
                                      }
                                      ByteReader reader{event.payload};
                                      const WireType type = ReadWireType(reader);
                                      if (type == WireType::OrderAck && Read(reader, ack))
                                      {
                                        acked = true;
                                      }
                                      else if (type == WireType::Snapshot)
                                      {
                                        const std::uint32_t tick = reader.ReadUInt32();
                                        const std::uint32_t marker = reader.ReadUInt32();
                                        const std::uint32_t sequence = reader.ReadUInt32();
                                        if (reader.Ok() && marker == CountingSimulation::SNAPSHOT_MARKER && tick > 0)
                                        {
                                          seenSeq = sequence;
                                          seenInSnapshot = seenInSnapshot || sequence == ORDER_SEQ;
                                        }
                                      }
                                    }
                                    // Both halves, and the snapshot half only
                                    // counts after the ack: a snapshot that
                                    // already carried the order before the ack
                                    // arrived would still be the loop closing.
                                    return acked && seenInSnapshot;
                                  });

    Assert::IsTrue(closed, L"the order loop never closed");
    Assert::AreEqual<std::uint8_t>(1, ack.accepted);
    Assert::AreEqual<std::uint32_t>(ORDER_SEQ, ack.orderSeq, L"the sequence is the game's echo, not the engine's guess");
    Assert::IsTrue(ack.serverOrderId != 0, L"an accepted order is given an id");
    Assert::AreEqual<std::uint16_t>(0, ack.reasonCode);
    Assert::AreEqual<std::uint32_t>(ORDER_SEQ, seenSeq);

    Assert::AreEqual<std::uint32_t>(1, simulation.OrdersSeen());
    Assert::IsTrue(simulation.LastOrderClientId() != 0, L"the order was attributed to the session that sent it");
    Assert::AreEqual<std::uint32_t>(0, host.RefusedOrderCount());

    host.Stop();
    host.Join();
  }

  TEST_METHOD(ARefusedOrderBouncesWithTheGamesOwnReason)
  {
    // The engine passes a reason code it cannot interpret. This simulation's
    // enum is a number no game uses, which is the point: if the value arrives
    // intact, nothing in between looked at it.
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter hello{buffer};
    WriteWireType(hello, WireType::Hello);
    Write(hello, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, hello.Written()));

    bool joined = false;
    Assert::IsTrue(WaitUntil(client,
                             [&]
                             {
                               TransportEvent event;
                               while (client.NextEvent(event))
                               {
                                 if (event.type != TransportEvent::Type::Message)
                                 {
                                   continue;
                                 }
                                 ByteReader reader{event.payload};
                                 Welcome welcome;
                                 if (ReadWireType(reader) == WireType::Welcome && Read(reader, welcome))
                                 {
                                   joined = true;
                                 }
                               }
                               return joined;
                             }));

    constexpr std::uint32_t ORDER_SEQ = 8;
    ByteWriter bad{buffer};
    WriteWireType(bad, WireType::OrderSubmit);
    bad.WriteUInt32(0xdeadbeefu); // Not the marker this simulation accepts.
    bad.WriteUInt32(ORDER_SEQ);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, bad.Written()));

    OrderAck ack;
    bool acked = false;
    Assert::IsTrue(WaitUntil(client,
                             [&]
                             {
                               TransportEvent event;
                               while (client.NextEvent(event))
                               {
                                 if (event.type != TransportEvent::Type::Message)
                                 {
                                   continue;
                                 }
                                 ByteReader reader{event.payload};
                                 if (ReadWireType(reader) == WireType::OrderAck && Read(reader, ack))
                                 {
                                   acked = true;
                                 }
                               }
                               return acked;
                             }),
                   L"a refusal must come back; a lost one leaves a ghost on screen forever");

    Assert::AreEqual<std::uint8_t>(0, ack.accepted);
    Assert::AreEqual<std::uint16_t>(CountingSimulation::REFUSED_REASON, ack.reasonCode);
    Assert::AreEqual<std::uint32_t>(ORDER_SEQ, ack.orderSeq, L"a refusal still says which order it was about");
    Assert::AreEqual<std::uint32_t>(0, ack.serverOrderId, L"nothing refused is given an id");

    Assert::IsTrue(WaitUntil(client, [&] { return host.RefusedOrderCount() == 1; }));

    host.Stop();
    host.Join();
  }

  TEST_METHOD(AnOrderBeforeTheHandshakeIsDroppedRatherThanAcked)
  {
    // There is no client id to attribute it to and no agreement that the two
    // builds share a schema. Acking would imply it was considered.
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != INVALID_CONNECTION);

    std::array<std::uint8_t, 128> buffer{};
    ByteWriter order{buffer};
    WriteWireType(order, WireType::OrderSubmit);
    order.WriteUInt32(CountingSimulation::ORDER_MARKER);
    order.WriteUInt32(1);
    Assert::IsTrue(client.Send(link, TransportChannel::Control, order.Written()));

    bool acked = false;
    (void)WaitUntil(client,
                    [&]
                    {
                      TransportEvent event;
                      while (client.NextEvent(event))
                      {
                        if (event.type == TransportEvent::Type::Message)
                        {
                          ByteReader reader{event.payload};
                          if (ReadWireType(reader) == WireType::OrderAck)
                          {
                            acked = true;
                          }
                        }
                      }
                      return acked;
                    },
                    400.0);

    Assert::IsFalse(acked, L"an order from a connection that never joined must not be acked");
    Assert::AreEqual<std::uint32_t>(0, simulation.OrdersSeen(), L"nor reach the simulation");

    host.Stop();
    host.Join();
  }

  TEST_METHOD(RefusesAMismatchedBuildAtTheDoor)
  {
    // Fail closed: a client built against different content is turned away with
    // a reason, not allowed in to disagree quietly (ADR-004 §3).
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    QuicTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, simulation.SchemaHash(), 0xdeadbeef, "wrong-content"});
    Assert::IsTrue(client.Send(link, TransportChannel::Control, writer.Written()));

    UpdateRequired update;
    bool refused = false;
    const bool answered = WaitUntil(client,
                                    [&]
                                    {
                                      TransportEvent event;
                                      while (client.NextEvent(event))
                                      {
                                        if (event.type != TransportEvent::Type::Message)
                                        {
                                          continue;
                                        }
                                        ByteReader reader{event.payload};
                                        if (ReadWireType(reader) == WireType::UpdateRequired && Read(reader, update))
                                        {
                                          refused = true;
                                        }
                                      }
                                      return refused;
                                    });

    Assert::IsTrue(answered, L"the server accepted a mismatched build, or said nothing");
    Assert::AreEqual(simulation.ContentHash(), update.serverContentHash);
    Assert::AreEqual<std::uint32_t>(0, host.SessionCount());

    host.Stop();
    host.Join();
  }
};

} // namespace NeuronServerTests
