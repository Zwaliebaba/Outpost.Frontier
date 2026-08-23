#include "pch.h"
#include "CppUnitTest.h"

#include "ApproachChain.h"
#include "AutoFollow.h"
#include "HudPalette.h"
#include "ToastStack.h"
#include "EntityTransits.h"
#include "OverlayMark.h"
#include "RenderWorld.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * The client's half of a two-step order, and the marks that go with it
 * (ADR-017 §2, §4, §5 -- Station Build Order T2).
 *
 * Three components, and the reason they share a file is that they share a
 * subject: a fleet flying at a station, arriving, and the second afterwards.
 *
 * **What none of these tests can reach** is what the marks look like. That the
 * shimmer pulses and the transit ring fades are numbers, and numbers are here;
 * that either reads as what it means needs a frame on a screen, which R1 says
 * in writing and this file does not pretend otherwise.
 */

namespace NeuronClientTests
{
namespace
{

[[nodiscard]] SceneEntity Ship(std::uint16_t _id, float _x, float _y, std::uint8_t _statusBits = 0)
{
  SceneEntity entity;
  entity.id = _id;
  entity.planeMetres = XMFLOAT2{_x, _y};
  entity.pickRadiusMetres = 12.0f;
  entity.statusBits = _statusBits;
  return entity;
}

[[nodiscard]] ContextAction DockAction(std::uint16_t _anchor = 7)
{
  ContextAction action;
  action.kind = 3;
  action.label = "DOCK";
  action.anchor = _anchor;
  action.available = true;
  return action;
}

[[nodiscard]] std::uint32_t CountOfKind(const OverlayMarkList& _list, OverlayKind _kind)
{
  std::uint32_t count = 0;
  for (const OverlayMark& mark : _list.marks)
  {
    if (mark.kind == static_cast<std::uint16_t>(_kind))
    {
      ++count;
    }
  }
  return count;
}

/// The bit this game means by undock protection. Named locally on purpose: the
/// library under test does not have a name for it, and a test that imported one
/// from GameLogic would be asserting the leak it is here to rule out.
constexpr std::uint8_t PROTECTED_BIT = 1u << 0;

[[nodiscard]] LocationBlock Block(std::uint16_t _anchor, std::uint16_t _ships, bool _crossing)
{
  LocationBlock block;
  block.anchor = _anchor;
  block.shipCount = _ships;
  block.stateLabel = _crossing ? "IN WARP" : "ON GRID";
  block.etaSeconds = _crossing ? 42.0f : -1.0f;
  return block;
}

} // namespace

TEST_CLASS(ApproachChainTests)
{
public:
  TEST_METHOD(AChainStartsInactiveAndSaysNothing)
  {
    const ApproachChain chain;
    Assert::IsFalse(chain.Active());
    Assert::IsNull(chain.Label());
  }

  TEST_METHOD(BeginningCarriesTheVerbAndTheFleet)
  {
    ApproachChain chain;
    const std::vector<EntityId> ships = {4, 5, 6};
    Assert::IsTrue(chain.Begin(DockAction(), ships));

    Assert::IsTrue(chain.Active());
    Assert::AreEqual<std::uint16_t>(3, chain.Kind());
    Assert::AreEqual<std::uint16_t>(7, chain.Anchor());
    Assert::AreEqual<std::size_t>(3, chain.Ships().size());
    Assert::AreEqual(std::string("DOCK"), std::string(chain.Label()));
  }

  TEST_METHOD(TheFleetIsCopiedSoLaterSelectionEditsCannotChangeIt)
  {
    // The whole reason the chain holds its own copy: the selection is a live
    // thing the player keeps editing, and an order that followed it would
    // quietly become an order about different ships.
    ApproachChain chain;
    std::vector<EntityId> ships = {4, 5, 6};
    Assert::IsTrue(chain.Begin(DockAction(), ships));

    ships.clear();
    ships.push_back(99);

    Assert::AreEqual<std::size_t>(3, chain.Ships().size());
    Assert::AreEqual<std::uint16_t>(4, chain.Ships()[0]);
  }

  TEST_METHOD(AnUnavailableActionOrAnEmptySelectionIsRefused)
  {
    ApproachChain chain;
    ContextAction unavailable = DockAction();
    unavailable.available = false;
    const std::vector<EntityId> ships = {4};

    Assert::IsFalse(chain.Begin(unavailable, ships));
    Assert::IsFalse(chain.Begin(DockAction(), {}));
    Assert::IsFalse(chain.Active());
  }

  TEST_METHOD(MoreShipsThanTheChainHoldsIsRefusedRatherThanTruncated)
  {
    // Refused, because an approach that silently dropped members would dock a
    // different fleet than the one the player pointed at.
    ApproachChain chain;
    std::vector<EntityId> ships;
    for (std::uint16_t id = 0; id < ApproachChain::MAX_SHIPS + 1; ++id)
    {
      ships.push_back(static_cast<std::uint16_t>(id + 1));
    }

    Assert::IsFalse(chain.Begin(DockAction(), ships));
    Assert::IsFalse(chain.Active());
  }

  TEST_METHOD(TheApproachLegsRefusalCancelsTheChain)
  {
    ApproachChain chain;
    const std::vector<EntityId> ships = {4};
    Assert::IsTrue(chain.Begin(DockAction(), ships));
    chain.NoteApproachSent(11);

    chain.NoteOrderRefused(10); // Somebody else's order.
    Assert::IsTrue(chain.Active());

    chain.NoteOrderRefused(11);
    Assert::IsFalse(chain.Active());
  }

  TEST_METHOD(SequenceZeroNeverMatchesAChainWhoseLegHasNotLeft)
  {
    // Zero is "nothing sent yet" and is also what a malformed payload acks
    // with, so it must never cancel a chain that has not sent anything.
    ApproachChain chain;
    const std::vector<EntityId> ships = {4};
    Assert::IsTrue(chain.Begin(DockAction(), ships));

    chain.NoteOrderRefused(0);
    Assert::IsTrue(chain.Active());
  }

  TEST_METHOD(AMemberLeavingTheWorldCancelsTheChain)
  {
    ApproachChain chain;
    const std::vector<EntityId> ships = {4, 5};
    Assert::IsTrue(chain.Begin(DockAction(), ships));

    const std::vector<EntityId> stillThere = {4, 5, 9};
    chain.NoteWorld(stillThere);
    Assert::IsTrue(chain.Active());

    const std::vector<EntityId> oneGone = {4, 9};
    chain.NoteWorld(oneGone);
    Assert::IsFalse(chain.Active());
  }

  TEST_METHOD(ASecondApproachReplacesTheFirst)
  {
    // One at a time on purpose: two chips promising two different arrivals is
    // a HUD saying something the player did not ask for.
    ApproachChain chain;
    Assert::IsTrue(chain.Begin(DockAction(7), std::vector<EntityId>{4}));
    Assert::IsTrue(chain.Begin(DockAction(8), std::vector<EntityId>{5, 6}));

    Assert::AreEqual<std::uint16_t>(8, chain.Anchor());
    Assert::AreEqual<std::size_t>(2, chain.Ships().size());
    Assert::AreEqual<std::uint32_t>(0, chain.ApproachSeq()); // The new chain's leg has not left.
  }
};

TEST_CLASS(EntityTransitTests)
{
public:
  TEST_METHOD(TheFirstSceneOfASessionIsNotAFleetArriving)
  {
    EntityTransitList transits;
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f), Ship(2, 10.0f, 0.0f)};
    transits.Note(entities, 100.0);

