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

/*
 * Every dash's position along its own lane, ascending.
 *
 * Projected onto the lane's axis rather than measured in x or y, because the
 * lane runs at whatever angle the camera gives it and a test that assumed
 * horizontal would pass for the wrong reason. The axis is taken from the first
 * dash and is safe to share: `EveryDashRunsTheSameWay` pins that they all carry
 * the same one.
 */
[[nodiscard]] std::vector<float> DashOffsets(const UiDrawList& _list)
{
  const std::vector<UiQuad> dashes = Segments(_list);
  std::vector<float> offsets;
  if (dashes.empty())
  {
    return offsets;
  }

  const float axisX = dashes.front().axisX;
  const float axisY = dashes.front().axisY;
  offsets.reserve(dashes.size());
  for (const UiQuad& dash : dashes)
  {
    offsets.push_back(dash.rect.x * axisX + dash.rect.y * axisY);
  }
  std::sort(offsets.begin(), offsets.end());
  return offsets;
}

/*
 * Distance between two phases *on the circle* they live on.
 *
 * 0.0 and 1.0 are the same point in the cycle, and at exact multiples of the
 * period floating point genuinely lands on either side of the join -- `1.1` is
 * not representable, so `fmod(5.5, 1.1)` comes back as very nearly `1.1` rather
 * than `0.0`. That is not a seam: the dash positions are the set
 * `{(step + phase) * pitch}`, which is unchanged by adding a whole cycle to the
 * phase, so the two answers draw the identical pattern. Comparing them as plain
 * numbers is what would be wrong.
 */
[[nodiscard]] float PhaseGap(float _a, float _b) noexcept
{
  const float raw = std::fabs(_a - _b);
  return std::min(raw, 1.0f - raw);
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
  TEST_METHOD(EveryLaneIsDashedAndColourSeparatesPromiseFromFact)
  {
    /*
     * An under-way lane used to be one solid segment, on the argument that
     * solid-versus-dashed was the difference between a promise and a fact.
     * **Owner's call, 2026-08-19: it is not** -- the crawl is what says the
     * ships are moving, so the state a player watches for most of an order's
     * life is the one that has to show it. Both are dashed now, and promise
     * versus fact is carried where it always mostly was: a half-transparent
     * cyan against an opaque green.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const OverlayTuning colours;
    const GhostLaneTuning tuning;

    const OrderGhost pending[] = {MakeGhost(GhostState::Pending, XMFLOAT2{-4000.0f, 0.0f}, XMFLOAT2{4000.0f, 0.0f})};
    UiDrawList dashedList;
    BuildGhostLanes(pending, {}, view, colours, tuning, 0.0, dashedList);
    const std::vector<UiQuad> dashes = Segments(dashedList);
    Assert::IsTrue(dashes.size() > 4, L"a pending lane is drawn as many dashes");

    const OrderGhost underWay[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{-4000.0f, 0.0f}, XMFLOAT2{4000.0f, 0.0f})};
    UiDrawList solidList;
    BuildGhostLanes(underWay, {}, view, colours, tuning, 0.0, solidList);
    const std::vector<UiQuad> underWayDashes = Segments(solidList);
    Assert::IsTrue(underWayDashes.size() > 4, L"and so is an accepted one, so its dashes can march");

    Assert::AreNotEqual(dashes.front().colourRgba, underWayDashes.front().colourRgba,
                        L"the two states must still be told apart, and colour is what does it");
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
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
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
    BuildGhostLanes(ghosts, {}, wide, OverlayTuning{}, GhostLaneTuning{}, 0.0, wideList);

    // The same lane through a narrower window into the middle of the screen.
    GhostLaneView narrow = MakeView(mapping);
    narrow.worldRect = UiRect{400.0f, 0.0f, 800.0f, static_cast<float>(VIEWPORT_HEIGHT)};
    UiDrawList narrowList;
    BuildGhostLanes(ghosts, {}, narrow, OverlayTuning{}, GhostLaneTuning{}, 0.0, narrowList);

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
    // A whole dash, in this view's pixels: the period is authored in metres, so
    // its screen width depends on what a pixel is worth here.
    const GhostLaneTuning laneTuning;
    const float fullWidth = laneTuning.dashPeriodPixels * narrow.scale * laneTuning.dashDutyCycle;
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
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    Assert::AreEqual<std::size_t>(0, Segments(list).size());
    Assert::AreEqual<std::size_t>(0, list.Runs().size(), L"and no label either");
  }

  TEST_METHOD(TheLabelIsTwoCentredLinesUnderTheFootprint)
  {
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f})};

    UiDrawList list;
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);

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
    BuildGhostLanes(ghosts, {}, nearView, OverlayTuning{}, GhostLaneTuning{}, 0.0, nearList);
    BuildGhostLanes(ghosts, {}, farView, OverlayTuning{}, GhostLaneTuning{}, 0.0, farList);

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
      BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, GhostLaneTuning{}, _now, list);
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
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
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
    BuildGhostLanes(chain, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    const std::vector<std::string> texts = Texts(list);

    // Two waypoint labels, then the name, the detail and the leg count.
    Assert::AreEqual(std::string{"30s"}, texts[0], L"the first leg's own prediction");
    Assert::AreEqual(std::string{"40s"}, texts[1], L"and the second's");
    Assert::AreEqual(std::string{"Move - Claw"}, texts[2]);
    Assert::AreEqual(std::string{"3 LEGS"}, texts.back(), L"the print's footer, and only when there is a queue");

    // A single leg keeps S11c's picture: a name and a detail line, nothing else.
    const OrderGhost single[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f})};
    UiDrawList plain;
    BuildGhostLanes(single, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, plain);
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
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
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
    BuildGhostLanes(chain, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
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

    /*
     * Crawl off for this one. It measures the chain's *geometry* across two
     * instants, and a marching dash pattern moves every dash a little between
     * them — real motion, but noise against the question being asked here.
     * Turning it off in the tuning is exactly what the knob is for.
     */
    GhostLaneTuning still;
    still.crawlSecondsPerCycle = 0.0f;

    const auto firstLegSpan = [&](double _now) {
      UiDrawList list;
      BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, still, _now, list);
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

    BuildGhostLanes({}, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    Assert::AreEqual<std::size_t>(0, list.Quads().size());

    GhostLaneView unsized = view;
    unsized.viewportWidth = 0;
    unsized.viewportHeight = 0;
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{3000.0f, 0.0f})};
    BuildGhostLanes(ghosts, {}, unsized, OverlayTuning{}, GhostLaneTuning{}, 0.0, list);
    Assert::AreEqual<std::size_t>(0, list.Quads().size());
  }
};

