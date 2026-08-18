#pragma once

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
  std::uint16_t m_worldId = 0;
  std::int64_t m_anchorX = 0;
  std::int64_t m_anchorY = 0;
  double m_roundTripMs = 0.0;
  std::uint64_t m_pingCount = 0;
  std::uint64_t m_pongCount = 0;
  std::int64_t m_lastPingCounter = 0;
  std::int64_t m_lastStatsCounter = 0;
  bool m_helloSent = false;
};

} // namespace Neuron
