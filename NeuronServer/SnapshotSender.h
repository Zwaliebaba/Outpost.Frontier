#pragma once

#include "Transport.h"
#include "Wire.h"

#include <array>
#include <cstdint>

/*
 * One client's state feed (ADR-018 A13, ADR-022 §1).
 *
 * The rule this file exists to obey is a single sentence from ADR-022 §1: *the
 * object that serialises is the object that knows a client.* There is
 * deliberately no "broadcast the bytes we already made" path to grow out of
 * later, because every replication decision after this one -- interest,
 * deltas, keyframes, per-grid views, per-viewer rosters -- is a decision about
 * a *viewer*, and a sender that had to be retrofitted with one would have to
 * be retrofitted at every one of those call sites at once.
 *
 * Today the bytes are the same for every client, so this buys nothing at
 * runtime and the cost is one serialisation per client instead of one per
 * tick. That trade is the whole point: it is paid now, while there is one
 * client and the price is zero, rather than on the day two clients must be
 * shown different worlds.
 *
 * What is per client and lives here: the buffer, the counters, and the viewer
 * this feed serves. What ADR-022 will add here: the ring of views as sent
 * (§2b), the last acked tick, and the keyframe path (§3).
 *
 * Sim thread only. A sender is touched from the tick loop and from nowhere
 * else, which is what lets the buffer be a member rather than a stack array
 * (ADR-007 §2).
 */

namespace Neuron
{

class Simulation;

/*
 * How often a viewer's summary frame goes out (ADR-016 §6, ADR-017 §1: "~1 Hz").
 *
 * Twenty ticks at ADR-002's fixed 20 Hz. The number is the engine's rather than
 * the game's because cadence is a link decision and not a game rule -- the same
 * split ADR-022 §4 draws between ranking and truncating.
 */
inline constexpr std::uint32_t SUMMARY_INTERVAL_TICKS = 20;

class SnapshotSender
{
public:
  SnapshotSender() = default;
  SnapshotSender(PlayerId _viewer, ConnectionId _connection, std::uint16_t _grid) noexcept
    : m_viewer(_viewer),
      m_connection(_connection),
      m_grid(_grid)
  {
  }

  /*
   * Serialises this viewer's snapshot and sends it.
   *
   * Returns false when the simulation would not write one, which at today's
   * scale means this viewer's grid outgrew a datagram -- the loud refusal
   * ADR-022 §6 keeps in place until the delta slice replaces it with priority
   * truncation. Nothing is sent in that case: a truncated snapshot is worse
   * than a missing one, because the client reads the absent ships as despawned
   * and resurrects them on the next tick.
   *
   * The refusal is counted here rather than only at the host, because "which
   * client is over cap" is a question a single global counter cannot answer
   * and a per-grid, per-viewer world is exactly where it gets asked.
   */
  [[nodiscard]] bool Send(Simulation& _simulation, Transport& _transport, std::uint32_t _tick);

  /// Summary frames actually put on the wire for this viewer.
  [[nodiscard]] std::uint32_t SummariesSent() const noexcept
  {
    return m_summariesSent;
  }

  /*
   * Points this feed at another grid, if the game allows it (ADR-016 §7).
   *
   * The verdict comes from the simulation and the enforcement stays here, which
   * is the seam's usual division: whether a view is legal is a fact about where
   * a commander's ships are, and the engine may not know that.
   *
   * A refused request leaves the feed exactly where it was. That is the honest
   * outcome and it is why `ViewChanged` echoes the grid: a client with two
   * requests in flight learns which one was turned down, and does not have to
   * infer its own view state from the next snapshot to arrive.
   */
  [[nodiscard]] bool RequestView(Simulation& _simulation, std::uint16_t _grid, std::uint16_t& _outReasonCode);

  /// Which grid this viewer is watching. The snapshot's header carries the same
  /// number, which is what lets the client tell a switch from a new frame.
  [[nodiscard]] std::uint16_t Grid() const noexcept
  {
    return m_grid;
  }

  /// Who this feed serves (ADR-018 D5). The durable player, never the
  /// connection: everything replication keys on outlives a socket.
  [[nodiscard]] PlayerId Viewer() const noexcept
  {
    return m_viewer;
  }
  [[nodiscard]] ConnectionId Connection() const noexcept
  {
    return m_connection;
  }

  [[nodiscard]] std::uint32_t SentCount() const noexcept
  {
    return m_sent;
  }
  /// Ticks this viewer was owed a snapshot and did not get one.
  [[nodiscard]] std::uint32_t OverCapCount() const noexcept
  {
    return m_overCap;
  }

private:
  /*
   * Sends this viewer's summary frame when one is due (ADR-016 §6).
   *
   * Staggered by the viewer's own id rather than fired for everyone on the same
   * tick. At one commander that is indistinguishable; at the shard ADR-018 D1
   * targets it is the difference between a flat trickle and a spike once a
   * second in which every session serialises at once, and it costs a modulo.
   *
   * Unreliable, like the snapshot and for the opposite of ADR-022 §3c's reason:
   * a keyframe takes a reliable stream because everything after it is a delta
   * against it, while a lost summary costs a second of staleness on a screen
   * that is about to be told again. Putting it on `Control` would park a roster
   * in front of the player's orders.
   */
  void SendSummaries(Simulation& _simulation, Transport& _transport, std::uint32_t _tick);

  PlayerId m_viewer = INVALID_PLAYER_ID;
  ConnectionId m_connection = INVALID_CONNECTION;

  /// The grid this viewer watches. Session state and nowhere else: the sim tier
  /// has no viewers (ADR-022 §1), so nothing about a camera reaches `World`.
  std::uint16_t m_grid = 0;

  std::uint32_t m_sent = 0;
  std::uint32_t m_summariesSent = 0;
  std::uint32_t m_overCap = 0;

  /// Logged once per client rather than once per process: with two viewers,
  /// "a snapshot did not fit" that names neither is a line that sends the next
  /// person to the wrong grid.
  bool m_overCapLogged = false;

  /// This client's own scratch. Not shared, and not a stack array: the day a
  /// sender retains the view it sent (ADR-022 §2b) this is where that lives.
  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> m_buffer{};
};

} // namespace Neuron
