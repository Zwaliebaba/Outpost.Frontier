#include "pch.h"
#include "CppUnitTest.h"

#include "MapScreen.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Neuron;

/*
 * The strategic map, device-free (U5a, `strategic-map.png` §1, ADR-016 §9,
 * ADR-018 D14, ADR-020 §5/§7).
 *
 * The biggest screen in the corpus, and the one with the most that can be
 * asserted without a GPU: a zone table, a camera, a cull and six hit tests. What
 * *cannot* be asserted here is the whole of U5's stated acceptance -- a visual
 * checkpoint against the print and a frame-budget measurement -- so this suite
 * is deliberately about the things a screenshot would not catch anyway: that the
 * pinch holds the point under the fingers, that the touch floor survives the
 * scale range on a print authored before touch was primary, that a node off
 * screen costs nothing, and that two overlapping targets resolve to the nearer
 * one rather than to bake order.
 *
 * The topology in these tests is a fixture rather than the real bake: this is
 * an *engine* suite, and it may not link GameLogic (CI enforces it). That is the
 * seam working -- if these tests needed the universe to run, the graph would not
 * be neutral.
 */

namespace NeuronClientTests
{
namespace
{

/// A three-constellation fixture: nine systems on a grid, wired in a chain.
/// Small enough to reason about by hand and shaped enough to have an extent, a
/// clustering and an off-screen half.
struct Fixture
{
  std::vector<MapNode> nodes;
  std::vector<MapLink> links;
  std::vector<MapGroup> groups;
  std::vector<MapRegion> regions;

  [[nodiscard]] MapTopology View() const noexcept
  {
    MapTopology topology;
    topology.nodes = nodes;
    topology.links = links;
    topology.groups = groups;
    topology.regions = regions;
    return topology;
  }
};

[[nodiscard]] Fixture MakeFixture()
{
  Fixture fixture;
  fixture.regions.push_back(MapRegion{0, 0.0f, 0.0f, "KESTREL REACH", "FRONTIER", StandingColour::Neutral});

  /*
   * Three clusters, 1000 units apart, three systems each, and the middle one
   * offset in y -- so the extent has area in both directions, which is what a
   * bake produces and what the fit has to cope with. The members sit 100 units
   * either side of their constellation's placed centre, which is what makes the
   * disc radius a number this file can assert.
   */
  for (std::uint16_t group = 0; group < 3; ++group)
  {
    const float centreX = static_cast<float>(group) * 1000.0f + 100.0f;
    const float centreY = group == 1 ? 1100.0f : 100.0f;
    fixture.groups.push_back(MapGroup{group, 0, centreX, centreY, "VEIL"});
    for (std::uint16_t member = 0; member < 3; ++member)
    {
      MapNode node;
      node.id = static_cast<std::uint16_t>(group * 3 + member + 1);
      node.group = group;
      node.x = centreX + (static_cast<float>(member) - 1.0f) * 100.0f;
      node.y = centreY;
      node.label = "ROOT-1";
      node.badge = "SEC 0.2";
      node.tint = StandingColour::Hostile;
      fixture.nodes.push_back(node);
    }
  }

  for (std::uint16_t index = 0; index + 1 < fixture.nodes.size(); ++index)
  {
    fixture.links.push_back(MapLink{index, static_cast<std::uint16_t>(index + 1)});
  }
  return fixture;
}

/// The print's own viewport, which ADR-020 §7 makes normative.
constexpr std::uint32_t PRINT_WIDTH = 1440;
constexpr std::uint32_t PRINT_HEIGHT = 900;

} // namespace

TEST_CLASS(MapLayoutTests)
{
public:
  TEST_METHOD(TheZonesAreThePrintsAtThePrintsSize)
  {
    /*
     * `strategic-map.png` §1 is authored at 1440x900 and ADR-020 §7 makes that
     * normative, so these are read off the source rather than chosen: a 46 px
     * top bar, a 240 px rail, a 280 px panel and a 120 px history rail, with the
     * graph taking what is left.
     */
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::AreEqual(48.0f, layout.topBar.height, 0.001f, L"the top bar carries controls, so it meets the floor");
    Assert::AreEqual(240.0f, layout.rail.width, 0.001f, L"the rail is the print's");
    Assert::AreEqual(280.0f, layout.panel.width, 0.001f, L"and so is the panel");
    Assert::AreEqual(120.0f, layout.history.height, 0.001f, L"and the history rail");
    Assert::AreEqual(920.0f, layout.graph.width, 0.001f, L"the graph takes what is left");

    // The three columns tile the width with no gap and no overlap.
    Assert::AreEqual(layout.rail.Right(), layout.graph.x, 0.001f, L"rail meets graph");
    Assert::AreEqual(layout.graph.Right(), layout.panel.x, 0.001f, L"graph meets panel");
    Assert::AreEqual(static_cast<float>(PRINT_WIDTH), layout.panel.Right(), 0.001f, L"and the panel ends the row");
  }

  TEST_METHOD(EveryTargetClearsTheTouchFloorAcrossTheWholeScaleRange)
  {
    /*
     * The finding this screen exists to survive. `strategic-map.png` was
     * authored 2026-08-08 with ~24 px overlay rows and ~20 px checkboxes --
     * mouse sizes -- and ADR-020's 2026-08-22 amendment made touch primary while
     * keeping the 48 px floor as target-size discipline for *every* interactive
     * widget. So the print's numbers are a floor away from what ships, and the
     * sweep is the whole slider range rather than one scale, because 0.8x is
     * where a floor that scaled would fail.
     */
    const MapScreenTuning tuning;
    const std::array<MapOverlayOption, 5> overlays = {MapOverlayOption{"A", nullptr, 1, true},
                                                      MapOverlayOption{"B", "FUTURE", 2, false},
                                                      MapOverlayOption{"C", "FUTURE", 3, false},
                                                      MapOverlayOption{"D", nullptr, 4, true},
                                                      MapOverlayOption{"E", "FUTURE", 5, false}};

    for (float scale = 0.8f; scale <= 1.61f; scale += 0.1f)
    {
      const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, scale, tuning);
      Assert::IsTrue(layout.topBar.height >= TARGET_FLOOR_PIXELS, L"the bar carrying BACK, SEARCH and MENU");
      Assert::IsTrue(layout.back.height >= TARGET_FLOOR_PIXELS, L"BACK itself");
      Assert::IsTrue(layout.setDestination.height >= TARGET_FLOOR_PIXELS, L"SET DESTINATION");
      Assert::IsTrue(layout.addWaypoint.height >= TARGET_FLOOR_PIXELS, L"ADD WAYPOINT");

      const MapRailZones rail = SplitMapRail(layout.rail, 5, 4, scale, tuning);

      std::array<MapOverlayRowRect, 8> overlayRows{};
      const std::uint32_t built = BuildMapOverlayRows(overlays, rail.overlayRows, scale, tuning, overlayRows);
      Assert::AreEqual(5u, built, L"all five overlays fit at every scale the slider offers");
      for (std::uint32_t index = 0; index < built; ++index)
      {
        Assert::IsTrue(overlayRows[index].rect.height >= TARGET_FLOOR_PIXELS, L"an overlay row is a target");
      }

      std::array<MapShowRowRect, 4> showRows{};
      const std::uint32_t shown = BuildMapShowRows(rail.showRows, scale, tuning, showRows);
      Assert::AreEqual(MAP_SHOW_TOGGLE_COUNT, shown, L"all three checkboxes fit");
      for (std::uint32_t index = 0; index < shown; ++index)
      {
        Assert::IsTrue(showRows[index].rect.height >= TARGET_FLOOR_PIXELS, L"a checkbox is a target");
      }
    }
  }

