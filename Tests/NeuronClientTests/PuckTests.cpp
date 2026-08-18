#include "pch.h"
#include "CppUnitTest.h"

#include "IsoCamera.h"
#include "OrderPuck.h"
#include "Picking.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * The order puck (Build Order S9, `puck-and-wheel.png` §2).
 *
 * The gesture is the whole of the client's contribution to an order: where the
 * fleet goes and which way it faces when it arrives. Both are decided from
 * pixels and a camera, so both are arithmetic, so both are asserted here rather
 * than discovered by dragging.
 *
 * The one that earns its place is `TheFacingIsMeasuredOnThePlaneNotTheScreen`.
 * Taking the drag's angle in pixels is right at a yaw of zero and wrong by
 * exactly the camera's yaw everywhere else -- a defect that is invisible in
 * every screenshot taken from the default view, which is the same shape as
 * ADR-006 §3a's handedness bug and is caught the same way: by rotating the
 * camera and checking the answer moved.
 */

namespace NeuronClientTests
{
namespace
{

[[nodiscard]] IsoCamera PuckCamera(float _yawRadians, float _zoomMetres = 8000.0f,
                                   const XMFLOAT2& _focus = XMFLOAT2{0.0f, 0.0f})
{
  IsoCamera camera;
  camera.SetViewport(1600, 900);
  camera.SetFocus(_focus);
  camera.SetYawRadians(_yawRadians);
  camera.SetZoomMetres(_zoomMetres);
  return camera;
}

[[nodiscard]] SceneEntity Ship(std::uint16_t _id, float _x, float _y)
{
  SceneEntity entity;
  entity.id = _id;
  entity.planeMetres = XMFLOAT2{_x, _y};
  entity.pickRadiusMetres = 20.0f;
  return entity;
}

/// The pixel a plane point sits at, so a test can aim the cursor at a known
/// place in the world rather than at a number of pixels that happens to work.
[[nodiscard]] XMFLOAT2 PixelsForPlanePoint(const IsoCamera& _camera, const XMFLOAT2& _planeMetres, std::uint32_t _width,
                                           std::uint32_t _height)
{
  XMFLOAT2 ndc{};
  Assert::IsTrue(PlaneToNdc(_camera.PlaneMappingForNdc(), _planeMetres, ndc), L"the mapping must be invertible");
  return XMFLOAT2{(ndc.x * 0.5f + 0.5f) * static_cast<float>(_width), (0.5f - ndc.y * 0.5f) * static_cast<float>(_height)};
}

/// Angles compared the short way round, because -pi and +pi are one heading.
void AssertAngleNear(float _expected, float _actual, float _tolerance, const wchar_t* _message)
{
  float difference = _actual - _expected;
  while (difference > std::numbers::pi_v<float>)
  {
    difference -= 2.0f * std::numbers::pi_v<float>;
  }
  while (difference < -std::numbers::pi_v<float>)
  {
    difference += 2.0f * std::numbers::pi_v<float>;
  }
  Assert::IsTrue(std::fabs(difference) <= _tolerance, _message);
}

} // namespace

TEST_CLASS(OrderPuckTests)
{
public:
  TEST_METHOD(APuckStartsIdleAndEndsIdle)
  {
    OrderPuck puck;
    Assert::IsFalse(puck.Active(), L"nothing is being ordered before a button goes down");
    Assert::IsFalse(puck.HasFacing(), L"an idle puck has no facing to report");

    puck.Begin(100.0f, 100.0f, false);
    Assert::IsTrue(puck.Active(), L"the press starts the gesture");

    puck.Cancel();
    Assert::IsFalse(puck.Active(), L"a cancelled gesture is over");
  }

  TEST_METHOD(AMoveWithNoPressIsIgnored)
  {
    // A cursor move that arrives after a cancel -- focus loss during a drag,
    // then the mouse travelling on -- must not restart the gesture, or an
    // alt-tab would leave a puck armed at whatever the cursor did next.
    OrderPuck puck;
    puck.Update(400.0f, 400.0f);
    Assert::IsFalse(puck.Active(), L"a move cannot begin a gesture");

    puck.Begin(100.0f, 100.0f, false);
    puck.Cancel();
    puck.Update(900.0f, 900.0f);
    Assert::IsFalse(puck.Active(), L"a cancelled gesture stays cancelled");
  }

  TEST_METHOD(ThePressPointIsTheDestination)
  {
    // The mouse binding's central decision: press names the place, the drag
    // names the heading. If the destination followed the cursor instead, every
    // order with a facing would land where the arc ended.
    const IsoCamera camera = PuckCamera(0.0f);
    const XMFLOAT2 wanted{2500.0f, -1200.0f};
    const XMFLOAT2 press = PixelsForPlanePoint(camera, wanted, 1600, 900);

    OrderPuck puck;
    puck.Begin(press.x, press.y, false);
    puck.Update(press.x + 300.0f, press.y - 220.0f);

    const PuckSample sample = puck.Resolve(camera.PlaneMappingForNdc(), 1600, 900);
    Assert::IsTrue(std::fabs(sample.targetMetres.x - wanted.x) < 1.0f, L"the destination is where the press landed");
    Assert::IsTrue(std::fabs(sample.targetMetres.y - wanted.y) < 1.0f, L"and not where the drag ended");
    Assert::IsTrue(sample.facingFromDrag, L"a 370-pixel arc is a facing");
  }

  TEST_METHOD(ASmallTwitchCarriesNoFacing)
  {
    // Below the slop the angle is dominated by the last pixel the mouse
    // happened to move, and honouring it would face a fleet somewhere nobody
    // chose. Reported as absent so the caller substitutes a real heading.
    const IsoCamera camera = PuckCamera(0.0f);

    OrderPuck puck;
    puck.Begin(800.0f, 450.0f, false);
    puck.Update(800.0f + OrderPuck::FACING_SLOP_PIXELS - 1.0f, 450.0f);
    Assert::IsFalse(puck.HasFacing(), L"inside the slop is not an arc");

    const PuckSample sample = puck.Resolve(camera.PlaneMappingForNdc(), 1600, 900);
    Assert::IsFalse(sample.facingFromDrag, L"and the sample says so");
    Assert::AreEqual(0.0f, sample.facingRadians, L"the absence of a facing is not a facing of zero degrees");
  }

  TEST_METHOD(TheSlopIsRoundNotSquare)
  {
    // Euclidean, unlike the selection box's Chebyshev slop. A diagonal drag of
    // 11 pixels on each axis is a 15-pixel arc and has a perfectly stable
    // angle; a box slop would have called it a twitch.
    OrderPuck puck;
    puck.Begin(500.0f, 500.0f, false);
    puck.Update(511.0f, 511.0f);
    Assert::IsTrue(puck.HasFacing(), L"a diagonal arc past the radius is an arc");

    puck.Begin(500.0f, 500.0f, false);
    puck.Update(500.0f + OrderPuck::FACING_SLOP_PIXELS + 1.0f, 500.0f);
    Assert::IsTrue(puck.HasFacing(), L"and so is a straight one");
  }

  TEST_METHOD(DraggingEastFacesEast)
  {
    // Sim space is x east, y north, and a heading is radians CCW from +x
    // (ADR-001 §3). At a yaw of zero, screen-right is east.
    const IsoCamera camera = PuckCamera(0.0f);

    OrderPuck puck;
    puck.Begin(800.0f, 450.0f, false);
    puck.Update(1000.0f, 450.0f);

    const PuckSample sample = puck.Resolve(camera.PlaneMappingForNdc(), 1600, 900);
    Assert::IsTrue(sample.facingFromDrag, L"the arc is long enough");
    AssertAngleNear(0.0f, sample.facingRadians, 0.02f, L"dragging right at yaw zero faces east");
  }

  TEST_METHOD(DraggingUpTheScreenFacesNorth)
  {
    // Screen-up is north at a yaw of zero, foreshortened but not rotated -- so
    // the heading is a quarter turn even though the pixels travelled less plane
    // distance than the same drag sideways would have.
    const IsoCamera camera = PuckCamera(0.0f);

    OrderPuck puck;
    puck.Begin(800.0f, 450.0f, false);
    puck.Update(800.0f, 250.0f);

    const PuckSample sample = puck.Resolve(camera.PlaneMappingForNdc(), 1600, 900);
    AssertAngleNear(std::numbers::pi_v<float> * 0.5f, sample.facingRadians, 0.02f, L"dragging up faces north");
  }

  TEST_METHOD(TheFacingIsMeasuredOnThePlaneNotTheScreen)
  {
    /*
     * The same drag, at two yaws, must give two headings -- and they must
     * differ by exactly the yaw. Measuring the angle in pixels would give the
     * same answer twice, which is correct at the default camera and wrong
     * everywhere the player actually plays.
     *
     * The *sign* is the part worth writing down. `ScreenRightOnPlane` is
     * `(cos yaw, -sin yaw)`, so orbiting the camera by a positive yaw sweeps
     * screen-right **clockwise** across the plane, and a screen-right drag's
     * heading goes down by the yaw rather than up. This test asserted the other
     * way round first and failed, which is what it is for: the relationship is
     * not guessable from the word "yaw".
     */
    const float quarter = std::numbers::pi_v<float> * 0.5f;

    OrderPuck puck;
    puck.Begin(800.0f, 450.0f, false);
    puck.Update(1000.0f, 450.0f);

    const PuckSample atZero = puck.Resolve(PuckCamera(0.0f).PlaneMappingForNdc(), 1600, 900);
    const PuckSample atQuarter = puck.Resolve(PuckCamera(quarter).PlaneMappingForNdc(), 1600, 900);

    Assert::IsTrue(atZero.facingFromDrag && atQuarter.facingFromDrag, L"both are arcs");
    AssertAngleNear(0.0f, atZero.facingRadians, 0.02f, L"screen-right is east at yaw zero");
    AssertAngleNear(-quarter, atQuarter.facingRadians, 0.02f, L"and south a quarter turn later");
    AssertAngleNear(atZero.facingRadians - quarter, atQuarter.facingRadians, 0.02f,
                    L"orbiting the camera a quarter turn turns the same drag's heading a quarter turn the other way");
  }

  TEST_METHOD(TheQueueModifierIsSampledAtPress)
  {
    // The same rule the selection's shift follows: a modifier released halfway
    // through must not change what the gesture already meant. Here it is
    // stronger, because releasing shift to reach for the mouse is a normal
    // thing to do and would silently replace a queue the player was building.
    OrderPuck puck;
    puck.Begin(800.0f, 450.0f, true);
    puck.Update(1000.0f, 450.0f);
    Assert::IsTrue(puck.Queued(), L"shift was down at the press");

    const PuckSample sample = puck.Resolve(PuckCamera(0.0f).PlaneMappingForNdc(), 1600, 900);
    Assert::IsTrue(sample.queued, L"and the sample still says so");

    puck.Begin(800.0f, 450.0f, false);
    Assert::IsFalse(puck.Queued(), L"a new gesture samples it again");
  }

  TEST_METHOD(ADegenerateViewportProducesNoFacingRatherThanANaN)
  {
    // A window reports a zero-size client area before its first resize, and
    // `PixelsToNdc` maps everything to the centre. Every pixel then maps to one
    // plane point, so there is no direction -- and atan2(0, 0) is a number
    // that would be carried into an order as if it meant something.
    OrderPuck puck;
    puck.Begin(0.0f, 0.0f, false);
    puck.Update(400.0f, 400.0f);

    const PuckSample sample = puck.Resolve(PuckCamera(0.0f).PlaneMappingForNdc(), 0, 0);
    Assert::IsFalse(sample.facingFromDrag, L"a degenerate mapping carries no heading");
    Assert::AreEqual(0.0f, sample.facingRadians, L"and reports none rather than an invented one");
  }
};

TEST_CLASS(FallbackFacingTests)
{
public:
  TEST_METHOD(AFleetArrivesFacingTheWayItTravelled)
  {
    // The answer for a plain right-click, which is the common case and says
    // nothing about heading. Ships at the origin, target due east: east.
    const std::vector<SceneEntity> entities{Ship(1, -10.0f, 0.0f), Ship(2, 10.0f, 0.0f)};
    const std::uint16_t ids[] = {1, 2};

    const float facing = TravelFacingRadians(entities, ids, XMFLOAT2{5000.0f, 0.0f}, 99.0f);
    AssertAngleNear(0.0f, facing, 0.001f, L"travelling east means arriving facing east");
  }

  TEST_METHOD(TheCentreIsTheAverageOfWhatIsActuallyThere)
  {
    const std::vector<SceneEntity> entities{Ship(1, 0.0f, 0.0f), Ship(2, 100.0f, 200.0f)};
    const std::uint16_t ids[] = {1, 2};

    XMFLOAT2 centre{};
    Assert::IsTrue(SelectionCentre(entities, ids, centre), L"two present ships have a centre");
    Assert::AreEqual(50.0f, centre.x, L"averaged on x");
    Assert::AreEqual(100.0f, centre.y, L"and on y");
  }

  TEST_METHOD(AnIdThatIsNotThereIsSkippedRatherThanCountedAtTheOrigin)
  {
    /*
     * The defect this exists to catch: folding a missing ship in at (0, 0)
     * drags the centre halfway across the map, which turns the fallback facing
     * into a heading pointing at nothing. `Selection::Retain` should have
     * dropped it the same frame -- this is the second line of defence, and it
     * is the same argument the overlay's own skip is made with.
     */
    const std::vector<SceneEntity> entities{Ship(1, 1000.0f, 1000.0f)};
    const std::uint16_t ids[] = {1, 777};

    XMFLOAT2 centre{};
    Assert::IsTrue(SelectionCentre(entities, ids, centre), L"one of the two is present");
    Assert::AreEqual(1000.0f, centre.x, L"the missing one contributes nothing");
    Assert::AreEqual(1000.0f, centre.y, L"rather than a ship at the origin");
  }

  TEST_METHOD(NothingPresentHasNoCentreAndNoFacing)
  {
    const std::vector<SceneEntity> entities{Ship(1, 0.0f, 0.0f)};
    const std::uint16_t ids[] = {42};

    XMFLOAT2 centre{7.0f, 9.0f};
    Assert::IsFalse(SelectionCentre(entities, ids, centre), L"a fleet of nothing has no centre");
    Assert::AreEqual(7.0f, centre.x, L"and the output is left alone");
    Assert::AreEqual(9.0f, centre.y, L"on both axes");

    Assert::AreEqual(1.25f, TravelFacingRadians(entities, ids, XMFLOAT2{100.0f, 0.0f}, 1.25f),
                     L"the caller's fallback stands");
  }

  TEST_METHOD(AFleetAlreadyOnTheTargetKeepsTheCallersAnswer)
  {
    // atan2(0, 0) is a direction nobody asked for, and a right-click on top of
    // the fleet is a real thing a player does when nudging a formation.
    const std::vector<SceneEntity> entities{Ship(1, 250.0f, -75.0f)};
    const std::uint16_t ids[] = {1};

    Assert::AreEqual(0.5f, TravelFacingRadians(entities, ids, XMFLOAT2{250.0f, -75.0f}, 0.5f),
                     L"no travel means no travel heading");
  }
};

TEST_CLASS(OrderIntentTests)
{
public:
  TEST_METHOD(TheIntentCarriesTheGamesNumbersAndTheClientsGeometry)
  {
    PuckSample sample;
    sample.targetMetres = XMFLOAT2{1234.0f, -567.0f};
    sample.facingRadians = 0.75f;
    sample.queued = true;

    OrderDefaults defaults;
    defaults.kind = 3;      // Whatever the game said. The client never reads it.
    defaults.parameter = 7; // Likewise.

    const std::uint16_t ids[] = {11, 22, 33};
    const OrderIntent intent = MakeOrderIntent(sample, defaults, ids, 9u);

    Assert::AreEqual<std::uint16_t>(3, intent.kind, L"the kind is the game's, copied");
    Assert::AreEqual<std::uint16_t>(7, intent.parameter, L"and so is the parameter");
    Assert::AreEqual(1234.0f, intent.targetXMetres, L"the target is the puck's");
    Assert::AreEqual(-567.0f, intent.targetYMetres, L"on both axes");
    Assert::AreEqual(0.75f, intent.facingRadians, L"and so is the facing");
    Assert::IsTrue(intent.queued, L"the queue chip rides along");
    Assert::AreEqual<std::uint32_t>(9, intent.orderSeq, L"the sequence the ack will match on");
    Assert::AreEqual<std::uint32_t>(3, intent.entityCount, L"three ships");
    Assert::IsTrue(intent.entityIds == ids, L"borrowed, not copied: the selection stays where it is");
  }

  TEST_METHOD(AnEmptySelectionStillMakesAnIntent)
  {
    // It has to: the refusal is `ValidateOrder`'s to give and the reason code is
    // the game's, so the client must be able to ask about an order it can
    // already guess the answer to. Guessing here would be a second validator.
    const PuckSample sample;
    const OrderDefaults defaults;
    const OrderIntent intent = MakeOrderIntent(sample, defaults, std::span<const std::uint16_t>{}, 1u);

    Assert::AreEqual<std::uint32_t>(0, intent.entityCount, L"no ships");
    Assert::AreEqual<std::uint32_t>(1, intent.orderSeq, L"but a real sequence");
  }
};

} // namespace NeuronClientTests