    Assert::IsTrue(transits.Empty());
  }

  TEST_METHOD(AShipThatVanishesLeavesARingWhereItWas)
  {
    EntityTransitList transits;
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f), Ship(2, 400.0f, 300.0f)}, 100.0);
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f)}, 100.1);

    Assert::AreEqual<std::size_t>(1, transits.Transits().size());
    const EntityTransit& transit = transits.Transits()[0];
    Assert::IsFalse(transit.arriving);
    // Its last known place, which is the only place the mark can go: by the
    // time the frame notices, there is nothing left to ask.
    Assert::AreEqual(400.0f, transit.planeMetres.x);
    Assert::AreEqual(300.0f, transit.planeMetres.y);
  }

  TEST_METHOD(AShipThatAppearsIsAnArrival)
  {
    EntityTransitList transits;
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f)}, 100.0);
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f), Ship(9, 50.0f, 0.0f)}, 100.1);

    Assert::AreEqual<std::size_t>(1, transits.Transits().size());
    Assert::IsTrue(transits.Transits()[0].arriving);
  }

  TEST_METHOD(AShipThatMerelyMovedIsNotAnEvent)
  {
    EntityTransitList transits;
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f)}, 100.0);
    transits.Note(std::vector<SceneEntity>{Ship(1, 900.0f, 900.0f)}, 100.1);

    Assert::IsTrue(transits.Empty());
  }

  TEST_METHOD(AFadeRetiresAfterItsSecond)
  {
    EntityTransitList transits;
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f), Ship(2, 5.0f, 0.0f)}, 100.0);
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f)}, 100.1);
    Assert::AreEqual<std::size_t>(1, transits.Transits().size());

    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f)}, 100.1 + EntityTransitList::FADE_SECONDS);
    Assert::IsTrue(transits.Empty());
  }

  TEST_METHOD(ClearingForgetsTheLastFrameToo)
  {
    // A link dropping is not a fleet going anywhere: without this the fleet on
    // screen would all read as departures and the next session's as arrivals.
    EntityTransitList transits;
    transits.Note(std::vector<SceneEntity>{Ship(1, 0.0f, 0.0f), Ship(2, 5.0f, 0.0f)}, 100.0);
    transits.Clear();

    transits.Note(std::vector<SceneEntity>{Ship(3, 0.0f, 0.0f)}, 101.0);
    Assert::IsTrue(transits.Empty());
  }

  TEST_METHOD(AGridSwitchIsNotAFleetLeavingAndAnotherArriving)
  {
    /*
     * The defect the ~200 ms settle exists to prevent, pinned at the level the
     * settle protects (ADR-016 §9).
     *
     * A grid switch changes every id on screen at once, and this list's whole
     * job is to notice ids appearing and disappearing -- so left to itself it
     * reports a whole fleet leaving and another arriving, and draws a ring for
     * every one of them. Nobody went anywhere; the camera moved. `ClientApp`
     * re-baselines through `Clear()` for the window it takes the interpolation
     * buffer to refill, and this is the assertion that re-baselining is what
     * `Clear()` actually does for a *populated* scene rather than only for an
     * empty one.
     */
    EntityTransitList transits;
    std::vector<SceneEntity> before;
    std::vector<SceneEntity> after;
    for (std::uint16_t id = 1; id <= 20; ++id)
    {
      before.push_back(Ship(id, static_cast<float>(id) * 10.0f, 0.0f));
      after.push_back(Ship(static_cast<std::uint16_t>(id + 500), static_cast<float>(id) * 10.0f, 400.0f));
    }
    transits.Note(before, 100.0);

    // What the settle does every frame it is running.
    transits.Clear();
    transits.Note(after, 100.05);

    Assert::IsTrue(transits.Empty(), L"a grid switch must not read as forty ships leaving");

    // And the frame after the settle is an ordinary frame again: one ship
    // leaving the new grid is still one ship leaving.
    after.pop_back();
    transits.Note(after, 100.2);
    Assert::AreEqual<std::size_t>(1, transits.Transits().size());
    Assert::IsFalse(transits.Transits()[0].arriving);
  }

  TEST_METHOD(AWholeFleetUndockingIsAWholeFleetOfRings)
  {
    EntityTransitList transits;
    std::vector<SceneEntity> before;
    for (std::uint16_t id = 1; id <= 24; ++id)
    {
      before.push_back(Ship(id, static_cast<float>(id) * 10.0f, 0.0f));
    }
    transits.Note(before, 100.0);
    transits.Note(std::vector<SceneEntity>{}, 100.1);

    Assert::AreEqual<std::size_t>(24, transits.Transits().size());
    Assert::AreEqual<std::uint64_t>(0, transits.DroppedCount());
  }
};

