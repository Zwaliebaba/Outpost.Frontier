#include "pch.h"
#include "CppUnitTest.h"

#include "GhostLane.h"
#include "IsoCamera.h"
#include "OrderGhost.h"
#include "UiDrawList.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;
using namespace DirectX;

/*
 * The ghost's lane -- ADR-006 §8's draw list *(B)*, deferred from S9 (S11c).
 *
 * Everything here is arithmetic over a mapping and a viewport, which is the
 * point: the primitive these tests drive is an *oriented* quad, and the one
 * thing about it that cannot be checked on this machine is whether the vertex
 * shader sweeps it the right way. So the CPU half is checked hard.
 */

namespace NeuronClientTests
{
namespace
{

/// A viewport-sized world rect, so a test that is not about clipping is not
/// accidentally about clipping.
constexpr std::uint32_t VIEWPORT_WIDTH = 1600;
constexpr std::uint32_t VIEWPORT_HEIGHT = 900;

[[nodiscard]] GhostLaneView MakeView(const PlaneMapping& _mapping)
{
  GhostLaneView view;
  view.mapping = _mapping;
  view.viewportWidth = VIEWPORT_WIDTH;
  view.viewportHeight = VIEWPORT_HEIGHT;
  view.worldRect = UiRect{0.0f, 0.0f, static_cast<float>(VIEWPORT_WIDTH), static_cast<float>(VIEWPORT_HEIGHT)};
  view.cellPixels = 8.0f;
  view.scale = 1.0f;
  return view;
}

/// A camera looking at the origin, wide enough that a few kilometres is a few
/// hundred pixels. The real one, not a hand-built mapping: the lane's whole job
/// is to agree with what the overlay drew.
[[nodiscard]] PlaneMapping MakeMapping()
{
  IsoCamera camera;
  camera.SetViewport(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
  camera.SetFocus(XMFLOAT2{0.0f, 0.0f});
  return camera.PlaneMappingForNdc();
}

[[nodiscard]] OrderGhost MakeGhost(GhostState _state, const XMFLOAT2& _origin, const XMFLOAT2& _target)
{
  OrderGhost ghost;
  ghost.clientOrderSeq = 1;
  ghost.state = _state;
  ghost.originMetres = _origin;
  ghost.targetMetres = _target;
  ghost.preview.etaSeconds = 92.0f;
  std::snprintf(ghost.preview.label, sizeof(ghost.preview.label), "Move - Claw");

  // Its own target is its first leg, which is what `OrderGhostList::Add` does.
  // A ghost built by hand without this has no plan and draws nothing.
  ghost.legs[0] = GhostLeg{_target, ghost.preview.etaSeconds};
  ghost.queuedLegCount = 1;
  return ghost;
}

/// The same ghost with waypoints appended -- what a chain looks like after an
/// append is accepted and merged.
[[nodiscard]] OrderGhost MakeChain(GhostState _state, const XMFLOAT2& _origin, std::span<const XMFLOAT2> _waypoints)
{
  OrderGhost ghost = MakeGhost(_state, _origin, _waypoints.front());
  ghost.queuedLegCount = 0;
  for (const XMFLOAT2& waypoint : _waypoints)
  {
    ghost.legs[ghost.queuedLegCount] = GhostLeg{waypoint, 30.0f + 10.0f * static_cast<float>(ghost.queuedLegCount)};
    ++ghost.queuedLegCount;
  }
  ghost.targetMetres = _waypoints.back();
  return ghost;
}

/// Every quad the list holds that is a segment, which is what a lane is made of.
[[nodiscard]] std::vector<UiQuad> Segments(const UiDrawList& _list)
{
  std::vector<UiQuad> found;
  for (const UiQuad& quad : _list.Quads())
  {
    if (quad.oriented)
    {
      found.push_back(quad);
    }
  }
  return found;
}

[[nodiscard]] std::vector<std::string> Texts(const UiDrawList& _list)
{
  std::vector<std::string> found;
  for (const UiTextRun& run : _list.Runs())
  {
    found.emplace_back(_list.Text(run));
  }
  return found;
}

} // namespace

TEST_CLASS(SegmentClipTests)
{
public:
  TEST_METHOD(ASegmentWhollyInsideIsUntouched)
  {
    const UiRect rect{0.0f, 0.0f, 100.0f, 100.0f};
    XMFLOAT2 start{10.0f, 20.0f};
    XMFLOAT2 end{80.0f, 90.0f};
    Assert::IsTrue(ClipSegmentToRect(rect, start, end));
    Assert::AreEqual(10.0f, start.x, 0.001f);
    Assert::AreEqual(20.0f, start.y, 0.001f);
    Assert::AreEqual(80.0f, end.x, 0.001f);
    Assert::AreEqual(90.0f, end.y, 0.001f);
  }

  TEST_METHOD(OneEndOutsideIsBroughtToTheEdge)
  {
    const UiRect rect{0.0f, 0.0f, 100.0f, 100.0f};
    XMFLOAT2 start{50.0f, 50.0f};
    XMFLOAT2 end{150.0f, 50.0f};
    Assert::IsTrue(ClipSegmentToRect(rect, start, end));
    Assert::AreEqual(50.0f, start.x, 0.001f, L"the inside end must not move");
    Assert::AreEqual(100.0f, end.x, 0.001f);
    Assert::AreEqual(50.0f, end.y, 0.001f);
  }

  TEST_METHOD(BothEndsOutsideButCrossingKeepsTheMiddle)
  {
    // The case a naive "is either endpoint inside" test gets wrong, and the
    // reason this is Liang-Barsky rather than two Contains calls: a lane from
    // far off one edge to far off the other crosses the whole screen.
    const UiRect rect{0.0f, 0.0f, 100.0f, 100.0f};
    XMFLOAT2 start{-50.0f, 50.0f};
    XMFLOAT2 end{150.0f, 50.0f};
    Assert::IsTrue(ClipSegmentToRect(rect, start, end));
    Assert::AreEqual(0.0f, start.x, 0.001f);
    Assert::AreEqual(100.0f, end.x, 0.001f);
  }

  TEST_METHOD(ASegmentWhollyOutsideIsRefusedAndLeftAlone)
  {
    /*
     * The endpoints must survive a false return. A caller that ignored the
     * result would then draw the *unclipped* line -- obviously wrong on screen
     * -- rather than a line between two half-clipped points, which would look
     * plausible and be nonsense.
     */
    const UiRect rect{0.0f, 0.0f, 100.0f, 100.0f};
    XMFLOAT2 start{200.0f, 200.0f};
    XMFLOAT2 end{300.0f, 250.0f};
    Assert::IsFalse(ClipSegmentToRect(rect, start, end));
    Assert::AreEqual(200.0f, start.x, 0.001f);
    Assert::AreEqual(300.0f, end.x, 0.001f);
  }

  TEST_METHOD(AParallelSegmentOutsideAnEdgeIsRefused)
  {
    // The zero-denominator branch, which is the one that is wrong if the
    // comparison is written the other way round: a horizontal line above the
    // rect is parallel to two of its edges and outside both.
    const UiRect rect{0.0f, 0.0f, 100.0f, 100.0f};
    XMFLOAT2 start{-10.0f, -5.0f};
    XMFLOAT2 end{110.0f, -5.0f};
    Assert::IsFalse(ClipSegmentToRect(rect, start, end));

    // And the same line inside is kept, clipped on x only.
    XMFLOAT2 insideStart{-10.0f, 50.0f};
    XMFLOAT2 insideEnd{110.0f, 50.0f};
    Assert::IsTrue(ClipSegmentToRect(rect, insideStart, insideEnd));
    Assert::AreEqual(0.0f, insideStart.x, 0.001f);
    Assert::AreEqual(100.0f, insideEnd.x, 0.001f);
    Assert::AreEqual(50.0f, insideStart.y, 0.001f, L"y was never clipped");
  }

  TEST_METHOD(ADiagonalIsClippedOnBothAxesAtOnce)
  {
    // Corner to corner and out the far side: both a vertical and a horizontal
    // edge bind, and the tighter of the two has to win on each end.
    const UiRect rect{0.0f, 0.0f, 100.0f, 100.0f};
    XMFLOAT2 start{-100.0f, -100.0f};
    XMFLOAT2 end{200.0f, 200.0f};
    Assert::IsTrue(ClipSegmentToRect(rect, start, end));
    Assert::AreEqual(0.0f, start.x, 0.001f);
    Assert::AreEqual(0.0f, start.y, 0.001f);
    Assert::AreEqual(100.0f, end.x, 0.001f);
    Assert::AreEqual(100.0f, end.y, 0.001f);
  }
};

TEST_CLASS(LaneDetailTests)
{
public:
  TEST_METHOD(TheDetailLineReadsLikeThePrint)
  {
    char buffer[48] = {};

    FormatLaneDetail(18400.0f, 101.0f, buffer, sizeof(buffer));
    Assert::AreEqual(std::string{"18.4 km - ETA 1m 41s"}, std::string{buffer});

    FormatLaneDetail(18400.0f, 41.0f, buffer, sizeof(buffer));
    Assert::AreEqual(std::string{"18.4 km - ETA 41s"}, std::string{buffer});
  }

  TEST_METHOD(AShortHopIsMetresRatherThanZeroPointZeroKilometres)
  {
    // `0.0 km` is a distance nobody ordered. Below a kilometre the print's own
    // switch is metres, and a station-keeping nudge is the common order.
    char buffer[48] = {};
    FormatLaneDetail(940.0f, 8.0f, buffer, sizeof(buffer));
    Assert::AreEqual(std::string{"940 m - ETA 8s"}, std::string{buffer});
  }

  TEST_METHOD(AnUnknownEtaLosesTheEtaAndKeepsTheDistance)
  {
    // A selection of stations: the game says it cannot tell. The distance is
    // still true, and printing a negative time would be the client answering a
    // question the game declined.
    char buffer[48] = {};
    FormatLaneDetail(3200.0f, -1.0f, buffer, sizeof(buffer));
    Assert::AreEqual(std::string{"3.2 km"}, std::string{buffer});
  }

  TEST_METHOD(TheSecondsRoundUpSoNothingArrivesInZero)
  {
    // `0s` under a ghost whose ships are visibly still moving is worse than
    // `1s` for the last fraction of a second.
    char buffer[48] = {};
    FormatLaneDetail(500.0f, 0.2f, buffer, sizeof(buffer));
    Assert::AreEqual(std::string{"500 m - ETA 1s"}, std::string{buffer});

    FormatLaneDetail(500.0f, 59.4f, buffer, sizeof(buffer));
    Assert::AreEqual(std::string{"500 m - ETA 1m 00s"}, std::string{buffer});
  }

  TEST_METHOD(ATinyBufferIsTerminatedRatherThanOverrun)
  {
    char buffer[6] = {};
    FormatLaneDetail(18400.0f, 101.0f, buffer, sizeof(buffer));
    Assert::IsTrue(std::strlen(buffer) < sizeof(buffer));
  }
};

TEST_CLASS(GhostLaneTests)
{
public:
  TEST_METHOD(APendingLaneIsDashedAndAnUnderWayOneIsSolid)
  {
    /*
     * The whole visual difference between a promise and a fact
     * (`puck-and-wheel.png` §4). One segment is a solid line; many is a dashed
     * one, and the count is what separates them.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const OverlayTuning colours;
    const GhostLaneTuning tuning;

    const OrderGhost pending[] = {MakeGhost(GhostState::Pending, XMFLOAT2{-4000.0f, 0.0f}, XMFLOAT2{4000.0f, 0.0f})};
    UiDrawList dashedList;
    BuildGhostLanes(pending, view, colours, tuning, 0.0, dashedList);
    const std::vector<UiQuad> dashes = Segments(dashedList);
    Assert::IsTrue(dashes.size() > 4, L"a pending lane is drawn as many dashes");

    const OrderGhost underWay[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{-4000.0f, 0.0f}, XMFLOAT2{4000.0f, 0.0f})};
    UiDrawList solidList;
    BuildGhostLanes(underWay, view, colours, tuning, 0.0, solidList);
    Assert::AreEqual<std::size_t>(1, Segments(solidList).size(), L"an accepted lane is one unbroken segment");
  }

  TEST_METHOD(EveryDashPointsAlongTheLane)
  {
    /*
     * The property the vertex shader depends on and no test on this machine can
     * see it use: an oriented quad's `rect` is a *centre* and a (length,
     * thickness), swept along a *unit* axis. A dash whose axis was not unit
     * length would draw at the wrong size, and one whose centre was really a
     * top-left would draw half a dash off its own line.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{-3000.0f, -2000.0f}, XMFLOAT2{3000.0f, 2500.0f})};

    UiDrawList list;
    BuildGhostLanes(ghosts, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    const std::vector<UiQuad> dashes = Segments(list);
    Assert::IsTrue(dashes.size() > 2);

    const float axisX = dashes.front().axisX;
    const float axisY = dashes.front().axisY;
    for (const UiQuad& dash : dashes)
    {
      const float length = std::sqrt(dash.axisX * dash.axisX + dash.axisY * dash.axisY);
      Assert::AreEqual(1.0f, length, 0.001f, L"the axis has to be a unit vector");
      Assert::AreEqual(axisX, dash.axisX, 0.001f, L"every dash on one lane runs the same way");
      Assert::AreEqual(axisY, dash.axisY, 0.001f);
      Assert::IsTrue(dash.rect.width > 0.0f && dash.rect.height > 0.0f);
    }

    // The dashes lie *on* the line between the endpoints, which is what puts
    // the centre in `rect.xy`: walking from one dash centre to the next must
    // move along the axis and nowhere else.
    for (std::size_t index = 1; index < dashes.size(); ++index)
    {
      const float stepX = dashes[index].rect.x - dashes[index - 1].rect.x;
      const float stepY = dashes[index].rect.y - dashes[index - 1].rect.y;
      const float across = stepX * -axisY + stepY * axisX;
      Assert::AreEqual(0.0f, across, 0.01f, L"a dash drifted off the lane it belongs to");
    }
  }

  TEST_METHOD(DashesDoNotCrawlWhenTheCameraPans)
  {
    /*
     * Phased off the lane's own origin rather than off the clip point.
     *
     * The clip point moves as the camera pans, so phasing from it would slide
     * every dash along the lane while the player scrolls -- motion that reads
     * as the *order* doing something. The check is that the dash pattern's
     * offset from the origin end is unchanged when the visible window moves.
     */
    const PlaneMapping mapping = MakeMapping();
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{-20000.0f, 0.0f}, XMFLOAT2{20000.0f, 0.0f})};

