#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

/*
 * The transport seam (ADR-003).
 *
 * QUIC-shaped on purpose, even though the MVP implementation is UDP: one
 * reliable ordered control channel, unreliable datagrams for state, explicit
 * connection lifecycle, and delivery only when the owning thread polls. Writing
 * the interface around UDP's shape and "fixing it later" is how an abstraction
 * ends up validated against exactly one implementation, so the loopback build
 * is treated as a network from the first line: it can lose, it can reorder, it
 * has an MTU.
 *
 * Threading (ADR-007): implementations may receive on their own threads, but
 * events surface only inside Poll(), on the thread that owns the transport.
 * Send is for the owning thread only.
 */

namespace Neuron
{

/// The whole datagram, header included. Nothing may depend on loopback being
/// willing to carry more than a real link would.
inline constexpr std::size_t MAX_DATAGRAM_BYTES = 1152;

enum class TransportChannel : std::uint8_t
{
  Control, // Reliable, ordered. Handshake and orders.
  State    // Unreliable, unordered. Snapshots and pings.
};

enum class ConnectionState : std::uint8_t
{
  Connecting,
  Connected,
  Draining,
  Closed
};

/// Why a connection ended. Carried on the wire, so the values are fixed.
enum class DisconnectReason : std::uint16_t
{
  None = 0,
  ClosedByPeer = 1,
  TimedOut = 2,
  Refused = 3,
  ProtocolError = 4,
  ShuttingDown = 5
};

using ConnectionId = std::uint32_t;
inline constexpr ConnectionId INVALID_CONNECTION = 0;

struct TransportStats
{
  double roundTripMs = 0.0;
  /// The floor: the fastest round trip observed. The smoothed figure above
  /// includes the peer's deliberate ack batching, so this is the honest
  /// measure of what the transport itself adds to the path.
  double minRoundTripMs = 0.0;
  std::uint64_t datagramsSent = 0;
  std::uint64_t datagramsReceived = 0;
  std::uint64_t bytesSent = 0;
  std::uint64_t bytesReceived = 0;
  std::uint64_t controlResends = 0;   // Loss on the reliable channel, made visible.
  std::uint64_t datagramsDropped = 0; // Oversized or malformed, counted rather than hidden.
};

struct TransportEvent
{
  enum class Type : std::uint8_t
  {
    None,
    Connected,
    Disconnected,
    Message
  };

  Type type = Type::None;
  ConnectionId connection = INVALID_CONNECTION;
  TransportChannel channel = TransportChannel::Control;
  DisconnectReason reason = DisconnectReason::None;

  /// Valid until the next Poll() -- copy anything that must outlive the event.
  std::span<const std::uint8_t> payload;
};

class Transport
{
public:
  virtual ~Transport() = default;

  /// Server side. Port 0 binds an ephemeral port; BoundPort() then reports it.
  [[nodiscard]] virtual bool Listen(std::uint16_t _port) = 0;

  /// Client side. Returns once the handshake is under way, not once it succeeds:
  /// the Connected event is what says the peer answered.
  [[nodiscard]] virtual ConnectionId Connect(const std::string& _host, std::uint16_t _port) = 0;

  /// Services sockets and timers. Must be called by the owning thread; nothing
  /// is delivered without it.
  virtual void Poll() = 0;

  /// Drains one queued event. Returns false when the queue is empty.
  [[nodiscard]] virtual bool NextEvent(TransportEvent& _outEvent) = 0;

  [[nodiscard]] virtual bool Send(ConnectionId _connection, TransportChannel _channel, std::span<const std::uint8_t> _payload) = 0;

  virtual void Close(ConnectionId _connection, DisconnectReason _reason) = 0;
  virtual void Shutdown() = 0;

  [[nodiscard]] virtual ConnectionState State(ConnectionId _connection) const = 0;
  [[nodiscard]] virtual TransportStats Stats(ConnectionId _connection) const = 0;
  [[nodiscard]] virtual std::uint16_t BoundPort() const = 0;
};

} // namespace Neuron
