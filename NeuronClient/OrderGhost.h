#pragma once

#include "EntityRecord.h"
#include "OrderIntent.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * The ghost lifecycle (`puck-and-wheel.png` §4).
 *
 * "The only client-side optimism left in the game", and the sheet means that
 * literally: a ghost is the one place the client draws something the server has
 * not confirmed. Everything else on screen is a replicated field (ADR-002 §4),
 * so this file is where the rule is bent, which is why the states are explicit
 * and finite rather than a flag on the order.
 *
 *   PENDING   -- sent, unacknowledged. Dashed. The ships have not moved: this
 *                is a promise the client is making on the server's behalf, and
 *                the print insists it must *look* like one.
 *   UNDER WAY -- the authority agreed. Solid, and it arrives on the same frame
 *                the ships start moving, so acceptance is never a separate
 *                notification: the world simply agrees with the ghost.
 *   REJECTED  -- refused. The footprint snaps back toward the fleet over 150 ms
 *                and the reason becomes a toast. Never a silent disappearance,
 *                because an order that vanishes without explanation is
 *                indistinguishable from a dropped packet.
 *
 * **A locally refused order gets a ghost too.** That is the whole point of the
 * pre-check: §4's BounceParity gate says a locally-refused order must feel
 * identical to a remotely-refused one, only faster. If a local refusal were a
 * log line and a remote one an animation, the two would be trivially
 * distinguishable and the parity claim would be about the reason code only.
 *
 * **Device-free and game-free.** Reason codes and order states are numbers the
 * game assigned; this file compares them and never interprets them (ADR-014
 * §5). Time arrives as a double so the bounce can be tested without a clock.
 */

namespace Neuron
{

enum class GhostState : std::uint8_t
{
  Pending = 0,
  UnderWay = 1,
  Rejected = 2
};

/*
 * One waypoint of a ghost's plan (S12).
 *
 * A queued order is one chain with a leg per waypoint (`puck-and-wheel.png` §4:
 * "waypoints render as a polyline with per-leg ETAs"), and this is one link of
 * it. Every leg keeps the ETA the game predicted for it *when it was queued* --
 * the authority only reports the leg it is currently flying, and a leg it has
 * not started has no measured number to replace the prediction with.
 */
struct GhostLeg
{
  DirectX::XMFLOAT2 targetMetres{};
  float etaSeconds = -1.0f;
};

/// One order the client is drawing on the server's behalf.
struct OrderGhost
{
  std::uint32_t clientOrderSeq = 0;
  std::uint32_t serverOrderId = 0;
  GhostState state = GhostState::Pending;

  /// The game's `OrderReason`, meaningful only when `Rejected`. Rendered
  /// through `WorldView::ReasonText`, which is the only thing that knows what
  /// the number is called.
  std::uint16_t reasonCode = 0;

  /// Refused by the local pre-check rather than by the authority. Recorded for
  /// the log and for tests -- deliberately *not* for the drawing, because the
  /// two must be indistinguishable on screen.
  bool local = false;

  DirectX::XMFLOAT2 targetMetres{};

  /// Where the group was when the order was given. The bounce retracts the
  /// footprint toward this point, which is what makes a refusal read as the
  /// order coming back rather than as the ghost blinking out.
  DirectX::XMFLOAT2 originMetres{};

  /// The footprint at commit -- the real formation solve, one mark per ship
  /// (ADR-005 §3). Kept rather than re-solved each frame because the ships have
  /// moved since, and re-solving would slide the promise around under a player
  /// who has not touched anything.
  OrderPreview preview;

  /// The authority's progress, once it has any. Zero while pending.
  std::uint8_t legIndex = 0;
  std::uint8_t legCount = 0;
  std::uint8_t memberCount = 0;

  /*
   * The authority's own ETA for the current leg, or negative while there is
   * none (S12).
   *
   * `preview.etaSeconds` is what the game predicted before the order was sent;
   * this is what it measured once the ships were moving. The label prefers this
   * the moment it exists, which is the frame the order is promoted -- so a
   * queued chain shows a prediction until the authority answers and a fact
   * afterwards, and never the prediction once the fact is available.
   */
  float authorityEtaSeconds = -1.0f;

  /*
   * The plan, waypoint by waypoint (S12).
   *
   * `legs[0]` is always this order's own target; an accepted *append* adds one.
   * `legCount` above is the authority's count and this is the client's, and
   * they are different numbers on purpose: the authority's is the truth and
   * arrives a round trip later, while this is what there is to draw *now*. When
   * they disagree the authority wins, which is how a mistaken chain corrects
   * itself.
   */
  GhostLeg legs[MAX_GHOST_LEGS] = {};
  std::uint8_t queuedLegCount = 0;