TEST_CLASS(AutoFollowTests)
{
public:
  TEST_METHOD(NothingToFollowWithNoBlocks)
  {
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget({}, 10));
  }

  TEST_METHOD(AnythingOfYoursHereMeansStay)
  {
    /*
     * The rule that keeps the camera from overruling the player. A station where
     * their ships are docked is a place they have a reason to be, even though
     * the fleet is somewhere else entirely.
     */
    const std::vector<LocationBlock> blocks = {
      Block(10, 3, false),   // docked, at the grid being watched
      Block(20, 12, false),  // the fleet, elsewhere
    };
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget(blocks, 10));
  }

  TEST_METHOD(AnEmptyGridFollowsTheFleet)
  {
    const std::vector<LocationBlock> blocks = {Block(20, 12, false)};
    Assert::AreEqual<std::uint16_t>(20, FollowTarget(blocks, 10));
  }

  TEST_METHOD(ACrossingIsNotADestination)
  {
    // A fleet mid-warp has no grid to stand on, and MayView would rightly refuse
    // a request to watch a world that does not hold it yet.
    const std::vector<LocationBlock> blocks = {Block(20, 12, true)};
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget(blocks, 10));
  }

  /*
   * ADR-018 D16's second presence edge, and what it is really about:
   * `FollowTarget` answers "stay put" for two situations that look identical
   * from outside and are opposite to a player.
   */
  TEST_METHOD(EverythingCrossingIsNotTheSameAsAReasonToStay)
  {
    /*
     * The whole distinction in one pair. Both calls to `FollowTarget` say
     * `NO_FOLLOW_TARGET`; one is a player with ships where they are standing
     * and one is a player watching an empty grid while everything they own is
     * mid-warp. Only the second goes to the map.
     */
    const std::vector<LocationBlock> staying = {Block(10, 3, false), Block(20, 12, true)};
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget(staying, 10));
    Assert::IsFalse(EveryFleetIsCrossing(staying, 10), L"ships are here, which is a reason to be here");

    const std::vector<LocationBlock> crossing = {Block(20, 12, true)};
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget(crossing, 10));
    Assert::IsTrue(EveryFleetIsCrossing(crossing, 10), L"nothing here and nothing to go to");
  }

  TEST_METHOD(AGridToGoToIsFollowsBusinessRatherThanTheMaps)
  {
    // One fleet crossing and one standing somewhere else: there *is* a grid
    // worth watching, so this is a follow and not a fallback. Both answers have
    // to agree about that or the client would push the map and then move the
    // camera out from under it.
    const std::vector<LocationBlock> blocks = {Block(20, 12, true), Block(30, 4, false)};
    Assert::AreEqual<std::uint16_t>(30, FollowTarget(blocks, 10));
    Assert::IsFalse(EveryFleetIsCrossing(blocks, 10));
  }

  TEST_METHOD(APlayerWithNothingIsNotCrossing)
  {
    /*
     * No blocks is a client that has been told nothing yet -- the frames before
     * the first summary lands -- and yanking the player to the map for it would
     * be the HUD reacting to its own ignorance. A block with no ships in it is
     * the same statement written down.
     */
    Assert::IsFalse(EveryFleetIsCrossing({}, 10));
    const std::vector<LocationBlock> empty = {Block(20, 0, true)};
    Assert::IsFalse(EveryFleetIsCrossing(empty, 10));
  }

  TEST_METHOD(WatchingTheGridAFleetIsArrivingAtIsAReasonToStay)
  {
    /*
     * The case the here-guard exists for, and the one the obvious test misses.
     *
     * A player watching grid 20 while their fleet crosses *to* grid 20 is doing
     * the most reasonable thing on this screen: waiting to see it land. Every
     * block is a crossing, so a rule that only asked "is anything standing
     * anywhere" would push them to the map at the exact moment they were
     * watching the arrival they planned.
     */
    const std::vector<LocationBlock> blocks = {Block(20, 12, true)};
    Assert::IsFalse(EveryFleetIsCrossing(blocks, 20), L"the crossing is coming here, and they are watching it");
    Assert::IsTrue(EveryFleetIsCrossing(blocks, 10), L"the same crossing, watched from somewhere else");
  }

  TEST_METHOD(SeveralCrossingsAreStillJustCrossings)
  {
    // The ordinary shape of a multi-fleet move: everything is in the air at
    // once, and there is nowhere to point a camera until something lands.
    const std::vector<LocationBlock> blocks = {Block(20, 12, true), Block(30, 4, true), Block(40, 1, true)};
    Assert::IsTrue(EveryFleetIsCrossing(blocks, 10));
  }

  TEST_METHOD(TheBiggestFleetWinsAndTiesBreakByAnchor)
  {
    // Two grids holding the same count is a real state, and the camera has to
    // land the same way twice or one situation plays differently on two machines.
    const std::vector<LocationBlock> bigger = {Block(30, 4, false), Block(20, 9, false)};
    Assert::AreEqual<std::uint16_t>(20, FollowTarget(bigger, 10));

    const std::vector<LocationBlock> tied = {Block(30, 6, false), Block(20, 6, false)};
    Assert::AreEqual<std::uint16_t>(20, FollowTarget(tied, 10));

    const std::vector<LocationBlock> reversed = {Block(20, 6, false), Block(30, 6, false)};
    Assert::AreEqual<std::uint16_t>(20, FollowTarget(reversed, 10), L"order of arrival must not decide it");
  }

  TEST_METHOD(ARefusedGridIsNotAskedForAgain)
  {
    // Otherwise the condition that produced the refusal produces it again the
    // moment the reply lands, and the player gets a toast per frame.
    const std::vector<LocationBlock> blocks = {Block(20, 12, false)};
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget(blocks, 10, 20));
  }

  TEST_METHOD(AnEmptyPlaceIsNotAPlace)
  {
    const std::vector<LocationBlock> blocks = {Block(20, 0, false)};
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget(blocks, 10));
  }

  /*
   * The pin's rule, asserted through the predicate the client branches on
   * (ADR-018 D16's *first* presence edge, U6).
   *
   * D16 gives a pinned camera that loses presence one destination -- the map --
   * and the condition for "lost presence" is not a third question: it is
   * `FollowTarget` finding somewhere to go, which is only ever true when
   * nothing of the player's is on the grid they are watching. The pair below is
   * what the pinned branch reads, and it is here so that a change to
   * `FollowTarget`'s here-guard breaks the pin's rule visibly rather than
   * turning it into a camera that freezes on an empty grid.
   */
  TEST_METHOD(TheConditionAPinnedCameraFallsToTheMapOnIsTheFollowItSuppresses)
  {
    const std::vector<LocationBlock> stay = {Block(10, 3, false), Block(20, 12, false)};
    Assert::AreEqual<std::uint16_t>(NO_FOLLOW_TARGET, FollowTarget(stay, 10),
                                    L"ships here: a pin holds and nothing else happens");

    const std::vector<LocationBlock> lost = {Block(20, 12, false)};
    Assert::AreEqual<std::uint16_t>(20, FollowTarget(lost, 10),
                                    L"nothing here and somewhere it went: unpinned this follows, pinned it maps");
  }
};

