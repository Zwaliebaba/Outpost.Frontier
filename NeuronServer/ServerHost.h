#pragma once

#include "ServerConfig.h"
#include "Simulation.h"
#include "SnapshotSender.h"
#include "Transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

/*
 * The authoritative host (ADR-008 §1).
 *
 * A self-contained service object: Start, Stop, Join. It owns the sim thread,
 * the listener and the session table, and it knows nothing about any client --
 * including the one in its own process. A disconnect returns it to an empty
 * server ticking along, never to a shutdown, because that posture is what the
 * multi-client future needs.
 *
 * This is the API a standalone OutpostServer.exe will use verbatim; headless
 * mode runs exactly this code path today so the claim stays true.
 */

namespace Neuron
{

/*
 * One connected client, and the feed that serves it.
 *
 * The sender is constructed with the session rather than attached to it, so
 * there is no window in which a session exists without the object that talks to
 * it (ADR-022 §1). It is a member and not a parallel vector for the reason the
 * fleet table gives for holding its names beside its hulls: two arrays that
 * have to be kept the same length eventually are not.
 */
struct SessionInfo
{
  SessionInfo(std::uint32_t _clientId, PlayerId _playerId, ConnectionId _connection, std::uint16_t _grid)
    : clientId(_clientId),
      playerId(_playerId),
      connection(_connection),
      sender(_playerId, _connection, _grid)
  {
  }

  /// The connection's id, minted when the socket opens and gone with it.
  std::uint32_t clientId = 0;

  /// Who is on the other end, across disconnects (ADR-018 D5). Retained rather
  /// than only sent, because replication keys on the player and not the socket.
  PlayerId playerId = INVALID_PLAYER_ID;

  ConnectionId connection = INVALID_CONNECTION;
  bool handshakeComplete = false;
  std::uint32_t lastPingTick = 0;

  SnapshotSender sender;
};

class ServerHost
{
public:
  ServerHost() = default;
  ~ServerHost();

  ServerHost(const ServerHost&) = delete;
  ServerHost& operator=(const ServerHost&) = delete;

  /// Binds and spawns the sim thread. Returns once the host is listening, so a
  /// caller may connect immediately afterwards.
  [[nodiscard]] bool Start(const ServerConfig& _config, Simulation& _simulation);

  /// Signals the sim thread to finish. Safe to call more than once.
  void Stop();

  /// Waits for the sim thread to exit. Stop() first, or this never returns.
  void Join();

  [[nodiscard]] bool Running() const noexcept
  {
    return m_running.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint16_t BoundPort() const noexcept
  {
    return m_boundPort;
  }

  // Read from any thread: these are counters for logging and the debug HUD.
  [[nodiscard]] std::uint32_t TickCount() const noexcept
  {
    return m_tick.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint32_t OverrunCount() const noexcept
  {
    return m_overruns.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint32_t SessionCount() const noexcept
  {
    return m_sessionCount.load(std::memory_order_relaxed);
  }
  /// Snapshots that could not be sent, counted per client and per tick: with
  /// one feed per session (A13) a tick that fails for two viewers is two.
  /// Non-zero means somebody has seen the world stop moving, so it belongs on
  /// the HUD next to the others rather than only in the log line that fires
  /// once. *Which* viewer is the sender's own `OverCapCount`; this is the
  /// session-wide number the strip reads.
  [[nodiscard]] std::uint32_t SnapshotFailureCount() const noexcept
  {
    return m_snapshotFailures.load(std::memory_order_relaxed);
  }
  /// Orders the simulation refused. A number that only ever climbs on one
  /// client is a client and a server that disagree about the world, which is
  /// the interesting failure ADR-014 §3's parity rule exists to make loud.
  [[nodiscard]] std::uint32_t RefusedOrderCount() const noexcept
  {
    return m_ordersRefused.load(std::memory_order_relaxed);
  }

private:
  void SimThread();
  void PollTransport();
  void LogNetStats();
  void HandleMessage(const TransportEvent& _event);
  void SendTo(ConnectionId _connection, TransportChannel _channel, const class ByteWriter& _writer);

  /*
   * Gives every joined session its own snapshot, through its own sender.
   *
   * One serialisation *per client*, not one for all of them (ADR-018 A13,
   * ADR-022 §1). The bytes are identical today, so this costs a serialisation
   * per client and buys nothing yet; what it buys is that the day a client is
   * owed a different world -- a different grid, a culled set, a delta against
   * what *it* last acked -- the shape is already right and only the sender's
   * insides change.
   */
  void SendSnapshots(std::uint32_t _tick);
  [[nodiscard]] SessionInfo* FindSession(ConnectionId _connection);

  ServerConfig m_config;
  Simulation* m_simulation = nullptr;
  std::unique_ptr<Transport> m_transport;
  std::vector<SessionInfo> m_sessions;
  std::uint32_t m_nextClientId = 1;
  std::int64_t m_lastStatsCounter = 0; // Sim thread only; nothing else reads it.

  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stopRequested{false};
  std::atomic<std::uint32_t> m_tick{0};
  std::atomic<std::uint32_t> m_overruns{0};
  std::atomic<std::uint32_t> m_sessionCount{0};
  std::atomic<std::uint32_t> m_snapshotFailures{0};
  std::atomic<std::uint32_t> m_ordersRefused{0};
  std::uint16_t m_boundPort = 0;
};

} // namespace Neuron
