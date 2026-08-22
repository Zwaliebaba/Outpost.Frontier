#include "pch.h"
#include "CppUnitTest.h"

#include "OrderGhost.h"
#include "OverlayMark.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * The ghost lifecycle and what it draws (Build Order S9, `puck-and-wheel.png` §4).
 *
 * The sheet's rule is short and absolute: every round trip has a ghost and
 * every rejection has a bounce and a reason, and "an order that vanishes with
 * no explanation is indistinguishable from a dropped packet". Most of what
 * follows is that sentence turned into assertions -- including the awkward
 * cases it implies and does not state, like a refusal whose ack arrives after
 * the snapshot that has already moved past it.
 *
 * Time is a parameter here, not a clock, so the 150 ms bounce is a number this
 * file can step through rather than sleep through.
 */

namespace NeuronClientTests
{
namespace
{

[[nodiscard]] OrderIntent Intent(std::uint32_t _seq, float _x = 1000.0f, float _y = 500.0f)
{
  OrderIntent intent;
  intent.orderSeq = _seq;
  intent.targetXMetres = _x;
  intent.targetYMetres = _y;
  return intent;
}

/// The ids a chained order names. Static so the span in `OrderIntent` outlives
/// the call -- the intent holds a pointer, not a copy.
const std::uint16_t CHAIN_SHIPS[] = {7, 8, 9};
const std::uint16_t OTHER_SHIPS[] = {21, 22};

/// An order over `_ships`, optionally asking to be queued behind whatever those
/// ships are already doing.
[[nodiscard]] OrderIntent FleetIntent(std::uint32_t _seq, std::span<const std::uint16_t> _ships, bool _queued, float _x,
                                      float _y = 0.0f)
{
  OrderIntent intent = Intent(_seq, _x, _y);
  intent.entityIds = _ships.data();
  intent.entityCount = static_cast<std::uint32_t>(_ships.size());
  intent.queued = _queued;
  return intent;
}

[[nodiscard]] OrderVerdict Accept(std::uint32_t _seq, std::uint32_t _serverOrderId)
{
  OrderVerdict verdict;
  verdict.accepted = true;
  verdict.orderSeq = _seq;
  verdict.serverOrderId = _serverOrderId;
  return verdict;
}

/// A preview with `_count` stations in a row, which is close enough to a Line
/// for a test that is about the ghost rather than about the solve.
[[nodiscard]] OrderPreview Preview(std::uint32_t _count, float _x = 1000.0f, float _y = 500.0f)
{
  OrderPreview preview;
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    Assert::IsTrue(preview.AddMark(_x + static_cast<float>(index) * 30.0f, _y), L"the preview must fit the test's marks");
  }
  preview.extentMetres = 45.0f;
  return preview;
}

[[nodiscard]] OrderVerdict Ack(std::uint32_t _seq, bool _accepted, std::uint16_t _reason = 0, std::uint32_t _serverId = 0)
{
  OrderVerdict verdict;
  verdict.accepted = _accepted;
  verdict.reasonCode = _reason;
  verdict.serverOrderId = _serverId;
  verdict.orderSeq = _seq;
  return verdict;
}

[[nodiscard]] OrderFeedback Running(std::uint32_t _seq, std::uint32_t _serverId, std::uint32_t _highWater)
{
  OrderFeedback feedback;
  feedback.lastOrderSeqProcessed = _highWater;

  OrderProgress progress;
  progress.serverOrderId = _serverId;
  progress.clientOrderSeq = _seq;
  progress.state = 0; // Underway, in the game's own enum. Copied, never read.
  progress.legIndex = 1;
  progress.legCount = 3;
  progress.memberCount = 4;
  Assert::IsTrue(feedback.Add(progress), L"one order fits");
  return feedback;
}

[[nodiscard]] const OrderGhost* FindGhost(const OrderGhostList& _list, std::uint32_t _seq)
{
  for (const OrderGhost& ghost : _list.Ghosts())
  {
    if (ghost.clientOrderSeq == _seq)
    {
      return &ghost;
    }
  }
  return nullptr;
}

} // namespace