  TEST_METHOD(TheReadoutsAreNotSizedAsTargets)
  {
    /*
     * The other half of the same rule, and the half that is easy to get wrong by
     * being generous: a legend row, a fact row and a route hop are drawn and
     * never pressed. Sizing them as targets would spend a finger's worth of a
     * 280 px panel on one line of text, which is `CountedChip`'s argument for
     * the same distinction on a smaller thing.
     */
    const MapScreenTuning tuning;
    const UiRect zone{0.0f, 0.0f, 240.0f, 600.0f};
    std::array<UiRect, 8> rows{};

    const std::uint32_t built = BuildMapRows(8, zone, tuning.factRowHeight, 0, rows);
    Assert::AreEqual(8u, built);
    Assert::IsTrue(rows[0].height < TARGET_FLOOR_PIXELS, L"a fact row is a readout, not a target");
    Assert::AreEqual(tuning.factRowHeight, rows[0].height, 0.001f, L"and it is exactly the print's height");
  }

  TEST_METHOD(TheRailGivesItsSlackToTheOverlayList)
  {
    /*
     * The print's `flex:1` on the overlay block, and it is the right block to
     * stretch: the legend and the checkboxes are exactly as tall as their
     * contents, and the overlay list is the one a sixth layer joins.
     *
     * The check that matters is that a *longer legend* does not move the
     * checkboxes, because SHOW is anchored to the rail's foot.
     */
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    const MapRailZones few = SplitMapRail(layout.rail, 5, 0, 1.0f, tuning);
    const MapRailZones many = SplitMapRail(layout.rail, 5, 8, 1.0f, tuning);

    Assert::AreEqual(few.showRows.y, many.showRows.y, 0.001f, L"the checkboxes do not move");
    Assert::AreEqual(few.overlayRows.y, many.overlayRows.y, 0.001f, L"nor does the top of the overlay list");
    Assert::IsTrue(many.legendRows.height > few.legendRows.height, L"the legend grows");
    Assert::IsTrue(many.legendRows.y < few.legendRows.y, L"upwards, into the overlay list's slack");
  }

  TEST_METHOD(ALegendWithNoRowsStillDrawsItsHeading)
  {
    /*
     * The visible stub, in the one place it is structural rather than cosmetic.
     * The only overlay with content today is a gradient over a per-system
     * number, which has no categories to list; the four with categories have no
     * content (ADR-016 §9). So the zone draws `SOVEREIGNTY` and nothing under
     * it -- which is what the print anticipates for content that does not exist,
     * and is not the same thing as the rail deciding to hide a block.
     */
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const MapRailZones zones = SplitMapRail(layout.rail, 5, 0, 1.0f, tuning);

    Assert::IsTrue(zones.legendHeading.height > 0.0f, L"the heading is drawn");
    Assert::AreEqual(0.0f, zones.legendRows.height, 0.001f, L"and there is nothing under it");
  }

  TEST_METHOD(ThePanelsActionsStayPutHoweverLongTheRouteIs)
  {
    /*
     * SET DESTINATION is the reason this panel exists, so a fourteen-jump route
     * -- §3's own worked example -- must not be able to push it off the bottom.
     * Anchored from the foot rather than flowed from the head, and the route
     * list scrolls into whatever is between.
     */
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    const MapPanelZones shortRoute = SplitMapPanel(layout.panelBody, 5, 3, 1.0f, tuning);
    const MapPanelZones longRoute = SplitMapPanel(layout.panelBody, 5, 40, 1.0f, tuning);

    Assert::AreEqual(shortRoute.routeSummary.y, longRoute.routeSummary.y, 0.001f, L"the summary is anchored too");
    Assert::IsTrue(longRoute.routeHops.Bottom() <= longRoute.routeSummary.y + 0.001f, L"and the hops stop above it");
    Assert::IsTrue(layout.setDestination.Bottom() < layout.addWaypoint.y + 0.001f, L"primary sits above secondary");
    Assert::IsTrue(layout.panelBody.Bottom() <= layout.setDestination.y + 0.001f, L"and the body above both");
  }