  /*
   * The first entity the order named, and the only one the chain needs.
   *
   * `World::IngestOrders` joins an append to "the group the first named ship
   * belongs to", and this is the client mirroring that rule so it can draw the
   * chain before the authority confirms it. It is the one thing the ghost
   * *predicts* about grouping rather than replicates -- which is what a ghost
   * is for (`puck-and-wheel.png` §4, "the only client-side optimism left in the
   * game") -- and the authority's `legCount` is what corrects it if the guess
   * was wrong.
   */
  EntityId firstEntityId = INVALID_ENTITY_ID;

  /// Whether this order asked to be queued. Kept because the merge happens on
  /// *acceptance* rather than on send: until the authority agrees, a queued
  /// waypoint is its own pending ghost and bounces on its own if refused.
  bool queued = false;

  /// When the current state began, in the client's own seconds.
  double stateSinceSeconds = 0.0;

  /*
   * When a still-PENDING ghost stops waiting for its ack, or negative while it
   * has no reason to.
   *
   * Set the first time a snapshot says the authority has passed this sequence
   * without listing the order: that is either a refusal whose ack is still in
   * flight or an order that was over before the client heard about it. The two
   * are indistinguishable from the snapshot, and retiring on the spot would
   * make a refusal vanish without a bounce -- the one outcome the print calls
   * indistinguishable from a dropped packet.
   *
   * Negative rather than zero for "not waiting", because zero is a real instant:
   * `Clock::SecondsSinceStart()` begins there, and an order given in the first
   * frame would otherwise read as already due.
   */
  double answerDueSeconds = -1.0;

  /*
   * Where a refusal retracts the last waypoint to.
   *
   * The waypoint before it, or the fleet's position when there is only one --
   * so a single-leg ghost bounces exactly as it always did, and a queued one
   * takes back the leg that was refused rather than the whole plan.
   *
   * Here rather than in either caller because the footprint ring
   * (`BuildGhostMarks`) and the lane (`BuildGhostLanes`) both retract, and two
   * answers would have the ring come home while the line pointed somewhere
   * else for the 150 ms the animation lasts.
   */
  [[nodiscard]] DirectX::XMFLOAT2 RetractTowardMetres() const noexcept
  {
    return queuedLegCount > 1 ? legs[queuedLegCount - 2].targetMetres : originMetres;
  }

  /*
   * Whether this order has stopped travelling and started *working*.
   *
   * Three conditions, and each removes a different way of being wrong:
   *
   * - **The authority agreed.** A pending ghost has none of the authority's
   *   numbers yet, and its zeroed `legCount` would read as "no leg left" on the
   *   frame the order was given.
   * - **No leg is left**, by the authority's own count rather than the client's
   *   guess: `legIndex` and `legCount` are what the snapshot said, and the
   *   client's `queuedLegCount` is what it hoped.
   * - **The game named the state.** Every kind runs out of legs eventually, and
   *   for all but one that means "about to be retired". The word is the game's
   *   answer to which this is (`OrderPreview::workingLabel`), so the engine
   *   never concludes from two numbers that a fleet is doing something in
   *   particular.
   *
   * A working order draws no lane, no footprint and no destination ticks: there
   * is no journey left to promise, and a ring over a fleet that has arrived is
   * the same defect as a lane pointing at somewhere it already got to.
   */
  [[nodiscard]] bool Working() const noexcept
  {
    return state == GhostState::UnderWay && legIndex >= legCount && preview.workingLabel[0] != '\0';
  }
};

class OrderGhostList
{
public:
  /// The print's bounce: 150 ms from refusal to gone.
  static constexpr double BOUNCE_SECONDS = 0.150;

  /*
   * How long a ghost the snapshot has passed waits for its ack before retiring.
   *
   * The ack is reliable and the snapshot is not, so the ack normally arrives
   * first and this window never opens. It opens when a lost datagram puts the
   * ack behind its own resend, and half a second is several resends on any link
   * this game tolerates -- long enough that a refusal still bounces, short
   * enough that a finished order does not sit on screen after the ships have
   * plainly arrived.
   */
  static constexpr double SETTLE_GRACE_SECONDS = 0.5;

  /*
   * How long a ghost may stay PENDING before the client gives up on it.
   *
   * Not a refusal -- there is no reason code for "nobody answered", and
   * inventing one would put a word in the game's mouth. It is a link failure,
   * and the distinction matters: the print's "never a silent disappearance"
   * rule is about *refusals*, which always carry a reason. A ghost that outlives
   * this is counted so the count can be surfaced, rather than left on screen
   * forever promising something no longer in flight.
   *
   * Five seconds is far past any round trip this game tolerates and far short
   * of a player forgetting they gave the order.
   */
  static constexpr double PENDING_TIMEOUT_SECONDS = 5.0;