/*
 * The fleet stepper (U6's focus polish, Tab).
 *
 * Pure and therefore assertable, which is the reason it is a free function at
 * all -- the surrounding call is a selection and a camera and neither can be
 * driven in a test project with no device.
 */
TEST_CLASS(FleetCycleTests)
{
public:
  TEST_METHOD(APlayerWithNoFleetsStepsNowhere)
  {
    Assert::AreEqual<std::uint32_t>(NO_FLEET_CYCLED, NextFleetInCycle({}, NO_FLEET_CYCLED));
    Assert::AreEqual<std::uint32_t>(NO_FLEET_CYCLED, NextFleetInCycle({}, 7));
  }

  TEST_METHOD(TheFirstPressTakesTheFirstFleet)
  {
    // The order on screen, top to bottom, which is what makes the key
    // predictable to somebody who has read the panel.
    const std::vector<std::uint32_t> keys = {3, 7, 9};
    Assert::AreEqual<std::uint32_t>(3, NextFleetInCycle(keys, NO_FLEET_CYCLED));
  }

  TEST_METHOD(ItStepsAndWraps)
  {
    const std::vector<std::uint32_t> keys = {3, 7, 9};
    Assert::AreEqual<std::uint32_t>(7, NextFleetInCycle(keys, 3));
    Assert::AreEqual<std::uint32_t>(9, NextFleetInCycle(keys, 7));
    Assert::AreEqual<std::uint32_t>(3, NextFleetInCycle(keys, 9), L"round the horn");
  }

  TEST_METHOD(AFleetThatWentAwayRestartsTheCycle)
  {
    /*
     * The case identity is used for, and the reason an index would have been
     * wrong.
     *
     * A wing docks between two presses and its key leaves the list. An index
     * would step to whatever inherited the slot -- silently, and only sometimes
     * -- so the player would find the key skipping a fleet with no way to tell
     * that it had. Restarting is visible and honest: the next press is the top
     * of the list.
     */
    const std::vector<std::uint32_t> keys = {3, 7, 9};
    Assert::AreEqual<std::uint32_t>(3, NextFleetInCycle(keys, 5));
  }

  TEST_METHOD(AWingAndAPlaceWithTheSameNumberAreDifferentStops)
  {
    /*
     * Wing 4 and anchor 4 are both small numbers and both real, so the walk
     * would fold them into one stop without the flag bit -- and the cycle would
     * skip whichever came second, on exactly the systems where a player has a
     * wing flying and ships docked elsewhere.
     */
    constexpr std::uint32_t PLACE = 0x8000'0000u;
    const std::vector<std::uint32_t> keys = {4, PLACE | 4};
    Assert::AreEqual<std::uint32_t>(PLACE | 4, NextFleetInCycle(keys, 4));
    Assert::AreEqual<std::uint32_t>(4, NextFleetInCycle(keys, PLACE | 4));
  }

  TEST_METHOD(OneFleetStepsToItself)
  {
    // Not a no-op on a real client: the press re-takes the wing and re-frames
    // it, which is what a player pressing it expects to happen.
    const std::vector<std::uint32_t> keys = {11};
    Assert::AreEqual<std::uint32_t>(11, NextFleetInCycle(keys, 11));
  }
};

