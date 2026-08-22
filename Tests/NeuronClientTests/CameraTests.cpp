#include "pch.h"
#include "CppUnitTest.h"

#include "InputMap.h"
#include "IsoCamera.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * The tactical camera and its bindings (Build Order S5).
 *
 * These are projection tests, not "does it compile" tests: they push a world
 * point through the camera's own matrices and check where it lands in clip
 * space. That is what caught the handedness defect this camera was first
 * written with -- with ADR-001's render basis and the RH entry points ADR-006
 * originally named, east projected to the *left* of the screen, and no amount
 * of reading the code says so (ADR-006 §3a records the correction).
 */

namespace NeuronClientTests
{

namespace
{

/// Row-vector convention, matching DirectXMath and the shader's mul(v, M).
XMFLOAT3 ProjectToClip(const XMFLOAT4X4& _viewProjection, float _x, float _y, float _z)
{
  const XMVECTOR clip = XMVector4Transform(XMVectorSet(_x, _y, _z, 1.0f), XMLoadFloat4x4(&_viewProjection));
  const float w = XMVectorGetW(clip);
  return XMFLOAT3{XMVectorGetX(clip) / w, XMVectorGetY(clip) / w, XMVectorGetZ(clip) / w};
}

IsoCamera MakeCamera(float _yawDegrees, float _zoomMetres = 8000.0f)
{
  IsoCamera camera;
  camera.SetViewport(1600, 900);
  camera.SetZoomMetres(_zoomMetres);
  camera.SetYawRadians(_yawDegrees * XM_PI / 180.0f);
  return camera;
}

constexpr float YAWS_DEGREES[] = {0.0f, 45.0f, 90.0f, 137.0f, -60.0f, 180.0f};

} // namespace

TEST_CLASS(IsoCameraProjectionTests)
{
public:
  TEST_METHOD(TheFocusIsAlwaysTheCentreOfTheScreen)
  {
    for (float yaw : YAWS_DEGREES)
    {
      IsoCamera camera = MakeCamera(yaw);
      camera.SetFocus(XMFLOAT2{1200.0f, -800.0f});

      const XMFLOAT2 focus = camera.Focus();
      const XMFLOAT3 centre = ProjectToClip(camera.ViewProjectionMatrix(), focus.x, 0.0f, focus.y);

      Assert::AreEqual(0.0f, centre.x, 1e-3f, L"the focus is centred horizontally at every yaw");
      Assert::AreEqual(0.0f, centre.y, 1e-3f, L"the focus is centred vertically at every yaw");
      Assert::IsTrue(centre.z > 0.0f && centre.z < 1.0f, L"the plane sits inside the depth range");
    }
  }

  TEST_METHOD(EastIsOnTheRightOfTheScreen)
  {
    // The regression that motivates the whole file. At yaw 0 the camera looks
    // north, so a point due east of the focus must land right of centre.
    IsoCamera camera = MakeCamera(0.0f);
    camera.SetFocus(XMFLOAT2{0.0f, 0.0f});

    const XMFLOAT3 east = ProjectToClip(camera.ViewProjectionMatrix(), 1000.0f, 0.0f, 0.0f);
    const XMFLOAT3 north = ProjectToClip(camera.ViewProjectionMatrix(), 0.0f, 0.0f, 1000.0f);

    Assert::IsTrue(east.x > 0.0f, L"east must be right of centre, not mirrored to the left");
    Assert::AreEqual(0.0f, east.y, 1e-3f, L"due east does not drift up or down the screen");
    Assert::IsTrue(north.y > 0.0f, L"north must be up the screen");
    Assert::AreEqual(0.0f, north.x, 1e-3f, L"due north does not drift sideways");
  }

  TEST_METHOD(TheViewportEdgesAreExactlyTheZoomHalfExtents)
  {
    for (float yaw : YAWS_DEGREES)
    {
      IsoCamera camera = MakeCamera(yaw);
      camera.SetFocus(XMFLOAT2{-400.0f, 250.0f});

      const XMFLOAT4X4 viewProjection = camera.ViewProjectionMatrix();
      const XMFLOAT2 focus = camera.Focus();
      const XMFLOAT2 right = camera.ScreenRightOnPlane();
      const XMFLOAT2 up = camera.ScreenUpOnPlane();

      const float halfWidthMetres = camera.ZoomMetres() * camera.AspectRatio();
      const XMFLOAT3 edge = ProjectToClip(viewProjection, focus.x + right.x * halfWidthMetres, 0.0f, focus.y + right.y * halfWidthMetres);
      Assert::AreEqual(1.0f, edge.x, 1e-3f, L"zoom * aspect metres to the right is the right edge");

      // Up the screen costs 1/sin(30 degrees) = 2x the plane distance, which is
      // the same foreshortening that makes ground circles 2:1 ellipses.
      const float halfHeightMetres = camera.ZoomMetres() * 2.0f;
      const XMFLOAT3 top = ProjectToClip(viewProjection, focus.x + up.x * halfHeightMetres, 0.0f, focus.y + up.y * halfHeightMetres);
      Assert::AreEqual(1.0f, top.y, 1e-3f, L"zoom / sin(elevation) metres forward is the top edge");
    }
  }

  TEST_METHOD(AGroundCircleProjectsAsATwoToOneEllipse)
  {
    // Normative (ADR-006 §4): the overlay pass draws selection rings as 2:1
    // plane ellipses because sin(30 degrees) is exactly 0.5. If this ratio ever
    // moves, every ring in the corpus is wrong.
    for (float yaw : YAWS_DEGREES)
    {
      IsoCamera camera = MakeCamera(yaw);
      camera.SetFocus(XMFLOAT2{0.0f, 0.0f});

      const XMFLOAT4X4 viewProjection = camera.ViewProjectionMatrix();
      const XMFLOAT2 right = camera.ScreenRightOnPlane();
      const XMFLOAT2 up = camera.ScreenUpOnPlane();
      constexpr float RADIUS_METRES = 1000.0f;

      const XMFLOAT3 across = ProjectToClip(viewProjection, right.x * RADIUS_METRES, 0.0f, right.y * RADIUS_METRES);
      const XMFLOAT3 along = ProjectToClip(viewProjection, up.x * RADIUS_METRES, 0.0f, up.y * RADIUS_METRES);

      const float majorPixels = std::fabs(across.x) * (1600.0f / 2.0f);
      const float minorPixels = std::fabs(along.y) * (900.0f / 2.0f);
      Assert::AreEqual(2.0f, majorPixels / minorPixels, 1e-3f, L"a ground circle is twice as wide as it is tall");
    }
  }

  TEST_METHOD(CosmeticHeightLiftsAShipWithoutMovingItSideways)
  {
    // ADR-001 §2: presentation may add cosmetic z. It must not shift a hull off
    // the plane point it is picked at, or selection stops matching what is seen.
    IsoCamera camera = MakeCamera(30.0f);
    camera.SetFocus(XMFLOAT2{0.0f, 0.0f});

    const XMFLOAT4X4 viewProjection = camera.ViewProjectionMatrix();
    const XMFLOAT3 onPlane = ProjectToClip(viewProjection, 0.0f, 0.0f, 0.0f);
    const XMFLOAT3 hovering = ProjectToClip(viewProjection, 0.0f, 250.0f, 0.0f);

    Assert::AreEqual(onPlane.x, hovering.x, 1e-4f, L"height is not a sideways shift");
    Assert::IsTrue(hovering.y > onPlane.y, L"height moves a ship up the screen");
  }

  TEST_METHOD(TheWholeGridStaysInsideTheClipRange)
  {
    // The eye distance exists to clear the far corner of a 40 km grid at full
    // zoom-out; a corner outside [0,1] would be a ship that vanishes.
    IsoCamera camera = MakeCamera(45.0f, IsoCamera::MAX_ZOOM_METRES);
    camera.SetFocus(XMFLOAT2{-IsoCamera::PLAY_AREA_HALF_EXTENT_METRES, -IsoCamera::PLAY_AREA_HALF_EXTENT_METRES});

    const XMFLOAT4X4 viewProjection = camera.ViewProjectionMatrix();
    for (float x : {-20000.0f, 20000.0f})
    {
      for (float z : {-20000.0f, 20000.0f})
      {
        const XMFLOAT3 corner = ProjectToClip(viewProjection, x, 500.0f, z);
        Assert::IsTrue(corner.z >= 0.0f && corner.z <= 1.0f, L"a grid corner must be inside the depth range");
      }
    }
  }
};

TEST_CLASS(IsoCameraStateTests)
{
public:
  TEST_METHOD(ZoomIsMultiplicativeAndClamped)
  {
    IsoCamera camera;
    camera.SetZoomMetres(8000.0f);

    camera.Zoom(1.0f);
    Assert::AreEqual(8000.0f / IsoCamera::ZOOM_STEP_FACTOR, camera.ZoomMetres(), 0.01f, L"a notch in divides by the step factor");
    camera.Zoom(-1.0f);
    Assert::AreEqual(8000.0f, camera.ZoomMetres(), 0.01f, L"a notch back returns to where it started");

    camera.Zoom(1000.0f);
    Assert::AreEqual(IsoCamera::MIN_ZOOM_METRES, camera.ZoomMetres(), 0.01f);
    camera.Zoom(-1000.0f);
    Assert::AreEqual(IsoCamera::MAX_ZOOM_METRES, camera.ZoomMetres(), 0.01f);

    camera.SetZoomMetres(-5.0f);
    Assert::AreEqual(IsoCamera::MIN_ZOOM_METRES, camera.ZoomMetres(), 0.01f, L"a nonsense zoom clamps rather than inverting the view");
  }

  TEST_METHOD(YawSnapsToDetentsAndStepsWholeOnes)
  {
    IsoCamera camera;
    camera.SetYawSnapDegrees(45.0f);

    camera.SetYawRadians(50.0f * XM_PI / 180.0f);
    camera.SnapYawToDetent();
    Assert::AreEqual(45.0f, camera.YawRadians() * 180.0f / XM_PI, 1e-3f);

    camera.NudgeYawDetents(1);
    Assert::AreEqual(90.0f, camera.YawRadians() * 180.0f / XM_PI, 1e-3f);
    camera.NudgeYawDetents(-3);
    Assert::AreEqual(-45.0f, camera.YawRadians() * 180.0f / XM_PI, 1e-3f);

    // A nudge after a free orbit lands on a detent rather than carrying the
    // drag's leftover fraction forward for ever.
    camera.SetYawRadians(0.0f);
    camera.Orbit(0.05f);
    camera.NudgeYawDetents(1);
    Assert::AreEqual(45.0f, camera.YawRadians() * 180.0f / XM_PI, 1e-3f);
  }

  TEST_METHOD(SnappingDisabledLeavesTheYawAlone)
  {
    IsoCamera camera;
    camera.SetYawSnapDegrees(0.0f);
    camera.SetYawRadians(0.3f);
    camera.SnapYawToDetent();
    camera.NudgeYawDetents(2);
    Assert::AreEqual(0.3f, camera.YawRadians(), 1e-6f);
  }

  TEST_METHOD(PanUndoesForeshorteningAndStaysInsideTheGrid)
  {
    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetZoomMetres(9000.0f); // 18000 metres over 900 pixels: 20 m/px.
    Assert::AreEqual(20.0f, camera.MetresPerPixel(), 1e-4f);

    camera.SetFocus(XMFLOAT2{0.0f, 0.0f});
    camera.PanPixels(100.0f, 0.0f);
    Assert::AreEqual(2000.0f, camera.Focus().x, 0.01f, L"100 px right is 2 km at yaw 0");
    Assert::AreEqual(0.0f, camera.Focus().y, 0.01f);

    camera.SetFocus(XMFLOAT2{0.0f, 0.0f});
    camera.PanPixels(0.0f, 100.0f);
    Assert::AreEqual(4000.0f, camera.Focus().y, 0.01f, L"100 px up is 4 km: the foreshortening is undone");

    camera.SetFocus(XMFLOAT2{0.0f, 0.0f});
    for (int i = 0; i < 200; ++i)
    {
      camera.PanPixels(1000.0f, 1000.0f);
    }
    Assert::AreEqual(IsoCamera::PLAY_AREA_HALF_EXTENT_METRES, camera.Focus().x, 0.01f, L"panning stops at the edge of the grid");
    Assert::AreEqual(IsoCamera::PLAY_AREA_HALF_EXTENT_METRES, camera.Focus().y, 0.01f);
  }

  TEST_METHOD(AZeroSizedViewportDoesNotBecomeTheCameraState)
  {
    // A minimised window reports 0x0. Adopting it would divide by zero on the
    // next projection.
    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetViewport(0, 0);
    Assert::AreEqual(1600.0f / 900.0f, camera.AspectRatio(), 1e-5f);
  }
};

TEST_CLASS(CameraFitTests)
{
public:
  /// The camera at the size the prints are drawn, so the aspect is a known
  /// 16:9 and the numbers below are arithmetic rather than approximately right.
  [[nodiscard]] static IsoCamera Framed()
  {
    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetYawRadians(0.0f);
    return camera;
  }

  TEST_METHOD(TheViewCentresOnTheBoxRatherThanOnTheCentreOfMass)
  {
    /*
     * Three ships, two of them piled on the origin and one a kilometre east.
     * The centre of *mass* is 333 m out; the middle of what has to fit is 500.
     * Framing on the centroid would push the straggler toward the edge for no
     * reason other than that its wingmates are bunched up.
     */
    IsoCamera camera = Framed();
    const XMFLOAT2 fleet[] = {XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{1000.0f, 0.0f}};
    camera.FocusOn(fleet, 0.0f);

    Assert::AreEqual(500.0f, camera.Focus().x, 0.01f);
    Assert::AreEqual(0.0f, camera.Focus().y, 0.01f);
  }

  TEST_METHOD(ASpreadAcrossTheScreenAndOneIntoItCostDifferentZooms)
  {
    /*
     * The reason the box is measured along the *screen* axes. Twenty kilometres
     * of fleet needs a different zoom depending on which way it lies, and the
     * two constants that decide it pull opposite ways: the view is wider than
     * it is tall (16:9), and the plane is foreshortened into the screen by
     * sin(30 degrees).
     *
     * East-west: half of 20 km over the aspect -> 5,625.
     * North-south: half of 20 km times the elevation sine -> 5,000.
     */
    IsoCamera eastWest = Framed();
    const XMFLOAT2 abreast[] = {XMFLOAT2{-10000.0f, 0.0f}, XMFLOAT2{10000.0f, 0.0f}};
    eastWest.FocusOn(abreast, 0.0f);
    Assert::AreEqual(5625.0f, eastWest.ZoomMetres(), 0.5f, L"the aspect decides when the fleet lies across");

    IsoCamera intoIt = Framed();
    const XMFLOAT2 inLine[] = {XMFLOAT2{0.0f, -10000.0f}, XMFLOAT2{0.0f, 10000.0f}};
    intoIt.FocusOn(inLine, 0.0f);
    Assert::AreEqual(5000.0f, intoIt.ZoomMetres(), 0.5f, L"and the foreshortening decides when it lies away");
  }

  TEST_METHOD(WhicheverAxisNeedsMoreZoomIsTheOneThatDecides)
  {
    // Fitting one axis and overflowing the other is not fitting.
    IsoCamera camera = Framed();
    const XMFLOAT2 box[] = {XMFLOAT2{-10000.0f, -10000.0f}, XMFLOAT2{10000.0f, 10000.0f}};
    camera.FocusOn(box, 0.0f);
    Assert::AreEqual(5625.0f, camera.ZoomMetres(), 0.5f, L"the wider requirement wins");
  }

  TEST_METHOD(OrbitingChangesWhichWayTheFleetIsSpread)
  {
    /*
     * The same fleet, the camera turned a quarter turn: what was across the
     * screen is now into it. A fit computed in world axes would not notice, and
     * would frame one of the two wrongly.
     */
    const XMFLOAT2 fleet[] = {XMFLOAT2{-10000.0f, 0.0f}, XMFLOAT2{10000.0f, 0.0f}};

    IsoCamera facingNorth = Framed();
    facingNorth.FocusOn(fleet, 0.0f);

    IsoCamera turned = Framed();
    turned.SetYawRadians(1.5707963f);
    turned.FocusOn(fleet, 0.0f);

    Assert::AreEqual(5625.0f, facingNorth.ZoomMetres(), 0.5f);
    Assert::AreEqual(5000.0f, turned.ZoomMetres(), 0.5f, L"a quarter turn makes it the other constraint");
  }

  TEST_METHOD(TheMarginBuysAirAroundTheOutermostHull)
  {
    // A framing that put the last ship exactly on the edge would read as the
    // fleet being clipped rather than as it being shown.
    IsoCamera tight = Framed();
    IsoCamera roomy = Framed();
    const XMFLOAT2 fleet[] = {XMFLOAT2{-10000.0f, 0.0f}, XMFLOAT2{10000.0f, 0.0f}};

    tight.FocusOn(fleet, 0.0f);
    roomy.FocusOn(fleet, 0.2f);

    Assert::AreEqual(6750.0f, roomy.ZoomMetres(), 0.5f, L"a fifth more box is a fifth more zoom");
    Assert::IsTrue(roomy.ZoomMetres() > tight.ZoomMetres());
    Assert::AreEqual(tight.Focus().x, roomy.Focus().x, 0.01f, L"and the margin does not move the middle");
  }

  TEST_METHOD(ANegativeMarginIsReadAsNone)
  {
    // Rather than as a crop. A caller doing arithmetic on chrome fractions can
    // land slightly under zero, and cropping the fleet is not what they meant.
    IsoCamera camera = Framed();
    IsoCamera none = Framed();
    const XMFLOAT2 fleet[] = {XMFLOAT2{-10000.0f, 0.0f}, XMFLOAT2{10000.0f, 0.0f}};

    camera.FocusOn(fleet, -0.5f);
    none.FocusOn(fleet, 0.0f);
    Assert::AreEqual(none.ZoomMetres(), camera.ZoomMetres(), 0.5f);
  }

  TEST_METHOD(AnAutomaticFramingNeverGoesCloserThanItsOwnFloor)
  {
    /*
     * A one-ship wing fits at any zoom, so the fit alone would slam the view
     * from a sixteen-kilometre field to the camera's minimum -- and pressing
     * two rows in turn would swing between them. The floor is on *this control*
     * and not on the camera: the wheel still reaches `MIN_ZOOM_METRES`.
     */
    IsoCamera camera = Framed();
    const XMFLOAT2 alone[] = {XMFLOAT2{4000.0f, -2000.0f}};
    camera.FocusOn(alone, 0.12f);

    Assert::AreEqual(IsoCamera::MIN_FIT_ZOOM_METRES, camera.ZoomMetres(), 0.5f);
    Assert::AreEqual(4000.0f, camera.Focus().x, 0.01f, L"but it still looks at the ship");
    Assert::AreEqual(-2000.0f, camera.Focus().y, 0.01f);

    Assert::IsTrue(IsoCamera::MIN_FIT_ZOOM_METRES > IsoCamera::MIN_ZOOM_METRES, L"the floor is above the camera's");
    camera.SetZoomMetres(IsoCamera::MIN_ZOOM_METRES);
    Assert::AreEqual(IsoCamera::MIN_ZOOM_METRES, camera.ZoomMetres(), 0.5f, L"which the wheel can still reach");
  }

  TEST_METHOD(FramingNothingLeavesTheViewWhereThePlayerPutIt)
  {
    IsoCamera camera = Framed();
    camera.SetFocus(XMFLOAT2{1234.0f, -567.0f});
    camera.SetZoomMetres(9000.0f);

    camera.FocusOn(std::span<const XMFLOAT2>{}, 0.12f);

    Assert::AreEqual(1234.0f, camera.Focus().x, 0.01f);
    Assert::AreEqual(-567.0f, camera.Focus().y, 0.01f);
    Assert::AreEqual(9000.0f, camera.ZoomMetres(), 0.5f);
  }

  TEST_METHOD(AFleetAtTheEdgeCannotPanTheViewOffThePlayArea)
  {
    // `SetFocus`' clamp still applies -- a framing is not an exemption from the
    // rule that keeps the camera out of empty float space.
    IsoCamera camera = Framed();
    const XMFLOAT2 farOut[] = {XMFLOAT2{500000.0f, 500000.0f}};
    camera.FocusOn(farOut, 0.0f);

    Assert::AreEqual(IsoCamera::PLAY_AREA_HALF_EXTENT_METRES, camera.Focus().x, 0.5f);
    Assert::AreEqual(IsoCamera::PLAY_AREA_HALF_EXTENT_METRES, camera.Focus().y, 0.5f);
  }

  TEST_METHOD(TheWholePlayAreaStillFitsInsideTheCamerasRange)
  {
    /*
     * The largest framing that can legitimately be asked for: two ships at
     * opposite corners of the play area. It has to come out *under* the
     * camera's ceiling, or the widest real fleet in this game would be one the
     * fit silently could not show.
     */
    IsoCamera camera = Framed();
    constexpr float EDGE = IsoCamera::PLAY_AREA_HALF_EXTENT_METRES;
    const XMFLOAT2 corners[] = {XMFLOAT2{-EDGE, -EDGE}, XMFLOAT2{EDGE, EDGE}};
    camera.FocusOn(corners, 0.12f);

    Assert::IsTrue(camera.ZoomMetres() < IsoCamera::MAX_ZOOM_METRES, L"and with room to spare");
    Assert::AreEqual(0.0f, camera.Focus().x, 0.5f, L"centred, because the corners are symmetric");
  }

  TEST_METHOD(CoordinatesThePlayAreaDoesNotAllowClampRatherThanBreakTheProjection)
  {
    // Not a fleet this game can have -- a robustness assertion. A zoom past the
    // ceiling would put the near and far planes somewhere the projection cannot
    // hold, which is a broken frame rather than a wide one.
    IsoCamera camera = Framed();
    const XMFLOAT2 nonsense[] = {XMFLOAT2{-200000.0f, 0.0f}, XMFLOAT2{200000.0f, 0.0f}};
    camera.FocusOn(nonsense, 0.0f);

    Assert::AreEqual(IsoCamera::MAX_ZOOM_METRES, camera.ZoomMetres(), 0.5f);
  }
};

TEST_CLASS(CameraInputTests)
{
public:
  TEST_METHOD(MiddleDragPansAgainstTheCursor)
  {
    const CameraTuning tuning;
    InputFrame input;
    input.viewportWidth = 1600;
    input.viewportHeight = 900;
    input.buttonDown[static_cast<std::uint32_t>(InputButton::Middle)] = true;
    input.cursorDeltaX = 10;
    input.cursorDeltaY = -6;

    const CameraIntent intent = MapCameraInput(input, tuning, 1.0f / 60.0f);
    Assert::AreEqual(-10.0f, intent.panRightPixels, 1e-4f, L"dragging right moves the focus left, so the plane follows the hand");
    Assert::AreEqual(-6.0f, intent.panUpPixels, 1e-4f);
    Assert::AreEqual(0.0f, intent.orbitRadians, 1e-6f, L"a plain middle-drag never orbits");
  }

  TEST_METHOD(AltTurnsTheSameDragIntoAnOrbitAndReleaseSnaps)
  {
    const CameraTuning tuning;
    InputFrame input;
    input.buttonDown[static_cast<std::uint32_t>(InputButton::Middle)] = true;
    input.actionDown[static_cast<std::uint32_t>(InputAction::Modifier)] = true;
    input.cursorDeltaX = 10;
    input.cursorDeltaY = 40;

    CameraIntent intent = MapCameraInput(input, tuning, 1.0f / 60.0f);
    Assert::AreEqual(10.0f * tuning.orbitRadiansPerPixel, intent.orbitRadians, 1e-6f);
    Assert::AreEqual(0.0f, intent.panRightPixels, 1e-6f, L"an orbit does not also pan");
    Assert::IsFalse(intent.snapYaw);

    input.buttonDown[static_cast<std::uint32_t>(InputButton::Middle)] = false;
    input.buttonReleased[static_cast<std::uint32_t>(InputButton::Middle)] = true;
    intent = MapCameraInput(input, tuning, 1.0f / 60.0f);
    Assert::IsTrue(intent.snapYaw, L"letting go of an orbit drops onto the nearest detent");
  }

  TEST_METHOD(TheLeftAndRightButtonsAreNotTheCamerasToTake)
  {
    // Box-select (S8) and the order puck (S9) need them. A binding chosen now
    // that S9 has to take back would be a worse kind of cheap.
    const CameraTuning tuning;
    InputFrame input;
    input.buttonDown[static_cast<std::uint32_t>(InputButton::Left)] = true;
    input.buttonDown[static_cast<std::uint32_t>(InputButton::Right)] = true;
    input.cursorDeltaX = 40;
    input.cursorDeltaY = 40;

    const CameraIntent intent = MapCameraInput(input, tuning, 1.0f / 60.0f);
    Assert::AreEqual(0.0f, intent.panRightPixels, 1e-6f);
    Assert::AreEqual(0.0f, intent.panUpPixels, 1e-6f);
    Assert::AreEqual(0.0f, intent.orbitRadians, 1e-6f);
  }

  TEST_METHOD(TheWheelKeepsItsFractions)
  {
    const CameraTuning tuning;
    InputFrame input;
    input.wheelSteps = 0.5f;
    Assert::AreEqual(0.5f, MapCameraInput(input, tuning, 1.0f / 60.0f).zoomSteps, 1e-6f);
  }

  TEST_METHOD(KeyEdgesStepDetentsAndKeyLevelsPan)
  {
    const CameraTuning tuning;
    InputFrame input;

    input.actionPressed[static_cast<std::uint32_t>(InputAction::YawRight)] = true;
    Assert::AreEqual(1, MapCameraInput(input, tuning, 1.0f / 60.0f).yawDetentSteps);

    // Holding the key is not a second detent: only the edge counts.
    input.actionPressed[static_cast<std::uint32_t>(InputAction::YawRight)] = false;
    input.actionDown[static_cast<std::uint32_t>(InputAction::YawRight)] = true;
    Assert::AreEqual(0, MapCameraInput(input, tuning, 1.0f / 60.0f).yawDetentSteps);

    InputFrame panning;
    panning.actionDown[static_cast<std::uint32_t>(InputAction::PanForward)] = true;
    const CameraIntent intent = MapCameraInput(panning, tuning, 0.5f);
    Assert::AreEqual(tuning.keyboardPanPixelsPerSecond * 0.5f, intent.panUpPixels, 1e-3f, L"a held pan key is rate x time");
  }

  TEST_METHOD(EdgePanNeedsFocusAndYieldsToADrag)
  {
    const CameraTuning tuning;
    InputFrame input;
    input.viewportWidth = 1600;
    input.viewportHeight = 900;
    input.cursorX = 1599;
    input.cursorY = 450;
    input.windowFocused = true;
    input.cursorInsideWindow = true;

    Assert::AreEqual(tuning.edgePanPixelsPerSecond, MapCameraInput(input, tuning, 1.0f).panRightPixels, 1e-3f);

    input.cursorY = 0;
    input.cursorX = 800;
    Assert::AreEqual(tuning.edgePanPixelsPerSecond, MapCameraInput(input, tuning, 1.0f).panUpPixels, 1e-3f,
                     L"the top edge pans forward, not backward");

    input.windowFocused = false;
    Assert::AreEqual(0.0f, MapCameraInput(input, tuning, 1.0f).panUpPixels, 1e-6f, L"an unfocused window does not edge-pan");

    input.windowFocused = true;
    input.buttonDown[static_cast<std::uint32_t>(InputButton::Middle)] = true;
    Assert::AreEqual(0.0f, MapCameraInput(input, tuning, 1.0f).panUpPixels, 1e-6f, L"a drag is not fought by the edge");
  }

  TEST_METHOD(ApplyingAnIntentMovesTheCamera)
  {
    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetZoomMetres(9000.0f);
    camera.SetYawSnapDegrees(45.0f);
    camera.SetFocus(XMFLOAT2{5000.0f, 5000.0f});
    camera.SetYawRadians(0.4f);

    CameraIntent intent;
    intent.resetView = true;
    ApplyCameraIntent(camera, intent);

    Assert::AreEqual(0.0f, camera.Focus().x, 1e-4f, L"reset returns to the anchor");
    Assert::AreEqual(0.0f, camera.YawRadians(), 1e-4f);

    CameraIntent zoomAndPan;
    zoomAndPan.zoomSteps = 1.0f;
    zoomAndPan.panRightPixels = 10.0f;
    ApplyCameraIntent(camera, zoomAndPan);

    // Pan is applied after zoom, so it uses the zoom the player just asked for
    // rather than the one they had a frame ago.
    Assert::AreEqual(9000.0f / IsoCamera::ZOOM_STEP_FACTOR, camera.ZoomMetres(), 0.01f);
    Assert::AreEqual(10.0f * camera.MetresPerPixel(), camera.Focus().x, 0.01f);
  }
};

} // namespace NeuronClientTests