TEST_CLASS(GhostLifecycleTests)
{
public:
  TEST_METHOD(ASentOrderIsPendingUntilSomebodyAnswers)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(3), XMFLOAT2{0.0f, 0.0f}, 10.0), L"the first ghost fits");

    const OrderGhost* ghost = FindGhost(ghosts, 1);
    Assert::IsNotNull(ghost, L"the ghost exists");
    Assert::IsTrue(ghost->state == GhostState::Pending, L"and is a promise, not a fact");
    Assert::AreEqual<std::uint32_t>(3, ghost->preview.markCount, L"carrying the footprint it was given");

    ghosts.Advance(10.05);
    Assert::AreEqual<std::size_t>(1, ghosts.Count(), L"a pending ghost does not expire on its own");
  }

  TEST_METHOD(AnAcceptedAckPromotesTheGhost)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(2), XMFLOAT2{0.0f, 0.0f}, 10.0));

    ghosts.OnVerdict(Ack(1, true, 0, 88), 10.02);

    const OrderGhost* ghost = FindGhost(ghosts, 1);
    Assert::IsNotNull(ghost, L"promotion is not deletion");
    Assert::IsTrue(ghost->state == GhostState::UnderWay, L"the world agreed with the ghost");
    Assert::AreEqual<std::uint32_t>(88, ghost->serverOrderId, L"and said which order it is");
  }

  TEST_METHOD(ARefusedAckBouncesWithTheGamesReason)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(2), XMFLOAT2{0.0f, 0.0f}, 10.0));

    ghosts.OnVerdict(Ack(1, false, 5), 10.0);

    const OrderGhost* ghost = FindGhost(ghosts, 1);
    Assert::IsNotNull(ghost, L"a refusal is a bounce, not a disappearance");
    Assert::IsTrue(ghost->state == GhostState::Rejected, L"rejected");
    Assert::AreEqual<std::uint16_t>(5, ghost->reasonCode, L"carrying the game's code, unread");
    Assert::IsFalse(ghost->local, L"and knowing the authority said it");
  }

  TEST_METHOD(TheBounceLastsExactlyOneHundredAndFiftyMilliseconds)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    ghosts.OnVerdict(Ack(1, false, 5), 10.0);

    ghosts.Advance(10.0 + OrderGhostList::BOUNCE_SECONDS - 0.001);
    Assert::AreEqual<std::size_t>(1, ghosts.Count(), L"still bouncing a millisecond before the end");

    ghosts.Advance(10.0 + OrderGhostList::BOUNCE_SECONDS);
    Assert::AreEqual<std::size_t>(0, ghosts.Count(), L"and gone at it");
  }

  TEST_METHOD(ALocalRefusalLooksExactlyLikeARemoteOne)
  {
    /*
     * BounceParity, as a test. The print requires a locally refused order to be
     * indistinguishable from a remotely refused one -- same ghost, same bounce,
     * same reason -- and only faster. `local` exists for the log and for this
     * assertion; nothing that draws is allowed to read it.
     */
    OrderGhostList remote;
    Assert::IsTrue(remote.Add(Intent(1), Preview(3), XMFLOAT2{0.0f, 0.0f}, 10.0));
    remote.OnVerdict(Ack(1, false, 5), 10.0);

    OrderGhostList local;
    Assert::IsTrue(local.Add(Intent(1), Preview(3), XMFLOAT2{0.0f, 0.0f}, 10.0));
    local.Refuse(1, 5, true, 10.0);

    const OrderGhost* fromServer = FindGhost(remote, 1);
    const OrderGhost* fromHere = FindGhost(local, 1);
    Assert::IsNotNull(fromServer, L"both exist");
    Assert::IsNotNull(fromHere, L"both exist");

    Assert::IsTrue(fromServer->state == fromHere->state, L"the same state");
    Assert::AreEqual(fromServer->reasonCode, fromHere->reasonCode, L"the same reason");
    Assert::AreEqual(fromServer->targetMetres.x, fromHere->targetMetres.x, L"in the same place");
    Assert::AreEqual<std::uint32_t>(fromServer->preview.markCount, fromHere->preview.markCount, L"with the same footprint");
    Assert::AreEqual(OrderGhostList::BounceFraction(*fromServer, 10.075),
                     OrderGhostList::BounceFraction(*fromHere, 10.075), L"and the same bounce at the same instant");

    Assert::IsFalse(fromServer->local, L"only the record of who said no differs");
    Assert::IsTrue(fromHere->local, L"and it is not drawn");
  }

  TEST_METHOD(ASecondRefusalDoesNotRestartTheBounce)
  {
    // A duplicated ack -- a control-channel resend arriving after the original
    // -- would otherwise hold the ghost on screen for another 150 ms each time.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    ghosts.OnVerdict(Ack(1, false, 5), 10.0);
    ghosts.OnVerdict(Ack(1, false, 5), 10.1);

    ghosts.Advance(10.0 + OrderGhostList::BOUNCE_SECONDS);
    Assert::AreEqual<std::size_t>(0, ghosts.Count(), L"the bounce ends when the first refusal said it would");
  }

  TEST_METHOD(AnAcceptanceCannotUnbounceAGhost)
  {
    // If the two halves of the loop disagree, what the player saw stands. A
    // ghost that bounced and then went solid would be the client contradicting
    // itself on screen.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    ghosts.OnVerdict(Ack(1, false, 5), 10.0);
    ghosts.OnVerdict(Ack(1, true, 0, 42), 10.01);

    const OrderGhost* ghost = FindGhost(ghosts, 1);
    Assert::IsNotNull(ghost, L"still bouncing");
    Assert::IsTrue(ghost->state == GhostState::Rejected, L"and still rejected");
  }

  TEST_METHOD(AnAckForAGhostNobodyIsTrackingIsIgnored)
  {
    OrderGhostList ghosts;
    ghosts.OnVerdict(Ack(77, false, 5), 10.0);
    Assert::IsTrue(ghosts.Empty(), L"a late answer does not resurrect a ghost the player stopped expecting");
  }

  TEST_METHOD(TheSnapshotPromotesAGhostWhoseAckWasLost)
  {
    // The path that survives a dropped ack: the order appears in the snapshot's
    // order area, which is the authority saying it is running.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(4), XMFLOAT2{0.0f, 0.0f}, 10.0));

    ghosts.OnFeedback(Running(1, 88, 1), 10.06);

    const OrderGhost* ghost = FindGhost(ghosts, 1);
    Assert::IsNotNull(ghost, L"still there");
    Assert::IsTrue(ghost->state == GhostState::UnderWay, L"promoted by the snapshot alone");
    Assert::AreEqual<std::uint32_t>(88, ghost->serverOrderId, L"which also named it");
    Assert::AreEqual<std::uint8_t>(1, ghost->legIndex, L"and said which leg");
    Assert::AreEqual<std::uint8_t>(3, ghost->legCount, L"of how many");
    Assert::AreEqual<std::uint8_t>(4, ghost->memberCount, L"for how many ships");
  }

  TEST_METHOD(APromotedGhostRetiresWhenItsOrderLeavesTheSnapshot)
  {
    // Order records exist only while an order does, so a promoted ghost that is
    // no longer listed has finished. The ships are now the record of it.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(2), XMFLOAT2{0.0f, 0.0f}, 10.0));
    ghosts.OnVerdict(Ack(1, true, 0, 88), 10.02);

    OrderFeedback finished;
    finished.lastOrderSeqProcessed = 1;
    ghosts.OnFeedback(finished, 12.0);

    Assert::IsTrue(ghosts.Empty(), L"an order that is over has nothing left to promise");
  }

  TEST_METHOD(APromotedGhostRetiresWhenTheAuthoritySaysItIsFinished)
  {
    /*
     * The other way an order ends, and the one that was missing.
     *
     * The authority keeps reporting a group after it completes, so "still in the
     * list" does not mean "still running" -- a ghost that only retired on
     * *disappearing* left its lane and its destination mark on screen for ever,
     * pointing at somewhere the fleet had already arrived. Reported and finished
     * has to retire it just as absence does.
     *
     * `finished` is a bool rather than the engine comparing `state` to a number:
     * the game says which of its states is over (ADR-014 §2).
     */
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(2), XMFLOAT2{0.0f, 0.0f}, 10.0));
    ghosts.OnVerdict(Ack(1, true, 0, 88), 10.02);

    OrderFeedback running = Running(1, 88, 1);
    ghosts.OnFeedback(running, 10.06);
    Assert::IsFalse(ghosts.Empty(), L"a running order is still a promise");

    OrderFeedback done = Running(1, 88, 1);
    done.orders[0].finished = true;
    ghosts.OnFeedback(done, 12.0);

    Assert::IsTrue(ghosts.Empty(), L"a kept promise is retired, not left on screen");
  }

  TEST_METHOD(APendingGhostThePassIsPastWaitsForItsAckBeforeRetiring)
  {
    /*
     * The race the grace exists for. The snapshot is unreliable and the ack is
     * not, so a resent ack can arrive after a snapshot whose high-water mark
     * has already passed the order. From the snapshot alone a refusal and a
     * finished order look identical -- both absent -- and retiring on the spot
     * would make a refusal vanish with no bounce.
     */
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(2), XMFLOAT2{0.0f, 0.0f}, 10.0));

    OrderFeedback passed;
    passed.lastOrderSeqProcessed = 1;
    ghosts.OnFeedback(passed, 10.06);
    Assert::AreEqual<std::size_t>(1, ghosts.Count(), L"not retired on the spot");

    ghosts.Advance(10.06 + OrderGhostList::SETTLE_GRACE_SECONDS - 0.01);
    Assert::AreEqual<std::size_t>(1, ghosts.Count(), L"still waiting inside the grace");

    // The ack the grace was held open for.
    ghosts.OnVerdict(Ack(1, false, 5), 10.5);
    const OrderGhost* ghost = FindGhost(ghosts, 1);
    Assert::IsNotNull(ghost, L"and it arrived in time");
    Assert::IsTrue(ghost->state == GhostState::Rejected, L"so the refusal still bounces");
  }

  TEST_METHOD(TheGraceEndsAndTheGhostRetiresIfNothingArrives)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(2), XMFLOAT2{0.0f, 0.0f}, 10.0));

    OrderFeedback passed;
    passed.lastOrderSeqProcessed = 1;
    ghosts.OnFeedback(passed, 10.06);

    ghosts.Advance(10.06 + OrderGhostList::SETTLE_GRACE_SECONDS);
    Assert::IsTrue(ghosts.Empty(), L"a ghost cannot wait forever for an answer that is not coming");
    Assert::AreEqual<std::uint64_t>(0, ghosts.TimedOutCount(), L"and this is not the link failing");
  }

  TEST_METHOD(AGhostAheadOfTheHighWaterMarkIsStillInFlight)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(7), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));

    OrderFeedback behind;
    behind.lastOrderSeqProcessed = 6;
    ghosts.OnFeedback(behind, 10.06);
    ghosts.Advance(10.06);

    const OrderGhost* ghost = FindGhost(ghosts, 7);
    Assert::IsNotNull(ghost, L"the authority has not reached this order yet");
    Assert::IsTrue(ghost->state == GhostState::Pending, L"so it is still a promise");
  }

  TEST_METHOD(AGhostNobodyEverAnswersIsGivenUpOnAndCounted)
  {
    // A link failure rather than a refusal: there is no reason code for "nobody
    // answered", and borrowing one would put a word in the game's mouth.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));

    ghosts.Advance(10.0 + OrderGhostList::PENDING_TIMEOUT_SECONDS - 0.01);
    Assert::AreEqual<std::size_t>(1, ghosts.Count(), L"five seconds is a long time to wait, and it waits");

    ghosts.Advance(10.0 + OrderGhostList::PENDING_TIMEOUT_SECONDS);
    Assert::IsTrue(ghosts.Empty(), L"then it stops promising");
    Assert::AreEqual<std::uint64_t>(1, ghosts.TimedOutCount(), L"and says so in a number");
  }

  TEST_METHOD(AnOrderThatNeverLeftIsForgottenRatherThanBounced)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));

    ghosts.Forget(1);
    Assert::IsTrue(ghosts.Empty(), L"there was no promise to take back");
    Assert::AreEqual<std::uint64_t>(0, ghosts.TimedOutCount(), L"and nothing failed");
  }

  TEST_METHOD(TheListRefusesADuplicateSequenceAndTheUnsentZero)
  {
    OrderGhostList ghosts;
    Assert::IsFalse(ghosts.Add(Intent(0), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0),
                    L"zero means not sent, and a ghost keyed on it could never be matched");
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    Assert::IsFalse(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0), L"and one sequence is one ghost");
  }

  TEST_METHOD(TheListFillsUpRatherThanTrackingWhatItCannotPromote)
  {
    OrderGhostList ghosts;
    for (std::uint32_t seq = 1; seq <= OrderGhostList::MAX_GHOSTS; ++seq)
    {
      Assert::IsTrue(ghosts.Add(Intent(seq), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0), L"up to the cap");
    }
    Assert::IsFalse(ghosts.Add(Intent(OrderGhostList::MAX_GHOSTS + 1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0),
                    L"and no further -- the caller must not send an order whose refusal has nowhere to land");
  }

  TEST_METHOD(ClearingDropsEveryPromiseTheLinkWasCarrying)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    Assert::IsTrue(ghosts.Add(Intent(2), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));

    ghosts.Clear();
    Assert::IsTrue(ghosts.Empty(), L"a disconnect invalidates everything the client was promising");
  }

  TEST_METHOD(TheBounceFractionRunsZeroToOneAndIsZeroForEverythingElse)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));

    const OrderGhost* pending = FindGhost(ghosts, 1);
    Assert::IsNotNull(pending, L"pending");
    Assert::AreEqual(0.0f, OrderGhostList::BounceFraction(*pending, 99.0), L"a pending ghost is not bouncing");

    ghosts.Refuse(1, 5, false, 10.0);
    const OrderGhost* rejected = FindGhost(ghosts, 1);
    Assert::IsNotNull(rejected, L"rejected");
    Assert::AreEqual(0.0f, OrderGhostList::BounceFraction(*rejected, 10.0), L"nothing has moved at the refusal");
    Assert::AreEqual(0.5f, OrderGhostList::BounceFraction(*rejected, 10.0 + OrderGhostList::BOUNCE_SECONDS * 0.5),
                     L"halfway at halfway");
    Assert::AreEqual(1.0f, OrderGhostList::BounceFraction(*rejected, 99.0), L"and clamped past the end");
  }
};

