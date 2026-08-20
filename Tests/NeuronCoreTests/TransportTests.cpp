#include "pch.h"
#include "CppUnitTest.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "Hash.h"
#include "QuicTransport.h"
#include "Transport.h"
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
 * These use real msquic connections over loopback rather than a fake. The
 * transport's whole job is the wire -- handshake, TLS, streams, datagrams --
 * and a test double would only prove the double works.
 */

namespace NeuronCoreTests
{
namespace
{

/// Polls both ends until the predicate holds or the deadline passes. Returns
/// false on timeout, so a failing test says "never happened" rather than hanging.
template <typename Predicate>
bool PumpUntil(QuicTransport& _a, QuicTransport& _b, Predicate _predicate, double _timeoutMs = 3000.0)
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
void Drain(QuicTransport& _transport, std::vector<TransportEvent::Type>& _outTypes, std::vector<std::vector<std::uint8_t>>& _outPayloads)
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

TEST_CLASS(QuicTransportTests)
{
public:
  TEST_METHOD(DeliversOnBothChannelsAcrossLoopback)
  {
    QuicTransport server;
    QuicTransport client;
    Assert::IsTrue(server.Listen(0), L"listener could not bind");
    Assert::IsTrue(server.BoundPort() != 0, L"ephemeral port was not reported");

    const ConnectionId toServer = client.Connect("127.0.0.1", server.BoundPort());
    Assert::IsTrue(toServer != INVALID_CONNECTION);

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

    // The server accepted the connection during the handshake, so it can answer.
    ConnectionId toClient = INVALID_CONNECTION;
    for (ConnectionId candidate = 1; candidate < 8; ++candidate)
    {
      if (server.State(candidate) == ConnectionState::Connected)
      {
        toClient = candidate;
        break;
      }
    }
    Assert::IsTrue(toClient != INVALID_CONNECTION, L"the server did not register the peer");

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
    QuicTransport server;
    QuicTransport client;
    Assert::IsTrue(server.Listen(0));
    const ConnectionId toServer = client.Connect("127.0.0.1", server.BoundPort());

    std::array<std::uint8_t, 256> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Hello);
    Write(writer, Hello{PROTOCOL_VERSION, 0xabcdef, 0x123456, "tester"});
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
    Assert::AreEqual<std::uint16_t>(PROTOCOL_VERSION, hello.protocolVersion);
    Assert::AreEqual<std::uint64_t>(0xabcdef, hello.schemaHash);
    Assert::IsTrue(hello.playerName == "tester");
    Assert::IsTrue(reader.FullyConsumed());
  }

  TEST_METHOD(RefusesAPayloadLargerThanADatagram)
  {
    // Nothing may depend on QUIC carrying more than the contract's datagram
    // cap (ADR-003): the cap is enforced here, not discovered in the field --
    // and it binds the reliable channel too, so nothing learns to lean on the
    // stream being roomier.
    QuicTransport server;
    QuicTransport client;
    Assert::IsTrue(server.Listen(0));
    const ConnectionId toServer = client.Connect("127.0.0.1", server.BoundPort());

    const std::vector<std::uint8_t> oversize(MAX_DATAGRAM_BYTES + 1, 0xcd);
    Assert::IsFalse(client.Send(toServer, TransportChannel::State, oversize));
    Assert::IsFalse(client.Send(toServer, TransportChannel::Control, oversize));

    const std::vector<std::uint8_t> largest(MAX_DATAGRAM_BYTES, 0xcd);
    Assert::IsTrue(client.Send(toServer, TransportChannel::State, largest));
  }

  TEST_METHOD(SendingOnADeadConnectionFailsQuietly)
  {
    QuicTransport client;
    const ConnectionId unknown = 999;
    const std::array<std::uint8_t, 1> payload{1};

    Assert::IsFalse(client.Send(unknown, TransportChannel::Control, payload));
    Assert::IsTrue(client.State(unknown) == ConnectionState::Closed);

    // Closing something that was never open must not fault either.
    client.Close(unknown, DisconnectReason::ClosedByPeer);
  }

  TEST_METHOD(ReportsAnEphemeralPortAndShutsDownCleanly)
  {
    QuicTransport transport;
    Assert::IsTrue(transport.Listen(0));
    const std::uint16_t port = transport.BoundPort();
    Assert::IsTrue(port != 0);

    transport.Shutdown();
    Assert::AreEqual<std::uint16_t>(0, transport.BoundPort());

    // Shutting down twice is a normal thing to do on a failure path.
    transport.Shutdown();
  }

