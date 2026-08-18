#pragma once

#include "Transport.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

/*
 * The MVP transport (ADR-003 §2): one non-blocking UDP socket per side, with a
 * deliberately dumb reliability layer on the control channel.
 *
 * Stop-and-wait: one unacknowledged control message in flight, resent on a
 * timer until the peer acknowledges it. That is enough for a handshake and an
 * order stream, it keeps ordering by construction, and it is small enough to
 * throw away when QuicTransport takes over. Nothing may grow features on it.
 *
 * Loopback loses packets too -- a full receive buffer is all it takes -- so the
 * reliability here is real, not ceremony.
 */

namespace Neuron
{

class UdpTransport final : public Transport
{
public:
  UdpTransport() = default;
  ~UdpTransport() override;

  UdpTransport(const UdpTransport&) = delete;
  UdpTransport& operator=(const UdpTransport&) = delete;

  [[nodiscard]] bool Listen(std::uint16_t _port) override;
  [[nodiscard]] ConnectionId Connect(const std::string& _host, std::uint16_t _port) override;
  void Poll() override;
  [[nodiscard]] bool NextEvent(TransportEvent& _outEvent) override;
  [[nodiscard]] bool Send(ConnectionId _connection, TransportChannel _channel, std::span<const std::uint8_t> _payload) override;
  void Close(ConnectionId _connection, DisconnectReason _reason) override;
  void Shutdown() override;
  [[nodiscard]] ConnectionState State(ConnectionId _connection) const override;
  [[nodiscard]] TransportStats Stats(ConnectionId _connection) const override;
  [[nodiscard]] std::uint16_t BoundPort() const override { return m_boundPort; }

  /// Header bytes ahead of every payload: channel/flags, sequence, ack, length.
  static constexpr std::size_t HeaderBytes = 7;
  static constexpr std::size_t MaxPayloadBytes = MaxDatagramBytes - HeaderBytes;

  /// How long an unanswered control message waits before it is sent again.
  static constexpr double ResendIntervalMs = 50.0;
  /// A connection with no traffic at all for this long is gone.
  static constexpr double TimeoutMs = 10000.0;

private:
  struct Endpoint
  {
    std::uint32_t address = 0; // Network order, as it arrives.
    std::uint16_t port = 0;

    [[nodiscard]] bool operator==(const Endpoint& _other) const noexcept
    {
      return address == _other.address && port == _other.port;
    }
  };

  struct PendingControl
  {
    std::vector<std::uint8_t> payload;
    std::uint16_t sequence = 0;
    std::int64_t lastSentCounter = 0;
    bool sent = false;
  };

  struct Connection
  {
    ConnectionId id = InvalidConnection;
    Endpoint endpoint;
    ConnectionState state = ConnectionState::Closed;
    TransportStats stats;

    std::uint16_t nextSequence = 1;
    std::uint16_t lastReceivedSequence = 0;
    std::uint16_t lastAckSent = 0;
    std::deque<PendingControl> controlQueue; // Front is in flight.
    std::int64_t lastReceivedCounter = 0;
  };

  [[nodiscard]] bool OpenSocket(std::uint16_t _port);
  void ReceiveAll();
  void ServiceResends();
  void ServiceTimeouts();
  [[nodiscard]] Connection* Find(ConnectionId _id);
  [[nodiscard]] const Connection* Find(ConnectionId _id) const;
  [[nodiscard]] Connection* FindByEndpoint(const Endpoint& _endpoint);
  [[nodiscard]] Connection& AddConnection(const Endpoint& _endpoint, ConnectionState _state);
  void SendRaw(Connection& _connection, TransportChannel _channel, std::uint16_t _sequence, std::uint16_t _ack,
               std::span<const std::uint8_t> _payload);
  void QueueEvent(TransportEvent::Type _type, ConnectionId _connection, TransportChannel _channel, DisconnectReason _reason,
                  std::span<const std::uint8_t> _payload);

  std::uintptr_t m_socket = ~static_cast<std::uintptr_t>(0); // INVALID_SOCKET without dragging in winsock here.
  std::uint16_t m_boundPort = 0;
  bool m_listening = false;

  std::unordered_map<ConnectionId, Connection> m_connections;
  ConnectionId m_nextConnectionId = 1;

  struct QueuedEvent
  {
    TransportEvent::Type type = TransportEvent::Type::None;
    ConnectionId connection = InvalidConnection;
    TransportChannel channel = TransportChannel::Control;
    DisconnectReason reason = DisconnectReason::None;
    std::vector<std::uint8_t> payload;
  };
  std::deque<QueuedEvent> m_events;
  std::vector<std::uint8_t> m_currentPayload; // Backs the span handed to the caller.
};

} // namespace Neuron
