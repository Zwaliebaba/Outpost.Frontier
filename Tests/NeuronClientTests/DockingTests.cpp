#include "pch.h"
#include "CppUnitTest.h"

#include "ApproachChain.h"
#include "AutoFollow.h"
#include "EntityTransits.h"
#include "OverlayMark.h"
#include "RenderWorld.h"

#include <DirectXMath.h>

#include <cstdint>
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
    const std::vector<std::uint16_t> ships = {4, 5, 6};
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
    std::vector<std::uint16_t> ships = {4, 5, 6};
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
    const std::vector<std::uint16_t> ships = {4};

    Assert::IsFalse(chain.Begin(unavailable, ships));
    Assert::IsFalse(chain.Begin(DockAction(), {}));
    Assert::IsFalse(chain.Active());
  }

  TEST_METHOD(MoreShipsThanTheChainHoldsIsRefusedRatherThanTruncated)
  {
    // Refused, because an approach that silently dropped members would dock a
    // different fleet than the one the player pointed at.
    ApproachChain chain;
    std::vector<std::uint16_t> ships;
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
    const std::vector<std::uint16_t> ships = {4};
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
    const std::vector<std::uint16_t> ships = {4};
    Assert::IsTrue(chain.Begin(DockAction(), ships));

    chain.NoteOrderRefused(0);
    Assert::IsTrue(chain.Active());
  }

  TEST_METHOD(AMemberLeavingTheWorldCancelsTheChain)
  {
    ApproachChain chain;
    const std::vector<std::uint16_t> ships = {4, 5};
    Assert::IsTrue(chain.Begin(DockAction(), ships));

    const std::vector<std::uint16_t> stillThere = {4, 5, 9};
    chain.NoteWorld(stillThere);
    Assert::IsTrue(chain.Active());

    const std::vector<std::uint16_t> oneGone = {4, 9};
    chain.NoteWorld(oneGone);
    Assert::IsFalse(chain.Active());
  }

  TEST_METHOD(ASecondApproachReplacesTheFirst)
  {
    // One at a time on purpose: two chips promising two different arrivals is
    // a HUD saying something the player did not ask for.
    ApproachChain chain;
    Assert::IsTrue(chain.Begin(DockAction(7), std::vector<std::uint16_t>{4}));
    Assert::IsTrue(chain.Begin(DockAction(8), std::vector<std::uint16_t>{5, 6}));

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
    const std::vector<std::uint16_t> selected = {1};
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