    GhostLaneView wide = MakeView(mapping);
    UiDrawList wideList;
    BuildGhostLanes(ghosts, wide, OverlayTuning{}, GhostLaneTuning{}, 0.0, wideList);

    // The same lane through a narrower window into the middle of the screen.
    GhostLaneView narrow = MakeView(mapping);
    narrow.worldRect = UiRect{400.0f, 0.0f, 800.0f, static_cast<float>(VIEWPORT_HEIGHT)};
    UiDrawList narrowList;
    BuildGhostLanes(ghosts, narrow, OverlayTuning{}, GhostLaneTuning{}, 0.0, narrowList);

    const std::vector<UiQuad> wideDashes = Segments(wideList);
    const std::vector<UiQuad> narrowDashes = Segments(narrowList);
    Assert::IsTrue(!narrowDashes.empty() && narrowDashes.size() < wideDashes.size());

    /*
     * Every *whole* dash the narrow window shows must be one the wide window
     * drew in the same place. If the phase came from the clip point they would
     * all be offset by a fraction of a pitch and none would match.
     *
     * Whole ones only, and that exception is the behaviour rather than a
     * loophole: the dash straddling the clip edge is trimmed to it, so its
     * centre genuinely does depend on where the edge is. Trimming one dash is
     * what a clip *is*; moving all of them would be the bug.
     */
    const float fullWidth = GhostLaneTuning{}.dashPixels;
    std::size_t compared = 0;
    for (const UiQuad& narrowDash : narrowDashes)
    {
      if (narrowDash.rect.width < fullWidth - 0.01f)
      {
        continue; // Trimmed by the edge it touches.
      }
      bool matched = false;
      for (const UiQuad& wideDash : wideDashes)
      {
        if (std::fabs(wideDash.rect.x - narrowDash.rect.x) < 0.01f && std::fabs(wideDash.rect.y - narrowDash.rect.y) < 0.01f)
        {
          matched = true;
          break;
        }
      }
      Assert::IsTrue(matched, L"a dash moved when only the visible window changed");
      ++compared;
    }
    Assert::IsTrue(compared > 20, L"almost every dash should have been whole, so the check has to have run");
  }