  TEST_METHOD(AViewportTooSmallForTheChromeClampsRatherThanInverting)
  {
    // `ResolveUiLayout`'s posture, and this screen has twenty-six zones to get
    // it wrong in: dragging a window small must not put a quad with a flipped
    // winding on screen.
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(200, 120, 1.0f, tuning);

    const UiRect zones[] = {layout.topBar,   layout.back,           layout.title,     layout.search,
                            layout.menu,     layout.rail,           layout.panel,     layout.panelHead,
                            layout.panelBody, layout.setDestination, layout.addWaypoint, layout.graph,
                            layout.hint,     layout.graphFooter,    layout.history,   layout.historyHeading,
                            layout.historySpans, layout.historyTrack};
    for (const UiRect& zone : zones)
    {
      Assert::IsTrue(zone.width >= 0.0f, L"never a negative width");
      Assert::IsTrue(zone.height >= 0.0f, L"nor a negative height");
    }
  }

  TEST_METHOD(NothingOnTheHistoryRailClaimsAPress)
  {
    /*
     * ADR-016 §9a.2: the scrubber is *drawn, inert and labelled*, because the
     * irreversible thing is the layout rather than the feature. There is no
     * `HitMapHistorySpan` to call, which is the point -- so what is asserted is
     * that the two hit tests this screen does have refuse a press down there,
     * and that the spans are not sized like targets. A target-sized control
     * that does nothing is a promise.
     */
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    std::array<UiRect, 8> spans{};
    const std::uint32_t built = BuildMapHistorySpans(layout.historySpans, 1.0f, tuning, spans);
    Assert::AreEqual(MAP_HISTORY_SPAN_COUNT, built, L"all five spans are drawn");
    Assert::IsTrue(spans[0].height < TARGET_FLOOR_PIXELS, L"and none of them is a target");

    const float x = spans[0].x + spans[0].width * 0.5f;
    const float y = spans[0].y + spans[0].height * 0.5f;
    Assert::IsTrue(HitMapTopBar(layout, x, y) == MapTopBarHit::None, L"the top bar does not reach down here");
    Assert::IsTrue(HitMapAction(layout, true, x, y) == MapActionHit::None, L"nor do the actions");
  }

  TEST_METHOD(TheHintSitsInsideTheGraphsOwnCorner)
  {
    // Top-right of the *graph* rather than of the viewport, which is the print's
    // placement and the one corner the panel never reaches into.
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::IsTrue(layout.hint.Right() <= layout.graph.Right() + 0.001f, L"inside the graph's right edge");
    Assert::IsTrue(layout.hint.x >= layout.graph.x, L"and its left");
    Assert::IsTrue(layout.hint.y >= layout.graph.y, L"and its top");
    Assert::IsTrue(layout.hint.y < layout.graph.y + layout.graph.height * 0.5f, L"in the upper half, as printed");
  }
};

