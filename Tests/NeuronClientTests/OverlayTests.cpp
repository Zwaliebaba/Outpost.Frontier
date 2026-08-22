#include "pch.h"
#include "CppUnitTest.h"

#include "IsoCamera.h"
#include "OverlayMark.h"
#include "RenderWorld.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * The marks the OverlayWorld pass draws (Build Order S8, ADR-006 §8).
 *
 * Which marks exist, where they sit and how big they are is arithmetic, so it
 * is asserted here. What a GPU still has to confirm is the half these tests
 * cannot reach: that rings occlude behind a Carrier hull and bars never do,
 * which is a depth state rather than a number.
 */

namespace NeuronClientTests
{
namespace
{

[[nodiscard]] SceneEntity Ship(std::uint16_t _id, float _x, float _y, float _pickRadius, std::uint8_t _hull = 255,
                               std::uint8_t _shield = 255, bool _stale = false)
{
  SceneEntity entity;
  entity.id = _id;
  entity.planeMetres = XMFLOAT2{_x, _y};
  entity.pickRadiusMetres = _pickRadius;
  entity.hullGauge = _hull;
  entity.shieldGauge = _shield;
  entity.stale = _stale;
  return entity;
}

/// Metres per pixel at an 8 km zoom and a 900-pixel viewport: 2 * 8000 / 900.
constexpr float METRES_PER_PIXEL_AT_8KM = 17.777779f;

/// Close enough in that the screen floor cannot dominate a hull radius.
constexpr float METRES_PER_PIXEL_CLOSE = 0.1f;

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

} // namespace

TEST_CLASS(OverlayMarkTests)
{
public:
  TEST_METHOD(NothingSelectedDrawsNothing)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f)};
    OverlayMarkList marks;
    BuildOverlayMarks(entities, {}, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual<std::size_t>(0, marks.marks.size());
    Assert::AreEqual<std::uint32_t>(0, marks.ringCount);
    Assert::AreEqual<std::uint32_t>(0, marks.BarCount());
  }

  TEST_METHOD(OneSelectedShipIsARingAndTwoBars)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f)};
    const std::vector<EntityId> selected = {1};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual<std::size_t>(3, marks.marks.size());
    Assert::AreEqual<std::uint32_t>(1, marks.ringCount);
    Assert::AreEqual<std::uint32_t>(2, marks.BarCount());
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::SelectionRing));
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::HullBar));
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::ShieldBar));
  }

  TEST_METHOD(RingsComeBeforeBarsSoTheSplitIsContiguous)
  {
    // The pass issues two draws over two ranges, so a bar in the ring range
    // would be drawn depth-tested and disappear behind the ship it describes.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f), Ship(2, 500.0f, 0.0f, 40.0f),
                                               Ship(3, 900.0f, 0.0f, 90.0f)};
    const std::vector<EntityId> selected = {1, 2, 3};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual<std::uint32_t>(3, marks.ringCount);
    Assert::AreEqual<std::uint32_t>(6, marks.BarCount());

    for (std::uint32_t index = 0; index < marks.ringCount; ++index)
    {
      Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OverlayKind::SelectionRing), marks.marks[index].kind);
    }
    for (std::size_t index = marks.ringCount; index < marks.marks.size(); ++index)
    {
      Assert::AreNotEqual<std::uint16_t>(static_cast<std::uint16_t>(OverlayKind::SelectionRing), marks.marks[index].kind);
    }
  }

  TEST_METHOD(ARingSitsOutsideTheHullItSurrounds)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 100.0f, -200.0f, 90.0f)};
    const std::vector<EntityId> selected = {1};
    const OverlayTuning tuning;

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, tuning, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual(90.0f + tuning.ringPadMetres, marks.marks[0].radiusMetres, 1e-3f);
    Assert::AreEqual(100.0f, marks.marks[0].anchorPlane.x, 1e-3f);
    Assert::AreEqual(-200.0f, marks.marks[0].anchorPlane.y, 1e-3f);
  }

  TEST_METHOD(TheScreenClampKeepsADistantRingVisible)
  {
    // A selected Interceptor at 40 km: twelve metres plus eight of padding is a
    // fraction of a pixel, and a ring nobody can see is a selection nobody can
    // see (ADR-006 §8's scaling law).
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f)};
    const std::vector<EntityId> selected = {1};
    const OverlayTuning tuning;

    IsoCamera camera;
    camera.SetViewport(1600, 900);
    camera.SetZoomMetres(IsoCamera::MAX_ZOOM_METRES);

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, tuning, camera.MetresPerPixel(), marks);

    const float expected = tuning.ringMinRadiusPixels * camera.MetresPerPixel();
    Assert::AreEqual(expected, marks.marks[0].radiusMetres, 1e-2f);
    Assert::IsTrue(marks.marks[0].radiusMetres > 12.0f + tuning.ringPadMetres, L"the clamp should dominate at 40 km");
  }

  TEST_METHOD(TheClampIsAFloorAndNotAReplacement)
  {
    // Zoomed in on a Carrier, the hull is bigger than the floor and wins.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 90.0f)};
    const std::vector<EntityId> selected = {1};
    const OverlayTuning tuning;

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, tuning, METRES_PER_PIXEL_CLOSE, marks);
    Assert::AreEqual(90.0f + tuning.ringPadMetres, marks.marks[0].radiusMetres, 1e-3f);
  }

  TEST_METHOD(AFrozenShipsRingIsColouredDifferently)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f, 255, 255, true)};
    const std::vector<EntityId> selected = {1};
    const OverlayTuning tuning;

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, tuning, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual(tuning.staleRingColourRgba, marks.marks[0].colourRgba,
                     L"a frozen ship must never be mistaken for a stopped one");
    Assert::AreNotEqual(tuning.ringColourRgba, marks.marks[0].colourRgba);
  }

  TEST_METHOD(AFrozenShipGetsAStaleMarkerWhetherOrNotItIsSelected)
  {
    // The icon sheet's STALE state marker (S14). Staleness is a fact about the
    // feed, not the selection: the ship the player most needs warned about is
    // the one they were not looking at.
    const std::vector<SceneEntity> entities = {Ship(1, 10.0f, 20.0f, 12.0f, 255, 255, true),
                                               Ship(2, 30.0f, 40.0f, 12.0f, 255, 255, false)};
    const OverlayTuning tuning;

    OverlayMarkList marks;
    BuildOverlayMarks(entities, {}, tuning, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual<std::uint32_t>(0, marks.ringCount, L"nothing selected, so no plane-lying marks");
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::StaleMarker),
                                    L"one marker for the one frozen ship, and none for the live one");

    const OverlayMark& marker = marks.marks[0];
    Assert::AreEqual(10.0f, marker.anchorPlane.x, 1e-6f);
    Assert::AreEqual(20.0f, marker.anchorPlane.y, 1e-6f);
    Assert::AreEqual(tuning.staleMarkerRadiusPixels, marker.halfWidthPixels, 1e-6f,
                     L"screen-facing: sized in pixels so it survives every zoom");
    Assert::AreEqual<std::uint16_t>(tuning.staleMarkerDashCount, marker.fill,
                                    L"dashed -- the sheet's convention for the unresolved");
    Assert::AreEqual(tuning.staleRingColourRgba, marker.colourRgba);
  }

  TEST_METHOD(TheStaleMarkerIsScreenFacingAndNeverInTheDepthTestedHalf)
  {
    // A readout about the feed must not hide behind the hull it is warning
    // about, so the marker lives past the ring/bar split with the bars.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f, 255, 255, true)};
    const std::vector<EntityId> selected = {1};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    Assert::IsTrue(static_cast<std::uint16_t>(OverlayKind::StaleMarker) >=
                   static_cast<std::uint16_t>(OverlayKind::FIRST_SCREEN_FACING));
    for (std::uint32_t index = 0; index < marks.ringCount; ++index)
    {
      Assert::AreNotEqual<std::uint16_t>(static_cast<std::uint16_t>(OverlayKind::StaleMarker), marks.marks[index].kind,
                                         L"no marker below the split");
    }
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::StaleMarker));
  }

  TEST_METHOD(AGaugeIsTwoFiftyFiveNotAHundred)
  {
    // A ship spawns at 255 and that is full. Treating it as a percentage would
    // put the bar two and a half times past its own end.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f, 255, 0)};
    const std::vector<EntityId> selected = {1};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    const OverlayMark& hull = marks.marks[marks.ringCount];
    const OverlayMark& shield = marks.marks[marks.ringCount + 1];

    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OverlayKind::HullBar), hull.kind);
    Assert::AreEqual<std::uint16_t>(65535, hull.fill, L"255 is full, and full is the whole bar");
    Assert::AreEqual<std::uint16_t>(static_cast<std::uint16_t>(OverlayKind::ShieldBar), shield.kind);
    Assert::AreEqual<std::uint16_t>(0, shield.fill);
  }

  TEST_METHOD(AHalfGaugeIsHalfABar)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f, 128, 64)};
    const std::vector<EntityId> selected = {1};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    // 128/255 and 64/255 of full scale, within one part in a thousand.
    Assert::AreEqual(128.0f / 255.0f, static_cast<float>(marks.marks[marks.ringCount].fill) / 65535.0f, 1e-3f);
    Assert::AreEqual(64.0f / 255.0f, static_cast<float>(marks.marks[marks.ringCount + 1].fill) / 65535.0f, 1e-3f);
  }

  TEST_METHOD(TheShieldBarSitsAboveTheHullBar)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f)};
    const std::vector<EntityId> selected = {1};
    const OverlayTuning tuning;

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, tuning, METRES_PER_PIXEL_CLOSE, marks);

    const OverlayMark& hull = marks.marks[marks.ringCount];
    const OverlayMark& shield = marks.marks[marks.ringCount + 1];

    Assert::AreEqual(tuning.barOffsetUpPixels, hull.offsetUpPixels, 1e-3f);
    Assert::IsTrue(shield.offsetUpPixels > hull.offsetUpPixels, L"shield above hull: it is what depletes first");

    // And they do not overlap: the gap is clear of both half-heights.
    const float clearance = shield.offsetUpPixels - hull.offsetUpPixels - 2.0f * tuning.barHalfHeightPixels;
    Assert::AreEqual(tuning.barGapPixels, clearance, 1e-3f);
  }

  TEST_METHOD(BarsAreSizedInPixelsAndRingsInMetres)
  {
    // The whole reason for two spaces: a bar has to stay legible as the camera
    // zooms, and a ring has to stay glued to the ship.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f)};
    const std::vector<EntityId> selected = {1};
    const OverlayTuning tuning;

    OverlayMarkList close;
    BuildOverlayMarks(entities, selected, tuning, METRES_PER_PIXEL_CLOSE, close);
    OverlayMarkList far;
    BuildOverlayMarks(entities, selected, tuning, METRES_PER_PIXEL_AT_8KM, far);

    // The bars are identical at both zooms.
    Assert::AreEqual(close.marks[close.ringCount].halfWidthPixels, far.marks[far.ringCount].halfWidthPixels, 1e-4f);
    Assert::AreEqual(close.marks[close.ringCount].halfHeightPixels, far.marks[far.ringCount].halfHeightPixels, 1e-4f);

    // The ring is not: it grew to meet the screen floor.
    Assert::IsTrue(far.marks[0].radiusMetres > close.marks[0].radiusMetres);

    // And a ring carries no pixel size, nor a bar a plane radius, so the shader
    // cannot read the wrong one.
    Assert::AreEqual(0.0f, close.marks[0].halfWidthPixels);
    Assert::AreEqual(0.0f, close.marks[close.ringCount].radiusMetres);
  }

  TEST_METHOD(ASelectedShipThatIsGoneDrawsNothing)
  {
    // Selection::Retain should have removed it the same frame. A ring at the
    // origin is a bug that looks like a feature, so this is the second line.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f)};
    const std::vector<EntityId> selected = {1, 77};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, selected, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual<std::uint32_t>(1, marks.ringCount);
    Assert::AreEqual<std::uint32_t>(2, marks.BarCount());
  }

  TEST_METHOD(ADamagedShipGetsBarsWithoutBeingSelected)
  {
    // The print's rule: bars over the selection plus anything with hull below
    // 100%. The damaged ship the player has not selected is exactly the one
    // the readout exists for; the healthy unselected one stays clean.
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f, 200, 255), Ship(2, 50.0f, 0.0f, 12.0f)};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, {}, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);

    Assert::AreEqual<std::uint32_t>(0, marks.ringCount, L"nothing selected, so no rings");
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::HullBar));
    Assert::AreEqual<std::uint32_t>(1, CountOfKind(marks, OverlayKind::ShieldBar));
    Assert::AreEqual(0.0f, marks.marks[0].anchorPlane.x, 1e-6f, L"and they sit on the hurt ship");
  }

  TEST_METHOD(TheHullBarBandsOnTheRosterStripsThresholds)
  {
    // Healthy, worn, low -- the same three bands at the same 70%/40% gauge
    // boundaries as `HullGaugeFill`, so a wing's roster strip and its ships'
    // bars cannot disagree about what worn means. Shield stays the allied fill.
    const OverlayTuning tuning;
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f, 200, 100), Ship(2, 10.0f, 0.0f, 12.0f, 150, 100),
                                               Ship(3, 20.0f, 0.0f, 12.0f, 50, 100)};

    OverlayMarkList marks;
    BuildOverlayMarks(entities, {}, tuning, METRES_PER_PIXEL_CLOSE, marks);
    Assert::AreEqual<std::uint32_t>(6, marks.BarCount());

    const auto hullColourAt = [&](std::size_t _ship) { return marks.marks[_ship * 2].colourRgba; };
    const auto shieldColourAt = [&](std::size_t _ship) { return marks.marks[_ship * 2 + 1].colourRgba; };
    Assert::AreEqual(tuning.hullColourRgba, hullColourAt(0), L"200 of 255 is healthy");
    Assert::AreEqual(tuning.hullWornColourRgba, hullColourAt(1), L"150 of 255 is worn");
    Assert::AreEqual(tuning.hullLowColourRgba, hullColourAt(2), L"50 of 255 is low");
    for (std::size_t ship = 0; ship < 3; ++ship)
    {
      Assert::AreEqual(tuning.shieldColourRgba, shieldColourAt(ship), L"the shield never changes band");
    }
  }

  TEST_METHOD(BuildingTwiceLeavesNoResidue)
  {
    const std::vector<SceneEntity> entities = {Ship(1, 0.0f, 0.0f, 12.0f), Ship(2, 500.0f, 0.0f, 40.0f)};
    OverlayMarkList marks;

    BuildOverlayMarks(entities, std::vector<EntityId>{1, 2}, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);
    Assert::AreEqual<std::size_t>(6, marks.marks.size());

    BuildOverlayMarks(entities, std::vector<EntityId>{2}, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);
    Assert::AreEqual<std::size_t>(3, marks.marks.size());
    Assert::AreEqual<std::uint32_t>(1, marks.ringCount);

    BuildOverlayMarks(entities, {}, OverlayTuning{}, METRES_PER_PIXEL_CLOSE, marks);
    Assert::AreEqual<std::size_t>(0, marks.marks.size());
    Assert::AreEqual<std::uint32_t>(0, marks.ringCount);
  }
};

} // namespace NeuronClientTests