  TEST_METHOD(ALaneOffScreenCostsNoQuads)
  {
    // Cost, not looks: the panels already cover anything outside the world
    // rect. A target 40 km away with the camera zoomed in is a lane megapixels
    // long, and dashing all of it is tens of thousands of quads nobody sees.
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {
        MakeGhost(GhostState::Pending, XMFLOAT2{-19000.0f, 19000.0f}, XMFLOAT2{-19000.0f, 18000.0f})};

    UiDrawList list;
    BuildGhostLanes(ghosts, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    Assert::AreEqual<std::size_t>(0, Segments(list).size());
    Assert::AreEqual<std::size_t>(0, list.Runs().size(), L"and no label either");
  }

  TEST_METHOD(TheLabelIsTwoCentredLinesUnderTheFootprint)
  {
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f})};

    UiDrawList list;
    BuildGhostLanes(ghosts, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);

    const std::vector<std::string> texts = Texts(list);
    Assert::AreEqual<std::size_t>(2, texts.size());
    Assert::AreEqual(std::string{"Move - Claw"}, texts[0], L"the game's own words, not the engine's");
    Assert::AreEqual(std::string{"3.0 km - ETA 1m 32s"}, texts[1]);

    // Centred on the same point, and the detail line below the name.
    const UiTextRun& name = list.Runs()[0];
    const UiTextRun& detail = list.Runs()[1];
    const float nameCentre = name.x + static_cast<float>(texts[0].size()) * view.cellPixels * 0.5f;
    const float detailCentre = detail.x + static_cast<float>(texts[1].size()) * view.cellPixels * 0.5f;
    Assert::AreEqual(nameCentre, detailCentre, 0.01f, L"the two lines share a centre");
    Assert::IsTrue(detail.y > name.y, L"the numbers go under the name");
  }

  TEST_METHOD(TheDistanceIsMeasuredOnThePlaneAndNotOnScreen)
  {
    /*
     * The label says how far the *ships* travel. The pixel length of the lane
     * is a fact about the zoom, and a label that changed when the player
     * scrolled the wheel would be reporting the camera.
     */
    const OrderGhost ghosts[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f})};

    IsoCamera near;
    near.SetViewport(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    near.SetFocus(XMFLOAT2{1500.0f, 0.0f});
    near.SetZoomMetres(2000.0f);

    IsoCamera far;
    far.SetViewport(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    far.SetFocus(XMFLOAT2{1500.0f, 0.0f});
    far.SetZoomMetres(8000.0f);

    GhostLaneView nearView = MakeView(near.PlaneMappingForNdc());
    GhostLaneView farView = MakeView(far.PlaneMappingForNdc());

    UiDrawList nearList;
    UiDrawList farList;
    BuildGhostLanes(ghosts, nearView, OverlayTuning{}, GhostLaneTuning{}, 0.0, nearList);
    BuildGhostLanes(ghosts, farView, OverlayTuning{}, GhostLaneTuning{}, 0.0, farList);

    const std::vector<std::string> nearTexts = Texts(nearList);
    const std::vector<std::string> farTexts = Texts(farList);
    Assert::AreEqual<std::size_t>(2, nearTexts.size());
    Assert::AreEqual<std::size_t>(2, farTexts.size());
    Assert::AreEqual(nearTexts[1], farTexts[1], L"the label must not report the zoom");
  }

  TEST_METHOD(ARefusedLaneRetractsWithItsFootprint)
  {
    /*
     * The bounce, from the same `BounceFraction` the footprint uses and with
     * the same clock. A lane that stayed at full length while the ring it
     * points at travelled home would come adrift from it for 150 ms -- which is
     * the whole duration of the animation.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    OrderGhost ghost = MakeGhost(GhostState::Rejected, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f});
    ghost.stateSinceSeconds = 100.0;
    const OrderGhost ghosts[] = {ghost};

    const auto laneLength = [&](double _now) {
      UiDrawList list;
      BuildGhostLanes(ghosts, view, OverlayTuning{}, GhostLaneTuning{}, _now, list);
      const std::vector<UiQuad> dashes = Segments(list);
      if (dashes.empty())
      {
        return 0.0f;
      }
      const UiQuad& first = dashes.front();
      const UiQuad& last = dashes.back();
      return std::fabs(last.rect.x - first.rect.x);
    };

    const float atRefusal = laneLength(100.0);
    const float halfway = laneLength(100.0 + OrderGhostList::BOUNCE_SECONDS * 0.5);
    Assert::IsTrue(atRefusal > 0.0f);
    Assert::IsTrue(halfway < atRefusal * 0.75f, L"the lane has to come home with the footprint");
  }

  TEST_METHOD(AQueuedChainIsOnePolylineThroughItsWaypoints)
  {
    /*
     * `puck-and-wheel.png` §4: "waypoints render as a polyline with per-leg
     * ETAs". One chain, not one ghost per waypoint -- so the dashes run fleet
     * to first waypoint to second, and every one of them belongs to the same
     * order.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const XMFLOAT2 waypoints[] = {XMFLOAT2{-2000.0f, 0.0f}, XMFLOAT2{2000.0f, 1500.0f}, XMFLOAT2{5000.0f, -1000.0f}};
    const OrderGhost ghosts[] = {MakeChain(GhostState::Pending, XMFLOAT2{-6000.0f, -1000.0f}, waypoints)};

    UiDrawList list;
    BuildGhostLanes(ghosts, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    const std::vector<UiQuad> dashes = Segments(list);
    Assert::IsTrue(dashes.size() > 6, L"three legs of dashes");

    // Three distinct directions, one per leg -- a chain drawn as a single
    // straight lane from the fleet to the last waypoint would have one.
    std::vector<std::pair<float, float>> axes;
    for (const UiQuad& dash : dashes)
    {
      const bool seen = std::any_of(axes.begin(), axes.end(), [&dash](const std::pair<float, float>& _axis) {
        return std::fabs(_axis.first - dash.axisX) < 0.001f && std::fabs(_axis.second - dash.axisY) < 0.001f;
      });
      if (!seen)
      {
        axes.emplace_back(dash.axisX, dash.axisY);
      }
    }
    Assert::AreEqual<std::size_t>(3, axes.size(), L"one heading per leg, and the chain has three");
  }

  TEST_METHOD(EveryWaypointButTheLastCarriesItsOwnEta)
  {
    /*
     * The per-leg labels. The last leg's number is already in the two-line
     * label under the footprint, so repeating it beside the ring would print
     * the same seconds twice -- which is why a single-leg ghost gets no
     * waypoint labels at all and looks exactly as it did in S11c.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const XMFLOAT2 waypoints[] = {XMFLOAT2{-2000.0f, 0.0f}, XMFLOAT2{2000.0f, 500.0f}, XMFLOAT2{4000.0f, -500.0f}};
    const OrderGhost chain[] = {MakeChain(GhostState::UnderWay, XMFLOAT2{-5000.0f, 0.0f}, waypoints)};

    UiDrawList list;
    BuildGhostLanes(chain, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    const std::vector<std::string> texts = Texts(list);

    // Two waypoint labels, then the name, the detail and the leg count.
    Assert::AreEqual(std::string{"30s"}, texts[0], L"the first leg's own prediction");
    Assert::AreEqual(std::string{"40s"}, texts[1], L"and the second's");
    Assert::AreEqual(std::string{"Move - Claw"}, texts[2]);
    Assert::AreEqual(std::string{"3 LEGS"}, texts.back(), L"the print's footer, and only when there is a queue");

    // A single leg keeps S11c's picture: a name and a detail line, nothing else.
    const OrderGhost single[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f})};
    UiDrawList plain;
    BuildGhostLanes(single, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, plain);
    Assert::AreEqual<std::size_t>(2, Texts(plain).size(), L"no waypoint label and no leg count for one leg");
  }

  TEST_METHOD(TheAuthoritysEtaIsUsedForTheLegItIsActuallyFlying)
  {
    /*
     * The leg under way gets the replicated number, which counts down; the legs
     * ahead of it keep the game's prediction, because the authority has not
     * started them and has nothing measured to say about them.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const XMFLOAT2 waypoints[] = {XMFLOAT2{-2000.0f, 0.0f}, XMFLOAT2{2000.0f, 500.0f}, XMFLOAT2{4000.0f, -500.0f}};
    OrderGhost chain = MakeChain(GhostState::UnderWay, XMFLOAT2{-5000.0f, 0.0f}, waypoints);
    chain.legIndex = 1;                    // The authority is flying the second leg.
    chain.authorityEtaSeconds = 17.0f;
    const OrderGhost ghosts[] = {chain};

    UiDrawList list;
    BuildGhostLanes(ghosts, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    const std::vector<std::string> texts = Texts(list);

    Assert::AreEqual(std::string{"30s"}, texts[0], L"leg one keeps its prediction");
    Assert::AreEqual(std::string{"17s"}, texts[1], L"leg two is the one being flown, so it is the authority's");
  }

  TEST_METHOD(TheDistanceIsTheWholePlanAndNotTheLastLeg)
  {
    // A player who queued three waypoints wants to know how far the fleet is
    // going, not how far the final hop is.
    const GhostLaneView view = MakeView(MakeMapping());
    const XMFLOAT2 waypoints[] = {XMFLOAT2{3000.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f}};
    const OrderGhost chain[] = {MakeChain(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, waypoints)};

    UiDrawList list;
    BuildGhostLanes(chain, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    const std::vector<std::string> texts = Texts(list);

    // 3 km then 3 km again: six, not the three the last leg alone would give.
    const auto detail = std::find_if(texts.begin(), texts.end(),
                                     [](const std::string& _text) { return _text.find(" km") != std::string::npos; });
    Assert::IsTrue(detail != texts.end(), L"there has to be a distance to check");
    Assert::IsTrue(detail->rfind("6.0 km", 0) == 0, L"the whole plan, walked leg by leg");
  }

  TEST_METHOD(ARefusedAppendTakesBackOnlyItsOwnLeg)
  {
    /*
     * The reason the merge happens on *acceptance*: a fifth leg is refused with
     * `QueueFull`, and a chain that had already absorbed it would bounce the
     * four legs the player still has. Here the chain is intact and only its
     * last waypoint retracts -- toward the waypoint before it, not the fleet.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const XMFLOAT2 waypoints[] = {XMFLOAT2{-3000.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f}};
    OrderGhost chain = MakeChain(GhostState::Rejected, XMFLOAT2{-9000.0f, 0.0f}, waypoints);
    chain.stateSinceSeconds = 100.0;
    const OrderGhost ghosts[] = {chain};

    const auto firstLegSpan = [&](double _now) {
      UiDrawList list;
      BuildGhostLanes(ghosts, view, OverlayTuning{}, GhostLaneTuning{}, _now, list);
      const std::vector<UiQuad> dashes = Segments(list);
      float minX = 1e9f;
      float maxX = -1e9f;
      for (const UiQuad& dash : dashes)
      {
        minX = std::min(minX, dash.rect.x);
        maxX = std::max(maxX, dash.rect.x);
      }
      return std::pair<float, float>{minX, maxX};
    };

    const auto atRefusal = firstLegSpan(100.0);
    const auto halfway = firstLegSpan(100.0 + OrderGhostList::BOUNCE_SECONDS * 0.5);

    Assert::AreEqual(atRefusal.first, halfway.first, 0.5f, L"the fleet end of the chain must not move");
    Assert::IsTrue(halfway.second < atRefusal.second - 1.0f, L"and the far end has to come home");
  }

  TEST_METHOD(NothingIsDrawnWithoutAViewportOrAGhost)
  {
    // The first frame, and every frame with no order in flight.
    const GhostLaneView view = MakeView(MakeMapping());
    UiDrawList list;

    BuildGhostLanes({}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    Assert::AreEqual<std::size_t>(0, list.Quads().size());

    GhostLaneView unsized = view;
    unsized.viewportWidth = 0;
    unsized.viewportHeight = 0;
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f})};
    BuildGhostLanes(ghosts, unsized, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    Assert::AreEqual<std::size_t>(0, list.Quads().size());
  }
};

} // namespace NeuronClientTests