  TEST_METHOD(ACloseReasonCrossesTheWire)
  {
    /*
     * Close() puts its DisconnectReason on the wire as the QUIC application
     * close code, so the peer can tell a deliberate goodbye from a crash --
     * something the UDP implementation could only approximate with a message.
     * The server closes with ShuttingDown; the client's Disconnected event
     * must carry that reason, not a guess.
     */
    QuicTransport server;
    QuicTransport client;
    Assert::IsTrue(server.Listen(0));
    const ConnectionId toServer = client.Connect("127.0.0.1", server.BoundPort());
    Assert::IsTrue(toServer != INVALID_CONNECTION);

    const bool connected = PumpUntil(server, client, [&] { return client.State(toServer) == ConnectionState::Connected; });
    Assert::IsTrue(connected, L"the handshake never completed");

    ConnectionId toClient = INVALID_CONNECTION;
    for (ConnectionId candidate = 1; candidate < 8; ++candidate)
    {
      if (server.State(candidate) == ConnectionState::Connected)
      {
        toClient = candidate;
        break;
      }
    }
    Assert::IsTrue(toClient != INVALID_CONNECTION, L"the server did not register the peer");

    server.Close(toClient, DisconnectReason::ShuttingDown);

    DisconnectReason heard = DisconnectReason::None;
    const bool told = PumpUntil(server, client,
                                [&]
                                {
                                  TransportEvent event;
                                  while (client.NextEvent(event))
                                  {
                                    if (event.type == TransportEvent::Type::Disconnected)
                                    {
                                      heard = event.reason;
                                    }
                                  }
                                  return heard != DisconnectReason::None;
                                });

    Assert::IsTrue(told, L"the client was never told");
    Assert::IsTrue(heard == DisconnectReason::ShuttingDown, L"the reason did not survive the wire");
  }
};

TEST_CLASS(WireTests)
{
public:
  TEST_METHOD(EveryMessageRoundTrips)
  {
    std::array<std::uint8_t, 512> buffer{};
    ByteWriter writer{buffer};

    // The anchor is deliberately a large negative value: the universe plane is
    // signed and runs to +/-9.2e18 metres (ADR-009 §1), so a field that was
    // narrowed or made unsigned anywhere along the way folds here rather than
    // in a session where a world quietly renders in the wrong place.
    constexpr std::int64_t ANCHOR_X = -4611686018427387904ll;
    constexpr std::int64_t ANCHOR_Y = 9223372036854775807ll;

    WriteWireType(writer, WireType::Welcome);
    Write(writer, Welcome{7, 1234, 20, 0xaaaa, 0xbbbb, 9, ANCHOR_X, ANCHOR_Y, "Vesta-3", "Frontier 0.4", "SEC 0.4"});

    ByteReader reader{writer.Written()};
    Assert::IsTrue(ReadWireType(reader) == WireType::Welcome);

    Welcome welcome;
    Assert::IsTrue(Read(reader, welcome));
    Assert::AreEqual<std::uint32_t>(7, welcome.clientId);
    Assert::AreEqual<std::uint32_t>(1234, welcome.tick);
    Assert::AreEqual<std::uint16_t>(20, welcome.tickRate);
    Assert::AreEqual<std::uint64_t>(0xaaaa, welcome.schemaHash);
    Assert::AreEqual<std::uint64_t>(0xbbbb, welcome.contentHash);
    Assert::AreEqual<std::uint16_t>(9, welcome.worldId);
    Assert::AreEqual(ANCHOR_X, welcome.anchorX);
    Assert::AreEqual(ANCHOR_Y, welcome.anchorY);
    Assert::AreEqual(std::string{"Vesta-3"}, welcome.worldName);
    Assert::AreEqual(std::string{"Frontier 0.4"}, welcome.worldDetail);
    Assert::AreEqual(std::string{"SEC 0.4"}, welcome.worldBadge);
    Assert::IsTrue(reader.FullyConsumed());
  }

  TEST_METHOD(AnOrderAckCarriesTheVerdictAndBothIdentities)
  {
    /*
     * Two identities on purpose (ADR-004 §7). `orderSeq` is the client's own
     * counter and the only field it can match a ghost on before anything comes
     * back; `serverOrderId` is what the authority assigned and what the
     * snapshot's order records are keyed by afterwards. A refusal carries the
     * sequence and a zero id, because nothing was given one.
     */
    std::array<std::uint8_t, 64> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::OrderAck);
    Write(writer, OrderAck{4242, 77, 5, 1});

    ByteReader reader{writer.Written()};
    Assert::IsTrue(ReadWireType(reader) == WireType::OrderAck);

    OrderAck ack;
    Assert::IsTrue(Read(reader, ack));
    Assert::AreEqual<std::uint32_t>(4242, ack.orderSeq);
    Assert::AreEqual<std::uint32_t>(77, ack.serverOrderId);
    Assert::AreEqual<std::uint16_t>(5, ack.reasonCode);
    Assert::AreEqual<std::uint8_t>(1, ack.accepted);
    Assert::IsTrue(reader.FullyConsumed());

    // A refusal: the sequence survives, the id is zero, and the reason is the
    // game's number passed through unread.
    ByteWriter refusal{buffer};
    WriteWireType(refusal, WireType::OrderAck);
    Write(refusal, OrderAck{9, 0, 3, 0});
    ByteReader back{refusal.Written()};
    (void)ReadWireType(back);
    OrderAck refused;
    Assert::IsTrue(Read(back, refused));
    Assert::AreEqual<std::uint32_t>(9, refused.orderSeq);
    Assert::AreEqual<std::uint32_t>(0, refused.serverOrderId);
    Assert::AreEqual<std::uint8_t>(0, refused.accepted);
  }

  TEST_METHOD(TheOrderTypeWordsAreFixedNumbers)
  {
    // Two builds disagreeing about which number means "order" is a wire break
    // that no schema hash over field names would catch, which is why the type
    // words are in `CORE_SCHEMA_TEXT` and asserted here.
    Assert::AreEqual<std::uint16_t>(9, static_cast<std::uint16_t>(WireType::OrderSubmit));
    Assert::AreEqual<std::uint16_t>(10, static_cast<std::uint16_t>(WireType::OrderAck));
    Assert::AreEqual<std::uint16_t>(8, static_cast<std::uint16_t>(WireType::Snapshot));
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
    Assert::AreNotEqual(hash, HashText(CORE_SCHEMA_TEXT)); // The version is folded in.
  }
};

} // namespace NeuronCoreTests
