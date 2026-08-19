#pragma once

#include "OrderIntent.h"
#include "Transport.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

/*
 * The client's end of the wire (ADR-004).
 *
 * Owns the transport, drives the handshake, and keeps the round-trip figure the
 * HUD's NET readout wants. It is polled from the frame loop and never blocks:
 * a client that waits on the network has stopped drawing.
 */

namespace Neuron
{

enum class ClientLinkState : std::uint8_t
{
  Idle,
  Connecting, // Socket open, Hello sent, waiting for the answer.
  Joined,     // Welcome received: the session exists.
  Rejected,   // The server refused us, and said why.
  Disconnected
};

class ClientConnection
{
public:
  ClientConnection() = default;
  ~ClientConnection();

  ClientConnection(const ClientConnection&) = delete;
  ClientConnection& operator=(const ClientConnection&) = delete;

  /// Opens the socket and sends Hello. Returns false only if the socket itself
  /// could not be created -- a refusal arrives later, as an answer.
  [[nodiscard]] bool Connect(const std::string& _host, std::uint16_t _port, std::uint64_t _schemaHash, std::uint64_t _contentHash,
                             const std::string& _playerName);

  /// Services the transport and the ping cadence. Call once a frame.
  void Poll();

  void Disconnect();

  [[nodiscard]] ClientLinkState State() const noexcept
  {
    return m_state;
  }
  [[nodiscard]] std::uint32_t ClientId() const noexcept
  {
    return m_clientId;
  }
  [[nodiscard]] std::uint32_t ServerTick() const noexcept
  {
    return m_serverTick;
  }
  [[nodiscard]] std::uint16_t ServerTickRate() const noexcept
  {
    return m_serverTickRate;
  }
  [[nodiscard]] double RoundTripMs() const noexcept
  {
    return m_roundTripMs;
  }
  [[nodiscard]] std::uint64_t PongCount() const noexcept
  {
    return m_pongCount;
  }

  /// Where the server says its world is anchored (ADR-009 §8). Meaningful only
  /// once Joined; zero before that. In `mode: "client"` this is the only way
  /// the client learns it, since it shares no configuration with the server.
  [[nodiscard]] std::uint16_t WorldId() const noexcept
  {
    return m_worldId;
  }
  [[nodiscard]] std::int64_t AnchorX() const noexcept
  {
    return m_anchorX;
  }
  [[nodiscard]] std::int64_t AnchorY() const noexcept
  {
    return m_anchorY;
  }

  /*
   * The world's display strings from `Welcome`, verbatim and unread -- what the
   * top bar's location cluster and badge draw (`tactical-hud.png`). Empty until
   * Joined, and the HUD draws its no-session state rather than a blank slot;
   * they are replicated session fields, so killing the feed does not blank them
   * any more than it blanks `WorldId`.
   */
  [[nodiscard]] const std::string& WorldName() const noexcept { return m_worldName; }
  [[nodiscard]] const std::string& WorldDetail() const noexcept { return m_worldDetail; }
  [[nodiscard]] const std::string& WorldBadge() const noexcept { return m_worldBadge; }

  /*
   * Snapshot payloads that arrived since the last drain, oldest first.
   *
   * The connection does not look inside them -- it cannot, they are the game's
   * (ADR-014 §5) -- so it holds them until the frame loop hands each one to the
   * world view. A span rather than a callback keeps the connection free of any
   * opinion about who consumes them, and keeps the ordering visible at the one
   * place that matters.
   */
  [[nodiscard]] std::span<const std::vector<std::uint8_t>> PendingSnapshots() const noexcept { return m_pendingSnapshots; }
  void ClearPendingSnapshots() noexcept { m_pendingSnapshots.clear(); }

