#pragma once

#include "DeltaReceiver.h"
#include "EntityRecord.h"
#include "OrderIntent.h"
#include "Transport.h"
#include "Wire.h"

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
  /// The optional pair reclaims a session the server has not yet expired
  /// (ADR-018 D5): the `PlayerId` and token a previous `Welcome` carried.
  /// Omitted, this is a first connection and the server mints a new commander.
  [[nodiscard]] bool Connect(const std::string& _host, std::uint16_t _port, std::uint64_t _schemaHash, std::uint64_t _contentHash,
                             const std::string& _playerName, PlayerId _resumePlayer = INVALID_PLAYER_ID,
                             std::uint64_t _resumeToken = 0);

  /// The token to offer back on a reconnect. Zero until a `Welcome` arrives.
  [[nodiscard]] std::uint64_t ResumeToken() const noexcept
  {
    return m_resumeToken;
  }

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

  /// Who the server says this is (ADR-018 D5). Distinct from `ClientId`, which
  /// names the *connection* -- everything player-keyed keys on this one, so
  /// that it survives the connection dropping.
  [[nodiscard]] PlayerId Player() const noexcept
  {
    return m_playerId;
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

  /// Which anchor this grid stands on. `WorldId()` describes where the grid is;
  /// this is the number a Dock or a station command names (ADR-017 §2, §3).
  [[nodiscard]] std::uint16_t GridAnchor() const noexcept
  {
    return m_gridAnchor;
  }

  /// Asks to watch another grid (ADR-016 §4). Reliable, because a view switch
  /// is something the player did once: a dropped request leaves them watching
  /// the world they asked to leave with nothing saying why. The answer arrives
  /// as a `ViewChanged`, accepted or not.
  [[nodiscard]] bool RequestView(std::uint16_t _gridAnchor);

  /// View answers since the last drain, oldest first. Held rather than applied,
  /// because what a refusal *means* on screen is the game's business and this
  /// library has no opinion about it.
  [[nodiscard]] std::span<const ViewChanged> PendingViewChanges() const noexcept { return m_pendingViewChanges; }
  void ClearPendingViewChanges() noexcept { m_pendingViewChanges.clear(); }
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
   * One assembled tick, waiting for the frame loop.
   *
   * **The connection reads the header now, and that is a change** (ADR-022).
   * It used to hold the whole snapshot payload unread, because the payload was
   * the game's from its first byte. Interest and delta made the envelope the
   * *engine's*: the delta header, the entity records and the baseline ring are
   * decisions about a viewer and a link (§1), and `EntityRecord` is NeuronCore's
   * type. What stays opaque is `tail` -- the game's own bytes for the tick,
   * carried and never parsed, exactly as a summary is.
   *
   * Owned storage rather than spans into the receiver: the frame loop drains
   * this on its own schedule, and a span into a ring that has moved on since is
   * the class of bug that only shows up under load.
   */
  struct ReceivedFrame
  {
    std::uint32_t tick = 0;
    std::uint16_t gridId = 0;
    std::uint16_t culledCount = 0;
    std::vector<EntityRecord> entities;
    std::vector<std::uint8_t> tail;
  };

  /*
   * Assembled ticks that arrived since the last drain, oldest first.
   *
   * A span rather than a callback keeps the connection free of any opinion
   * about who consumes them, and keeps the ordering visible at the one place
   * that matters.
   */
  [[nodiscard]] std::span<const ReceivedFrame> PendingFrames() const noexcept { return m_pendingFrames; }
  void ClearPendingFrames() noexcept { m_pendingFrames.clear(); }

  /// Delta parts that named a baseline this client does not hold (ADR-022 §2b).
  /// Zero in steady state; a number that climbs means acks are not arriving and
  /// the server should be answering with keyframes.
  [[nodiscard]] std::uint32_t OrphanedPartCount() const noexcept { return m_delta.OrphanedPartCount(); }

  /// Keyframes taken. One per join and per view switch; more than that means
  /// the link is losing acks (ADR-022 §2c).
  [[nodiscard]] std::uint32_t KeyframeCount() const noexcept { return m_delta.KeyframeCount(); }

  /*
   * Tells the server where this viewer is looking and what they have selected
   * (ADR-022 §4).
   *
   * ADR-016 §7 said the server had no business holding this; ADR-022 §1 is what
   * changed it, because relevance is a property of a viewer and §5a's guarantee
   * cannot be kept for a selection nobody told the server about.
   *
   * Unreliable, and sent when the client decides it has changed. A lost one
   * costs one tick ranked against a slightly stale camera, which is a worse
   * ordering and never a wrong one -- so paying for reliability here would buy
   * nothing and queue behind the player's orders.
   */
  [[nodiscard]] bool SendViewFocus(const ViewFocus& _focus);

  /*
   * Summary payloads, held the same way and looked into just as little.
   *
   * A separate queue rather than a shared one, because the two feeds have
   * different cadences and different lifetimes: a snapshot is superseded by the
   * next tick's and the freshest is the only one worth keeping, while a summary
   * arrives about once a second and describes places the client is *not*
   * watching. Dropping the oldest is right for one and wrong for the other, so
   * they do not share a policy.
   */
  [[nodiscard]] std::span<const std::vector<std::uint8_t>> PendingSummaries() const noexcept { return m_pendingSummaries; }
  void ClearPendingSummaries() noexcept { m_pendingSummaries.clear(); }
  [[nodiscard]] std::uint64_t SummaryCount() const noexcept { return m_summaryCount; }

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

  /// Learned from the `Welcome`, and offered back on a resume when resumes
  /// exist. Kept beside `m_clientId` rather than instead of it, because they
  /// answer different questions.
  PlayerId m_playerId = INVALID_PLAYER_ID;

  /// What this client offers back, and what it was last given. Separate from
  /// `m_playerId` because the id is what the server SAYS we are and these are
  /// what we CLAIM -- conflating them would make a failed resume look like a
  /// successful one.
  PlayerId m_resumePlayer = INVALID_PLAYER_ID;
  std::uint64_t m_resumeToken = 0;
  std::uint32_t m_serverTick = 0;
  std::uint16_t m_serverTickRate = 0;

  /*
   * Bounded: a frame loop that fell far behind should drop the oldest ticks
   * rather than grow without limit.
   *
   * **Still safe to drop under deltas, and the reason has changed.** A full
   * snapshot was idempotent, so the newest was the only one that had to arrive.
   * An assembled frame is idempotent for the same *outcome* but a different
   * cause: `DeltaReceiver` keeps the picture, so a frame dropped here is a tick
   * the game never draws, not a tick the client forgets. The baseline and the
   * ack are untouched by anything this queue does.
   */
  static constexpr std::size_t MAX_PENDING_SNAPSHOTS = 8;
  std::vector<ReceivedFrame> m_pendingFrames;

  /// Reassembly, the baseline ring and the whole-tick ack rule (ADR-022 §2).
  DeltaReceiver m_delta;

  /// Summaries waiting to be read. Capped like the snapshots, and for the one
  /// reason that is the same: a queue nobody drains must not grow without
  /// bound. The oldest goes, because a newer roster for a station describes it
  /// better than an older one does.
  static constexpr std::size_t MAX_PENDING_SUMMARIES = 8;
  std::vector<std::vector<std::uint8_t>> m_pendingSummaries;
  std::uint64_t m_summaryCount = 0;
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
  std::uint16_t m_gridAnchor = 0;
  std::vector<ViewChanged> m_pendingViewChanges;
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