TEST_CLASS(ToastActionTests)
{
public:
  TEST_METHOD(ACriticalWithoutAnActionBehavesExactlyAsBefore)
  {
    ToastStack stack;
    Assert::IsTrue(stack.Raise(ToastPriority::Critical, 1, "HULL CRITICAL", "MARROW", 100.0));
    Assert::AreEqual<std::size_t>(1, stack.Visible().size());
    Assert::IsFalse(stack.Visible()[0].HasAction());
    Assert::IsTrue(stack.Visible()[0].WaitsForAction());

    // Waits for ever, which is what "until acted" means.
    stack.Advance(100.0 + 10000.0);
    Assert::AreEqual<std::size_t>(1, stack.Visible().size());
  }

  TEST_METHOD(AnActionIsCarriedAndHandedBackUntouched)
  {
    ToastStack stack;
    Assert::IsTrue(stack.RaiseWithAction(ToastPriority::Critical, 1, "FLEET UNDER FIRE", "KESSLER", "JUMP TO", 4242, 100.0));
    Assert::IsTrue(stack.Visible()[0].HasAction());
    Assert::AreEqual(std::string("JUMP TO"), std::string(stack.Visible()[0].actionLabel));

    std::uint32_t key = 0;
    Assert::IsTrue(stack.Act(0, key, 105.0));
    Assert::AreEqual<std::uint32_t>(4242, key, L"the raiser's own number, not the engine's index");
  }

  TEST_METHOD(AnActedCriticalCollapsesAfterThirtySecondsAndNotBefore)
  {
    ToastStack stack;
    Assert::IsTrue(stack.RaiseWithAction(ToastPriority::Critical, 1, "HEAD", "DETAIL", "VIEW", 7, 100.0));

    // Untouched, it waits: a critical is the level that does not time out.
    stack.Advance(100.0 + 120.0);
    Assert::AreEqual<std::size_t>(1, stack.Visible().size());

    std::uint32_t key = 0;
    Assert::IsTrue(stack.Act(0, key, 200.0));
    Assert::IsTrue(stack.Visible()[0].Acted());
    Assert::IsFalse(stack.Visible()[0].WaitsForAction(), L"acting is what gives a waiting row an expiry");

    stack.Advance(200.0 + TOAST_ACTED_COLLAPSE_SECONDS - 1.0);
    Assert::AreEqual<std::size_t>(1, stack.Visible().size(), L"gone too early would take the confirmation with it");

    stack.Advance(200.0 + TOAST_ACTED_COLLAPSE_SECONDS);
    Assert::AreEqual<std::size_t>(0, stack.Visible().size());
  }

  TEST_METHOD(ActingTwiceOrOnANonActionRowIsRefused)
  {
    ToastStack stack;
    Assert::IsTrue(stack.Raise(ToastPriority::Critical, 1, "NO CHIP", "", 100.0));
    std::uint32_t key = 99;
    Assert::IsFalse(stack.Act(0, key, 101.0), L"a row with no action has no key to give");
    Assert::AreEqual<std::uint32_t>(99, key, L"and must not have written one");
    Assert::IsFalse(stack.Act(7, key, 101.0), L"nor may an index past the end");

    ToastStack acted;
    Assert::IsTrue(acted.RaiseWithAction(ToastPriority::Critical, 1, "H", "D", "GO", 5, 100.0));
    Assert::IsTrue(acted.Act(0, key, 101.0));
    Assert::IsFalse(acted.Act(0, key, 102.0), L"a second press must not restart the collapse");
  }

  TEST_METHOD(CoalescingKeepsTheFirstRowsAction)
  {
    // The chip may already be under a finger, and a button that changes what it
    // does between the reach and the press is the worst surprise a HUD can
    // spring.
    ToastStack stack;
    Assert::IsTrue(stack.RaiseWithAction(ToastPriority::Critical, 1, "HEAD", "FIRST", "JUMP TO", 11, 100.0));
    Assert::IsFalse(stack.RaiseWithAction(ToastPriority::Critical, 1, "HEAD", "SECOND", "VIEW", 22, 101.0));

    Assert::AreEqual<std::uint32_t>(2, stack.Visible()[0].count);
    Assert::AreEqual(std::string("JUMP TO"), std::string(stack.Visible()[0].actionLabel));
    std::uint32_t key = 0;
    Assert::IsTrue(stack.Act(0, key, 102.0));
    Assert::AreEqual<std::uint32_t>(11, key);
  }

  TEST_METHOD(AnActedRowThatCoalescesDoesNotReturnToWaiting)
  {
    /*
     * The trap the dwell guard exists for. Critical has no dwell, so the
     * coalesce path's "restart the dwell" would have set `now + -1` -- an expiry
     * in the past, erasing the row on the next sweep. No critical could reach
     * that branch before acts existed, because one always waited.
     */
    ToastStack stack;
    Assert::IsTrue(stack.RaiseWithAction(ToastPriority::Critical, 1, "HEAD", "D", "GO", 5, 100.0));
    std::uint32_t key = 0;
    Assert::IsTrue(stack.Act(0, key, 100.0));

    Assert::IsFalse(stack.Raise(ToastPriority::Critical, 1, "HEAD", "AGAIN", 101.0));
    stack.Advance(102.0);
    Assert::AreEqual<std::size_t>(1, stack.Visible().size(), L"the row must not vanish the frame after it coalesced");
  }
};

