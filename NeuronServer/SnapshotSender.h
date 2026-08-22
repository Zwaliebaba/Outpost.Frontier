#pragma once

#include "EntityRecord.h"
#include "Transport.h"
#include "Wire.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

/*
 * One client's state feed (ADR-018 A13, ADR-022).
 *
 * The rule this file exists to obey is a single sentence from ADR-022 §1: *the
 * object that serialises is the object that knows a client.* There is
 * deliberately no "broadcast the bytes we already made" path, because every
 * replication decision here is a decision about a *viewer*: which grid they are
 * watching, where their camera is, what they have selected, what they last
 * acked, and what their link can afford this tick.
 *
 * **U3d-b turned that shape into the mechanism it was shaped for.** What this
 * object now does, per viewer, per tick:
 *
 *   1. asks the game to **rank** the grid (`RankRelevance`, §4), which never
 *      truncates and returns how many of the front are guaranteed;
 *   2. **truncates to a byte budget**, never inside the guaranteed prefix
 *      (§5c) -- when that prefix alone exceeds the budget, the budget loses and
 *      the overrun is counted;
 *   3. asks the game to fill records for exactly those entities (§8);
 *   4. **delta-encodes against the view it last sent** and the client acked
 *      (§2b) -- not against the world, which is the subtlety the whole design
 *      turns on;
 *   5. packs the result into as many datagrams as it takes (§3b), each
 *      independently applicable;
 *   6. or, when the acked tick has fallen out of the ring, sends a **keyframe**
 *      on the `Bulk` stream and starts again (§2c).
 *
 * Sim thread only. A sender is touched from the tick loop and from nowhere
 * else, which is what lets the buffers be members rather than stack arrays
 * (ADR-007 §2) -- and it is what makes the ring of sent views affordable.
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

/*
 * How many bytes of entity records one viewer's tick may cost (ADR-022 §5b).
 *
 * **A bandwidth figure, not a datagram size.** The 1,152-byte datagram cap never
 * moves; what moves is how many of them a tick may use. That distinction is what
 * let `EntityRecord::id` widen to u32 -- the record no longer has to fit a
 * "everything in one packet" constraint that never really described the game.
 *
 * 8,192 bytes is 356 records a tick, and 1.31 Mbps at 20 Hz. The numbers it sits
 * between are both on the record: ADR-018 D15.6's envelope is ~200 owned ships,
 * which is ~4.6 KB and must never be culled (§5c), and D4's 1,024-entity cap
 * uncompressed is ~23.5 KB/tick ≈ 3.8 Mbps, which is the figure interest exists
 * to avoid paying. This is comfortably above the first and well under the
 * second, which is what §5b asks for.
 *
 * A default rather than a constant carved into the sender: §5b calls sizing a
 * deployment number, so `ServerConfig` carries it and this is what a config that
 * says nothing gets.
 */