TEST_CLASS(GhostMarkTests)
{
public:
  TEST_METHOD(NoGhostsDrawNothing)
  {
    OverlayMarkList marks;
    const OverlayTuning tuning;
    BuildGhostMarks(std::span<const OrderGhost>{}, tuning, 1.0f, 10.0, marks);
    Assert::AreEqual<std::size_t>(0, marks.marks.size(), L"nothing promised, nothing drawn");
  }

  TEST_METHOD(AnOrderWithNoPlaceOnThisGridDrawsNothingOnIt)
  {
    /*
     * A warp's destination is an anchor, not a point (ADR-016 §3), so there is
     * nowhere on this plane to ring, tick or run a lane to.
     *
     * Left to itself the ghost drew its footprint at whatever the preview
     * carried -- in practice wherever the gesture landed -- and promised the
     * fleet would assemble there while it was in fact leaving the system. The
     * more carefully the formation was solved, the more convincing the wrong
     * answer looked, which is why this is asserted rather than trusted to the
     * solve returning nothing.
     */
    OrderGhostList ghosts;
    OrderPreview elsewhere = Preview(4);
    elsewhere.onThisGrid = false;
    Assert::IsTrue(ghosts.Add(Intent(1), elsewhere, XMFLOAT2{0.0f, 0.0f}, 10.0));

    OverlayMarkList marks;
    const OverlayTuning tuning;
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0, marks);
    Assert::AreEqual<std::size_t>(0, marks.marks.size(), L"no footprint, no ticks, nowhere to put them");

    // And the ghost is still a ghost: it exists, it is pending, and the chrome
    // is what carries it. Dropping the promise would be worse than drawing it
    // in the wrong place.
    Assert::AreEqual<std::size_t>(1, ghosts.Ghosts().size());
    Assert::AreEqual<std::size_t>(1, ghosts.PendingCount());
  }

  TEST_METHOD(AGhostIsAFootprintAndOneTickPerShip)
  {
    // ADR-005 §3's rule made visible: one station tick per ship, never
    // decorative. A footprint showing three ticks for four ships would have
    // lied about the formation before the order left the client.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(4), XMFLOAT2{0.0f, 0.0f}, 10.0));

    OverlayMarkList marks;
    const OverlayTuning tuning;
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0, marks);

    Assert::AreEqual<std::size_t>(5, marks.marks.size(), L"one footprint and four ticks");
    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OverlayKind::OrderFootprint), marks.marks[0].kind,
                                    L"the footprint first");
    for (std::size_t index = 1; index < marks.marks.size(); ++index)
    {
      Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OverlayKind::OrderStation), marks.marks[index].kind,
                                      L"then the stations");
    }
  }

  TEST_METHOD(GhostMarksLieOnThePlaneSoTheyGoInWithTheRings)
  {
    /*
     * The split is what the pass draws with: `[0, ringCount)` is depth-tested
     * against hulls and the rest is not. A footprint appended past the boundary
     * would be drawn in the screen-facing half, where it could never be
     * occluded by the Carrier it is lying under -- and the bug would look like
     * a depth-state problem rather than an ordering one.
     */
    std::vector<SceneEntity> entities;
    SceneEntity ship;
    ship.id = 1;
    ship.planeMetres = XMFLOAT2{0.0f, 0.0f};
    ship.pickRadiusMetres = 20.0f;
    ship.hullGauge = 255;
    ship.shieldGauge = 128;
    entities.push_back(ship);

    const std::uint16_t selected[] = {1};
    OverlayMarkList marks;
    const OverlayTuning tuning;
    BuildOverlayMarks(entities, selected, tuning, 1.0f, marks);
    Assert::AreEqual<std::uint32_t>(1, marks.ringCount, L"one selection ring");
    Assert::AreEqual<std::size_t>(3, marks.marks.size(), L"and its two bars");

    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(2), XMFLOAT2{0.0f, 0.0f}, 10.0));
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0, marks);

    Assert::AreEqual<std::uint32_t>(4, marks.ringCount, L"the ghost's three marks joined the plane-lying half");
    Assert::AreEqual<std::size_t>(6, marks.marks.size(), L"and nothing was lost");
    Assert::AreEqual<std::uint32_t>(2, marks.BarCount(), L"the bars are still the bars");

    for (std::uint32_t index = 0; index < marks.ringCount; ++index)
    {
      Assert::IsTrue(marks.marks[index].kind < static_cast<std::uint16_t>(OverlayKind::FIRST_SCREEN_FACING),
                     L"every mark below the split lies on the plane");
    }
    for (std::size_t index = marks.ringCount; index < marks.marks.size(); ++index)
    {
      Assert::IsTrue(marks.marks[index].kind >= static_cast<std::uint16_t>(OverlayKind::FIRST_SCREEN_FACING),
                     L"and every mark above it faces the screen");
    }
  }

  TEST_METHOD(APendingFootprintIsDashedAndAPromotedOneIsNot)
  {
    // The entire visual difference between the print's PENDING panel and its
    // ACCEPTED one, in one field.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));

    OverlayMarkList marks;
    const OverlayTuning tuning;
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0, marks);
    Assert::AreEqual(tuning.ghostDashCount, marks.marks[0].fill, L"a promise is dashed");

    ghosts.OnVerdict(Ack(1, true, 0, 5), 10.02);
    marks.Clear();
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0, marks);
    Assert::AreEqual<std::uint16_t>(0, marks.marks[0].fill, L"a fact is solid");
  }

  TEST_METHOD(EachStateHasItsOwnColour)
  {
    const OverlayTuning tuning;

    OrderGhostList pending;
    Assert::IsTrue(pending.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    OverlayMarkList pendingMarks;
    BuildGhostMarks(pending.Ghosts(), tuning, 1.0f, 10.0, pendingMarks);
    Assert::AreEqual(tuning.ghostPendingColourRgba, pendingMarks.marks[0].colourRgba, L"pending");

    OrderGhostList underWay;
    Assert::IsTrue(underWay.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    underWay.OnVerdict(Ack(1, true, 0, 5), 10.0);
    OverlayMarkList underWayMarks;
    BuildGhostMarks(underWay.Ghosts(), tuning, 1.0f, 10.0, underWayMarks);
    Assert::AreEqual(tuning.ghostUnderWayColourRgba, underWayMarks.marks[0].colourRgba, L"under way");

    OrderGhostList rejected;
    Assert::IsTrue(rejected.Add(Intent(1), Preview(1), XMFLOAT2{0.0f, 0.0f}, 10.0));
    rejected.Refuse(1, 5, false, 10.0);
    OverlayMarkList rejectedMarks;
    BuildGhostMarks(rejected.Ghosts(), tuning, 1.0f, 10.0, rejectedMarks);
    Assert::AreEqual(tuning.ghostRejectedColourRgba, rejectedMarks.marks[0].colourRgba, L"rejected");
  }

  TEST_METHOD(ARefusedGhostTravelsBackTowardTheFleetKeepingItsShape)
  {
    /*
     * "The ghost snaps back over 150 ms." Back *toward the fleet*, and as one
     * piece: a footprint that collapsed into its own centre would read as the
     * formation imploding rather than as the order being returned.
     */
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1, 1000.0f, 0.0f), Preview(2, 1000.0f, 0.0f), XMFLOAT2{0.0f, 0.0f}, 10.0));
    ghosts.Refuse(1, 5, false, 10.0);

    const OverlayTuning tuning;
    OverlayMarkList atStart;
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0, atStart);

    OverlayMarkList halfway;
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0 + OrderGhostList::BOUNCE_SECONDS * 0.5, halfway);

    Assert::AreEqual(1000.0f, atStart.marks[0].anchorPlane.x, L"it starts where the order pointed");
    Assert::AreEqual(500.0f, halfway.marks[0].anchorPlane.x, L"and is halfway home at halfway");

    // The shape: both stations moved by the same displacement, so the gap
    // between them is what it was.
    const float gapAtStart = atStart.marks[2].anchorPlane.x - atStart.marks[1].anchorPlane.x;
    const float gapHalfway = halfway.marks[2].anchorPlane.x - halfway.marks[1].anchorPlane.x;
    Assert::AreEqual(gapAtStart, gapHalfway, L"the formation keeps its shape on the way back");

    // And it fades out rather than blinking.
    Assert::IsTrue((halfway.marks[0].colourRgba >> 24) < (atStart.marks[0].colourRgba >> 24),
                   L"the bounce fades as it travels");
  }

  TEST_METHOD(ThePuckIsTheSameSizeWhateverTheFleetIs)
  {
    /*
     * The bug this pins, which reached a screenshot before it reached a test.
     *
     * The puck used to be `max(preview.extentMetres, floor)`, and the extent of
     * a Line is *half its length* -- so eleven ships put a green ellipse across
     * the whole viewport, circumscribing a formation it touches at two points.
     * The ticks are the footprint; the ring marks where the player pointed, and
     * where they pointed is one place regardless of how many ships are going.
     */
    const OverlayTuning tuning;

    OrderGhostList small;
    OrderPreview twoShips = Preview(2);
    twoShips.extentMetres = 60.0f;
    Assert::IsTrue(small.Add(Intent(1), twoShips, XMFLOAT2{0.0f, 0.0f}, 10.0));
    OverlayMarkList smallMarks;
    BuildGhostMarks(small.Ghosts(), tuning, 1.0f, 10.0, smallMarks);

    OrderGhostList large;
    OrderPreview manyShips = Preview(40);
    manyShips.extentMetres = 5000.0f; // A Battleship line, kilometres across.
    Assert::IsTrue(large.Add(Intent(1), manyShips, XMFLOAT2{0.0f, 0.0f}, 10.0));
    OverlayMarkList largeMarks;
    BuildGhostMarks(large.Ghosts(), tuning, 1.0f, 10.0, largeMarks);

    Assert::AreEqual(smallMarks.marks[0].radiusMetres, largeMarks.marks[0].radiusMetres,
                     L"the puck is where the order points, not how big the fleet is");
    Assert::AreEqual(tuning.puckRadiusPixels, largeMarks.marks[0].radiusMetres, L"and it is its own size, in pixels");

    // The shape is still reported, and reported truthfully: forty ships get
    // forty ticks, spread over the kilometres the extent describes.
    Assert::AreEqual<std::size_t>(41, largeMarks.marks.size(), L"one puck and forty stations");
    Assert::AreEqual<std::size_t>(3, smallMarks.marks.size(), L"one puck and two stations");
  }

  TEST_METHOD(ThePuckTracksZoomRatherThanTheWorld)
  {
    // Pixels through `_metresPerPixel`, like the gauge bars: the mark holds its
    // size on screen while the world under it shrinks. The mutation this
    // catches is anyone reinstating `extentMetres` as the radius, which would
    // make the two answers below identical.
    const OverlayTuning tuning;
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(Intent(1), Preview(3), XMFLOAT2{0.0f, 0.0f}, 10.0));

    OverlayMarkList closeUp;
    BuildGhostMarks(ghosts.Ghosts(), tuning, 1.0f, 10.0, closeUp);

    OverlayMarkList zoomedOut;
    BuildGhostMarks(ghosts.Ghosts(), tuning, 40.0f, 10.0, zoomedOut); // 40 m per pixel.

    Assert::AreEqual(tuning.puckRadiusPixels * 1.0f, closeUp.marks[0].radiusMetres, L"close up");
    Assert::AreEqual(tuning.puckRadiusPixels * 40.0f, zoomedOut.marks[0].radiusMetres, L"and zoomed out");
    Assert::AreEqual(tuning.stationRadiusPixels * 40.0f, zoomedOut.marks[1].radiusMetres, L"the ticks do the same");
  }
};

