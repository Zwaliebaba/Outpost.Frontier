#include "pch.h"
#include "CppUnitTest.h"

#include "RoutePlan.h"

#include <array>
#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The route feeder (U4, ADR-016 §8, `strategic-map.png` §3).
 *
 * The print's own worked problem: a fourteen-jump route into a four-slot queue,
 * answered by keeping the route client-side and feeding one order per completed
 * hop. This is that feeding, and every claim below is about *when* the next hop
 * becomes sendable -- which is the only thing this file decides.
 *
 * Nothing here needs a device, a world or a wire: a plan goes in, feedback goes
 * in, and the answer is which destination to send next.
 */

namespace NeuronClientTests
{
namespace
{

/// The authority saying an order is still running, or over.
[[nodiscard]] OrderFeedback Working(std::uint32_t _seq, bool _finished, std::uint32_t _highWater = 0)
{
  OrderFeedback feedback;
  feedback.lastOrderSeqProcessed = _highWater;
  OrderProgress progress;
  progress.clientOrderSeq = _seq;
  progress.finished = _finished;
  (void)feedback.Add(progress);
  return feedback;
}

/// And the authority saying nothing about it at all, having passed it.
[[nodiscard]] OrderFeedback Silent(std::uint32_t _highWater)
{
  OrderFeedback feedback;
  feedback.lastOrderSeqProcessed = _highWater;
  return feedback;
}

/*
 * Three legs, as a game would compose them: a kind the client cannot spell, an
 * anchor it never reads, and the hop each belongs to.
 *
 * The hop numbers are what a real two-jump plan looks like for a fleet already
 * standing on its first gate -- warp *through* it (hop 0), then warp to the
 * next gate and through that one (hop 1). Written realistically rather than as
 * three zeroes because `HopCount` reads the last leg, and a fixture the builder
 * could never produce would prove nothing about the number a player sees.
 */
constexpr std::uint16_t WARP = 3;
constexpr RouteLeg LEGS[] = {{WARP, 11, 0}, {WARP, 22, 1}, {WARP, 33, 1}};

} // namespace

/*
 * The one decision that governs whether a route advances at all.
 *
 * `FleetIsInScene` is what turns "the client cannot see this fleet" into a hold
 * rather than a halt, so its edges are worth pinning: a partial fleet, an empty
 * one, and a scene that has moved on.
 */
TEST_CLASS(RouteVisibilityTests)
{
public:
  TEST_METHOD(AFleetTheClientCanSeeIsJudgeable)
  {
    const EntityId fleet[] = {7, 9};
    const EntityId scene[] = {3, 7, 9, 11};
    Assert::IsTrue(FleetIsInScene(fleet, scene));
  }

  TEST_METHOD(OneMemberMissingHoldsTheWholeLeg)
  {
    /*
     * Every member, not any -- and this is the assertion that says so. A route
     * order names the whole fleet and the authority refuses the whole thing for
     * one id it cannot place, so a client that sent on a majority would be
     * sending an order it had reason to expect back.
     */
    const EntityId fleet[] = {7, 9};
    const EntityId scene[] = {3, 7, 11};
    Assert::IsFalse(FleetIsInScene(fleet, scene));
  }

  TEST_METHOD(AFleetOnAnotherGridIsNotJudgeable)
  {
    // The ordinary case from the second leg onward: the fleet warped away and
    // this client is still watching where it was.
    const EntityId fleet[] = {7, 9};
    const EntityId scene[] = {3, 11};
    Assert::IsFalse(FleetIsInScene(fleet, scene));
  }

  TEST_METHOD(AnEmptyFleetIsNotJudgeableRatherThanVacuouslyTrue)
  {
    // Answering "yes, vacuously" would let the caller send an order for nobody.
    const EntityId scene[] = {3, 7};
    Assert::IsFalse(FleetIsInScene({}, scene));
  }

  TEST_METHOD(AnEmptySceneJudgesNothing)
  {
    // The frame before the first snapshot lands, and every frame after a
    // disconnect. Held, which is what a client that has been told nothing owes.
    const EntityId fleet[] = {7};
    Assert::IsFalse(FleetIsInScene(fleet, {}));
  }
};

TEST_CLASS(RoutePlanTests)
{
public:
  TEST_METHOD(APlanOffersOneHopAndThenWaits)
  {
    /*
     * The whole decision in one assertion. The queue holds four and a route can
     * be fourteen, so the plan hands over *one* destination and will not hand
     * over the next until the authority says the first is over -- which is what
     * "no schema change, no server work" bought.
     */
    RoutePlan plan;
    plan.Set(LEGS);

    Assert::IsTrue(plan.State() == RouteState::Ready, L"a fresh plan has something to send");
    Assert::AreEqual(static_cast<std::uint16_t>(11), plan.Ready().anchor, L"and it is the first leg");
    Assert::AreEqual(WARP, plan.Ready().kind, L"with the kind the game chose");

    plan.NoteSent(7);
    Assert::IsTrue(plan.State() == RouteState::Flying);
    Assert::AreEqual(INVALID_ROUTE_ANCHOR, plan.Ready().anchor, L"and nothing more is offered while it flies");
  }

  TEST_METHOD(TheNextHopBecomesSendableWhenTheAuthoritySaysTheLastIsOver)
  {
    RoutePlan plan;
    plan.Set(LEGS);
    plan.NoteSent(7);

    plan.NoteFeedback(Working(7, false));
    Assert::AreEqual(INVALID_ROUTE_ANCHOR, plan.Ready().anchor, L"an order still running holds the plan");

    plan.NoteFeedback(Working(7, true));
    Assert::AreEqual(static_cast<std::uint16_t>(22), plan.Ready().anchor, L"and a finished one advances it");
    Assert::AreEqual(2u, plan.LegNumber(), L"the HUD's 2 of 3");
    Assert::AreEqual(3u, plan.LegCount());
  }

  TEST_METHOD(AnOrderThatFinishedBetweenTwoSnapshotsStillAdvancesThePlan)
  {
    /*
     * The path that is easy to miss and would stall every route one hop short.
     *
     * `OrderFeedback::orders` lists what the authority is *still working on*, so
     * an order that started and finished between two snapshots appears in none
     * of them. What says it happened is the header's high-water mark passing the
     * sequence -- one monotonic number closing the loop for every order at once.
     * A hop is twenty seconds and a snapshot is fifty milliseconds, so this is
     * not the common case; it is the case a lost snapshot creates.
     */
    RoutePlan plan;
    plan.Set(LEGS);
    plan.NoteSent(7);

    plan.NoteFeedback(Silent(6));
    Assert::AreEqual(INVALID_ROUTE_ANCHOR, plan.Ready().anchor, L"silence below the mark is not an answer");

    plan.NoteFeedback(Silent(7));
    Assert::AreEqual(static_cast<std::uint16_t>(22), plan.Ready().anchor, L"silence past it is");
  }

  TEST_METHOD(APlanRunsOutRatherThanRepeatingItsLastHop)
  {
    RoutePlan plan;
    plan.Set(LEGS);
    for (std::uint32_t seq = 1; seq <= 3; ++seq)
    {
      Assert::IsTrue(plan.Ready().anchor != INVALID_ROUTE_ANCHOR, L"every leg is offered once");
      plan.NoteSent(seq);
      plan.NoteFeedback(Working(seq, true));
    }

    Assert::IsTrue(plan.State() == RouteState::None, L"an arrived plan is over");
    Assert::IsFalse(plan.Active());
    Assert::AreEqual(INVALID_ROUTE_ANCHOR, plan.Ready().anchor, L"and offers nothing more");
    Assert::AreEqual(0u, plan.Remaining());
  }

  TEST_METHOD(AHaltedPlanIsNotAnAbsentOne)
  {
    /*
     * The two look identical to a draw that only asks "is there a route" and
     * are opposite to a player: one is a plan that arrived and one is a plan
     * that did not. §8 prices the second as an accepted cost -- a fleet held at
     * a gate -- and an accepted cost the player is not told about is a bug.
     */
    RoutePlan plan;
    plan.Set(LEGS);
    plan.NoteSent(7);
    plan.Halt();

    Assert::IsTrue(plan.State() == RouteState::Halted);
    Assert::IsFalse(plan.Active(), L"a halted plan sends nothing");
    Assert::AreEqual(INVALID_ROUTE_ANCHOR, plan.Ready().anchor);
    Assert::AreEqual(3u, plan.LegCount(), L"and still knows how far it was going");
  }

  TEST_METHOD(AnArrivedPlanCannotBeHalted)
  {
    // Halting a finished plan would put a "held at" notice on a fleet that
    // arrived -- the one report a player would act on and should not.
    RoutePlan plan;
    plan.Set(std::span<const RouteLeg>{LEGS, 1});
    plan.NoteSent(1);
    plan.NoteFeedback(Working(1, true));
    Assert::IsTrue(plan.State() == RouteState::None);

    plan.Halt();
    Assert::IsTrue(plan.State() == RouteState::None, L"an arrival is not a halt");
  }

  TEST_METHOD(APlanLongerThanTheCapIsRefusedWholeRatherThanTruncated)
  {
    /*
     * A truncated plan strands a fleet somewhere the player believes is on the
     * way, and they find out by looking at it hours later. Refused whole, the
     * caller can say so at the moment of the press.
     *
     * The cap is *derived* from the panel's, at two legs a hop, so a route the
     * panel drew is always one the feeder will accept.
     */
    std::array<RouteLeg, MAX_ROUTE_LEGS + 1> tooLong{};
    for (std::size_t index = 0; index < tooLong.size(); ++index)
    {
      tooLong[index] = RouteLeg{WARP, static_cast<std::uint16_t>(index + 1)};
    }

    RoutePlan plan;
    plan.Set(tooLong);
    Assert::IsTrue(plan.State() == RouteState::None, L"nothing is planned");
    Assert::AreEqual(0u, plan.LegCount());

    // And exactly the cap is fine, which is what makes the refusal a boundary
    // rather than a margin.
    plan.Set(std::span<const RouteLeg>{tooLong.data(), MAX_ROUTE_LEGS});
    Assert::IsTrue(plan.State() == RouteState::Ready);
    Assert::AreEqual(MAX_ROUTE_LEGS, plan.LegCount());
  }

  TEST_METHOD(SettingASecondPlanReplacesTheFirstOutright)
  {
    // SET DESTINATION on a new system while a route is running is a new route,
    // not an appended one -- ADD WAYPOINT is the other button, and conflating
    // them would make the primary action's meaning depend on hidden state.
    RoutePlan plan;
    plan.Set(LEGS);
    plan.NoteSent(7);

    const RouteLeg replacement[] = {{WARP, 44}};
    plan.Set(replacement);
    Assert::AreEqual(1u, plan.LegCount());
    Assert::AreEqual(static_cast<std::uint16_t>(44), plan.Ready().anchor, L"the new plan starts at its own first hop");

    // And the old hop's feedback cannot advance it, because the sequence it was
    // waiting on went with the plan that owned it.
    plan.NoteFeedback(Working(7, true));
    Assert::AreEqual(static_cast<std::uint16_t>(44), plan.Ready().anchor, L"a stale answer moves nothing");
  }

  TEST_METHOD(AnEmptyPlanIsNoPlan)
  {
    // The route to where you already are: the solver returns the origin alone,
    // the caller drops it, and what is left is nothing to send.
    RoutePlan plan;
    plan.Set(std::span<const RouteLeg>{});
    Assert::IsTrue(plan.State() == RouteState::None);
    Assert::AreEqual(INVALID_ROUTE_ANCHOR, plan.Ready().anchor);
  }

  TEST_METHOD(TheNumberAPlayerSeesCountsJumpsRatherThanOrders)
  {
    /*
     * The HUD chip and the map's panel describe one journey, so they must not
     * describe it with two numbers.
     *
     * This plan is three *orders* across two *jumps*, which is what a fleet
     * standing on its first gate actually flies. `LegCount` is the honest
     * three; `HopCount` is the two the panel drew. Feeding the chip the former
     * would put `3/3` beside a route the map listed as two jumps.
     */
    RoutePlan plan;
    plan.Set(LEGS);

    Assert::AreEqual(std::uint32_t{3}, plan.LegCount(), L"three orders");
    Assert::AreEqual(std::uint32_t{2}, plan.HopCount(), L"across two jumps");
    Assert::AreEqual(std::uint32_t{1}, plan.HopNumber(), L"and the fleet is on the first of them");

    // Through the first gate. That order is a whole jump, so the count moves.
    plan.NoteSent(1);
    plan.NoteFeedback(Working(1, true));
    Assert::AreEqual(std::uint32_t{2}, plan.LegNumber());
    Assert::AreEqual(std::uint32_t{2}, plan.HopNumber(), L"the second jump has begun");

    /*
     * And the leg *within* a jump does not move it. This is the assertion the
     * whole field exists for: two orders, one jump, one number.
     */
    plan.NoteSent(2);
    plan.NoteFeedback(Working(2, true));
    Assert::AreEqual(std::uint32_t{3}, plan.LegNumber(), L"the third order");
    Assert::AreEqual(std::uint32_t{2}, plan.HopNumber(), L"is still the second jump");
    Assert::AreEqual(std::uint32_t{2}, plan.HopCount(), L"and the total never moved");
  }

  TEST_METHOD(AnArrivedPlanReadsAsFullyTravelledRatherThanPastItsEnd)
  {
    /*
     * What the chip would say in the frame a route completes.
     *
     * `m_hop` walks past the last leg when the plan ends, so `HopNumber` has no
     * leg to read. It answers `HopCount` instead -- `2/2` rather than an index
     * off the end of the array. The state is `None` by then and nothing draws
     * it, but a getter that read out of bounds to say so would be one refactor
     * away from being the bug it looks like.
     */
    RoutePlan plan;
    plan.Set(LEGS);
    for (std::uint32_t seq = 1; seq <= 3; ++seq)
    {
      plan.NoteSent(seq);
      plan.NoteFeedback(Working(seq, true));
    }

    Assert::IsTrue(plan.State() == RouteState::None, L"the plan is over");
    Assert::AreEqual(std::uint32_t{2}, plan.HopNumber(), L"and reads as arrived rather than as past the end");
    Assert::AreEqual(std::uint32_t{0}, plan.Remaining());
  }

  TEST_METHOD(APlanWithNoLegsCountsNoJumps)
  {
    // The zero case, because `HopCount` reads `m_legs[m_count - 1]` and an
    // unsigned count of zero is the index that wraps.
    RoutePlan plan;
    Assert::AreEqual(std::uint32_t{0}, plan.HopCount());
    Assert::AreEqual(std::uint32_t{0}, plan.HopNumber());
  }

  TEST_METHOD(FeedbackBeforeAnythingWasSentChangesNothing)
  {
    // The frame between a plan being set and its first hop going out is a real
    // frame, and the authority is still answering about somebody else's orders
    // in it.
    RoutePlan plan;
    plan.Set(LEGS);
    plan.NoteFeedback(Working(99, true, 99));
    Assert::AreEqual(static_cast<std::uint16_t>(11), plan.Ready().anchor, L"still the first leg");
    Assert::AreEqual(1u, plan.LegNumber());
  }
};

} // namespace NeuronClientTests