  /*
   * Sends one order, framed and reliable.
   *
   * `_payload` is the game's bytes and nothing else -- the type word is this
   * function's, exactly as the server's snapshot framing is `ServerHost`'s. The
   * connection does not look inside, and could not: an order is game semantics
   * (ADR-014 §5).
   *
   * The **control** channel, which is the one guarantee this message needs.
   * ADR-003 puts reliability there and a lost order is not self-correcting the
   * way a lost snapshot is: the next snapshot supersedes a missing one, and
   * nothing supersedes an order that never arrived. The player would see a
   * ghost that never promotes and a fleet that never moves.
   *
   * Returns false if the link is not joined or the order does not fit a
   * datagram, and the caller must then treat it as not sent -- there is no
   * ghost to leave on screen for something that never left.
   */
  [[nodiscard]] bool SendOrder(std::span<const std::uint8_t> _payload);

  /*
   * Acks that arrived since the last drain, oldest first.
   *
   * Unlike snapshots these are a NeuronCore struct rather than opaque bytes,
   * because every field is a number this library already defines -- a sequence
   * the client chose, an id the server assigned, and a reason code passed
   * through unread (ADR-004 §7). Drained rather than dispatched so the client
   * decides when its ghosts are allowed to change.
   */
  [[nodiscard]] std::span<const OrderVerdict> PendingVerdicts() const noexcept { return m_pendingVerdicts; }
  void ClearPendingVerdicts() noexcept { m_pendingVerdicts.clear(); }

  /// Orders put on the wire, and acks that came back. The two should track each
  /// other; a gap that does not close is the link, not the game.
  [[nodiscard]] std::uint64_t OrderSendCount() const noexcept { return m_orderSendCount; }
  [[nodiscard]] std::uint64_t OrderAckCount() const noexcept { return m_orderAckCount; }

  /// Snapshots seen and dropped for arriving faster than they are drained. The
  /// second should be zero; a rising number means the frame loop is starving.
  [[nodiscard]] std::uint64_t SnapshotCount() const noexcept { return m_snapshotCount; }
  [[nodiscard]] std::uint64_t SnapshotOverflowCount() const noexcept { return m_snapshotOverflowCount; }

  /// Datagram counters for the HUD's NET readout. Loss on the reliable channel
  /// shows up as controlResends; on the unreliable one it is the gap between
  /// pings sent and pongs counted.
  [[nodiscard]] TransportStats Stats() const;
  [[nodiscard]] std::uint64_t PingCount() const noexcept
  {
    return m_pingCount;
  }

private:
  void HandleMessage(const TransportEvent& _event);
  void SendHello();
  void SendPing();
  void LogNetStats();

  std::unique_ptr<Transport> m_transport;
  ConnectionId m_connection = INVALID_CONNECTION;
  ClientLinkState m_state = ClientLinkState::Idle;

  std::uint64_t m_schemaHash = 0;
  std::uint64_t m_contentHash = 0;
  std::string m_playerName;

  std::uint32_t m_clientId = 0;
  std::uint32_t m_serverTick = 0;
  std::uint16_t m_serverTickRate = 0;

  /// Bounded: a frame that fell far behind should drop the oldest snapshots
  /// rather than grow without limit, because full snapshots are idempotent and
  /// the newest is the only one that has to arrive.
  static constexpr std::size_t MAX_PENDING_SNAPSHOTS = 8;
  std::vector<std::vector<std::uint8_t>> m_pendingSnapshots;
  std::uint64_t m_snapshotCount = 0;
  std::uint64_t m_snapshotOverflowCount = 0;

  /// Unbounded in the way the snapshot queue deliberately is not: acks are
  /// reliable, arrive at the rate the player gives orders, and are drained
  /// every frame. There is no rate at which this grows that is not already a
  /// frame loop that has stopped running.
  std::vector<OrderVerdict> m_pendingVerdicts;
  std::uint64_t m_orderSendCount = 0;
  std::uint64_t m_orderAckCount = 0;
  std::uint16_t m_worldId = 0;
  std::int64_t m_anchorX = 0;
  std::int64_t m_anchorY = 0;
  std::string m_worldName;
  std::string m_worldDetail;
  std::string m_worldBadge;
  double m_roundTripMs = 0.0;
  std::uint64_t m_pingCount = 0;
  std::uint64_t m_pongCount = 0;
  std::int64_t m_lastPingCounter = 0;
  std::int64_t m_lastStatsCounter = 0;
  bool m_helloSent = false;
};

} // namespace Neuron
