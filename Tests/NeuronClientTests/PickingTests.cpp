#include "pch.h"
#include "CppUnitTest.h"

#include "IsoCamera.h"
#include "Picking.h"
#include "Selection.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * Picking and selection (Build Order S8, ADR-006 §11).
 *
 * The whole point of an orthographic camera over a plane is that this is
 * arithmetic rather than a GPU readback, and arithmetic can be asserted. These
 * are the tests the slice was accepted against: point and box picks over known
 * layouts, and the screen floor that keeps a distant Interceptor clickable.
 *
 * The strongest one here is `TheInverseMappingRoundTripsThroughTheRealCamera`.
 * `PlaneToNdc` claims to invert `PlaneMappingForNdc`, and a hand-derived 2x2
 * inverse is exactly the kind of thing that reads correctly with the sign of
 * one term wrong -- which is how ADR-006 §3a's handedness defect survived
 * review and died to a round trip.
 */

namespace NeuronClientTests
{
namespace
{

/*
 * A camera in a known state, so a test says what it is testing.
 *
 * Named apart from `CameraTests.cpp`'s `MakeCamera` on purpose: that one takes
 * yaw in *degrees* and this one takes radians, and two helpers with one name
 * and different units is a trap even with a translation unit between them.
 */
[[nodiscard]] IsoCamera PickCamera(float _yawRadians, float _zoomMetres, const XMFLOAT2& _focus = XMFLOAT2{0.0f, 0.0f})
{
  IsoCamera camera;
  camera.SetViewport(1600, 900);
  camera.SetFocus(_focus);
  camera.SetYawRadians(_yawRadians);
  camera.SetZoomMetres(_zoomMetres);
  return camera;
}

[[nodiscard]] SceneEntity Target(std::uint16_t _id, float _x, float _y, float _radius)
{
  SceneEntity entity;
  entity.id = _id;
  entity.planeMetres = XMFLOAT2{_x, _y};
  entity.pickRadiusMetres = _radius;
  return entity;
}

/// The plane point an NDC lands on, the forward direction, spelled out so a
/// test can build a cursor position from a ship position.
[[nodiscard]] XMFLOAT2 PlanePointForNdc(const PlaneMapping& _mapping, float _ndcX, float _ndcY)
{
  return XMFLOAT2{_mapping.origin.x + _mapping.rightPerNdc.x * _ndcX + _mapping.upPerNdc.x * _ndcY,
                  _mapping.origin.y + _mapping.rightPerNdc.y * _ndcX + _mapping.upPerNdc.y * _ndcY};
}

/// The four hull radii the class table actually ships, so the numbers here are
/// the numbers the game picks with rather than round ones chosen to pass.
constexpr float INTERCEPTOR_RADIUS_METRES = 12.0f;
constexpr float CARRIER_RADIUS_METRES = 90.0f;

} // namespace

TEST_CLASS(PlaneMappingInverseTests)
{
public:
  TEST_METHOD(TheInverseMappingRoundTripsThroughTheRealCamera)
  {
    // Every combination that changes the mapping: yaw rotates the axes, zoom
    // scales them, and the focus translates the origin. If the inverse has a
    // sign or an axis swapped, one of these lands somewhere else.
    constexpr float YAWS[] = {0.0f, 0.4f, XM_PIDIV2, 2.7f, -1.1f};
    constexpr float ZOOMS[] = {600.0f, 8000.0f, 39000.0f};
    constexpr XMFLOAT2 FOCI[] = {XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{1500.0f, -4200.0f}};
    constexpr XMFLOAT2 NDCS[] = {XMFLOAT2{0.0f, 0.0f},  XMFLOAT2{1.0f, 1.0f},   XMFLOAT2{-1.0f, 1.0f},
                                 XMFLOAT2{0.37f, -0.8f}, XMFLOAT2{-0.9f, -0.2f}};

    for (const float yaw : YAWS)
    {
      for (const float zoom : ZOOMS)
      {
        for (const XMFLOAT2& focus : FOCI)
        {
          const IsoCamera camera = PickCamera(yaw, zoom, focus);
          const PlaneMapping mapping = camera.PlaneMappingForNdc();

          for (const XMFLOAT2& ndc : NDCS)
          {
            const XMFLOAT2 plane = PlanePointForNdc(mapping, ndc.x, ndc.y);

            XMFLOAT2 back{};
            Assert::IsTrue(PlaneToNdc(mapping, plane, back));

            // Absolute, not relative: NDC is bounded by +/-1, so a tolerance in
            // absolute units is the same tolerance everywhere on screen. 1e-4
            // is far under a pixel at any viewport this ships with.
            Assert::AreEqual(ndc.x, back.x, 1e-4f);
            Assert::AreEqual(ndc.y, back.y, 1e-4f);
          }
        }
      }
    }
  }

  TEST_METHOD(ADegenerateMappingIsRefusedRatherThanDividedBy)
  {
    // What a zero-size viewport produces before the first resize. Dividing by
    // the zero determinant would give infinities, and an infinity compares as
    // inside every box -- a select-all on the first frame.
    PlaneMapping mapping;
    mapping.origin = XMFLOAT2{100.0f, 200.0f};
    mapping.rightPerNdc = XMFLOAT2{0.0f, 0.0f};
    mapping.upPerNdc = XMFLOAT2{0.0f, 0.0f};

    XMFLOAT2 ndc{1.0f, 1.0f};
    Assert::IsFalse(PlaneToNdc(mapping, XMFLOAT2{0.0f, 0.0f}, ndc));
    Assert::AreEqual(0.0f, ndc.x);
    Assert::AreEqual(0.0f, ndc.y);
  }

  TEST_METHOD(ParallelAxesAreDegenerateToo)
  {
    // Not reachable from IsoCamera, but the determinant test has to be about
    // the axes spanning the plane rather than about either being zero.
    PlaneMapping mapping;
    mapping.rightPerNdc = XMFLOAT2{100.0f, 50.0f};
    mapping.upPerNdc = XMFLOAT2{200.0f, 100.0f};

    XMFLOAT2 ndc{};
    Assert::IsFalse(PlaneToNdc(mapping, XMFLOAT2{10.0f, 10.0f}, ndc));
  }
};

TEST_CLASS(PointPickTests)
{
public:
  TEST_METHOD(TheShipUnderTheCursorIsThePickedOne)
  {
    const std::vector<SceneEntity> targets = {Target(7, 0.0f, 0.0f, INTERCEPTOR_RADIUS_METRES),
                                             Target(9, 500.0f, 0.0f, INTERCEPTOR_RADIUS_METRES)};

    Assert::AreEqual<std::uint32_t>(7, PickPoint(targets, XMFLOAT2{3.0f, 2.0f}, 0.0f));
    Assert::AreEqual<std::uint32_t>(9, PickPoint(targets, XMFLOAT2{498.0f, -1.0f}, 0.0f));
  }

  TEST_METHOD(EmptySpacePicksNothing)
  {
    const std::vector<SceneEntity> targets = {Target(7, 0.0f, 0.0f, INTERCEPTOR_RADIUS_METRES)};

    // Just outside the radius, and a long way outside it.
    Assert::AreEqual<std::uint32_t>(INVALID_ENTITY_ID, PickPoint(targets, XMFLOAT2{13.0f, 0.0f}, 0.0f));
    Assert::AreEqual<std::uint32_t>(INVALID_ENTITY_ID, PickPoint(targets, XMFLOAT2{9000.0f, 9000.0f}, 0.0f));
    Assert::AreEqual<std::uint32_t>(INVALID_ENTITY_ID, PickPoint({}, XMFLOAT2{0.0f, 0.0f}, 100.0f));
  }

  TEST_METHOD(OverlappingShipsResolveToTheNearestRatherThanTheFirst)
  {
    // The normal case in a formation. "Whichever came first in the array" is
    // not an answer a player can predict, and the array order is the sample
    // order, which is the spawn order, which is nothing they can see.
    const std::vector<SceneEntity> targets = {Target(1, 0.0f, 0.0f, 100.0f), Target(2, 40.0f, 0.0f, 100.0f),
                                             Target(3, 80.0f, 0.0f, 100.0f)};

    Assert::AreEqual<std::uint32_t>(1, PickPoint(targets, XMFLOAT2{5.0f, 0.0f}, 0.0f));
    Assert::AreEqual<std::uint32_t>(2, PickPoint(targets, XMFLOAT2{45.0f, 0.0f}, 0.0f));
    Assert::AreEqual<std::uint32_t>(3, PickPoint(targets, XMFLOAT2{85.0f, 0.0f}, 0.0f));

    // Reversing the array must not reverse the answer.
    const std::vector<SceneEntity> reversed = {targets[2], targets[1], targets[0]};
    Assert::AreEqual<std::uint32_t>(2, PickPoint(reversed, XMFLOAT2{45.0f, 0.0f}, 0.0f));
  }

  TEST_METHOD(ABiggerHullIsEasierToHitBecauseItIsBigger)
  {
    const std::vector<SceneEntity> targets = {Target(1, 0.0f, 0.0f, INTERCEPTOR_RADIUS_METRES),
                                             Target(2, 1000.0f, 0.0f, CARRIER_RADIUS_METRES)};

    // 50 m from each: inside the Carrier, outside the Interceptor.
    Assert::AreEqual<std::uint32_t>(INVALID_ENTITY_ID, PickPoint(targets, XMFLOAT2{50.0f, 0.0f}, 0.0f));
    Assert::AreEqual<std::uint32_t>(2, PickPoint(targets, XMFLOAT2{1050.0f, 0.0f}, 0.0f));
  }

  TEST_METHOD(TheScreenFloorMakesADistantInterceptorClickable)
  {
    const std::vector<SceneEntity> targets = {Target(1, 0.0f, 0.0f, INTERCEPTOR_RADIUS_METRES)};

    // At full zoom out, eight pixels is far more than twelve metres, so the
    // floor is what the pick actually uses.
    const IsoCamera far = PickCamera(0.0f, IsoCamera::MAX_ZOOM_METRES);
    const float floorMetres = far.ScreenFloorMetres(Selection::PICK_FLOOR_PIXELS);
    Assert::IsTrue(floorMetres > INTERCEPTOR_RADIUS_METRES * 10.0f, L"the floor should dominate a hull radius at 40 km");

    Assert::AreEqual<std::uint32_t>(INVALID_ENTITY_ID, PickPoint(targets, XMFLOAT2{200.0f, 0.0f}, 0.0f));
    Assert::AreEqual<std::uint32_t>(1, PickPoint(targets, XMFLOAT2{200.0f, 0.0f}, floorMetres));
  }

  TEST_METHOD(TheFloorIsAFloorAndNotAReplacement)
  {
    const std::vector<SceneEntity> targets = {Target(1, 0.0f, 0.0f, CARRIER_RADIUS_METRES)};

    // Zoomed right in, the floor is metres and the Carrier is ninety of them.
    const IsoCamera close = PickCamera(0.0f, IsoCamera::MIN_ZOOM_METRES);
    const float floorMetres = close.ScreenFloorMetres(Selection::PICK_FLOOR_PIXELS);
    Assert::IsTrue(floorMetres < CARRIER_RADIUS_METRES, L"a close-in floor should be smaller than a capital hull");

    // 80 m out: inside the hull radius, outside the floor. The hull wins.
    Assert::AreEqual<std::uint32_t>(1, PickPoint(targets, XMFLOAT2{80.0f, 0.0f}, floorMetres));
  }

  TEST_METHOD(TheScreenFloorTracksZoomAndUndoesTheForeshortening)
  {
    const IsoCamera close = PickCamera(0.0f, 1000.0f);
    const IsoCamera far = PickCamera(0.0f, 20000.0f);

    // Linear in zoom: twenty times the zoom is twenty times the metres.
    Assert::AreEqual(20.0f, far.ScreenFloorMetres(8.0f) / close.ScreenFloorMetres(8.0f), 1e-3f);

    // And exactly 1/sin(30 degrees) = 2x the unforeshortened metres-per-pixel,
    // which is what makes the floor a floor in the *tight* screen direction.
    Assert::AreEqual(2.0f * close.MetresPerPixel() * 8.0f, close.ScreenFloorMetres(8.0f), 1e-3f);
  }
};

TEST_CLASS(BoxPickTests)
{
public:
  TEST_METHOD(ABoxCatchesWhatIsInsideItAndNothingElse)
  {
    const IsoCamera camera = PickCamera(0.0f, 8000.0f);
    const PlaneMapping mapping = camera.PlaneMappingForNdc();

    // Placed by NDC so the test says where they are on *screen*, which is what
    // a drag rectangle is expressed in.
    const XMFLOAT2 inside = PlanePointForNdc(mapping, 0.1f, 0.1f);
    const XMFLOAT2 alsoInside = PlanePointForNdc(mapping, 0.4f, -0.3f);
    const XMFLOAT2 outside = PlanePointForNdc(mapping, 0.9f, 0.9f);

    const std::vector<SceneEntity> targets = {Target(1, inside.x, inside.y, 20.0f), Target(2, outside.x, outside.y, 20.0f),
                                             Target(3, alsoInside.x, alsoInside.y, 20.0f)};

    std::vector<EntityId> hits;
    PickBox(targets, mapping, XMFLOAT2{-0.5f, -0.5f}, XMFLOAT2{0.5f, 0.5f}, hits);

    Assert::AreEqual<std::size_t>(2, hits.size());
    Assert::AreEqual<std::uint16_t>(1, hits[0]);
    Assert::AreEqual<std::uint16_t>(3, hits[1]);
  }

  TEST_METHOD(ADragInAnyDirectionIsTheSameBox)
  {
    const IsoCamera camera = PickCamera(1.2f, 3000.0f, XMFLOAT2{-800.0f, 1200.0f});
    const PlaneMapping mapping = camera.PlaneMappingForNdc();

    const XMFLOAT2 middle = PlanePointForNdc(mapping, 0.0f, 0.0f);
    const std::vector<SceneEntity> targets = {Target(42, middle.x, middle.y, 20.0f)};

    constexpr XMFLOAT2 CORNERS[][2] = {{XMFLOAT2{-0.5f, -0.5f}, XMFLOAT2{0.5f, 0.5f}},
                                       {XMFLOAT2{0.5f, 0.5f}, XMFLOAT2{-0.5f, -0.5f}},
                                       {XMFLOAT2{-0.5f, 0.5f}, XMFLOAT2{0.5f, -0.5f}},
                                       {XMFLOAT2{0.5f, -0.5f}, XMFLOAT2{-0.5f, 0.5f}}};

    for (const auto& corners : CORNERS)
    {
      std::vector<EntityId> hits;
      PickBox(targets, mapping, corners[0], corners[1], hits);
      Assert::AreEqual<std::size_t>(1, hits.size());
      Assert::AreEqual<std::uint16_t>(42, hits[0]);
    }
  }

  TEST_METHOD(ABoxCatchesCentresRatherThanOverlaps)
  {
    // A careful drag around a wing must not pick up the Carrier parked beside
    // it just because the box clipped its hull.
    const IsoCamera camera = PickCamera(0.0f, 8000.0f);
    const PlaneMapping mapping = camera.PlaneMappingForNdc();

    // Just outside the right edge of the box, with a radius wide enough to
    // reach well inside it.
    const XMFLOAT2 justOutside = PlanePointForNdc(mapping, 0.55f, 0.0f);
    const std::vector<SceneEntity> targets = {Target(1, justOutside.x, justOutside.y, 4000.0f)};

    std::vector<EntityId> hits;
    PickBox(targets, mapping, XMFLOAT2{-0.5f, -0.5f}, XMFLOAT2{0.5f, 0.5f}, hits);
    Assert::AreEqual<std::size_t>(0, hits.size());
  }

  TEST_METHOD(TheBoxRotatesWithTheCamera)
  {
    /*
     * The rectangle is axis-aligned on *screen* and a parallelogram on the
     * plane. A ship that is inside at one yaw and outside at another is the
     * behaviour; picking against plane-aligned bounds would not do this.
     *
     * The box was chosen from measured NDC rather than reasoned about, after an
     * earlier version of this test asserted a case that sat exactly on an edge:
     * a ship at (2000, 0) with an 8 km zoom lands at NDC (0.140625, 0) at yaw
     * zero and at (0, 0.125) at a quarter turn, and the old box started at
     * x = 0 -- so "outside" rested entirely on the sign of a zero, which g++
     * and MSVC disagreed about. Every edge below clears its point by at least
     * 0.05 of NDC, some forty pixels at this viewport.
     */
    const std::vector<SceneEntity> targets = {Target(1, 2000.0f, 0.0f, 20.0f)};
    const XMFLOAT2 cornerA{0.05f, -0.06f};
    const XMFLOAT2 cornerB{0.30f, 0.06f};

    std::vector<EntityId> atZero;
    PickBox(targets, PickCamera(0.0f, 8000.0f).PlaneMappingForNdc(), cornerA, cornerB, atZero);
    Assert::AreEqual<std::size_t>(1, atZero.size(), L"x 0.141 is inside [0.05, 0.30] and y 0 is inside [-0.06, 0.06]");

    // At a quarter turn it is outside on *both* axes -- x by 0.05 and y by
    // 0.065 -- so neither comparison alone is carrying the assertion.
    std::vector<EntityId> atQuarterTurn;
    PickBox(targets, PickCamera(XM_PIDIV2, 8000.0f).PlaneMappingForNdc(), cornerA, cornerB, atQuarterTurn);
    Assert::AreEqual<std::size_t>(0, atQuarterTurn.size());

    // And at a half turn it is on the far side of the screen entirely.
    std::vector<EntityId> atHalfTurn;
    PickBox(targets, PickCamera(XM_PI, 8000.0f).PlaneMappingForNdc(), cornerA, cornerB, atHalfTurn);
    Assert::AreEqual<std::size_t>(0, atHalfTurn.size());
  }

  TEST_METHOD(TheBoxEdgeIsInclusive)
  {
    /*
     * Pinned deliberately, because the previous test found this out by
     * accident. The mapping is built by hand with axis-aligned axes and a
     * power-of-two scale, so a target can sit *exactly* on an edge instead of a
     * rounding away from it: 512 / 1024 is 0.5 with no error in binary, on any
     * compiler.
     */
    PlaneMapping mapping;
    mapping.origin = XMFLOAT2{0.0f, 0.0f};
    mapping.rightPerNdc = XMFLOAT2{1024.0f, 0.0f};
    mapping.upPerNdc = XMFLOAT2{0.0f, 1024.0f};

    const std::vector<SceneEntity> onTheEdge = {Target(1, 512.0f, 0.0f, 1.0f)};
    std::vector<EntityId> hits;
    PickBox(onTheEdge, mapping, XMFLOAT2{-0.5f, -0.5f}, XMFLOAT2{0.5f, 0.5f}, hits);
    Assert::AreEqual<std::size_t>(1, hits.size(), L"a ship exactly on the edge is in the box");

    // One representable step further out is not.
    const std::vector<SceneEntity> justOutside = {Target(1, 513.0f, 0.0f, 1.0f)};
    hits.clear();
    PickBox(justOutside, mapping, XMFLOAT2{-0.5f, -0.5f}, XMFLOAT2{0.5f, 0.5f}, hits);
    Assert::AreEqual<std::size_t>(0, hits.size());
  }

  TEST_METHOD(HitsAreAppendedRatherThanAssigned)
  {
    const IsoCamera camera = PickCamera(0.0f, 8000.0f);
    const PlaneMapping mapping = camera.PlaneMappingForNdc();
    const XMFLOAT2 middle = PlanePointForNdc(mapping, 0.0f, 0.0f);
    const std::vector<SceneEntity> targets = {Target(5, middle.x, middle.y, 20.0f)};

    std::vector<EntityId> hits = {99};
    PickBox(targets, mapping, XMFLOAT2{-0.5f, -0.5f}, XMFLOAT2{0.5f, 0.5f}, hits);
    Assert::AreEqual<std::size_t>(2, hits.size());
    Assert::AreEqual<std::uint16_t>(99, hits[0]);
  }
};

TEST_CLASS(SelectionGestureTests)
{
public:
  /// A viewport, a camera and three ships at known screen positions, so a test
  /// can drive the gesture in pixels the way the window does.
  struct Fixture
  {
    static constexpr std::uint32_t WIDTH = 1600;
    static constexpr std::uint32_t HEIGHT = 900;

    IsoCamera camera = PickCamera(0.0f, 8000.0f);
    PlaneMapping mapping = camera.PlaneMappingForNdc();
    std::vector<SceneEntity> targets;

    Fixture()
    {
      // At NDC (-0.5, 0), (0, 0) and (0.5, 0) -- left, centre and right of
      // screen, all on the horizontal midline.
      for (int index = 0; index < 3; ++index)
      {
        const float ndcX = -0.5f + 0.5f * static_cast<float>(index);
        const XMFLOAT2 plane = PlanePointForNdc(mapping, ndcX, 0.0f);
        targets.push_back(Target(static_cast<std::uint32_t>(index + 1), plane.x, plane.y, 60.0f));
      }
    }

    /// Pixel coordinates of an NDC point, the way the window reports a cursor.
    [[nodiscard]] static float PixelX(float _ndcX) noexcept { return (_ndcX + 1.0f) * 0.5f * static_cast<float>(WIDTH); }
    [[nodiscard]] static float PixelY(float _ndcY) noexcept { return (1.0f - _ndcY) * 0.5f * static_cast<float>(HEIGHT); }

    void Release(Selection& _selection) const
    {
      _selection.EndDrag(targets, mapping, WIDTH, HEIGHT, camera.ScreenFloorMetres(Selection::PICK_FLOOR_PIXELS));
    }
  };

  TEST_METHOD(AClickSelectsTheShipUnderIt)
  {
    const Fixture fixture;
    Selection selection;

    selection.BeginDrag(Fixture::PixelX(0.0f), Fixture::PixelY(0.0f), false);
    fixture.Release(selection);

    Assert::AreEqual<std::size_t>(1, selection.Count());
    Assert::IsTrue(selection.Contains(2));
  }

  TEST_METHOD(AClickOnEmptySpaceClearsTheSelection)
  {
    const Fixture fixture;
    Selection selection;
    selection.Add(1);
    selection.Add(3);

    selection.BeginDrag(Fixture::PixelX(0.0f), Fixture::PixelY(0.9f), false);
    fixture.Release(selection);

    Assert::IsTrue(selection.Empty(), L"a plain click on nothing is how a player deselects");
  }

  TEST_METHOD(ASmallTwitchIsStillAClick)
  {
    const Fixture fixture;
    Selection selection;

    // Three pixels: inside the slop, so this must not become a box that
    // selects nothing.
    selection.BeginDrag(Fixture::PixelX(0.0f), Fixture::PixelY(0.0f), false);
    selection.UpdateDrag(Fixture::PixelX(0.0f) + 3.0f, Fixture::PixelY(0.0f) + 2.0f);
    Assert::IsFalse(selection.DragIsBox());
    fixture.Release(selection);

    Assert::AreEqual<std::size_t>(1, selection.Count());
    Assert::IsTrue(selection.Contains(2));
  }

  TEST_METHOD(ADeliberateDragIsABox)
  {
    const Fixture fixture;
    Selection selection;

    // From above-left of ship 1 to below-right of ship 2, leaving ship 3 out.
    selection.BeginDrag(Fixture::PixelX(-0.8f), Fixture::PixelY(0.3f), false);
    selection.UpdateDrag(Fixture::PixelX(0.2f), Fixture::PixelY(-0.3f));
    Assert::IsTrue(selection.DragIsBox());
    fixture.Release(selection);

    Assert::AreEqual<std::size_t>(2, selection.Count());
    Assert::IsTrue(selection.Contains(1));
    Assert::IsTrue(selection.Contains(2));
    Assert::IsFalse(selection.Contains(3));
  }

  TEST_METHOD(ShiftClickTogglesRatherThanReplaces)
  {
    const Fixture fixture;
    Selection selection;

    selection.BeginDrag(Fixture::PixelX(-0.5f), Fixture::PixelY(0.0f), false);
    fixture.Release(selection);
    Assert::AreEqual<std::size_t>(1, selection.Count());

    selection.BeginDrag(Fixture::PixelX(0.5f), Fixture::PixelY(0.0f), true);
    fixture.Release(selection);
    Assert::AreEqual<std::size_t>(2, selection.Count());
    Assert::IsTrue(selection.Contains(1));
    Assert::IsTrue(selection.Contains(3));

    // The same shift-click again takes it back out.
    selection.BeginDrag(Fixture::PixelX(0.5f), Fixture::PixelY(0.0f), true);
    fixture.Release(selection);
    Assert::AreEqual<std::size_t>(1, selection.Count());
    Assert::IsTrue(selection.Contains(1));
  }

  TEST_METHOD(ShiftClickOnEmptySpaceLeavesTheSelectionAlone)
  {
    const Fixture fixture;
    Selection selection;
    selection.Add(1);

    selection.BeginDrag(Fixture::PixelX(0.0f), Fixture::PixelY(0.9f), true);
    fixture.Release(selection);

    Assert::AreEqual<std::size_t>(1, selection.Count(), L"shift means adjust, and clearing is the opposite of adjusting");
    Assert::IsTrue(selection.Contains(1));
  }

  TEST_METHOD(APlainBoxReplacesAndAShiftBoxMerges)
  {
    const Fixture fixture;
    Selection selection;
    selection.Add(3);

    // A plain box over ship 1 only: ship 3 goes.
    selection.BeginDrag(Fixture::PixelX(-0.8f), Fixture::PixelY(0.3f), false);
    selection.UpdateDrag(Fixture::PixelX(-0.3f), Fixture::PixelY(-0.3f));
    fixture.Release(selection);
    Assert::AreEqual<std::size_t>(1, selection.Count());
    Assert::IsTrue(selection.Contains(1));

    // A shift box over ship 3: ship 1 stays.
    selection.BeginDrag(Fixture::PixelX(0.3f), Fixture::PixelY(0.3f), true);
    selection.UpdateDrag(Fixture::PixelX(0.8f), Fixture::PixelY(-0.3f));
    fixture.Release(selection);
    Assert::AreEqual<std::size_t>(2, selection.Count());
    Assert::IsTrue(selection.Contains(1));
    Assert::IsTrue(selection.Contains(3));
  }

  TEST_METHOD(AShiftBoxDoesNotDuplicateWhatIsAlreadySelected)
  {
    const Fixture fixture;
    Selection selection;
    selection.Add(1);

    selection.BeginDrag(Fixture::PixelX(-0.8f), Fixture::PixelY(0.3f), true);
    selection.UpdateDrag(Fixture::PixelX(0.2f), Fixture::PixelY(-0.3f));
    fixture.Release(selection);

    Assert::AreEqual<std::size_t>(2, selection.Count(), L"ship 1 was already in and must not be counted twice");
  }

  TEST_METHOD(TheModifierIsSampledAtPressAndNotAtRelease)
  {
    // Letting go of shift mid-drag must not turn an adjust into a replace: the
    // gesture the player started is the gesture they get.
    const Fixture fixture;
    Selection selection;
    selection.Add(1);

    selection.BeginDrag(Fixture::PixelX(0.5f), Fixture::PixelY(0.0f), true);
    fixture.Release(selection);

    Assert::AreEqual<std::size_t>(2, selection.Count());
  }

  TEST_METHOD(ACancelledDragChangesNothing)
  {
    const Fixture fixture;
    Selection selection;
    selection.Add(1);

    selection.BeginDrag(Fixture::PixelX(0.0f), Fixture::PixelY(0.0f), false);
    selection.CancelDrag();
    Assert::IsFalse(selection.Dragging());

    // A release after the cancel is a release with no drag, and does nothing.
    fixture.Release(selection);
    Assert::AreEqual<std::size_t>(1, selection.Count());
    Assert::IsTrue(selection.Contains(1));
  }

  TEST_METHOD(DragIsBoxIsFalseWhileNotDragging)
  {
    Selection selection;
    Assert::IsFalse(selection.DragIsBox());
    Assert::IsFalse(selection.Dragging());
  }

  TEST_METHOD(RetainDropsShipsThatAreNoLongerThere)
  {
    Selection selection;
    selection.Set(std::vector<EntityId>{1, 2, 3});

    const std::vector<SceneEntity> stillAlive = {Target(1, 0.0f, 0.0f, 10.0f), Target(3, 100.0f, 0.0f, 10.0f)};
    selection.Retain(stillAlive);

    Assert::AreEqual<std::size_t>(2, selection.Count());
    Assert::IsTrue(selection.Contains(1));
    Assert::IsFalse(selection.Contains(2), L"a ship that died must leave the selection with it");
    Assert::IsTrue(selection.Contains(3));

    selection.Retain({});
    Assert::IsTrue(selection.Empty());
  }

  TEST_METHOD(AddAndToggleRefuseTheInvalidId)
  {
    Selection selection;
    selection.Add(INVALID_ENTITY_ID);
    Assert::IsTrue(selection.Empty());

    selection.Toggle(INVALID_ENTITY_ID);
    Assert::IsTrue(selection.Empty(), L"a click on nothing must not select nothing-as-a-thing");
  }

  TEST_METHOD(AddIsIdempotent)
  {
    Selection selection;
    selection.Add(4);
    selection.Add(4);
    Assert::AreEqual<std::size_t>(1, selection.Count());
  }
};

TEST_CLASS(NdcToPixelsTests)
{
public:
  TEST_METHOD(ItIsTheExactInverseOfPixelsToNdc)
  {
    /*
     * ADR-006 §11's "one code path", now in both directions. Three callers turn
     * pixels into a plane point and the ghost's lane goes the other way; a
     * private flip in that file would be the fourth place the y-axis is
     * negated, and the first three live together precisely so there is no
     * fourth to get wrong.
     */
    constexpr std::uint32_t width = 1600;
    constexpr std::uint32_t height = 900;
    const float samples[][2] = {{0.0f, 0.0f},     {800.0f, 450.0f}, {1600.0f, 900.0f},
                                {1.0f, 899.0f},   {1599.0f, 1.0f},  {-40.0f, 1200.0f}};
    for (const auto& sample : samples)
    {
      const XMFLOAT2 ndc = PixelsToNdc(sample[0], sample[1], width, height);
      const XMFLOAT2 back = NdcToPixels(ndc, width, height);
      Assert::AreEqual(sample[0], back.x, 0.001f);
      Assert::AreEqual(sample[1], back.y, 0.001f);
    }
  }

  TEST_METHOD(TheCentreOfNdcIsTheCentreOfTheViewport)
  {
    // The sign of the y flip, stated once rather than inferred from a round
    // trip that would pass with both negations wrong.
    const XMFLOAT2 centre = NdcToPixels(XMFLOAT2{0.0f, 0.0f}, 1600, 900);
    Assert::AreEqual(800.0f, centre.x, 0.001f);
    Assert::AreEqual(450.0f, centre.y, 0.001f);

    const XMFLOAT2 topLeft = NdcToPixels(XMFLOAT2{-1.0f, 1.0f}, 1600, 900);
    Assert::AreEqual(0.0f, topLeft.x, 0.001f);
    Assert::AreEqual(0.0f, topLeft.y, 0.001f, L"ndc y is up and pixel y is down");
  }

  TEST_METHOD(AZeroSizeViewportCollapsesRatherThanDividing)
  {
    // What a window reports before its first resize. The forward function maps
    // everything to the centre of nothing; this maps back to its corner.
    const XMFLOAT2 pixels = NdcToPixels(XMFLOAT2{0.5f, -0.5f}, 0, 0);
    Assert::AreEqual(0.0f, pixels.x, 0.001f);
    Assert::AreEqual(0.0f, pixels.y, 0.001f);
  }
};

} // namespace NeuronClientTests
