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

  /// Writes a tiny recognisable payload so a test can prove the datagram
  /// carried the simulation's bytes and not the engine's idea of them. The
  /// last accepted order rides along, which is what "state in the next
  /// snapshot" means for a simulation with no world.
  [[nodiscard]] bool WriteSnapshot(std::uint32_t _tick, ByteWriter& _writer) override
  {
    _writer.WriteUInt32(_tick);
    _writer.WriteUInt32(SNAPSHOT_MARKER);
    _writer.WriteUInt32(m_lastAcceptedSeq.load(std::memory_order_relaxed));
    ++m_snapshotsWritten;
    return _writer.Ok();
  }

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
  [[nodiscard]] WorldMeta World() const override { return WorldMeta{42, -4200000000ll, 1750000000ll}; }

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
    Assert::AreEqual(world.anchorX, welcome.anchorX);
    Assert::AreEqual(world.anchorY, welcome.anchorY);

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
  TEST_METHOD(BroadcastsTheSimulationsOwnBytesToAJoinedClient)
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
    std::uint32_t snapshotTick = 0;
    std::uint32_t marker = 0;
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
                                              if (reader.Ok())
                                              {
                                                snapshotTick = tick;
                                                marker = payloadMarker;
                                                ++snapshotsSeen;
                                              }
                                            }
                                          }
                                          // Two, so this cannot pass on a single
                                          // snapshot that happened to be sent.
                                          return joined && snapshotsSeen >= 2;
                                        });

    Assert::IsTrue(sawSnapshots, L"the server never broadcast a snapshot to a joined client");
    Assert::AreEqual(CountingSimulation::SNAPSHOT_MARKER, marker, L"the payload was not the simulation's");
    Assert::IsTrue(snapshotTick > 0, L"a snapshot was sent for a tick the world never ran");
    Assert::IsTrue(snapshotTick <= host.TickCount(), L"a snapshot ran ahead of the tick counter");
    Assert::IsTrue(simulation.SnapshotsWritten() >= snapshotsSeen, L"more snapshots arrived than were written");
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