TEST_CLASS(HudPaletteTests)
{
public:
  TEST_METHOD(AnUnknownNameFallsBackToTheDefault)
  {
    const HudPalette fallback = ResolveHudPalette("chartreuse");
    const HudPalette base = ResolveHudPalette("default");
    Assert::AreEqual(base.phosphor, fallback.phosphor);
    Assert::AreEqual(base.hostile, fallback.hostile);
  }

  TEST_METHOD(DeuteranopiaMovesTheOwnColourAndTritanopiaDoesNot)
  {
    const HudPalette base = ResolveHudPalette("default");
    const HudPalette deut = ResolveHudPalette("deuteranopia");
    const HudPalette trit = ResolveHudPalette("tritanopia");

    // Own-fleet green and hostile red are the pair that collapses for
    // deuteranopia, so the whole chrome ramp is re-anchored; tritanopia
    // separates green and red fine and keeps it.
    Assert::AreNotEqual(base.phosphor, deut.phosphor);
    Assert::AreEqual(base.phosphor, trit.phosphor);
    Assert::AreNotEqual(base.hostile, deut.hostile);
    Assert::AreNotEqual(base.hostile, trit.hostile);
  }

  TEST_METHOD(EveryTableDerivesItsLinesFromItsOwnColour)
  {
    // The one rule that keeps a palette a table rather than forty independent
    // decisions: a table cannot end up with borders from one hue and text from
    // another.
    for (const std::string_view name : {"default", "deuteranopia", "tritanopia"})
    {
      const HudPalette table = ResolveHudPalette(name);
      Assert::AreEqual(WithAlpha(table.phosphor, 0x4Du), table.borderStrong);
      Assert::AreEqual(WithAlpha(table.phosphor, 0x38u), table.border);
      Assert::AreEqual(WithAlpha(table.phosphor, 0x24u), table.rule);
    }
  }

  TEST_METHOD(EverySemanticIsDistinctWithinATable)
  {
    // The property a colour-vision table exists for. Two semantics that packed
    // to the same value would be a palette that had lost the distinction it was
    // authored to keep.
    for (const std::string_view name : {"default", "deuteranopia", "tritanopia"})
    {
      const HudPalette t = ResolveHudPalette(name);
      const std::uint32_t semantics[] = {t.hostile, t.critical, t.caution, t.allied, t.neutral};
      for (std::size_t a = 0; a < std::size(semantics); ++a)
      {
        for (std::size_t b = a + 1; b < std::size(semantics); ++b)
        {
          Assert::AreNotEqual(semantics[a], semantics[b]);
        }
      }
    }
  }
};