TEST_CLASS(MapCameraTests)
{
public:
  TEST_METHOD(TheFitShowsEverySystemInsideTheViewport)
  {
    /*
     * The entry camera, and the one property that makes it the entry camera: at
     * the fit, the whole graph is on screen. The padding is why the assertion is
     * against the graph rect rather than against the rect grown by a margin --
     * a fit that put the outermost systems' centres exactly on the boundary
     * would put half of every edge dot outside it.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera camera = FitMapCamera(extent, layout.graph, tuning);

    for (const MapNode& node : fixture.nodes)
    {
      const MapPoint point = MapProject(camera, layout.graph, node.x, node.y);
      Assert::IsTrue(point.x >= layout.graph.x, L"inside the left edge");
      Assert::IsTrue(point.x <= layout.graph.Right(), L"and the right");
      Assert::IsTrue(point.y >= layout.graph.y, L"and the top");
      Assert::IsTrue(point.y <= layout.graph.Bottom(), L"and the bottom");
    }
  }

  TEST_METHOD(ProjectingAndUnprojectingAreInverses)
  {
    // Not a tautology: it is the property the pinch is built on, and a fit that
    // guarded a division by zero in only one of the two would break it.
    const MapScreenTuning tuning;
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapCamera camera{500.0f, 250.0f, 0.35f};

    const MapPoint screen = MapProject(camera, graph, 812.0f, -140.0f);
    const MapPoint back = MapUnproject(camera, graph, screen.x, screen.y);
    Assert::AreEqual(812.0f, back.x, 0.01f, L"the same point comes back");
    Assert::AreEqual(-140.0f, back.y, 0.01f);
  }

  TEST_METHOD(APinchHoldsThePointUnderTheFingers)
  {
    /*
     * The one property that makes a pinch feel like a pinch, and the first
     * consumer of `GestureState::pinchScale` anywhere in this client -- I1 built
     * the gesture and nothing had a use for a zoom until now.
     *
     * Without this the zoom is about the centre of the viewport, so a player
     * pinching *towards* a system watches it slide away from them. The defect is
     * small at one step and compounds over a gesture, which is exactly the kind
     * a screenshot does not catch.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    // Off centre, but not so far that the camera's own clamp starts pulling: the
    // clamp wins where it bites, which is the case below rather than this one.
    const float anchorX = graph.x + graph.width * 0.5f + 120.0f;
    const float anchorY = graph.y + graph.height * 0.5f + 90.0f;
    const MapPoint before = MapUnproject(fit, graph, anchorX, anchorY);

    const MapCamera zoomed = PinchMapCamera(fit, 2.5f, anchorX, anchorY, extent, graph, tuning);
    const MapPoint after = MapUnproject(zoomed, graph, anchorX, anchorY);

    Assert::IsTrue(zoomed.pixelsPerUnit > fit.pixelsPerUnit, L"spreading zooms in");
    Assert::AreEqual(before.x, after.x, 0.05f, L"and the system between the fingers stays between them");
    Assert::AreEqual(before.y, after.y, 0.05f);
  }

  TEST_METHOD(TheCentreClampWinsOverTheAnchorAtTheGraphsEdge)
  {
    /*
     * The other half of the pinch's contract, and the one worth pinning because
     * it is a *chosen* failure rather than an accident: pinching about a point
     * near the edge asks for a camera centre outside the graph, and the two
     * promises cannot both be kept. The clamp is the one kept -- a pinch that
     * slides a little is still a pinch, and a camera off the universe is a
     * screen the player has to find their way back from.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    const MapCamera zoomed = PinchMapCamera(fit, 8.0f, graph.x + 2.0f, graph.y + 2.0f, extent, graph, tuning);
    Assert::IsTrue(zoomed.centreX >= extent.minX - 0.001f, L"the centre is still on the graph");
    Assert::IsTrue(zoomed.centreX <= extent.maxX + 0.001f);
    Assert::IsTrue(zoomed.centreY >= extent.minY - 0.001f);
    Assert::IsTrue(zoomed.centreY <= extent.maxY + 0.001f);
    Assert::IsTrue(zoomed.pixelsPerUnit > fit.pixelsPerUnit, L"and the zoom still happened");
  }

  TEST_METHOD(AUniverseOnALineFitsAlongTheAxisItHas)
  {
    /*
     * A caught defect, kept as a test because the failure was silent. The first
     * draft called an extent empty when *either* axis was zero, so a bake laid
     * out on a line fell through to the one-pixel-per-unit fallback and drew
     * itself off the side of the viewport at whatever scale the units happened
     * to be. A graph is unfittable when it has no extent in either direction;
     * with extent in one, that is the axis the fit uses.
     */
    const MapScreenTuning tuning;
    Fixture fixture;
    for (std::uint16_t index = 0; index < 5; ++index)
    {
      fixture.nodes.push_back(
          MapNode{static_cast<std::uint16_t>(index + 1), 0, static_cast<float>(index) * 4000.0f, 250.0f, "ROOT-1",
                  nullptr, StandingColour::Neutral});
    }

    const MapExtent extent = MapExtentOf(fixture.View());
    Assert::IsFalse(extent.Empty(), L"a line has an extent");

    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapCamera camera = FitMapCamera(extent, graph, tuning);
    for (const MapNode& node : fixture.nodes)
    {
      const MapPoint point = MapProject(camera, graph, node.x, node.y);
      Assert::IsTrue(point.x >= graph.x && point.x <= graph.Right(), L"and every system lands inside the viewport");
    }
  }

  TEST_METHOD(TheZoomFloorIsTheFitAndTheCeilingIsTheTuningsMultiple)
  {
    /*
     * There is nothing to see past the whole universe, and letting the player
     * zoom out into empty space is how a map gets lost. The ceiling is where the
     * system view takes over (U6).
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    const MapCamera out = PinchMapCamera(fit, 0.01f, graph.x + 400.0f, graph.y + 300.0f, extent, graph, tuning);
    Assert::AreEqual(fit.pixelsPerUnit, out.pixelsPerUnit, 0.0001f, L"closing at the fit does not zoom out further");

    MapCamera in = fit;
    for (int step = 0; step < 40; ++step)
    {
      in = PinchMapCamera(in, 2.0f, graph.x + 400.0f, graph.y + 300.0f, extent, graph, tuning);
    }
    Assert::AreEqual(fit.pixelsPerUnit * tuning.maxZoomFactor, in.pixelsPerUnit, 0.001f, L"and the ceiling holds");
  }

  TEST_METHOD(APinchThatIsNotAPinchChangesNothing)
  {
    // `pinchScale` is a *ratio*, so zero and negative are not reachable through
    // the gesture -- but a caller that handed over a pixel delta instead would
    // produce both, and a camera with a zero or negative zoom draws a NaN world
    // that every later frame inherits.
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    const MapCamera zero = PinchMapCamera(fit, 0.0f, graph.x, graph.y, extent, graph, tuning);
    Assert::IsTrue(zero.pixelsPerUnit > 0.0f, L"a zero ratio is refused rather than applied");
  }

  TEST_METHOD(PanningFollowsTheFingerAndStopsAtTheGraphsEdge)
  {
    /*
     * Dragging right moves the world right, which moves the camera *left* -- the
     * sign every direct-manipulation surface uses and the one a caller gets
     * backwards. And the clamp: a map that can be flung into empty space is a
     * map the player has to find their way back on.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    const MapCamera dragged = PanMapCamera(fit, 100.0f, 0.0f, extent, graph, tuning);
    Assert::IsTrue(dragged.centreX < fit.centreX, L"dragging right walks the camera left");

    const MapCamera flung = PanMapCamera(fit, -100000.0f, -100000.0f, extent, graph, tuning);
    Assert::IsTrue(flung.centreX <= extent.maxX + 0.001f, L"and the centre stays on the graph");
    Assert::IsTrue(flung.centreY <= extent.maxY + 0.001f);
  }

  TEST_METHOD(PinchingInWalksDownThroughTheThreeLevels)
  {
    /*
     * `strategic-map.png` §1's hint box in one assertion: *"REGION · KESTREL
     * REACH / pinch in ▸ CONSTELLATION ▸ SYSTEM"*. The level is a property of
     * the camera and not a mode the player switches -- there is no level button
     * anywhere on the print, because zooming *is* the navigation.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());

    MapCamera camera = FitMapCamera(extent, graph, tuning);
    Assert::IsTrue(MapLevelOf(camera, extent, graph, tuning) == MapPinchLevel::Region, L"the fit is the region");

    camera.pixelsPerUnit /= tuning.constellationLevelFraction;
    Assert::IsTrue(MapLevelOf(camera, extent, graph, tuning) == MapPinchLevel::Constellation, L"then constellation");

    camera.pixelsPerUnit = FitMapCamera(extent, graph, tuning).pixelsPerUnit / tuning.systemLevelFraction;
    Assert::IsTrue(MapLevelOf(camera, extent, graph, tuning) == MapPinchLevel::System, L"then system");
  }

  TEST_METHOD(AGameWithNoUniverseOpensTheScreenAnyway)
  {
    /*
     * `MapTopology::Empty` promises the surface opens on a client whose game has
     * no universe -- chrome and an empty viewport rather than a refusal. The one
     * way that promise fails *after* the check is a fit that divides by a zero
     * extent and puts a NaN in the camera, which every later frame inherits.
     */
    const MapScreenTuning tuning;
    const MapTopology empty;
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(empty);

    Assert::IsTrue(extent.Empty(), L"no nodes, no extent");
    const MapCamera camera = FitMapCamera(extent, graph, tuning);
    Assert::IsTrue(camera.pixelsPerUnit > 0.0f, L"and a legible camera rather than a division by zero");
    Assert::IsTrue(MapLevelOf(camera, extent, graph, tuning) == MapPinchLevel::Region, L"at the coarsest level");
  }

  TEST_METHOD(OneNodeIsAnExtentWithNoAreaAndStillFits)
  {
    // The other degenerate bake: a universe of one system has a real node and no
    // width, which is drawable and not fittable. It must not take the same path
    // as "no nodes at all" by accident -- the centre is the node, not the origin.
    const MapScreenTuning tuning;
    Fixture fixture;
    fixture.nodes.push_back(MapNode{7, 0, 640.0f, -220.0f, "ROOT-7", nullptr, StandingColour::Neutral});

    const MapExtent extent = MapExtentOf(fixture.View());
    Assert::IsTrue(extent.Empty(), L"a point has no area");
    Assert::AreEqual(640.0f, extent.CentreX(), 0.001f, L"but it does have a centre");

    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapCamera camera = FitMapCamera(extent, graph, tuning);
    const MapPoint point = MapProject(camera, graph, 640.0f, -220.0f);
    Assert::AreEqual(graph.x + graph.width * 0.5f, point.x, 0.001f, L"and it lands in the middle of the viewport");
  }
};

