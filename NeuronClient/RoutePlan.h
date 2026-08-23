#pragma once

#include "MapView.h"
#include "OrderIntent.h"

#include <array>
#include <cstdint>
#include <span>

/*
 * A multi-hop plan, fed to the authority one order at a time (ADR-016 §8, U4).
 *
 * `strategic-map.png` §3 states the problem this exists for and the ruling that
 * shapes it. The map promises a fourteen-jump route; `OrderQueue` holds **four
 * slots**; and of the three ways out -- widen the queue, make a route one order
 * the server expands, or keep the route client-side and feed the queue one jump
 * at a time -- *"Decided: the third. The map plans, the client feeds. No schema
 * change, no server work."*
 *
 * This is the feeding half. It holds a sequence of destinations and sends
 * nothing: the caller sends, tells this what sequence number it used, and hands
 * over the authority's feedback each frame. When the hop's order is **over**,
 * the next one becomes sendable.
 *
 * **The trigger is `OrderProgress::finished`, and nothing new was needed for
 * it.** That flag exists so a ghost can retire -- *"the game knows which of its
 * states is finished, and says so here"* -- and "the ghost may retire" and "the
 * fleet has arrived" are the same fact read for two purposes. A route feeder
 * that asked a new seam question would have been a second answer to a question
 * the client was already being told.
 *
 * **It knows nothing about systems, gates or jumps -- or about warping.** A leg
 * is two opaque numbers: a kind and an anchor, composed by the game
 * (`WorldView::BuildRoutePlan`) and echoed here. That is stronger than it looks:
 * the client cannot *spell* `Warp`, so it could not have built these legs even
 * if it knew where to fly. What makes this an engine type is that it would feed
 * a chain of anything -- the shape is "send, wait for done, send the next".
 *
 * **A hop is usually two legs and the client must not know that either.**
 * Crossing a gate means warping to the gate on this side and then warping
 * through it, and a fleet already standing on the gate needs only the second.
 * Which of those a route needs is a fact about gates, so the game composes the
 * sequence and this flies whatever length it is handed.
 *
 * **The accepted cost is §8's, and it is a rule rather than a bug.** A plan
 * lives in one client's memory, so a player who disconnects mid-route has a
 * fleet that stops at the next gate. §8 priced that when it chose the third
 * option; ADR-016 §9a.1 records where it gets *reported* (ADR-018 D19's event
 * record), which is a separate piece of work and is named on the slice.
 */

namespace Neuron
{

/// What `RouteLeg::anchor` holds when there is no leg. Zero is a legal anchor
/// id, so the sentinel is the one a `Warp` could never name.
inline constexpr std::uint16_t INVALID_ROUTE_ANCHOR = 0xffffu;

/*
 * One order of a plan: what to send, and where.
 *
 * Both numbers are the game's and both are echoed. `kind` is what goes in
 * `OrderIntent::kind` -- the client cannot spell the verb that flies a route
 * and must not learn to, which is `OrderKindOption`'s rule applied to an order
 * nobody pressed a button for.
 */
struct RouteLeg
{
  std::uint16_t kind = 0;
  std::uint16_t anchor = INVALID_ROUTE_ANCHOR;

  /*
   * Which hop of the route this leg belongs to, counting from zero.
   *
   * Carried rather than derived, and it is what stops the HUD contradicting the
   * map. The panel lists a route in **jumps** and a plan holds it in **orders**,
   * and the two counts differ -- five jumps is nine or ten orders. A chip that
   * said `7/10` beside a panel that said five jumps would be describing the
   * same journey with two numbers, and the player would have no way to tell
   * which was wrong.
   *
   * The grouping is the game's because the pairing is: only the half that knows
   * a gate is crossed in two orders knows which two. This side reads the number
   * and never learns what it counts.
   */
  std::uint16_t hop = 0;
};

/*
 * How many legs a plan holds.
 *
 * **Twice the hops the panel draws, and derived rather than picked.** A hop
 * across a gate is two orders -- warp to the gate on this side, then warp
 * through it -- so a route the panel lists as fourteen jumps is up to
 * twenty-eight orders. Deriving it is what keeps the two from disagreeing: a
 * plan capped independently could refuse a route the panel had already drawn.
 */
inline constexpr std::uint32_t MAX_ROUTE_LEGS = MAX_MAP_ROUTE_HOPS * 2u;

/*
 * Where a plan is.
 *
 * Four states and no more: a plan is a queue of one, so the only questions are
 * whether there is one, whether its current hop is in flight, and -- if it
 * stopped -- whether it stopped because it finished or because something
 * refused it.
 */
enum class RouteState : std::uint8_t
{
  /// No plan. The common state, and what `Clear` returns to.
  None = 0,

  /// A hop is ready to send. The caller sends it and reports the sequence.
  Ready,

  /// A hop is in flight and the authority has not said it is over.
  Flying,

