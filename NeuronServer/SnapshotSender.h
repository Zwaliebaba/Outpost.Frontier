#pragma once

#include "Simulation.h"
#include "Transport.h"
#include "Wire.h"

#include <array>
#include <cstdint>

/*
 * One client's state stream (ADR-022 §1, ADR-018 A13).
 *
 * **There is one of these per client, from its first line.** That is the whole
 * reason the object exists, and it is a rule rather than a preference: ADR-022
 * puts interest and delta in the session role because *relevance is a property
 * of a viewer, and the sim tier has no viewers*. A sender that serialised once
 * and sent the same bytes to everyone would be exactly the shape that decision
 * forbids -- and it would be load-bearing by the time the delta slice arrived,
 * because "broadcast the bytes we already made" is not a path that grows into
 * "encode against what *this* client last acked". So the object that serialises
 * is the object that knows a client, starting now, while there is only ever one
 * of them and the shape costs nothing to hold.
 *
 * What that buys before any of ADR-022 is built:
 *
 * - **`StationRoster` has somewhere correct to live.** ADR-017 §1 makes a
 *   roster private to its owner. On a broadcast-shaped sender that promise is a
 *   silent leak nothing catches, because nothing on the roadmap before U3c runs
 *   two clients (ADR-022 §1). Addressed per viewer, it is a wire fact from the
 *   first message instead of a test nobody can write yet.
 * - **The over-cap refusal is a per-client event.** It is counted and logged
 *   naming *which* client saw the world stop moving -- the designed behaviour
 *   until ADR-022 §6 replaces refusal with priority truncation.
 *
 * What it deliberately does **not** do yet: rank, cull, or delta-encode.
 * `Simulation::WriteSnapshot` still writes the whole grid, the viewer is passed
 * and not yet read, and `baselineTick` is still zero on the wire. The retained
 * per-client baseline ring of ADR-022 §2b belongs in this object when that
 * slice lands; until then this is the same full snapshot it always was, sent
 * from a place that can grow one.
 *
 * Threading: owned by the sim thread with the session it belongs to, and
 * touched by nothing else. The counters are plain integers for that reason.
 */

namespace Neuron
{

class ByteWriter;

/// What this client's tick came to. `Refused` means the simulation could not
/// fit the snapshot, which is a client seeing nothing move rather than a client
/// seeing a stale frame -- worth distinguishing at the call site.
enum class SnapshotSendResult : std::uint8_t
{
  Sent,
  Refused,
};

class SnapshotSender
{
public:
  SnapshotSender() = default;
  SnapshotSender(ConnectionId _connection, std::uint32_t _clientId, PlayerId _viewer) noexcept
    : m_connection(_connection), m_clientId(_clientId), m_viewer(_viewer)
  {
  }

  /*
   * Serialises this tick for this viewer and sends it.
   *
   * Unreliable and unordered on purpose: full snapshots are idempotent, so a
   * lost one costs a tick of freshness and a resent one would arrive after the
   * snapshot that superseded it (ADR-004 §6).
   */
  SnapshotSendResult Send(Simulation& _simulation, Transport& _transport, std::uint32_t _tick);

  [[nodiscard]] ConnectionId Connection() const noexcept { return m_connection; }
  [[nodiscard]] PlayerId Viewer() const noexcept { return m_viewer; }

  /// Snapshots this client received, and ticks it did not. The pair is per
  /// client because the failure is: one viewer over the cap does not stop the
  /// world for the rest of them.
  [[nodiscard]] std::uint32_t SentCount() const noexcept { return m_sent; }
  [[nodiscard]] std::uint32_t RefusedCount() const noexcept { return m_refused; }

private:
  ConnectionId m_connection = INVALID_CONNECTION;
  std::uint32_t m_clientId = 0;
  PlayerId m_viewer = INVALID_PLAYER_ID;

  std::uint32_t m_sent = 0;
  std::uint32_t m_refused = 0;

  /*
   * The buffer is a member rather than a local, and that is not about the
   * stack. A snapshot is *this client's* bytes now, and ADR-022 §2b's baseline
   * ring -- the views this sender actually transmitted -- is the state that
   * makes delta-under-culling safe. It hangs off this object when it arrives;
   * the buffer is the first tenant.
   */
  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> m_buffer{};
};

} // namespace Neuron