inline constexpr std::uint32_t DEFAULT_TICK_BUDGET_BYTES = 8192;

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
   * Serialises this viewer's update for the tick and sends it.
   *
   * Returns false only when there was nothing to send at all -- a grid the
   * simulation would not rank. **It no longer returns false because a grid grew
   * too big**, which is the whole of ADR-022 §6: a grid over budget produces a
   * *partial* view with an honest `culledCount`, never silence. The refusal
   * that used to live here (and the "this client will see nothing move" line
   * beside it) was the correct behaviour while a full snapshot in one datagram
   * was the only format; it is not the only format any more.
   */
  [[nodiscard]] bool Send(Simulation& _simulation, Transport& _transport, std::uint32_t _tick);

  /// Summary frames actually put on the wire for this viewer.
  [[nodiscard]] std::uint32_t SummariesSent() const noexcept
  {
    return m_summariesSent;
  }

  /*
   * One of this viewer's orders was accepted by the authority (ADR-022 §7).
   *
   * The session's half of the order feedback loop, and it lives here because
   * ADR-022 §7 moved it out of the world: `lastOrderSeqProcessed` was
   * world-global, written as a max across *every* submitter and folded into the
   * world hash, so one commander's order sequence perturbed the other's ghost
   * promotion and a replay's hash depended on which client happened to submit.
   * Per viewer is what it always read as, and this is the object that is one
   * per viewer.
   *
   * **Accepted only.** A refusal carries a sequence too, and advancing on one
   * would promote a ghost for an order the authority turned down -- the exact
   * opposite of what the field is for. Highest wins, so an ack arriving out of
   * order cannot walk the number backwards.
   */
  void NoteOrderAccepted(std::uint32_t _orderSeq) noexcept
  {
    m_lastOrderSeqProcessed = std::max(m_lastOrderSeqProcessed, _orderSeq);
  }

  /// The highest sequence this viewer has had accepted. On the wire in the
  /// game's own tick tail, which is where it always was (ADR-022 §7).
  [[nodiscard]] std::uint32_t LastOrderSeqProcessed() const noexcept
  {
    return m_lastOrderSeqProcessed;
  }

  /*
   * The client says it holds the whole of a tick (ADR-022 §2a).
   *
   * **Highest wins and anything older is ignored**, which is what makes
   * reordering a non-event on the unreliable channel this rides. An ack naming
   * a grid this feed is not watching is dropped: a view switch invalidates
   * every baseline, and an ack in flight across one would otherwise resurrect a
   * picture of the world the viewer has left.
   */
  void NoteAck(const SnapshotAck& _ack) noexcept;

  /*
   * Where the viewer is looking and what they have selected (ADR-022 §4).
   *
   * Held so the next `RankRelevance` can be asked the question §4 actually
   * poses. A focus naming another grid is kept anyway rather than dropped --
   * the client may be describing the grid it is switching to -- but it is only
   * *used* while it agrees with the feed's grid, so a stale camera can never
   * rank the wrong world.
   */
  void NoteFocus(const ViewFocus& _focus);

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
   *
   * **An accepted one drops every baseline**, because a switch is a mid-session
   * join (ADR-022 §3a): the two grids share an id space without sharing a
   * single ship, so a delta against the old world would describe changes to
   * hulls that are not there.
   */
  [[nodiscard]] bool RequestView(Simulation& _simulation, std::uint16_t _grid, std::uint16_t& _outReasonCode);

  /// Which grid this viewer is watching. Every header carries the same number,
  /// which is what lets the client tell a switch from a new frame.
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

  /// This viewer's byte budget per tick (ADR-022 §5b). A deployment number, so
  /// the host sets it from config rather than the sender choosing.
  void SetTickBudgetBytes(std::uint32_t _bytes) noexcept
  {
    m_tickBudgetBytes = _bytes;
  }

  [[nodiscard]] std::uint32_t SentCount() const noexcept
  {
    return m_sent;
  }

  /// Keyframes sent (ADR-022 §3). One per join, per view switch, and per time
  /// the client's acked tick fell out of the ring -- so a number that climbs
  /// steadily in steady state is a link that is not keeping up.
  [[nodiscard]] std::uint32_t KeyframesSent() const noexcept
  {
    return m_keyframesSent;
  }

  /*
   * Ticks on which the guaranteed prefix alone exceeded the budget (§5c).
   *
   * Counted rather than acted on, and beside `tickOverrun` in spirit: the
   * owned ships went out regardless, because culling a player's own fleet to
   * hit a bandwidth number is the one outcome ADR-022 is written to prevent.
   * A number that is not zero is a deployment whose budget is too small for its
   * fleet envelope, which is a decision for a person.
   */
  [[nodiscard]] std::uint32_t InterestOverrunCount() const noexcept
  {
    return m_interestOverrun;
  }

  /// What the last update told the client it was not being shown (§5d).
  [[nodiscard]] std::uint16_t LastCulledCount() const noexcept
  {
    return m_lastCulledCount;
  }

  /// How many entities the client is currently holding for this grid -- the
  /// size of the view as sent. The half of `culledCount` that is not on the
  /// wire, and what a test asserts the guarantee against.
  [[nodiscard]] std::size_t ViewSize() const noexcept
  {
    return m_picture.size();
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
   * Unreliable, like the delta and for the opposite of ADR-022 §3c's reason: a
   * keyframe takes a reliable stream because everything after it is a delta
   * against it, while a lost summary costs a second of staleness on a screen
   * that is about to be told again.
   */
  void SendSummaries(Simulation& _simulation, Transport& _transport, std::uint32_t _tick);

  /// Everything a view switch invalidates: the ring, the ack, and the picture.
  void DropBaselines() noexcept;

  /// The ring slot a tick would occupy, and whether it holds that tick.
  [[nodiscard]] const std::vector<EntityRecord>* BaselineAt(std::uint32_t _tick) const noexcept;

  /// Ranks, truncates and asks the game for records. Leaves the chosen ids in
  /// `m_ranked` (whole grid) and the records in `m_records`.
  void BuildInterest(Simulation& _simulation, std::uint32_t _tick);

  void SendKeyframe(Simulation& _simulation, Transport& _transport, std::uint32_t _tick, std::span<const std::uint8_t> _tail);
  void SendDelta(Transport& _transport, std::uint32_t _tick, std::uint32_t _baselineTick, std::span<const std::uint8_t> _tail);

  PlayerId m_viewer = INVALID_PLAYER_ID;
  ConnectionId m_connection = INVALID_CONNECTION;

  /// The grid this viewer watches. Session state and nowhere else: the sim tier
  /// has no viewers (ADR-022 §1), so nothing about a camera reaches `World`.
  std::uint16_t m_grid = 0;

  std::uint32_t m_sent = 0;
  std::uint32_t m_summariesSent = 0;
  std::uint32_t m_keyframesSent = 0;
  std::uint32_t m_interestOverrun = 0;
  std::uint16_t m_lastCulledCount = 0;

  /// Session state, not world state (ADR-022 §7). It survives a view switch --
  /// a fleet ordered on one grid is still ordered when the camera moves -- and
  /// it does not survive the player.
  std::uint32_t m_lastOrderSeqProcessed = 0;

  std::uint32_t m_tickBudgetBytes = DEFAULT_TICK_BUDGET_BYTES;

  /*
   * The viewer's camera, as last described (ADR-022 §4).
   *
   * Session state that ADR-016 §7 said the server had no business holding, and
   * that ADR-022 §1 requires it to hold: relevance is a property of a viewer,
   * and §5a's guarantee cannot be kept for a selection nobody told the server
   * about.
   */
  ViewFocus m_focus;
  std::vector<std::uint32_t> m_selection;

  /// The highest tick this client has said it holds whole, and the grid it said
  /// it about. Zero means "nothing acked", which is a join and takes §2c's
  /// keyframe path like every other case that has no baseline.
  std::uint32_t m_ackedTick = 0;
  bool m_hasAck = false;

  /*
   * The ring of views **as sent** (ADR-022 §2b).
   *
   * Indexed by `tick % BASELINE_RING_TICKS`, each slot remembering which tick it
   * holds so a wrap cannot be mistaken for a hit. What is stored is the client's
   * whole picture after that tick's update -- not the delta, and not the world:
   * under culling the client's picture is a *subset*, so delta-encoding against
   * the grid's true state would describe changes the client never had a baseline
   * for. That is the classic bug this ring exists to make impossible.
   */
  struct SentView
  {
    std::uint32_t tick = 0;
    bool used = false;
    std::vector<EntityRecord> records;
  };
  std::array<SentView, BASELINE_RING_TICKS> m_ring{};

  /// The picture the client holds right now -- the newest sent view, kept out of
  /// the ring so it can be edited in place rather than copied to be edited.
  std::vector<EntityRecord> m_picture;

  /// Scratch, reused every tick so ranking and serialising cost no allocation
  /// once the vectors are warm. Sim thread only, like everything here.
  std::vector<std::uint32_t> m_ranked;
  std::vector<EntityRecord> m_records;
  std::vector<EntityRecord> m_changed;
  std::vector<std::uint32_t> m_left;

  /*
   * Ids that have left interest and have not been named yet.
   *
   * Part zero has to hold the game's tail as well as the departures, and a
   * whole fleet warping out at once can name more ids than fit. They are
   * carried rather than dropped, because an id that is never named is a ghost
   * hull frozen on the plane forever (§5e) -- the one failure the list exists to
   * prevent.
   */
  std::vector<std::uint32_t> m_pendingLeft;

  /// This client's own scratch. Not shared, and not a stack array: a tick may
  /// fill it many times over as it packs its parts.
  std::array<std::uint8_t, MAX_DATAGRAM_BYTES> m_buffer{};

  /// The keyframe's, which is not datagram-shaped (ADR-022 §3c).
  std::vector<std::uint8_t> m_bulkBuffer;
};

} // namespace Neuron
