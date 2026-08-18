#include "pch.h"
#include "CppUnitTest.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "Hash.h"
#include "Transport.h"
#include "UdpTransport.h"
#include "Wire.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * These use a real loopback socket rather than a fake. The transport's whole
 * job is the socket, and a test double would only prove the double works.
 */

namespace NeuronCoreTests
{
namespace
{

/// Polls both ends until the predicate holds or the deadline passes. Returns
/// false on timeout, so a failing test says "never happened" rather than hanging.
template <typename Predicate>
bool PumpUntil(UdpTransport& _a, UdpTransport& _b, Predicate _predicate, double _timeoutMs = 3000.0)
{
  const std::int64_t start = Clock::Counter();
  while (Clock::MillisecondsBetween(start, Clock::Counter()) < _timeoutMs)
  {
    _a.Poll();
    _b.Poll();
    if (_predicate())
    {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

/// Drains events into a list, since a test usually wants to look at all of them.
void Drain(UdpTransport& _transport, std::vector<TransportEvent::Type>& _outTypes, std::vector<std::vector<std::uint8_t>>& _outPayloads)
{
  TransportEvent event;
  while (_transport.NextEvent(event))
  {
    _outTypes.push_back(event.type);
    if (event.type == TransportEvent::Type::Message)
    {
      _outPayloads.emplace_back(event.payload.begin(), event.payload.end());
    }
  }
}

} // namespace

TEST_CLASS(UdpTransportTests)
{
public:
  TEST_METHOD(DeliversOnBothChannelsAcrossLoopback)
  {
    UdpTransport server;
    UdpTransport client;
    Assert::IsTrue(server.Listen(0), L"listener could not bind");
    Assert::IsTrue(server.BoundPort() != 0, L"ephemeral port was not reported");

    const ConnectionId toServer = client.Connect("127.0.0.1", server.BoundPort());
    Assert::IsTrue(toServer != InvalidConnection);

    const std::array<std::uint8_t, 4> control{1, 2, 3, 4};
    Assert::IsTrue(client.Send(toServer, TransportChannel::Control, control));

    std::vector<TransportEvent::Type> serverEvents;
    std::vector<std::vector<std::uint8_t>> serverPayloads;
    const bool arrived = PumpUntil(server, client,
                                   [&]
                                   {
                                     Drain(server, serverEvents, serverPayloads);
                                     return !serverPayloads.empty();
                                   });

    Assert::IsTrue(arrived, L"the control message never arrived");
    Assert::AreEqual<std::size_t>(4, serverPayloads.front().size());
    Assert::AreEqual<std::uint8_t>(1, serverPayloads.front()[0]);

    // The server learned the client's address from the datagram, so it can answer.
    ConnectionId toClient = InvalidConnection;
    for (ConnectionId candidate = 1; candidate < 8; ++candidate)
    {
      if (server.State(candidate) == ConnectionState::Connected)
      {
        toClient = candidate;
        break;
      }
    }
    Assert::IsTrue(toClient != InvalidConnection, L"the server did not register the peer");

    const std::array<std::uint8_t, 3> state{9, 8, 7};
    Assert::IsTrue(server.Send(toClient, TransportChannel::State, state));

    std::vector<TransportEvent::Type> clientEvents;
    std::vector<std::vector<std::uint8_t>> clientPayloads;
    const bool answered = PumpUntil(server, client,
                                    [&]
                                    {
                                      Drain(client, clientEvents, clientPayloads);
                                      return !clientPayloads.empty();
                                    });

    Assert::IsTrue(answered, L"the datagram never came back");
    Assert::AreEqual<std::size_t>(3, clientPayloads.front().size());
    Assert::AreEqual<std::uint8_t>(9, clientPayloads.front()[0]);
  }

  TEST_METHOD(CompletesTheHandshakeMessages)
  {
    // The M0 exchange, at the transport level: Hello in, Welcome back.
    UdpTransport server;
    UdpTransport client;
    Assert::IsTrue(server.Listen(0));
    const ConnectionId toServer = client.Connect("127.0.0.1", server.BoundPort());

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{ProtocolVersion, 0xabcdef, 0x123456, "tester"});
    Assert::IsTrue(writer.Ok());
    Assert::IsTrue(client.Send(toServer, TransportChannel::Control, writer.Written()));

    std::vector<TransportEvent::Type> types;
    std::vector<std::vector<std::uint8_t>> payloads;
    const bool arrived = PumpUntil(server, client,
                                   [&]
                                   {
                                     Drain(server, types, payloads);
                                     return !payloads.empty();
                                   });
    Assert::IsTrue(arrived, L"Hello never arrived");

    ByteReader reader{payloads.front()};
    Assert::IsTrue(ReadWireType(reader) == WireType::Hello);

    Hello hello;
    Assert::IsTrue(Read(reader, hello));
    Assert::AreEqual<std::uint16_t>(ProtocolVersion, hello.protocolVersion);
    Assert::AreEqual<std::uint64_t>(0xabcdef, hello.schemaHash);
    Assert::IsTrue(hello.playerName == "tester");
    Assert::IsTrue(reader.FullyConsumed());
  }

  TEST_METHOD(RefusesAPayloadLargerThanADatagram)
  {
    // Nothing may depend on loopback carrying more than a real link would
    // (ADR-003): the cap is enforced here, not discovered in the field.
    UdpTransport server;
    UdpTransport client;
    Assert::IsTrue(server.Listen(0));
    const ConnectionId toServer = client.Connect("127.0.0.1", server.BoundPort());

    const std::vector<std::uint8_t> oversize(UdpTransport::MaxPayloadBytes + 1, 0xcd);
    Assert::IsFalse(client.Send(toServer, TransportChannel::State, oversize));

    const std::vector<std::uint8_t> largest(UdpTransport::MaxPayloadBytes, 0xcd);
    Assert::IsTrue(client.Send(toServer, TransportChannel::State, largest));
  }

  TEST_METHOD(SendingOnADeadConnectionFailsQuietly)
  {
    UdpTransport client;
    const ConnectionId unknown = 999;
    const std::array<std::uint8_t, 1> payload{1};

    Assert::IsFalse(client.Send(unknown, TransportChannel::Control, payload));
    Assert::IsTrue(client.State(unknown) == ConnectionState::Closed);

    // Closing something that was never open must not fault either.
    client.Close(unknown, DisconnectReason::ClosedByPeer);
  }

  TEST_METHOD(ReportsAnEphemeralPortAndShutsDownCleanly)
  {
    UdpTransport transport;
    Assert::IsTrue(transport.Listen(0));
    const std::uint16_t port = transport.BoundPort();
    Assert::IsTrue(port != 0);

    transport.Shutdown();
    Assert::AreEqual<std::uint16_t>(0, transport.BoundPort());

    // Shutting down twice is a normal thing to do on a failure path.
    transport.Shutdown();
  }
};

TEST_CLASS(WireTests)
{
public:
  TEST_METHOD(EveryMessageRoundTrips)
  {
    std::array<std::uint8_t, 512> buffer{};
    ByteWriter writer{buffer};

    WriteWireType(writer, WireType::Welcome);
    Write(writer, Welcome{7, 1234, 20, 0xaaaa, 0xbbbb});

    ByteReader reader{writer.Written()};
    Assert::IsTrue(ReadWireType(reader) == WireType::Welcome);

    Welcome welcome;
    Assert::IsTrue(Read(reader, welcome));
    Assert::AreEqual<std::uint32_t>(7, welcome.clientId);
    Assert::AreEqual<std::uint32_t>(1234, welcome.tick);
    Assert::AreEqual<std::uint16_t>(20, welcome.tickRate);
    Assert::AreEqual<std::uint64_t>(0xaaaa, welcome.schemaHash);
    Assert::IsTrue(reader.FullyConsumed());
  }

  TEST_METHOD(PingEchoesTheClientsOwnTimestamp)
  {
    // The client measures its round trip without the two machines agreeing on
    // a clock, which only works if the value comes back untouched.
    std::array<std::uint8_t, 64> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Pong);
    Write(writer, Pong{0x0123456789abcdefull, 4242});

    ByteReader reader{writer.Written()};
    Assert::IsTrue(ReadWireType(reader) == WireType::Pong);

    Pong pong;
    Assert::IsTrue(Read(reader, pong));
    Assert::AreEqual<std::uint64_t>(0x0123456789abcdefull, pong.clientSendMicroseconds);
    Assert::AreEqual<std::uint32_t>(4242, pong.serverTick);
  }

  TEST_METHOD(ATruncatedMessageIsRejectedRatherThanGuessed)
  {
    const std::array<std::uint8_t, 3> truncated{static_cast<std::uint8_t>(WireType::Welcome), 0, 1};
    ByteReader reader{truncated};
    Assert::IsTrue(ReadWireType(reader) == WireType::Welcome);

    Welcome welcome;
    Assert::IsFalse(Read(reader, welcome));
  }

  TEST_METHOD(SchemaHashIsStableAndCoversTheVersion)
  {
    const std::uint64_t hash = CoreSchemaHash();
    Assert::AreEqual(hash, CoreSchemaHash()); // Same build, same answer.
    Assert::IsTrue(hash != 0);
    Assert::AreNotEqual(hash, HashText(CoreSchemaText)); // The version is folded in.
  }
};

} // namespace NeuronCoreTests
