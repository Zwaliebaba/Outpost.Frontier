#include "pch.h"
#include "CppUnitTest.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"
#include "UdpTransport.h"
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

  void WriteSnapshot(std::uint32_t, ByteWriter&) override {}

  [[nodiscard]] OrderVerdict ApplyOrderBytes(std::uint32_t, std::span<const std::uint8_t>) override { return OrderVerdict{}; }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0xfeedfacecafebeefull; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0x0123456789abcdefull; }

  [[nodiscard]] std::uint32_t Ticks() const noexcept { return m_ticks.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t LastTick() const noexcept { return m_lastTick.load(std::memory_order_relaxed); }

private:
  std::atomic<std::uint32_t> m_ticks{0};
  std::atomic<std::uint32_t> m_lastTick{0};
};

template <typename Predicate>
bool WaitUntil(UdpTransport& _transport, Predicate _predicate, double _timeoutMs = 5000.0)
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
    NullSimulation simulation;
    for (int i = 0; i < 10; ++i)
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
    const std::uint32_t ticks = simulation.Ticks();

    host.Stop();
    host.Join();

    // 500 ms at 20 Hz is ten ticks. The bounds are loose because a shared CI
    // runner is not a real-time system; what is being checked is that the loop
    // ticks at all and does not sprint.
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

    UdpTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());
    Assert::IsTrue(link != InvalidConnection);

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{ProtocolVersion, simulation.SchemaHash(), simulation.ContentHash(), "harness"});
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
    Assert::AreEqual<std::uint32_t>(1, host.SessionCount());

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

  TEST_METHOD(RefusesAMismatchedBuildAtTheDoor)
  {
    // Fail closed: a client built against different content is turned away with
    // a reason, not allowed in to disagree quietly (ADR-004 §3).
    CountingSimulation simulation;
    ServerHost host;
    ServerConfig config;
    config.port = 0;
    Assert::IsTrue(host.Start(config, simulation));

    UdpTransport client;
    const ConnectionId link = client.Connect("127.0.0.1", host.BoundPort());

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{ProtocolVersion, simulation.SchemaHash(), 0xdeadbeef, "wrong-content"});
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