/*
 * The crawl: dashes marching toward the destination.
 *
 * Two halves, and the split is the point. `DashCrawlPhase` is a pure function
 * of two doubles and is asserted directly -- wrap continuity is the property
 * that decides whether the animation seams once a cycle, and it is arithmetic.
 * The other half is what the phase *does* to the pattern, which is checked
 * through the built quads because that is the only place direction lives.
 */
TEST_CLASS(DashCrawlTests)
{
public:
  TEST_METHOD(ThePhaseWrapsWithoutASeam)
  {
    constexpr double PERIOD = 1.1;

    // The property that matters: a whole cycle later is the same point in the
    // cycle. If this drifts, the pattern jumps once every 1.1 s for ever.
    //
    // Compared on the circle, not as numbers -- 5.5 is one of the times where
    // `fmod` lands just under the period rather than at zero, and calling that a
    // failure would be the test misunderstanding what a phase is (`PhaseGap`).
    for (const double t : {0.0, 0.37, 1.09, 5.5, 900.0})
    {
      Assert::IsTrue(PhaseGap(DashCrawlPhase(t, PERIOD), DashCrawlPhase(t + PERIOD, PERIOD)) < 1e-5f,
                     L"one period later is the same phase");
      Assert::IsTrue(PhaseGap(DashCrawlPhase(t, PERIOD), DashCrawlPhase(t + 10.0 * PERIOD, PERIOD)) < 1e-5f,
                     L"and ten periods later too");
    }

    // Approaching the join from below reaches 1 and stepping past it reaches 0 --
    // the two ends of the same dash.
    Assert::IsTrue(DashCrawlPhase(PERIOD - 1e-6, PERIOD) > 0.999f, L"just before the wrap the phase is nearly one");
    Assert::IsTrue(DashCrawlPhase(PERIOD + 1e-6, PERIOD) < 0.001f, L"just after it, nearly zero");
  }

  TEST_METHOD(ThePatternDoesNotJumpAcrossTheWrap)
  {
    /*
     * The phase test above is arithmetic; this is the claim a person watching a
     * lane for several minutes would be making. Straddle the join and follow one
     * dash: if the wrap were a seam it would snap back by a whole pitch here,
     * once every 1.1 s, for ever.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f})};
    const GhostLaneTuning tuning;
    const double period = tuning.crawlSecondsPerCycle;

    const auto trackedOffset = [&](double _now) {
      UiDrawList list;
      BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, _now, list);
      const std::vector<float> offsets = DashOffsets(list);
      Assert::IsTrue(offsets.size() > 2, L"a pending lane dashes");
      return offsets[offsets.size() / 2];
    };

    // A thousand cycles in, so this is also the check that a long session does
    // not accumulate drift.
    const double justBefore = period * 1000.0 - 1e-4;
    const float before = trackedOffset(justBefore);
    const float after = trackedOffset(justBefore + 2e-4);

    const float pitch = tuning.dashPeriodPixels * view.scale;
    Assert::IsTrue(std::fabs(after - before) < pitch * 0.05f,
                   L"crossing the wrap must move the pattern by a hair, not by a pitch");
  }

  TEST_METHOD(ThePhaseStaysInRangeAndAdvancesLinearly)
  {
    constexpr double PERIOD = 1.1;
    Assert::AreEqual(0.0f, DashCrawlPhase(0.0, PERIOD), 1e-6f);
    Assert::AreEqual(0.25f, DashCrawlPhase(PERIOD * 0.25, PERIOD), 1e-6f);
    Assert::AreEqual(0.5f, DashCrawlPhase(PERIOD * 0.5, PERIOD), 1e-6f);

    // An hour in, which is where a float elapsed time would already be
    // quantising the phase into visible steps. Wrapping in doubles first is
    // what keeps this exact.
    Assert::AreEqual(0.25f, DashCrawlPhase(3600.0 * PERIOD + PERIOD * 0.25, PERIOD), 1e-5f);

    for (const double t : {0.0, 0.1, 1.0, 12.7, 1e5})
    {
      const float phase = DashCrawlPhase(t, PERIOD);
      Assert::IsTrue(phase >= 0.0f && phase < 1.0f, L"a phase is 0..1");
    }
  }

  TEST_METHOD(ANonPositivePeriodIsNoCrawl)
  {
    // The hook a reduce-motion setting pulls: turning the animation off is a
    // tuning value, not a branch at the call site.
    Assert::AreEqual(0.0f, DashCrawlPhase(7.3, 0.0), 1e-6f);
    Assert::AreEqual(0.0f, DashCrawlPhase(7.3, -1.0), 1e-6f);

    // And a negative time still answers a positive phase -- `fmod` keeps the
    // dividend's sign, and a phase of -0.3 would step the pattern backwards.
    const float phase = DashCrawlPhase(-0.275, 1.1);
    Assert::IsTrue(phase >= 0.0f && phase < 1.0f);
    Assert::AreEqual(0.75f, phase, 1e-6f);
  }

  TEST_METHOD(DashesTravelTowardTheDestination)
  {
    /*
     * The sign, which is the whole feature: get it wrong and a move order reads
     * as a retreat.
     *
     * Measured by projecting dash centres onto the lane's own axis -- the quads
     * are oriented and every dash on one lane shares that axis, which the suite
     * above already pins -- and following one dash a quarter-cycle. A quarter is
     * chosen so the nearest dash at the later time is unambiguously the same
     * dash rather than its neighbour.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f})};
    const GhostLaneTuning tuning;

    UiDrawList early;
    UiDrawList later;
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, 0.0, early);
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, tuning.crawlSecondsPerCycle * 0.25, later);

    const std::vector<float> before = DashOffsets(early);
    const std::vector<float> after = DashOffsets(later);
    Assert::IsTrue(before.size() > 2 && after.size() > 2, L"a pending lane dashes");

    const float tracked = before[before.size() / 2];
    float nearest = after.front();
    for (const float offset : after)
    {
      if (std::fabs(offset - tracked) < std::fabs(nearest - tracked))
      {
        nearest = offset;
      }
    }
    Assert::IsTrue(nearest > tracked, L"a quarter cycle later the dash has moved toward the destination");
  }

  TEST_METHOD(ThePatternIsTheSameSizeAtEveryCameraDistance)
  {
    /*
     * The dash is a fixed size on screen, whatever the camera is doing.
     *
     * This test used to assert the opposite -- a world-space period, four times
     * the pixels at four times the zoom -- and the opposite was wrong on a real
     * frame: the same order came out 90 px a cycle zoomed in and about a pixel
     * zoomed out. The lane is Ui-pass chrome and the eye judges it against the
     * screen, so the period is pixels now (owner, 2026-08-19).
     *
     * Driven through two real cameras a zoom factor of four apart, so this
     * fails if anything reintroduces a camera term.
     */
    const GhostLaneTuning tuning;
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f})};

    const auto pitchAtZoom = [&](float _zoomMetres) {
      IsoCamera camera;
      camera.SetViewport(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
      camera.SetFocus(XMFLOAT2{3000.0f, 0.0f});
      camera.SetZoomMetres(_zoomMetres);

      UiDrawList list;
      BuildGhostLanes(ghosts, {}, MakeView(camera.PlaneMappingForNdc()), OverlayTuning{}, tuning, 0.0, list);
      const std::vector<float> offsets = DashOffsets(list);
      Assert::IsTrue(offsets.size() > 2, L"a pending lane dashes at every zoom");
      return offsets[1] - offsets[0];
    };

    Assert::AreEqual(pitchAtZoom(8000.0f), pitchAtZoom(2000.0f), 0.01f,
                     L"four times the zoom, the same dash on screen");
    Assert::AreEqual(tuning.dashPeriodPixels, pitchAtZoom(8000.0f), 0.01f, L"and it is the authored size");
  }

  TEST_METHOD(TheCrawlHoldsItsAgreedSpeedAcrossRetunings)
  {
    /*
     * The speed is `dashPeriodPixels / crawlSecondsPerCycle`, and this pins the
     * result rather than the inputs -- deliberately, because the pattern's size
     * has been retuned three times and every one of those changes the speed
     * unless the cycle time moves with it. Tripling the cycle without tripling
     * the time would have tripled the march as a side effect of a change that
     * was only ever about how big the dashes look.
     *
     * **If you meant to change the speed, change the number here too.** This
     * failing is the question "did you mean to?", not an accusation.
     */
    constexpr float AGREED_PIXELS_PER_SECOND = 5.5f;

    const GhostLaneTuning tuning;
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f})};

    const double elapsed = tuning.crawlSecondsPerCycle * 0.25;
    UiDrawList early;
    UiDrawList later;
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, 0.0, early);
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, elapsed, later);

    const std::vector<float> before = DashOffsets(early);
    const std::vector<float> after = DashOffsets(later);
    Assert::IsTrue(before.size() > 2 && after.size() > 2, L"the lane dashes");

    // Follow one dash a quarter-cycle: near enough that the closest dash later
    // is unambiguously the same one.
    const float tracked = before[before.size() / 2];
    float nearest = after.front();
    for (const float offset : after)
    {
      if (std::fabs(offset - tracked) < std::fabs(nearest - tracked))
      {
        nearest = offset;
      }
    }

    const float pixelsPerSecond = (nearest - tracked) / static_cast<float>(elapsed);
    Assert::AreEqual(AGREED_PIXELS_PER_SECOND, pixelsPerSecond, 0.2f,
                     L"the dashes must still travel at the agreed speed");
  }

  TEST_METHOD(TheHudScaleStillSizesTheDash)
  {
    // Fixed against the *camera*, not against the HUD: a player who scales the
    // interface up scales its chrome with it, and the lane is chrome.
    const GhostLaneTuning tuning;
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f})};

    const auto pitchAtScale = [&](float _scale) {
      GhostLaneView view = MakeView(MakeMapping());
      view.scale = _scale;
      UiDrawList list;
      BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, 0.0, list);
      const std::vector<float> offsets = DashOffsets(list);
      Assert::IsTrue(offsets.size() > 2, L"a pending lane dashes");
      return offsets[1] - offsets[0];
    };

    Assert::AreEqual(2.0f, pitchAtScale(2.0f) / pitchAtScale(1.0f), 0.02f, L"twice the HUD is twice the dash");
  }

  TEST_METHOD(ACommittedLaneCrawlsBecauseTheShipsAreMoving)
  {
    /*
     * The state the crawl exists for. An order under way is the one a player
     * watches for most of its life, and the marching dashes are what say the
     * ships are actually going somewhere -- so this is the case that must
     * animate, not the one that must hold still.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::UnderWay, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f})};
    const GhostLaneTuning tuning;

    UiDrawList early;
    UiDrawList later;
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, 0.0, early);
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, tuning, tuning.crawlSecondsPerCycle * 0.25, later);

    const std::vector<float> before = DashOffsets(early);
    const std::vector<float> after = DashOffsets(later);
    Assert::IsTrue(before.size() > 2 && after.size() > 2, L"an under-way lane dashes");

    const float tracked = before[before.size() / 2];
    float nearest = after.front();
    for (const float offset : after)
    {
      if (std::fabs(offset - tracked) < std::fabs(nearest - tracked))
      {
        nearest = offset;
      }
    }
    Assert::IsTrue(nearest > tracked, L"and its dashes march toward the destination");
  }

  TEST_METHOD(TheLaneStartsAtTheShipRatherThanWhereItWasOrdered)
  {
    /*
     * The tail follows the fleet. Left anchored to `originMetres` the line pins
     * itself to a patch of empty space the ships left seconds ago and grows a
     * tail as they close on the target, instead of shortening.
     */
    const GhostLaneView view = MakeView(MakeMapping());
    OrderGhost ghost = MakeGhost(GhostState::UnderWay, XMFLOAT2{-6000.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f});
    ghost.firstEntityId = 7;
    const OrderGhost ghosts[] = {ghost};

    // The ship has flown most of the way. The lane must have shortened with it.
    SceneEntity moved;
    moved.id = 7;
    moved.planeMetres = XMFLOAT2{4000.0f, 0.0f};
    const SceneEntity entities[] = {moved};

    const auto laneSpan = [&](std::span<const SceneEntity> _entities) {
      UiDrawList list;
      GhostLaneTuning still;
      still.crawlSecondsPerCycle = 0.0f; // Geometry, not animation.
      BuildGhostLanes(ghosts, _entities, view, OverlayTuning{}, still, 0.0, list);
      const std::vector<float> offsets = DashOffsets(list);
      Assert::IsTrue(offsets.size() > 1, L"the lane draws");
      return offsets.back() - offsets.front();
    };

    Assert::IsTrue(laneSpan(entities) < laneSpan({}) * 0.6f,
                   L"a ship two thirds of the way there leaves a much shorter lane than its origin does");
  }

  TEST_METHOD(ADespawnedShipFallsBackToTheOrigin)
  {
    // A lane starting nowhere is worse than one starting stale, so a ghost whose
    // ship has died keeps drawing from where the order was given.
    const GhostLaneView view = MakeView(MakeMapping());
    OrderGhost ghost = MakeGhost(GhostState::UnderWay, XMFLOAT2{-6000.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f});
    ghost.firstEntityId = 7;
    const OrderGhost ghosts[] = {ghost};

    SceneEntity other;
    other.id = 99; // Not the ordering ship.
    other.planeMetres = XMFLOAT2{4000.0f, 0.0f};
    const SceneEntity entities[] = {other};

    UiDrawList withGhostShip;
    UiDrawList withoutAnyShip;
    BuildGhostLanes(ghosts, entities, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, withGhostShip);
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, GhostLaneTuning{}, 0.0, withoutAnyShip);

    const std::vector<float> present = DashOffsets(withGhostShip);
    const std::vector<float> absent = DashOffsets(withoutAnyShip);
    Assert::AreEqual(absent.size(), present.size(), L"an unrelated ship changes nothing");
    Assert::AreEqual(absent.front(), present.front(), 0.001f);
  }

  TEST_METHOD(CrawlOffLeavesTheLaneStatic)
  {
    GhostLaneTuning still;
    still.crawlSecondsPerCycle = 0.0f;

    const GhostLaneView view = MakeView(MakeMapping());
    const OrderGhost ghosts[] = {MakeGhost(GhostState::Pending, XMFLOAT2{0.0f, 0.0f}, XMFLOAT2{6000.0f, 0.0f})};

    UiDrawList early;
    UiDrawList later;
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, still, 0.0, early);
    BuildGhostLanes(ghosts, {}, view, OverlayTuning{}, still, 4.7, later);

    const std::vector<float> before = DashOffsets(early);
    const std::vector<float> after = DashOffsets(later);
    Assert::AreEqual(before.size(), after.size(), L"the same dashes");
    for (std::size_t index = 0; index < before.size(); ++index)
    {
      Assert::AreEqual(before[index], after[index], 0.001f, L"and in the same places");
    }
  }
};

} // namespace NeuronClientTests