  /// As many as the game can report progress for. Beyond this the client is
  /// tracking orders the snapshot has no room to describe, so it would be
  /// drawing ghosts it can never promote.
  static constexpr std::size_t MAX_GHOSTS = MAX_ORDER_PROGRESS;

  /*
   * Records an order the client is about to send, or is about to refuse itself.
   *
   * Returns false when the list is full, and the caller must then *not* send:
   * an order in flight with no ghost is an order whose refusal has nowhere to
   * land, which is the one outcome §4 forbids outright.
   */
  [[nodiscard]] bool Add(const OrderIntent& _intent, const OrderPreview& _preview, const DirectX::XMFLOAT2& _originMetres,
                         double _nowSeconds);

  /// Refuses a ghost by sequence. `_local` records which side said no; both
  /// look the same on screen, which is the parity the print asks for.
  void Refuse(std::uint32_t _clientOrderSeq, std::uint16_t _reasonCode, bool _local, double _nowSeconds);

  /*
   * Drops a ghost without a bounce, for an order that never left.
   *
   * Not a refusal and deliberately not drawn as one: a send that failed has no
   * reason code, and borrowing one would tell the player the game said no when
   * the socket did. There was no promise, so there is nothing to take back.
   */
  void Forget(std::uint32_t _clientOrderSeq) noexcept;

  /*
   * Applies an ack.
   *
   * Accepted promotes the ghost and records the server's id; refused bounces
   * it. An ack for a sequence nobody is tracking is ignored -- it is a late
   * answer to a ghost that already timed out, and resurrecting it would put a
   * bounce on screen seconds after the player stopped expecting one.
   */
  void OnVerdict(const OrderVerdict& _verdict, double _nowSeconds);

  /*
   * Applies the authority's view of what is still live.
   *
   * A ghost with a matching progress record is UNDER WAY and takes the
   * record's legs and members. A ghost already past the high-water mark with no
   * record has been decided and is no longer running: order records exist only
   * while an order does. A promoted one retires at once; a still-pending one is
   * given `SETTLE_GRACE_SECONDS` first, because from here a refusal whose ack is
   * in flight looks exactly like an order that finished early.
   *
   * This is the promotion path that survives a lost ack. It cannot carry a
   * refusal -- a refused order and a completed one look identical from here,
   * both being absent -- which is exactly why S9b put the ack on the reliable
   * control channel rather than trusting the snapshot with it.
   */
  void OnFeedback(const OrderFeedback& _feedback, double _nowSeconds);

  /// Expires finished bounces and abandoned pending ghosts. Called once a
  /// frame, before the marks are built.
  void Advance(double _nowSeconds);

  [[nodiscard]] std::span<const OrderGhost> Ghosts() const noexcept { return m_ghosts; }
  [[nodiscard]] std::size_t Count() const noexcept { return m_ghosts.size(); }
  [[nodiscard]] bool Empty() const noexcept { return m_ghosts.empty(); }

  /// Orders sent and not yet answered -- the optimistic window, which is what
  /// the context bar's `⏳ N ORDERS PENDING` chip counts (`tactical-hud.png`).
  /// Promoted and bouncing ghosts are not pending: the authority has spoken.
  [[nodiscard]] std::size_t PendingCount() const noexcept
  {
    std::size_t pending = 0;
    for (const OrderGhost& ghost : m_ghosts)
    {
      pending += ghost.state == GhostState::Pending ? 1 : 0;
    }
    return pending;
  }

  /// Ghosts abandoned for want of an answer. Should stay zero on a healthy
  /// link; a rising number is the link, not the game.
  [[nodiscard]] std::uint64_t TimedOutCount() const noexcept { return m_timedOut; }

  /// Everything, at once -- a disconnect invalidates every promise the client
  /// was making, and keeping them would leave the last session's ghosts over
  /// the next one's fleet.
  void Clear() noexcept;

  /*
   * How far through its bounce a refused ghost is, 0 at the refusal and 1 at
   * the end. Zero for any other state, so a caller can lerp unconditionally.
   */
  [[nodiscard]] static float BounceFraction(const OrderGhost& _ghost, double _nowSeconds) noexcept;

private:
  [[nodiscard]] OrderGhost* Find(std::uint32_t _clientOrderSeq) noexcept;

  /// The chain an accepted append belongs to, by the rule the authority uses:
  /// the plan whose first named ship is this one's. Null when there is none.
  [[nodiscard]] OrderGhost* FindChain(const OrderGhost& _appending) noexcept;

  std::vector<OrderGhost> m_ghosts;
  std::uint64_t m_timedOut = 0;
};

} // namespace Neuron
