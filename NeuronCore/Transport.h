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

/*
 * The ceiling on one `Bulk` message (ADR-022 §3c).
 *
 * `Bulk` is the one channel a datagram-sized cap would be wrong for: a keyframe
 * is a baseline rather than fresh state, it must arrive whole, and at ADR-018
 * D4's 1,024-entity cap it is roughly 24 KB. Fragmenting it over datagrams with
 * an ack-and-resend scheme is re-implementing a reliable stream beside a
 * protocol that has one, which ADR-003 rejected in its general form.
 *
 * 65,535 because that is what the two-byte length prefix on a reliable stream
 * can express, so this is the framing's own ceiling rather than a number
 * somebody chose. The keyframe at the cap sits comfortably under it; a message
 * that did not would need a wider prefix, which is a wire break and should be
 * one.
 */
inline constexpr std::size_t MAX_BULK_BYTES = 65535;

enum class TransportChannel : std::uint8_t
{
  Control, // Reliable, ordered. Handshake and orders.
  State,   // Unreliable, unordered. Snapshots and pings.

  /*
   * Reliable, ordered, and separate from `Control` (ADR-022 §3c, amending
   * ADR-003 §1, which promised exactly one reliable ordered channel).
   *
   * **The reason is head-of-line blocking, and it cuts both ways.** ADR-004
   * rejected reliable snapshots because a hitch would stall fresh state behind
   * a resend, and that argument still holds for the per-tick path -- which is
   * why deltas stay on `State`. But a keyframe is not fresh state: it is the
   * *baseline* for all the fresh state after it, it must arrive intact, and it
   * is not a datagram-shaped object. Putting it on `Control` would park it in
   * front of the player's orders.
   *
   * QUIC gives independent streams for nothing, so the correct answer is a
   * stream of its own and the cost of deciding it is one enumerator. Carries up
   * to `MAX_BULK_BYTES` rather than `MAX_DATAGRAM_BYTES`.
   */
  Bulk
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

  /// Sends one whole message. `Control` and `State` are capped at
  /// `MAX_DATAGRAM_BYTES`; `Bulk` at `MAX_BULK_BYTES` (ADR-022 §3c). A payload
  /// past its channel's cap is refused rather than split -- fragmentation
  /// policy belongs to whoever knows what the bytes mean.
  [[nodiscard]] virtual bool Send(ConnectionId _connection, TransportChannel _channel, std::span<const std::uint8_t> _payload) = 0;

  virtual void Close(ConnectionId _connection, DisconnectReason _reason) = 0;
  virtual void Shutdown() = 0;

  [[nodiscard]] virtual ConnectionState State(ConnectionId _connection) const = 0;
  [[nodiscard]] virtual TransportStats Stats(ConnectionId _connection) const = 0;
  [[nodiscard]] virtual std::uint16_t BoundPort() const = 0;
};

} // namespace Neuron