  /*
   * The plan stopped short, and the player is owed the reason.
   *
   * Separate from `None` because the two look identical to a draw that only
   * asks "is there a route" and are opposite to a player: one is a plan that
   * arrived and one is a plan that did not.
   */
  Halted
};

/*
 * Whether this client can judge an order for a fleet at all.
 *
 * A free function beside the plan rather than a loop inside the frame, because
 * it is the one decision in the feeder that changes whether a route advances --
 * and a decision that decides that belongs somewhere a test can reach.
 *
 * **It asks the client's own scene, not the game's reason code.** The tempting
 * version reads the pre-check's verdict and holds when the reason is "unknown
 * ship". That means the engine deciding which of the game's reason codes means
 * *"ask me later"*, which is game semantics arriving through the one door
 * ADR-014 §4 keeps shut. The client already knows which entities it has been
 * sent; whether the fleet is among them is a question about its own knowledge,
 * and it can answer that itself.
 *
 * Linear in both spans and called once a frame at most, over a fleet that is
 * tens of ships against a scene that is hundreds -- which is cheaper than the
 * pre-check it guards.
 */
[[nodiscard]] bool FleetIsInScene(std::span<const EntityId> _fleet, std::span<const EntityId> _scene) noexcept;

/*
 * The plan.
 *
 * Fixed storage rather than a vector, which is this client's habit for a reason
 * that applies here: a route is set on a press and read on every frame after
 * it, and a HUD must not allocate to describe where the player is going.
 */
class RoutePlan
{
public:
  /*
   * Takes a plan: the legs to fly, in order.
   *
   * A *route* is a sequence of places and a *plan* is a sequence of orders --
   * there is no order for staying where you are, and crossing a gate is two.
   * The game does that conversion, in `BuildRoutePlan`, which is why what
   * arrives here is already the orders.
   *
   * An empty span clears. So does a plan longer than the cap -- refused whole
   * rather than truncated, because a plan that silently stopped nine jumps
   * short would strand a fleet somewhere the player believes is on the way.
   */
  void Set(std::span<const RouteLeg> _legs) noexcept;

  void Clear() noexcept;

  [[nodiscard]] RouteState State() const noexcept { return m_state; }
  [[nodiscard]] bool Active() const noexcept { return m_state == RouteState::Ready || m_state == RouteState::Flying; }

  /// The leg to send now, or a leg with `INVALID_ROUTE_ANCHOR` when nothing is
  /// ready -- because one is already flying, or the plan is over.
  [[nodiscard]] RouteLeg Ready() const noexcept;

  /*
   * Records that the caller sent the ready hop, and with which sequence.
   *
   * The sequence is what ties this plan to the authority's answers, and it is
   * the caller's rather than this file's because the caller is what owns the
   * order stream -- the same number it gives the ghost.
   */
  void NoteSent(std::uint32_t _clientOrderSeq) noexcept;

  /*
   * Reads a frame's feedback and advances when the flying hop is over.
   *
   * **Two ways an order ends and both count.** It appears in `orders` with
   * `finished` set, which is the ordinary path; or it does not appear at all
   * while `lastOrderSeqProcessed` has passed it, which is the path a fleet takes
   * when the order finished between two snapshots. The second is not an
   * optimisation -- a plan that waited for a record that had already been and
   * gone would stall for ever one hop short.
   */
  void NoteFeedback(const OrderFeedback& _feedback) noexcept;

  /*
   * Stops the plan where it is, because something refused the hop.
   *
   * Told rather than inferred: the *caller* pre-checks, so the caller is what
   * holds the verdict, and a feeder that guessed a refusal from silence would
   * eventually halt a plan that was merely slow.
   */
  void Halt() noexcept;

  /// Which leg is next, counting from one, and how many there are. The log's
  /// numbers, and meaningless on a plan that is `None`.
  [[nodiscard]] std::uint32_t LegNumber() const noexcept { return m_hop + 1u; }
  [[nodiscard]] std::uint32_t LegCount() const noexcept { return m_count; }

  /*
   * The same pair in jumps, which is the pair a player is shown.
   *
   * `RouteLeg::hop` says what the HUD's `3/11` counts; these two read it. The
   * count comes from the *last* leg rather than from a stored total because the
   * last leg is by construction in the last hop, and one source cannot drift
   * from itself.
   */
  [[nodiscard]] std::uint32_t HopNumber() const noexcept
  {
    return m_hop < m_count ? static_cast<std::uint32_t>(m_legs[m_hop].hop) + 1u : HopCount();
  }
  [[nodiscard]] std::uint32_t HopCount() const noexcept
  {
    return m_count > 0 ? static_cast<std::uint32_t>(m_legs[m_count - 1u].hop) + 1u : 0u;
  }

  /// How many are left to fly, including the one in flight.
  [[nodiscard]] std::uint32_t Remaining() const noexcept { return m_hop < m_count ? m_count - m_hop : 0u; }

private:
  std::array<RouteLeg, MAX_ROUTE_LEGS> m_legs{};
  std::uint32_t m_count = 0;
  std::uint32_t m_hop = 0;
  std::uint32_t m_flyingSeq = 0;
  RouteState m_state = RouteState::None;
};

} // namespace Neuron