TEST_CLASS(StatusAndTransitMarkTests)
{
public:
  TEST_METHOD(NoMaskMeansNoMarksAtAll)
  {
    // The default, and every game but this one: the engine carries the byte and
    // has no opinion about a single bit in it.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, PROTECTED_BIT)};
    OverlayMarkList marks;
    BuildStatusMarks(entities, OverlayTuning{}, 0.0, marks);

    Assert::AreEqual<std::size_t>(0, marks.marks.size());
  }

  TEST_METHOD(AFlaggedShipIsMarkedAndAnUnflaggedOneIsNot)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, PROTECTED_BIT), Ship(2, 100.0f, 0.0f, 0)};
    OverlayTuning tuning;
    tuning.statusMarkBits = PROTECTED_BIT;

    OverlayMarkList marks;
    BuildStatusMarks(entities, tuning, 0.0, marks);

    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::StatusMarker));
    Assert::AreEqual(0.0f, marks.marks[0].anchorPlane.x);
  }

  TEST_METHOD(ABitOutsideTheMaskIsIgnored)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 1u << 3)};
    OverlayTuning tuning;
    tuning.statusMarkBits = PROTECTED_BIT;

    OverlayMarkList marks;
    BuildStatusMarks(entities, tuning, 0.0, marks);
    Assert::AreEqual<std::size_t>(0, marks.marks.size());
  }

  TEST_METHOD(TheShimmerPulsesButNeverGoesOut)
  {
    /*
     * The floor matters more than the swing: a mark that reached zero would be
     * invisible for part of every cycle, and a player glancing at the plane at
     * the wrong instant would read a protected ship as an unprotected one.
     */
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, PROTECTED_BIT)};
    OverlayTuning tuning;
    tuning.statusMarkBits = PROTECTED_BIT;

    std::uint8_t lowest = 255;
    std::uint8_t highest = 0;
    for (int step = 0; step < 32; ++step)
    {
      OverlayMarkList marks;
      BuildStatusMarks(entities, tuning, tuning.statusMarkPulseSeconds * step / 32.0, marks);
      Assert::AreEqual<std::size_t>(1, marks.marks.size());
      const auto alpha = static_cast<std::uint8_t>(marks.marks[0].colourRgba >> 24);
      lowest = alpha < lowest ? alpha : lowest;
      highest = alpha > highest ? alpha : highest;
    }

    Assert::IsTrue(lowest > 0x60, L"the shimmer must never fall to invisible");
    Assert::IsTrue(highest > lowest, L"a shimmer that does not move is a second ring");
  }

  TEST_METHOD(ADepartureFadesOutAndAnArrivalFadesIn)
  {
    EntityTransit leaving;
    leaving.planeMetres = XMFLOAT2{0.0f, 0.0f};
    leaving.radiusMetres = 12.0f;
    leaving.startSeconds = 100.0;
    leaving.arriving = false;

    EntityTransit joining = leaving;
    joining.arriving = true;

    const std::vector<EntityTransit> transits = {leaving, joining};

    OverlayMarkList early;
    BuildTransitMarks(transits, OverlayTuning{}, 1.0f, 100.1, early);
    OverlayMarkList late;
    BuildTransitMarks(transits, OverlayTuning{}, 1.0f, 100.9, late);

    Assert::AreEqual<std::uint32_t>(2, CountOfKind(early, OverlayKind::TransitRing));

    const auto leavingEarly = static_cast<std::uint8_t>(early.marks[0].colourRgba >> 24);
    const auto leavingLate = static_cast<std::uint8_t>(late.marks[0].colourRgba >> 24);
    Assert::IsTrue(leavingLate < leavingEarly, L"a departure fades out");

    const auto joiningEarly = static_cast<std::uint8_t>(early.marks[1].colourRgba >> 24);
    const auto joiningLate = static_cast<std::uint8_t>(late.marks[1].colourRgba >> 24);
    Assert::IsTrue(joiningLate > joiningEarly, L"an arrival resolves in");
  }

  TEST_METHOD(BothDirectionsGrowOutward)
  {
    // A ring that collapsed inward on a docking ship would read as the ship
    // being crushed rather than as it going somewhere.
    EntityTransit transit;
    transit.planeMetres = XMFLOAT2{0.0f, 0.0f};
    transit.radiusMetres = 120.0f;
    transit.startSeconds = 100.0;

    for (const bool arriving : {false, true})
    {
      transit.arriving = arriving;
      const std::vector<EntityTransit> transits = {transit};

      OverlayMarkList early;
      BuildTransitMarks(transits, OverlayTuning{}, 1.0f, 100.05, early);
      OverlayMarkList late;
      BuildTransitMarks(transits, OverlayTuning{}, 1.0f, 100.95, late);

      Assert::IsTrue(late.marks[0].halfWidthPixels > early.marks[0].halfWidthPixels);
    }
  }

  TEST_METHOD(AFinishedTransitDrawsNothing)
  {
    EntityTransit transit;
    transit.radiusMetres = 12.0f;
    transit.startSeconds = 100.0;

    OverlayMarkList marks;
    BuildTransitMarks(std::vector<EntityTransit>{transit}, OverlayTuning{}, 1.0f,
                      100.0 + EntityTransitList::FADE_SECONDS, marks);
    Assert::AreEqual<std::size_t>(0, marks.marks.size());
  }

  TEST_METHOD(BothFamiliesAppendSoTheRingSplitIsUntouched)
  {
    /*
     * Screen-facing, so they go on the end: a mark inserted among the rings
     * would be drawn depth-tested and disappear behind the hull it is about.
     */
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, PROTECTED_BIT)};
    const std::vector<EntityId> selected = {1};
    OverlayTuning tuning;
    tuning.statusMarkBits = PROTECTED_BIT;

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, tuning, 0.1f, marks);
    const std::uint32_t ringsBefore = marks.ringCount;

    BuildStatusMarks(entities, tuning, 0.0, marks);

    EntityTransit transit;
    transit.radiusMetres = 12.0f;
    transit.startSeconds = 100.0;
    BuildTransitMarks(std::vector<EntityTransit>{transit}, tuning, 1.0f, 100.5, marks);

    Assert::AreEqual(ringsBefore, marks.ringCount);
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::StatusMarker));
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::TransitRing));
  }
};

} // namespace NeuronClientTests