/*
 * The queued chain (S12c, `puck-and-wheel.png` §4).
 *
 * One order is one ghost with a leg per waypoint. The merge happens when the
 * authority *accepts* the append rather than when the client sends it, and
 * every test below is really about that one decision: a refused append must
 * bounce alone, and an accepted one must join.
 */
TEST_CLASS(GhostChainTests)
{
public:
  TEST_METHOD(AnAcceptedAppendJoinsTheChainRatherThanStandingBesideIt)
  {
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(FleetIntent(1, CHAIN_SHIPS, false, 1000.0f), Preview(3), XMFLOAT2{0.0f, 0.0f}, 0.0));
    ghosts.OnVerdict(Accept(1, 55), 0.1);
    Assert::AreEqual<std::size_t>(1, ghosts.Count());

    // The queued waypoint is its own pending ghost until the authority agrees:
    // it has to be, or a refusal would have nowhere to land.
    Assert::IsTrue(ghosts.Add(FleetIntent(2, CHAIN_SHIPS, true, 4000.0f), Preview(3, 4000.0f), XMFLOAT2{0.0f, 0.0f}, 0.2));
    Assert::AreEqual<std::size_t>(2, ghosts.Count(), L"two ghosts while the append is in flight");

    ghosts.OnVerdict(Accept(2, 55), 0.3);
    Assert::AreEqual<std::size_t>(1, ghosts.Count(), L"and one once it is accepted");

    const OrderGhost& chain = ghosts.Ghosts()[0];
    Assert::AreEqual<std::uint8_t>(2, chain.queuedLegCount, L"the chain grew a leg");
    Assert::AreEqual(4000.0f, chain.legs[1].targetMetres.x, 0.01f);
    Assert::AreEqual(4000.0f, chain.targetMetres.x, 0.01f, L"and its far end moved to the new waypoint");
    Assert::AreEqual<std::uint32_t>(2, chain.clientOrderSeq,
                                    L"named after the most recent order, exactly as the authority names the group");
  }

  TEST_METHOD(ARefusedAppendBouncesAloneAndLeavesTheChainAlone)
  {
    /*
     * The fifth leg, refused with `QueueFull`. This is the whole reason the
     * merge waits for acceptance: absorbing the waypoint on send would have the
     * refusal retract a plan the player still has.
     */
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(FleetIntent(1, CHAIN_SHIPS, false, 1000.0f), Preview(3), XMFLOAT2{0.0f, 0.0f}, 0.0));
    ghosts.OnVerdict(Accept(1, 55), 0.1);

    Assert::IsTrue(ghosts.Add(FleetIntent(2, CHAIN_SHIPS, true, 4000.0f), Preview(3, 4000.0f), XMFLOAT2{0.0f, 0.0f}, 0.2));
    ghosts.Refuse(2, 5, false, 0.3); // Any reason code; the ghost only forwards it.

    Assert::AreEqual<std::size_t>(2, ghosts.Count(), L"the refused waypoint is still on screen, bouncing");

    const OrderGhost* chain = nullptr;
    const OrderGhost* refused = nullptr;
    for (const OrderGhost& ghost : ghosts.Ghosts())
    {
      (ghost.state == GhostState::Rejected ? refused : chain) = &ghost;
    }
    Assert::IsNotNull(chain);
    Assert::IsNotNull(refused);
    Assert::AreEqual<std::uint8_t>(1, chain->queuedLegCount, L"the chain never took the leg it was refused");
    Assert::IsTrue(chain->state == GhostState::UnderWay, L"and is not bouncing");
    Assert::AreEqual<std::uint32_t>(1, chain->clientOrderSeq, L"nor was it renamed by an order that failed");
  }

  TEST_METHOD(AnAppendForDifferentShipsStartsItsOwnChain)
  {
    /*
     * The chain is resolved by the ship the authority resolves it by -- the
     * first one the order names (`World::IngestOrders`). A second fleet queued
     * while the first is under way is a second plan, not a fifth waypoint on
     * somebody else's.
     */
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(FleetIntent(1, CHAIN_SHIPS, false, 1000.0f), Preview(3), XMFLOAT2{0.0f, 0.0f}, 0.0));
    ghosts.OnVerdict(Accept(1, 55), 0.1);

    Assert::IsTrue(ghosts.Add(FleetIntent(2, OTHER_SHIPS, true, 4000.0f), Preview(2, 4000.0f), XMFLOAT2{0.0f, 0.0f}, 0.2));
    ghosts.OnVerdict(Accept(2, 56), 0.3);

    Assert::AreEqual<std::size_t>(2, ghosts.Count(), L"two fleets, two plans");
    for (const OrderGhost& ghost : ghosts.Ghosts())
    {
      Assert::AreEqual<std::uint8_t>(1, ghost.queuedLegCount);
    }
  }

  TEST_METHOD(AnAppendWithNoChainToJoinStandsOnItsOwn)
  {
    // The client mirrors the authority's fallback: validation passes because
    // there is no queue to be full, and the order becomes a plan of its own.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(FleetIntent(9, CHAIN_SHIPS, true, 4000.0f), Preview(3, 4000.0f), XMFLOAT2{0.0f, 0.0f}, 0.0));
    ghosts.OnVerdict(Accept(9, 77), 0.1);

    Assert::AreEqual<std::size_t>(1, ghosts.Count());
    Assert::AreEqual<std::uint8_t>(1, ghosts.Ghosts()[0].queuedLegCount);
    Assert::IsTrue(ghosts.Ghosts()[0].state == GhostState::UnderWay);
    Assert::AreEqual<std::uint32_t>(77, ghosts.Ghosts()[0].serverOrderId);
  }

  TEST_METHOD(TheChainStopsGrowingAtTheEnginesCap)
  {
    // The queue's real cap is the game's and the authority enforces it; this is
    // the engine refusing to write past its own buffer, which is a different
    // promise and has to hold whatever the game says.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(FleetIntent(1, CHAIN_SHIPS, false, 1000.0f), Preview(3), XMFLOAT2{0.0f, 0.0f}, 0.0));
    ghosts.OnVerdict(Accept(1, 55), 0.1);

    for (std::uint32_t seq = 2; seq <= MAX_GHOST_LEGS + 3; ++seq)
    {
      const float x = 1000.0f + static_cast<float>(seq) * 500.0f;
      Assert::IsTrue(ghosts.Add(FleetIntent(seq, CHAIN_SHIPS, true, x), Preview(3, x), XMFLOAT2{0.0f, 0.0f}, 0.2));
      ghosts.OnVerdict(Accept(seq, 55), 0.3);
    }

    std::uint8_t longest = 0;
    for (const OrderGhost& ghost : ghosts.Ghosts())
    {
      longest = std::max(longest, ghost.queuedLegCount);
    }
    Assert::AreEqual<std::uint8_t>(static_cast<std::uint8_t>(MAX_GHOST_LEGS), longest, L"full, and no further");
  }

  TEST_METHOD(TheChainRetractsToItsPreviousWaypointAndNotToTheFleet)
  {
    // `RetractTowardMetres`, which the footprint ring and the lane both read so
    // they cannot come home to two different places.
    OrderGhostList ghosts;
    Assert::IsTrue(ghosts.Add(FleetIntent(1, CHAIN_SHIPS, false, 1000.0f), Preview(3), XMFLOAT2{-9000.0f, 0.0f}, 0.0));
    ghosts.OnVerdict(Accept(1, 55), 0.1);

    const OrderGhost& single = ghosts.Ghosts()[0];
    Assert::AreEqual(-9000.0f, single.RetractTowardMetres().x, 0.01f, L"one leg goes home to the fleet");

    Assert::IsTrue(ghosts.Add(FleetIntent(2, CHAIN_SHIPS, true, 4000.0f), Preview(3, 4000.0f), XMFLOAT2{-9000.0f, 0.0f}, 0.2));
    ghosts.OnVerdict(Accept(2, 55), 0.3);

    Assert::AreEqual(1000.0f, ghosts.Ghosts()[0].RetractTowardMetres().x, 0.01f,
                     L"a queued one goes back to the waypoint before it");
  }
};

} // namespace NeuronClientTests