TEST_CLASS(MapGraphTests)
{
public:
  TEST_METHOD(ASystemOutsideTheViewportCostsNothing)
  {
    /*
     * The cull, which is what makes 2,500 nodes (`MAX_MAP_NODES`) a budget
     * rather than a hope. `UiScrollState`'s whole-row rule in two dimensions: a
     * node off screen emits nothing at all, rather than emitting a rect that the
     * draw then has to reject.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 64> nodes{};
    std::array<MapLinkSegment, 64> links{};
    std::array<MapGroupDisc, 16> groups{};
    const MapShowFlags show;

    const MapGraphCounts all =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Region, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(9u, all.nodes, L"the fit shows every system");

    // Zoom hard onto the first cluster; the far two are off screen.
    MapCamera close = fit;
    close.pixelsPerUnit = fit.pixelsPerUnit * 20.0f;
    close.centreX = 100.0f;
    close.centreY = 100.0f;

    const MapGraphCounts some =
        BuildMapGraph(fixture.View(), close, graph, MapPinchLevel::System, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::IsTrue(some.nodes < all.nodes, L"and a zoomed view builds fewer");
    Assert::IsTrue(some.nodes > 0u, L"but not none");
  }

  TEST_METHOD(AGateLinkWithOneEndOffScreenStillDraws)
  {
    /*
     * Deliberate, and the opposite of the node rule. A gate whose far side is
     * off the viewport is exactly the one a player is looking for -- it is the
     * way out of what they can see -- so a link is culled only when *both* ends
     * are gone.
     */
    const MapScreenTuning tuning;
    Fixture fixture;
    fixture.nodes.push_back(MapNode{1, 0, 0.0f, 0.0f, "ROOT-1", nullptr, StandingColour::Neutral});
    fixture.nodes.push_back(MapNode{2, 0, 100000.0f, 0.0f, "ROOT-2", nullptr, StandingColour::Neutral});
    fixture.links.push_back(MapLink{0, 1});
    fixture.groups.push_back(MapGroup{0, 0, 0.0f, 0.0f, "VEIL"});

    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    MapCamera camera;
    camera.centreX = 0.0f;
    camera.centreY = 0.0f;
    camera.pixelsPerUnit = 1.0f;

    std::array<MapNodeRect, 8> nodes{};
    std::array<MapLinkSegment, 8> links{};
    std::array<MapGroupDisc, 4> groups{};
    const MapShowFlags show;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), camera, graph, MapPinchLevel::System, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(1u, counts.nodes, L"only the near system is built");
    Assert::AreEqual(1u, counts.links, L"but the link out of it is");
  }

  TEST_METHOD(ALinkIntoNothingIsDroppedRatherThanDrawnToTheOrigin)
  {
    // Authored content cannot produce one -- the bake places gates in symmetric
    // pairs -- but a hand-edited file could, and a line from a system to the
    // origin is a gate the player would try to fly.
    const MapScreenTuning tuning;
    Fixture fixture;
    fixture.nodes.push_back(MapNode{1, 0, 0.0f, 0.0f, "ROOT-1", nullptr, StandingColour::Neutral});
    fixture.links.push_back(MapLink{0, 99});

    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapCamera camera{0.0f, 0.0f, 1.0f};

    std::array<MapNodeRect, 8> nodes{};
    std::array<MapLinkSegment, 8> links{};
    std::array<MapGroupDisc, 4> groups{};
    const MapShowFlags show;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), camera, graph, MapPinchLevel::System, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(0u, counts.links, L"a link into nothing is not a line");
  }

  TEST_METHOD(TheRegionLevelDrawsHullsAndNoSystemNames)
  {
    /*
     * The print's own rule for the coarsest level -- *"region zoom: count and
     * brightness only"* -- because a label at 6 px per system is a smear rather
     * than a name. The hulls are what a player navigates by up there.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 64> nodes{};
    std::array<MapLinkSegment, 64> links{};
    std::array<MapGroupDisc, 16> groups{};
    const MapShowFlags show;

    const MapGraphCounts region =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Region, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(0u, region.labels, L"no system names at the region level");
    Assert::AreEqual(3u, region.groups, L"and three constellation hulls");

    const MapGraphCounts constellation =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Constellation, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(9u, constellation.labels, L"and names from the constellation level in");

    const MapGraphCounts system =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::System, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(0u, system.groups, L"inside a constellation, its own outline is the whole screen");
  }

  TEST_METHOD(ACheckboxThatIsOffBuildsNothingRatherThanDrawingNothing)
  {
    /*
     * The point of the SHOW controls at 2,500 nodes. A checkbox that only
     * suppressed the *draw* would still pay for the projection, the cull and the
     * label measurement, which is most of the cost -- so an unchecked box is
     * work not done rather than work discarded.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 64> nodes{};
    std::array<MapLinkSegment, 64> links{};
    std::array<MapGroupDisc, 16> groups{};

    MapShowFlags show;
    show.gateLinks = false;
    show.systemNames = false;
    show.constellationHulls = false;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Constellation, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(0u, counts.links, L"no gate links");
    Assert::AreEqual(0u, counts.labels, L"no system names");
    Assert::AreEqual(0u, counts.groups, L"no hulls");
    Assert::AreEqual(9u, counts.nodes, L"and the systems themselves, which have no checkbox");
  }

  TEST_METHOD(TheLabelBudgetIsStableAcrossFramesThatDidNotMove)
  {
    /*
     * What the budget buys instead of de-confliction, which is U5b's and needs a
     * glyph metric and a look: *stability*. A static view labels the same nodes
     * every frame. A budget spent by distance-to-centre would relabel on every
     * sub-pixel camera drift, and a label that flickers is worse than one that
     * is missing.
     */
    MapScreenTuning tuning;
    tuning.maxLabels = 4;

    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 64> first{};
    std::array<MapNodeRect, 64> second{};
    std::array<MapLinkSegment, 64> links{};
    std::array<MapGroupDisc, 16> groups{};
    const MapShowFlags show;

    const MapGraphCounts a =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Constellation, show, 8.0f, 1.0f, tuning, first, links,
                      groups);
    const MapGraphCounts b =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Constellation, show, 8.0f, 1.0f, tuning, second,
                      links, groups);

    Assert::AreEqual(4u, a.labels, L"the budget is spent and not exceeded");
    Assert::AreEqual(a.labels, b.labels, L"and the same frame twice labels the same count");
    for (std::uint32_t index = 0; index < a.nodes; ++index)
    {
      Assert::AreEqual(first[index].labelled, second[index].labelled, L"and the same nodes");
    }
  }

  TEST_METHOD(AnUnlabelledNodeCarriesACollapsedRectRatherThanAStaleOne)
  {
    // So a caller that forgot to check `labelled` draws nothing rather than
    // drawing at the origin, which is the failure mode that looks like a bug in
    // the camera.
    MapScreenTuning tuning;
    tuning.maxLabels = 1;

    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 64> nodes{};
    std::array<MapLinkSegment, 64> links{};
    std::array<MapGroupDisc, 16> groups{};
    const MapShowFlags show;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Constellation, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(1u, counts.labels);
    Assert::IsTrue(nodes[0].labelled, L"the first node inside the budget is labelled");
    Assert::IsFalse(nodes[1].labelled, L"and the next one is not");
    Assert::AreEqual(0.0f, nodes[1].label.width, 0.001f, L"with a collapsed rect rather than a stale one");
  }

  TEST_METHOD(AConstellationsDiscReachesItsOwnFurthestSystem)
  {
    /*
     * The hull is a disc rather than the print's blob outline, and that is the
     * *bake's* shape rather than a simplification of the drawing: U1's
     * clustering invariant is "every system is nearer its own constellation's
     * placed centre than any other's", which is a statement about radii. So two
     * of these overlapping on screen would be the bake having broken its own
     * invariant, visibly.
     */
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 64> nodes{};
    std::array<MapLinkSegment, 64> links{};
    std::array<MapGroupDisc, 16> groups{};
    const MapShowFlags show;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Region, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(3u, counts.groups);

    for (std::uint32_t index = 0; index < counts.groups; ++index)
    {
      Assert::AreEqual(3u, groups[index].nodeCount, L"three systems apiece");
      Assert::IsTrue(groups[index].radiusPixels > 0.0f, L"and a disc that is not a point");
    }

    // The fixture's centres are at x = base + 100 with members at base, +100,
    // +200 -- so the furthest member is 100 units out, in both directions.
    Assert::AreEqual(100.0f * fit.pixelsPerUnit, groups[0].radiusPixels, 0.01f, L"exactly the furthest member");
  }

  TEST_METHOD(AConstellationWithNoSystemsIsNotDrawn)
  {
    // An outline around nothing is a place the player would try to go.
    const MapScreenTuning tuning;
    Fixture fixture = MakeFixture();
    fixture.groups.push_back(MapGroup{3, 0, 400.0f, 100.0f, "EMPTY"});

    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 64> nodes{};
    std::array<MapLinkSegment, 64> links{};
    std::array<MapGroupDisc, 16> groups{};
    const MapShowFlags show;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Region, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(3u, counts.groups, L"the fourth constellation holds nothing and draws nothing");
  }

  TEST_METHOD(AGraphLargerThanItsBuffersTruncatesRatherThanOverruns)
  {
    // The `UiDrawList` contract on a bigger thing: a caller that hands over a
    // short span gets a graph missing its tail rather than a write past the end.
    const MapScreenTuning tuning;
    const Fixture fixture = MakeFixture();
    const UiRect graph{240.0f, 46.0f, 920.0f, 708.0f};
    const MapExtent extent = MapExtentOf(fixture.View());
    const MapCamera fit = FitMapCamera(extent, graph, tuning);

    std::array<MapNodeRect, 2> nodes{};
    std::array<MapLinkSegment, 1> links{};
    std::array<MapGroupDisc, 1> groups{};
    const MapShowFlags show;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), fit, graph, MapPinchLevel::Region, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(2u, counts.nodes, L"as many nodes as the span holds");
    Assert::AreEqual(1u, counts.links, L"and links");
    Assert::AreEqual(1u, counts.groups, L"and hulls");
  }
};

