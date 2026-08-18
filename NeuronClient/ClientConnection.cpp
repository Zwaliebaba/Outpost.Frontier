#include "pch.h"

#include "ClientConnection.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Clock.h"
#include "Log.h"
#include "QuicTransport.h"
#include "Wire.h"

#include <array>

namespace Neuron
{
namespace
{

/// Often enough for a live RTT readout, rare enough to be invisible next to
/// snapshot traffic.
constexpr double PING_INTERVAL_MS = 250.0;

/// How often the NET line goes to the log. Frequent enough to see a link
/// degrade, rare enough that nobody turns logging off because of it.
constexpr double STATS_INTERVAL_MS = 5000.0;

} // namespace

ClientConnection::~ClientConnection()
{
  Disconnect();
}

bool ClientConnection::Connect(const std::string& _host, std::uint16_t _port, std::uint64_t _schemaHash, std::uint64_t _contentHash,
                               const std::string& _playerName)
{
  Disconnect();

  m_schemaHash = _schemaHash;
  m_contentHash = _contentHash;
  m_playerName = _playerName;

  auto transport = std::make_unique<QuicTransport>();
  m_connection = transport->Connect(_host, _port);
  if (m_connection == INVALID_CONNECTION)
  {
    return false;
  }

  m_transport = std::move(transport);
  m_state = ClientLinkState::Connecting;
  m_helloSent = false;
  SendHello();
  return true;
}

void ClientConnection::SendHello()
{
  Hello hello;
  hello.protocolVersion = PROTOCOL_VERSION;
  hello.schemaHash = m_schemaHash;
  hello.contentHash = m_contentHash;
  hello.playerName = m_playerName;

  std::array<std::uint8_t, 256> buffer{};
  ByteWriter writer{buffer};
  WriteWireType(writer, WireType::Hello);
  Write(writer, hello);
  if (!writer.Ok())
  {
    NEURON_LOG_ERROR("Hello did not fit its buffer");
    return;
  }

  // Control channel: the handshake is exactly the traffic that must arrive.
  if (m_transport->Send(m_connection, TransportChannel::Control, writer.Written()))
  {
    m_helloSent = true;
  }
}

void ClientConnection::SendPing()
{
  std::array<std::uint8_t, 32> buffer{};
  ByteWriter writer{buffer};
  WriteWireType(writer, WireType::Ping);
  Write(writer, Ping{static_cast<std::uint64_t>(Clock::MicrosecondsSinceStart())});
  // Unreliable: a lost ping is answered by the next one, and a resent one would
  // measure the wrong thing.
  (void)m_transport->Send(m_connection, TransportChannel::State, writer.Written());
  m_lastPingCounter = Clock::Counter();
  ++m_pingCount;
}

bool ClientConnection::SendOrder(std::span<const std::uint8_t> _payload)
{
  if (m_state != ClientLinkState::Joined || m_transport == nullptr || _payload.empty())
  {
    return false;
  }

  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> buffer{};
  ByteWriter writer{buffer};
  WriteWireType(writer, WireType::OrderSubmit);
  writer.WriteBytes(_payload.data(), _payload.size());
  if (!writer.Ok())
  {
    // Longer than a datagram. The game's own cap makes this unreachable for an
    // order it would build, so reaching it means a payload from somewhere else
    // -- and a truncated order is worse than none.
    return false;
  }

  if (!m_transport->Send(m_connection, TransportChannel::Control, writer.Written()))
  {
    return false;
  }
  ++m_orderSendCount;
  return true;
}

TransportStats ClientConnection::Stats() const
{
  return m_transport == nullptr ? TransportStats{} : m_transport->Stats(m_connection);
}

void ClientConnection::LogNetStats()
{
  const TransportStats stats = Stats();

  // Unreliable loss is the honest measure here: pings that never came back.
  // The reliable channel reports its own losses as resends, so both channels
  // are visible rather than one being inferred from the other (S4).
  const std::uint64_t lost = m_pingCount > m_pongCount ? m_pingCount - m_pongCount : 0;
  const double lossPercent = m_pingCount == 0 ? 0.0 : 100.0 * static_cast<double>(lost) / static_cast<double>(m_pingCount);

  NEURON_LOG_INFO("net(client %u): rtt %.2f ms, ping loss %.1f%% (%llu/%llu), resends %llu, dgrams %llu/%llu, bytes %llu/%llu", m_clientId,
                  m_roundTripMs, lossPercent, static_cast<unsigned long long>(lost), static_cast<unsigned long long>(m_pingCount),
                  static_cast<unsigned long long>(stats.controlResends), static_cast<unsigned long long>(stats.datagramsSent),
                  static_cast<unsigned long long>(stats.datagramsReceived), static_cast<unsigned long long>(stats.bytesSent),
                  static_cast<unsigned long long>(stats.bytesReceived));
  m_lastStatsCounter = Clock::Counter();
}

void ClientConnection::HandleMessage(const TransportEvent& _event)
{
  ByteReader reader{_event.payload};
  switch (ReadWireType(reader))
  {
  case WireType::Welcome:
  {
    Welcome welcome;
    if (!Read(reader, welcome))
    {
      return;
    }
    m_clientId = welcome.clientId;
    m_serverTick = welcome.tick;
    m_serverTickRate = welcome.tickRate;
    m_worldId = welcome.worldId;
    m_anchorX = welcome.anchorX;
    m_anchorY = welcome.anchorY;
    m_state = ClientLinkState::Joined;
    m_lastStatsCounter = Clock::Counter(); // The first NET line is due a full interval after joining.
    NEURON_LOG_INFO("joined as client %u (server at tick %u, %u Hz)", m_clientId, m_serverTick, static_cast<unsigned>(m_serverTickRate));
    NEURON_LOG_INFO("world %u anchored at (%lld, %lld)", static_cast<unsigned>(m_worldId), static_cast<long long>(m_anchorX),
                    static_cast<long long>(m_anchorY));
    return;
  }

  case WireType::UpdateRequired:
  {
    UpdateRequired update;
    (void)Read(reader, update);
    // Fail closed and say which half disagrees, so the answer is "rebuild" or
    // "re-export content" rather than "it does not connect".
    NEURON_LOG_ERROR("refused: build mismatch (server schema %llx content %llx; ours %llx / %llx)",
                     static_cast<unsigned long long>(update.serverSchemaHash), static_cast<unsigned long long>(update.serverContentHash),
                     static_cast<unsigned long long>(m_schemaHash), static_cast<unsigned long long>(m_contentHash));
    m_state = ClientLinkState::Rejected;
    return;
  }

  case WireType::Snapshot:
  {
    // Everything after the type word is the game's, byte for byte. The
    // connection's whole job here is to not touch it.
    const std::span<const std::uint8_t> payload = reader.Remaining();
    if (payload.empty())
    {
      return;
    }

    ++m_snapshotCount;
    if (m_pendingSnapshots.size() >= MAX_PENDING_SNAPSHOTS)
    {
      // Drop the oldest. Full snapshots supersede each other (ADR-004 §6), so
      // the freshest is the one worth keeping when the frame loop is behind.
      m_pendingSnapshots.erase(m_pendingSnapshots.begin());
      ++m_snapshotOverflowCount;
    }
    m_pendingSnapshots.emplace_back(payload.begin(), payload.end());
    return;
  }

  case WireType::OrderAck:
  {
    OrderAck ack;
    if (!Read(reader, ack))
    {
      return;
    }
    ++m_orderAckCount;

    // Straight into `OrderVerdict`, which is the type both seams speak
    // (OrderIntent.h): the same four numbers the client's own pre-check
    // produces, so a ghost cannot tell where its answer came from. The reason
    // code is the game's and is copied without being read.
    OrderVerdict verdict;
    verdict.accepted = ack.accepted != 0;
    verdict.reasonCode = ack.reasonCode;
    verdict.serverOrderId = ack.serverOrderId;
    verdict.orderSeq = ack.orderSeq;
    m_pendingVerdicts.push_back(verdict);
    return;
  }

  case WireType::Refuse:
  {
    Refuse refuse;
    (void)Read(reader, refuse);
    NEURON_LOG_ERROR("refused by the server (reason %u)", static_cast<unsigned>(refuse.reason));
    m_state = ClientLinkState::Rejected;
    return;
  }

  case WireType::Pong:
  {
    Pong pong;
    if (!Read(reader, pong))
    {
      return;
    }
    const auto now = static_cast<std::uint64_t>(Clock::MicrosecondsSinceStart());
    m_roundTripMs = static_cast<double>(now - pong.clientSendMicroseconds) / 1000.0;
    m_serverTick = pong.serverTick;
    if (m_pongCount == 0)
    {
      // The heartbeat has crossed the loopback and come back: milestone M0.
      NEURON_LOG_INFO("first pong: server tick %u, round trip %.3f ms", m_serverTick, m_roundTripMs);
    }
    ++m_pongCount;
    return;
  }

  case WireType::Goodbye:
  {
    NEURON_LOG_INFO("server said goodbye");
    m_state = ClientLinkState::Disconnected;
    return;
  }

  default:
    return;
  }
}

void ClientConnection::Poll()
{
  if (m_transport == nullptr)
  {
    return;
  }

  m_transport->Poll();

  TransportEvent event;
  while (m_transport->NextEvent(event))
  {
    switch (event.type)
    {
    case TransportEvent::Type::Message:
      HandleMessage(event);
      break;

    case TransportEvent::Type::Disconnected:
      if (m_state != ClientLinkState::Rejected)
      {
        NEURON_LOG_WARNING("connection lost (reason %u)", static_cast<unsigned>(event.reason));
        m_state = ClientLinkState::Disconnected;
      }
      break;

    default:
      break;
    }
  }

  if (!m_helloSent && m_state == ClientLinkState::Connecting)
  {
    SendHello(); // The socket refused it earlier; try again now.
  }

  if (m_state != ClientLinkState::Joined)
  {
    return;
  }

  const std::int64_t now = Clock::Counter();
  if (Clock::MillisecondsBetween(m_lastPingCounter, now) >= PING_INTERVAL_MS)
  {
    SendPing();
  }
  if (Clock::MillisecondsBetween(m_lastStatsCounter, now) >= STATS_INTERVAL_MS)
  {
    LogNetStats();
  }
}

void ClientConnection::Disconnect()
{
  if (m_transport == nullptr)
  {
    return;
  }

  if (m_state == ClientLinkState::Joined || m_state == ClientLinkState::Connecting)
  {
    std::array<std::uint8_t, 32> buffer{};
    ByteWriter writer{buffer};
    WriteWireType(writer, WireType::Goodbye);
    Write(writer, Goodbye{static_cast<std::uint16_t>(DisconnectReason::ClosedByPeer)});
    (void)m_transport->Send(m_connection, TransportChannel::Control, writer.Written());
    m_transport->Poll(); // Give the goodbye its chance to leave before the socket closes.
  }

  m_transport->Shutdown();
  m_transport.reset();
  m_connection = INVALID_CONNECTION;
  m_state = ClientLinkState::Disconnected;

  // Undelivered promises die with the link. A verdict drained after a
  // reconnect would be answering an order the new session never heard, and the
  // sequence numbers restart, so it could match the wrong ghost outright.
  m_pendingVerdicts.clear();
  m_pendingSnapshots.clear();
}

} // namespace Neuron