TEST_CLASS(MapHitTests)
{
public:
  TEST_METHOD(ThePipIsSixPixelsAndTheTargetIsAFingers)
  {
    /*
     * The 48 px floor applied to the *graph*, which is the one place on this
     * screen it changes behaviour rather than a number. A system draws as a 6 px
     * pip at the region level and a finger is eight times that, so the target is
     * floored and the dot is not.
     */
    const MapScreenTuning tuning;
    const UiRect graph{0.0f, 0.0f, 900.0f, 700.0f};
    Fixture fixture;
    fixture.nodes.push_back(MapNode{1, 0, 0.0f, 0.0f, "ROOT-1", nullptr, StandingColour::Neutral});

    const MapCamera camera{0.0f, 0.0f, 1.0f};
    std::array<MapNodeRect, 4> nodes{};
    std::array<MapLinkSegment, 4> links{};
    std::array<MapGroupDisc, 4> groups{};
    const MapShowFlags show;

    const MapGraphCounts counts =
        BuildMapGraph(fixture.View(), camera, graph, MapPinchLevel::Region, show, 8.0f, 1.0f, tuning, nodes, links,
                      groups);
    Assert::AreEqual(1u, counts.nodes);
    Assert::AreEqual(tuning.nodeDotRegion, nodes[0].dot.width, 0.001f, L"the pip is the print's size");

    // 20 px off the centre is well outside a 6 px dot and well inside a 48 px
    // target.
    const float x = nodes[0].point.x + 20.0f;
    const float y = nodes[0].point.y;
    Assert::IsFalse(nodes[0].dot.Contains(x, y), L"outside the dot");
    Assert::IsTrue(HitMapNode({nodes.data(), counts.nodes}, x, y, 1.0f, tuning) == &nodes[0], L"and inside the target");
  }

  TEST_METHOD(TheNearestSystemWinsWhenTwoTargetsOverlap)
  {
    /*
     * The consequence of a finger-sized target over a 6 px pip, and the reason
     * `HitMapNode` measures rather than returning the first it finds: at the
     * region level two adjacent systems can both be under one finger, and
     * first-found would let *bake order* decide which one a tap selected. Bake
     * order is not something a player can see.
     */
    const MapScreenTuning tuning;
    std::array<MapNodeRect, 2> nodes{};
    nodes[0].point = MapPoint{100.0f, 100.0f};
    nodes[0].node = 0;
    nodes[1].point = MapPoint{120.0f, 100.0f};
    nodes[1].node = 1;

    // Nearer the second, and inside both targets.
    const MapNodeRect* hit = HitMapNode(nodes, 116.0f, 100.0f, 1.0f, tuning);
    Assert::IsNotNull(hit, L"a press between two systems still selects one");
    Assert::AreEqual(1u, hit->node, L"and it is the nearer one, not the first built");

    const MapNodeRect* other = HitMapNode(nodes, 104.0f, 100.0f, 1.0f, tuning);
    Assert::IsNotNull(other);
    Assert::AreEqual(0u, other->node, L"and it swaps when the finger does");
  }

  TEST_METHOD(APressInEmptySpaceSelectsNothing)
  {
    // Which is what makes tapping the void a deselect rather than a
    // near-miss on whatever happened to be closest on the whole screen.
    const MapScreenTuning tuning;
    std::array<MapNodeRect, 1> nodes{};
    nodes[0].point = MapPoint{100.0f, 100.0f};

    Assert::IsNull(HitMapNode(nodes, 600.0f, 400.0f, 1.0f, tuning), L"far away is nothing");
  }

  TEST_METHOD(ADisabledOverlayRefusesThePressAndStaysOnTheRail)
  {
    /*
     * Four of the five overlays have no content today (ADR-016 §9, §9a.3) and
     * are *visible stubs*: drawn, disabled, and carrying the game's own reason.
     * A rail that hid them would grow entries later for no visible reason --
     * `BuildSettingsNav`'s argument for the blocked ACCOUNT section, and
     * `StationTab`'s for a service that has not shipped.
     */
    const MapScreenTuning tuning;
    const std::array<MapOverlayOption, 5> overlays = {MapOverlayOption{"SOVEREIGNTY", "FUTURE", 1, false},
                                                      MapOverlayOption{"ACTIVITY HEAT", "FUTURE", 2, false},
                                                      MapOverlayOption{"INTEL PINGS", "FUTURE", 3, false},
                                                      MapOverlayOption{"SECURITY BAND", nullptr, 4, true},
                                                      MapOverlayOption{"RESOURCES", "FUTURE", 5, false}};

    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const MapRailZones rail = SplitMapRail(layout.rail, 5, 0, 1.0f, tuning);

    std::array<MapOverlayRowRect, 8> rows{};
    const std::uint32_t built = BuildMapOverlayRows(overlays, rail.overlayRows, 1.0f, tuning, rows);
    Assert::AreEqual(5u, built, L"every overlay gets a row, including the stubs");

    const std::span<const MapOverlayRowRect> span{rows.data(), built};
    const UiRect& stub = rows[0].rect;
    Assert::IsNull(HitMapOverlayRow(span, stub.x + 4.0f, stub.y + 4.0f), L"and a stub refuses the press");

    const UiRect& live = rows[3].rect;
    const MapOverlayRowRect* hit = HitMapOverlayRow(span, live.x + 4.0f, live.y + 4.0f);
    Assert::IsNotNull(hit, L"while the one with content takes it");
    Assert::AreEqual(3u, hit->overlay);
  }

  TEST_METHOD(TheCheckboxesAnswerEachOther)
  {
    // Three flags and a hit test, and the only thing worth asserting is that the
    // row a press lands on is the flag that changes -- which is the whole reason
    // the layout and the hit test are in one file.
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);
    const MapRailZones rail = SplitMapRail(layout.rail, 5, 0, 1.0f, tuning);

    std::array<MapShowRowRect, 4> rows{};
    const std::uint32_t built = BuildMapShowRows(rail.showRows, 1.0f, tuning, rows);
    Assert::AreEqual(MAP_SHOW_TOGGLE_COUNT, built);

    MapShowFlags flags;
    const std::span<const MapShowRowRect> span{rows.data(), built};
    const MapShowRowRect* hit = HitMapShowRow(span, rows[1].rect.x + 4.0f, rows[1].rect.y + 4.0f);
    Assert::IsNotNull(hit);
    Assert::IsTrue(hit->toggle == MapShowToggle::SystemNames, L"the second row is the second toggle");

    flags.Set(hit->toggle, false);
    Assert::IsFalse(flags.Get(MapShowToggle::SystemNames), L"and setting it is what the press means");
    Assert::IsTrue(flags.Get(MapShowToggle::GateLinks), L"without touching its neighbours");
    Assert::IsTrue(flags.Get(MapShowToggle::ConstellationHulls));
  }

  TEST_METHOD(TheTopBarsThreeControlsAreTheOnlyThingsInIt)
  {
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    Assert::IsTrue(HitMapTopBar(layout, layout.back.x + 4.0f, 10.0f) == MapTopBarHit::Back);
    Assert::IsTrue(HitMapTopBar(layout, layout.search.x + 4.0f, 10.0f) == MapTopBarHit::Search);
    Assert::IsTrue(HitMapTopBar(layout, layout.menu.x + 4.0f, 10.0f) == MapTopBarHit::Menu);

    // The title is data and never a control, whatever it says.
    Assert::IsTrue(HitMapTopBar(layout, layout.title.x + 4.0f, 10.0f) == MapTopBarHit::None,
                   L"the region's name is a readout");
  }

  TEST_METHOD(TheActionsAreDrawnAndRefusedRatherThanHidden)
  {
    /*
     * `station-screen.png` §2's *"disabled with a reason rather than hidden"*,
     * applied to the two buttons that are the point of the panel. A player who
     * has selected nothing still sees where SET DESTINATION will be, which is
     * what stops the panel reshuffling under their finger the moment they do.
     */
    const MapScreenTuning tuning;
    const MapScreenLayout layout = ResolveMapScreen(PRINT_WIDTH, PRINT_HEIGHT, 1.0f, tuning);

    const float x = layout.setDestination.x + 4.0f;
    const float y = layout.setDestination.y + 4.0f;
    Assert::IsTrue(HitMapAction(layout, false, x, y) == MapActionHit::None, L"nothing to route to, nothing happens");
    Assert::IsTrue(HitMapAction(layout, true, x, y) == MapActionHit::SetDestination, L"and it fires when there is");

    const float wx = layout.addWaypoint.x + 4.0f;
    const float wy = layout.addWaypoint.y + 4.0f;
    Assert::IsTrue(HitMapAction(layout, true, wx, wy) == MapActionHit::AddWaypoint, L"and the secondary is its own");
    Assert::IsTrue(layout.setDestination.height > layout.addWaypoint.height,
                   L"the primary is taller, which is what makes it primary without a second colour");
  }
};

} // namespace NeuronClientTests
