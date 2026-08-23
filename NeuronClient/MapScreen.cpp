#include "pch.h"

#include "MapScreen.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

[[nodiscard]] float SafeScale(float _scale) noexcept
{
  return _scale > 0.0f ? _scale : 1.0f;
}

/// The words the *screen* owns, in checkbox order. Everything else on this
/// surface that is a word arrives through `MapView.h`; these three describe
/// what this renderer draws, which is what keeps them legal here.
constexpr const char* SHOW_TOGGLE_NAMES[MAP_SHOW_TOGGLE_COUNT] = {"Gate links", "System names",
                                                                  "Constellation hulls"};

/// The history rail's spans, verbatim from the print. Drawn and never pressed.
constexpr const char* HISTORY_SPAN_NAMES[MAP_HISTORY_SPAN_COUNT] = {"24H", "7D", "30D", "90D", "ALL"};

constexpr const char* PINCH_LEVEL_NAMES[3] = {"REGION", "CONSTELLATION", "SYSTEM"};

/// A rect that never inverts, whatever it is handed. `ResolveUiLayout`'s
/// posture, spelled once for a file with twenty-six zones in it.
[[nodiscard]] UiRect Clamped(float _x, float _y, float _width, float _height) noexcept
{
  return UiRect{_x, _y, std::max(0.0f, _width), std::max(0.0f, _height)};
}

} // namespace

const char* MapShowToggleName(MapShowToggle _toggle) noexcept
{
  const auto index = static_cast<std::uint32_t>(_toggle);
  return index < MAP_SHOW_TOGGLE_COUNT ? SHOW_TOGGLE_NAMES[index] : "";
}

bool MapShowFlags::Get(MapShowToggle _toggle) const noexcept
{
  switch (_toggle)
  {
  case MapShowToggle::GateLinks:
    return gateLinks;
  case MapShowToggle::SystemNames:
    return systemNames;
  case MapShowToggle::ConstellationHulls:
    return constellationHulls;
  }
  return false;
}

void MapShowFlags::Set(MapShowToggle _toggle, bool _on) noexcept
{
  switch (_toggle)
  {
  case MapShowToggle::GateLinks:
    gateLinks = _on;
    return;
  case MapShowToggle::SystemNames:
    systemNames = _on;
    return;
  case MapShowToggle::ConstellationHulls:
    constellationHulls = _on;
    return;
  }
}

const char* MapHistorySpanName(std::uint32_t _span) noexcept
{
  return _span < MAP_HISTORY_SPAN_COUNT ? HISTORY_SPAN_NAMES[_span] : "";
}

const char* MapPinchLevelName(MapPinchLevel _level) noexcept
{
  const auto index = static_cast<std::uint32_t>(_level);
  return index < 3u ? PINCH_LEVEL_NAMES[index] : "";
}

MapExtent MapExtentOf(const MapTopology& _topology) noexcept
{
  MapExtent extent;
  if (_topology.nodes.empty())
  {
    return extent;
  }

  extent.minX = _topology.nodes[0].x;
  extent.maxX = _topology.nodes[0].x;
  extent.minY = _topology.nodes[0].y;
  extent.maxY = _topology.nodes[0].y;
  for (const MapNode& node : _topology.nodes)
  {
    extent.minX = std::min(extent.minX, node.x);
    extent.maxX = std::max(extent.maxX, node.x);
    extent.minY = std::min(extent.minY, node.y);
    extent.maxY = std::max(extent.maxY, node.y);
  }
  return extent;
}

MapPoint MapProject(const MapCamera& _camera, const UiRect& _graph, float _x, float _y) noexcept
{
  const float originX = _graph.x + _graph.width * 0.5f;
  const float originY = _graph.y + _graph.height * 0.5f;
  return MapPoint{originX + (_x - _camera.centreX) * _camera.pixelsPerUnit,
                  originY + (_y - _camera.centreY) * _camera.pixelsPerUnit};
}

MapPoint MapUnproject(const MapCamera& _camera, const UiRect& _graph, float _screenX, float _screenY) noexcept
{
  // A camera with no zoom cannot be inverted, and a division here would put a
  // NaN into the centre that every later frame would inherit.
  const float perUnit = _camera.pixelsPerUnit > 0.0f ? _camera.pixelsPerUnit : 1.0f;
  const float originX = _graph.x + _graph.width * 0.5f;
  const float originY = _graph.y + _graph.height * 0.5f;
  return MapPoint{_camera.centreX + (_screenX - originX) / perUnit, _camera.centreY + (_screenY - originY) / perUnit};
}

MapCamera FitMapCamera(const MapExtent& _extent, const UiRect& _graph, const MapScreenTuning& _tuning) noexcept
{
  MapCamera camera;
  camera.centreX = _extent.CentreX();
  camera.centreY = _extent.CentreY();

  /*
   * An empty extent -- no nodes, or every node at one point -- is drawable and
   * not fittable, so it gets a legible default rather than a division by zero.
   * `MapTopology::Empty` promises the surface opens on a game with no universe,
   * and a NaN camera would be the one way that promise fails after the check.
   */
  if (_extent.Empty() || _graph.width <= 0.0f || _graph.height <= 0.0f)
  {
    camera.pixelsPerUnit = 1.0f;
    return camera;
  }

  const float pad = std::max(0.0f, _tuning.fitPadding);
  const float usableWidth = std::max(1.0f, _graph.width - pad * 2.0f);
  const float usableHeight = std::max(1.0f, _graph.height - pad * 2.0f);

  /*
   * The tighter axis wins, so the whole graph fits rather than the wider half of
   * it filling the zone -- and an axis with *no* extent does not enter the
   * comparison at all. A universe laid out on a line has a real width and no
   * height, and dividing by that height would either fault or produce an
   * infinity that fits nothing.
   */
  float perUnit = 0.0f;
  if (_extent.Width() > 0.0f)
  {
    perUnit = usableWidth / _extent.Width();
  }
  if (_extent.Height() > 0.0f)
  {
    const float byHeight = usableHeight / _extent.Height();
    perUnit = perUnit > 0.0f ? std::min(perUnit, byHeight) : byHeight;
  }

  camera.pixelsPerUnit = perUnit > 0.0f ? perUnit : 1.0f;
  return camera;
}

MapPinchLevel MapLevelOf(const MapCamera& _camera, const MapExtent& _extent, const UiRect& _graph,
                         const MapScreenTuning& _tuning) noexcept
{
  /*
   * Stated against the fit rather than against pixels per unit, so the three
   * thresholds and `maxZoomFactor` are in one currency: 1 is "the whole graph",
   * 0.5 is "half of it". A shard whose universe is a thousand times wider sits
   * at exactly the same numbers, which is the point -- the bake's scale is the
   * bake's business.
   */
  const MapCamera fit = FitMapCamera(_extent, _graph, _tuning);
  if (_camera.pixelsPerUnit <= 0.0f || fit.pixelsPerUnit <= 0.0f)
  {
    return MapPinchLevel::Region;
  }

  const float visibleFraction = fit.pixelsPerUnit / _camera.pixelsPerUnit;
  if (visibleFraction <= _tuning.systemLevelFraction)
  {
    return MapPinchLevel::System;
  }
  if (visibleFraction <= _tuning.constellationLevelFraction)
  {
    return MapPinchLevel::Constellation;
  }
  return MapPinchLevel::Region;
}

namespace
{

/// The camera's centre, kept inside the graph's own bounds. A map that can be
/// flung into empty space is a map the player has to find their way back on.
[[nodiscard]] MapCamera ClampCentre(MapCamera _camera, const MapExtent& _extent) noexcept
{
  if (_extent.Width() <= 0.0f && _extent.Height() <= 0.0f)
  {
    return _camera;
  }
  _camera.centreX = std::clamp(_camera.centreX, _extent.minX, _extent.maxX);
  _camera.centreY = std::clamp(_camera.centreY, _extent.minY, _extent.maxY);
  return _camera;
}

} // namespace

MapCamera PanMapCamera(const MapCamera& _camera, float _deltaX, float _deltaY, const MapExtent& _extent,
                       const UiRect& _graph, const MapScreenTuning& _tuning) noexcept
{
  (void)_graph;
  (void)_tuning;

  const float perUnit = _camera.pixelsPerUnit > 0.0f ? _camera.pixelsPerUnit : 1.0f;

  // Minus, because the delta is the finger's and the map follows it: dragging
  // right moves the world right, which moves the camera left.
  MapCamera moved = _camera;
  moved.centreX -= _deltaX / perUnit;
  moved.centreY -= _deltaY / perUnit;
  return ClampCentre(moved, _extent);
}

MapCamera PinchMapCamera(const MapCamera& _camera, float _scaleFactor, float _anchorX, float _anchorY,
                         const MapExtent& _extent, const UiRect& _graph, const MapScreenTuning& _tuning) noexcept
{
  const MapCamera fit = FitMapCamera(_extent, _graph, _tuning);
  const float minPerUnit = fit.pixelsPerUnit;
  const float maxPerUnit = fit.pixelsPerUnit * std::max(1.0f, _tuning.maxZoomFactor);

  // A ratio at or below zero is not a pinch; `GestureState::pinchScale` cannot
  // produce one, and a caller that scaled by a delta rather than a ratio would.
  const float factor = _scaleFactor > 0.0f ? _scaleFactor : 1.0f;

  // Where the fingers are, in map units, before anything moves.
  const MapPoint before = MapUnproject(_camera, _graph, _anchorX, _anchorY);

  MapCamera moved = _camera;
  moved.pixelsPerUnit = std::clamp(_camera.pixelsPerUnit * factor, minPerUnit, maxPerUnit);

  /*
   * And put it back under them. Without this the zoom is about the centre of
   * the viewport, so a player pinching towards a system watches it slide away
   * -- the defect is small at one step and compounds over a gesture.
   */
  const MapPoint after = MapUnproject(moved, _graph, _anchorX, _anchorY);
  moved.centreX += before.x - after.x;
  moved.centreY += before.y - after.y;

  // Where the clamp bites it wins and the anchor slides: a pinch that slides a
  // little is still a pinch, and a camera that has walked off the universe is a
  // screen the player has to find their way back from.
  return ClampCentre(moved, _extent);
}

MapScreenLayout ResolveMapScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight, float _scale,
                                 const MapScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const auto width = static_cast<float>(_viewportWidth);
  const auto height = static_cast<float>(_viewportHeight);

  MapScreenLayout layout;
  layout.scale = scale;
  layout.viewport = UiRect{0.0f, 0.0f, width, height};

  /*
   * The top bar carries three controls, so the *bar* meets the floor rather
   * than the words in it -- N3's caught defect, which is not going to be caught
   * twice: a target that overhangs its own bar overlaps whatever is beneath.
   */
  const float topBarHeight = std::min(TargetHeightPixels(_tuning.topBarHeight, scale), height);
  layout.topBar = UiRect{0.0f, 0.0f, width, topBarHeight};

  const float pad = _tuning.topBarPaddingX * scale;
  const float gap = _tuning.topBarGap * scale;
  const float backWidth = std::min(_tuning.backWidth * scale, width);
  layout.back = Clamped(pad, 0.0f, backWidth, topBarHeight);

  const float menuWidth = std::min(_tuning.menuWidth * scale, std::max(0.0f, width - backWidth));
  const float searchWidth = std::min(_tuning.searchWidth * scale, std::max(0.0f, width - backWidth - menuWidth));
  layout.menu = Clamped(width - pad - menuWidth, 0.0f, menuWidth, topBarHeight);
  layout.search = Clamped(layout.menu.x - gap - searchWidth, 0.0f, searchWidth, topBarHeight);

  const float titleX = layout.back.Right() + gap;
  layout.title = Clamped(titleX, 0.0f, layout.search.x - gap - titleX, topBarHeight);

  const float contentTop = topBarHeight;
  const float historyHeight = std::min(_tuning.historyHeight * scale, std::max(0.0f, height - contentTop));
  const float contentHeight = std::max(0.0f, height - contentTop - historyHeight);

  /*
   * Three columns: rail, graph, panel. The rail and the panel take their widths
   * and the graph takes what is left -- clamped so a narrow window eats into
   * them rather than giving the graph a negative width.
   */
  const float railWidth = std::min(_tuning.railWidth * scale, width);
  const float panelWidth = std::min(_tuning.panelWidth * scale, std::max(0.0f, width - railWidth));
  const float graphWidth = std::max(0.0f, width - railWidth - panelWidth);

  layout.rail = Clamped(0.0f, contentTop, railWidth, contentHeight);
  layout.panel = Clamped(width - panelWidth, contentTop, panelWidth, contentHeight);

  /*
   * The graph, with its count line under it.
   *
   * The footer is *outside* the projected zone rather than drawn over it,
   * because a line of text over a node is a line of text that hides a system --
   * and the one thing the footer says is how many systems there are.
   */
  const float footerHeight = std::min(_tuning.graphFooterHeight * scale, contentHeight);
  layout.graph = Clamped(railWidth, contentTop, graphWidth, contentHeight - footerHeight);
  layout.graphFooter = Clamped(railWidth, contentTop + layout.graph.height, graphWidth, footerHeight);

  // The hint floats in the graph's top-right corner, which is the print's
  // placement and the one corner the route panel never reaches into.
  const float hintInset = _tuning.hintInset * scale;
  const float hintWidth = std::min(_tuning.hintWidth * scale, layout.graph.width);
  const float hintHeight = std::min(_tuning.hintHeight * scale, layout.graph.height);
  layout.hint = Clamped(layout.graph.Right() - hintInset - hintWidth, layout.graph.y + hintInset, hintWidth,
                        hintHeight);

  /*
   * The panel: a head, two bottom-anchored actions, and the body between them.
   *
   * Anchored from both ends rather than flowed from the top, because SET
   * DESTINATION is the reason this panel exists and a route long enough to
   * scroll must not be able to push it off the screen.
   */
  const float panelPadX = _tuning.panelPaddingX * scale;
  const float panelPadY = _tuning.panelPaddingY * scale;
  const float panelInnerX = layout.panel.x + panelPadX;
  const float panelInnerWidth = std::max(0.0f, layout.panel.width - panelPadX * 2.0f);
  const float panelInnerHeight = std::max(0.0f, layout.panel.height - panelPadY * 2.0f);

  const float headHeight = std::min(_tuning.panelHeadHeight * scale, panelInnerHeight);
  layout.panelHead = Clamped(panelInnerX, layout.panel.y + panelPadY, panelInnerWidth, headHeight);

  const float setHeight = std::min(TargetHeightPixels(_tuning.setDestinationHeight, scale), panelInnerHeight);
  const float addHeight =
      std::min(TargetHeightPixels(_tuning.addWaypointHeight, scale), std::max(0.0f, panelInnerHeight - setHeight));
  const float actionGap = _tuning.actionGap * scale;
  const float panelFloor = layout.panel.Bottom() - panelPadY;

  layout.addWaypoint = Clamped(panelInnerX, panelFloor - addHeight, panelInnerWidth, addHeight);
  layout.setDestination = Clamped(panelInnerX, layout.addWaypoint.y - actionGap - setHeight, panelInnerWidth,
                                  setHeight);

  const float bodyTop = layout.panelHead.Bottom() + _tuning.panelGap * scale;
  /*
   * VIEW above the other two, and last in the stack so it is the first to be
   * squeezed out at a small viewport.
   *
   * That order is the ruling `SplitMapRail` already made once on this screen:
   * when there is not room for everything, the thing that gives way is the one
   * whose absence costs the player least. A missing SET DESTINATION is a route
   * they cannot fly; a missing VIEW is a grid they can still reach from the
   * roster's own blocks, which are the primary focus switch (ADR-016 §7).
   */
  const float viewHeight = std::min(TargetHeightPixels(_tuning.viewHeight, scale),
                                    std::max(0.0f, panelInnerHeight - setHeight - addHeight - actionGap * 2.0f));
  layout.view = Clamped(panelInnerX, layout.setDestination.y - actionGap - viewHeight, panelInnerWidth, viewHeight);

  const float actionsTop = viewHeight > 0.0f ? layout.view.y : layout.setDestination.y;
  layout.panelBody = Clamped(panelInnerX, bodyTop, panelInnerWidth, actionsTop - actionGap - bodyTop);

  /*
   * The history rail: a heading with the spans on it, the track under it, and
   * the transport row the track's own height absorbs. Drawn and inert
   * (ADR-016 §9a.2) -- which is why nothing here is floored and nothing below
   * hit-tests it.
   */
  layout.history = Clamped(0.0f, height - historyHeight, width, historyHeight);
  const float historyPadX = _tuning.historyPaddingX * scale;
  const float historyPadY = _tuning.historyPaddingY * scale;
  const float historyInnerX = layout.history.x + historyPadX;
  const float historyInnerWidth = std::max(0.0f, width - historyPadX * 2.0f);
  const float headingHeight = std::min(_tuning.historyHeadingHeight * scale, historyHeight);

  layout.historyHeading = Clamped(historyInnerX, layout.history.y + historyPadY, historyInnerWidth, headingHeight);

  // The spans sit at the right of the heading row, which is where the print
  // puts them: the rail's caption is on the left and its scale on the right.
  const float spansWidth =
      std::min((_tuning.historySpanWidth + _tuning.historySpanGap) * scale * static_cast<float>(MAP_HISTORY_SPAN_COUNT),
               historyInnerWidth);
  layout.historySpans = Clamped(layout.historyHeading.Right() - spansWidth, layout.historyHeading.y, spansWidth,
                                headingHeight);

  const float trackTop = layout.historyHeading.Bottom() + _tuning.historyGap * scale;
  const float trackHeight = std::min(_tuning.historyTrackHeight * scale,
                                     std::max(0.0f, layout.history.Bottom() - historyPadY - trackTop));
  layout.historyTrack = Clamped(historyInnerX, trackTop, historyInnerWidth, trackHeight);

  return layout;
}

MapRailZones SplitMapRail(const UiRect& _rail, std::uint32_t _overlayCount, std::uint32_t _legendCount, float _scale,
                          const MapScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const float padX = _tuning.railPaddingX * scale;
  const float padY = _tuning.railPaddingY * scale;
  const float gap = _tuning.railGap * scale;
  const float innerX = _rail.x + padX;
  const float innerWidth = std::max(0.0f, _rail.width - padX * 2.0f);
  const float heading = _tuning.headingHeight * scale;

  const float showRow = TargetHeightPixels(_tuning.showRowHeight, scale) + _tuning.showRowGap * scale;
  const float legendRow = _tuning.legendRowHeight * scale;
  const float overlayRow = TargetHeightPixels(_tuning.overlayRowHeight, scale) + _tuning.overlayRowGap * scale;

  MapRailZones zones;

  /*
   * SHOW is anchored to the rail's foot: three fixed toggles whose position the
   * player learns, and a rail whose foot moved with the legend's length would be
   * a rail you have to re-read.
   */
  const float railFloor = _rail.Bottom() - padY;
  const float showHeight = std::min(showRow * static_cast<float>(MAP_SHOW_TOGGLE_COUNT),
                                    std::max(0.0f, railFloor - _rail.y - padY));
  zones.showRows = Clamped(innerX, railFloor - showHeight, innerWidth, showHeight);
  zones.showHeading = Clamped(innerX, zones.showRows.y - heading, innerWidth, heading);

  /*
   * The overlay list takes its natural height at the top, and it is the block
   * that keeps it.
   *
   * At 1.6x on a 900-pixel window the print's rail wants more height than there
   * is -- the touch floor has raised every row and the chrome has not shrunk --
   * so something loses. It is not this: `strategic-map.png` §2 makes the overlay
   * switch the rail's reason for existing, and an overlay list missing SECURITY
   * BAND is a control the player cannot reach.
   */
  zones.overlayHeading = Clamped(innerX, _rail.y + padY, innerWidth, heading);
  const float overlayTop = zones.overlayHeading.Bottom();
  const float overlayWanted = overlayRow * static_cast<float>(_overlayCount);
  const float overlayRoom = std::max(0.0f, zones.showHeading.y - gap - overlayTop);
  zones.overlayRows = Clamped(innerX, overlayTop, innerWidth, std::min(overlayWanted, overlayRoom));

  /*
   * The legend fills what is between, bottom-anchored so it sits against SHOW
   * the way the print groups them -- which puts the slack between the overlay
   * list and the legend, exactly where the print's `flex:1` on the overlay block
   * puts it.
   *
   * And it is the block that gives way: a legend showing its heading and no rows
   * is a visible stub, which is what the four overlays without content already
   * are. It scrolls, through `BuildMapRows`' first-row argument.
   */
  const float legendRoom = std::max(0.0f, zones.showHeading.y - gap - zones.overlayRows.Bottom());
  const float legendHeadingHeight = std::min(heading, legendRoom);
  const float legendHeight =
      std::min(legendRow * static_cast<float>(_legendCount), std::max(0.0f, legendRoom - legendHeadingHeight));
  const float legendBottom = zones.showHeading.y - gap;
  zones.legendRows = Clamped(innerX, legendBottom - legendHeight, innerWidth, legendHeight);
  zones.legendHeading = Clamped(innerX, zones.legendRows.y - legendHeadingHeight, innerWidth, legendHeadingHeight);

  return zones;
}

MapPanelZones SplitMapPanel(const UiRect& _body, std::uint32_t _factCount, std::uint32_t _hopCount, float _scale,
                            const MapScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const float gap = _tuning.panelGap * scale;
  const float heading = _tuning.headingHeight * scale;

  MapPanelZones zones;

  const float factsWanted = _tuning.factRowHeight * scale * static_cast<float>(_factCount);
  zones.facts = Clamped(_body.x, _body.y, _body.width, std::min(factsWanted, _body.height));

  const float routeTop = zones.facts.Bottom() + (_factCount > 0u ? gap : 0.0f);
  zones.routeHeading = Clamped(_body.x, routeTop, _body.width, std::min(heading, std::max(0.0f, _body.Bottom() -
                                                                                                   routeTop)));

  /*
   * The summary is anchored to the body's foot and the hop list takes what is
   * between, because `SAFEST · +2 JUMPS` is a statement about the whole route
   * and a route long enough to scroll would otherwise scroll it away.
   */
  const float summaryHeight =
      std::min(_tuning.routeSummaryHeight * scale, std::max(0.0f, _body.Bottom() - zones.routeHeading.Bottom()));
  zones.routeSummary = Clamped(_body.x, _body.Bottom() - summaryHeight, _body.width, summaryHeight);

  const float hopsTop = zones.routeHeading.Bottom();
  const float hopsRoom = std::max(0.0f, zones.routeSummary.y - hopsTop);
  const float hopsWanted = _tuning.routeHopHeight * scale * static_cast<float>(_hopCount);
  zones.routeHops = Clamped(_body.x, hopsTop, _body.width, std::min(hopsWanted, hopsRoom));

  return zones;
}

std::uint32_t BuildMapRows(std::uint32_t _rowCount, const UiRect& _zone, float _rowHeight, std::uint32_t _firstRow,
                           std::span<UiRect> _outRows) noexcept
{
  if (_rowHeight <= 0.0f || _zone.height <= 0.0f)
  {
    return 0;
  }

  std::uint32_t written = 0;
  float pen = _zone.y;
  for (std::uint32_t row = _firstRow; row < _rowCount; ++row)
  {
    if (written >= _outRows.size())
    {
      break;
    }
    // The whole-row cull: a row that would run past the bottom is not emitted,
    // because there is no clip rectangle to hide the half of it hanging out.
    if (pen + _rowHeight > _zone.Bottom() + 0.001f)
    {
      break;
    }
    _outRows[written] = UiRect{_zone.x, pen, _zone.width, _rowHeight};
    pen += _rowHeight;
    ++written;
  }
  return written;
}

std::uint32_t BuildMapOverlayRows(std::span<const MapOverlayOption> _overlays, const UiRect& _zone, float _scale,
                                  const MapScreenTuning& _tuning, std::span<MapOverlayRowRect> _outRows) noexcept
{
  const float scale = SafeScale(_scale);
  const float rowHeight = TargetHeightPixels(_tuning.overlayRowHeight, scale);
  const float rowGap = _tuning.overlayRowGap * scale;

  std::uint32_t written = 0;
  float pen = _zone.y;
  for (std::uint32_t index = 0; index < _overlays.size(); ++index)
  {
    if (written >= _outRows.size() || pen + rowHeight > _zone.Bottom() + 0.001f)
    {
      break;
    }
    MapOverlayRowRect& row = _outRows[written];
    row.rect = UiRect{_zone.x, pen, _zone.width, rowHeight};
    row.overlay = index;
    row.enabled = _overlays[index].enabled;
    pen += rowHeight + rowGap;
    ++written;
  }
  return written;
}

const MapOverlayRowRect* HitMapOverlayRow(std::span<const MapOverlayRowRect> _rows, float _x, float _y) noexcept
{
  for (const MapOverlayRowRect& row : _rows)
  {
    if (row.enabled && row.rect.Contains(_x, _y))
    {
      return &row;
    }
  }
  return nullptr;
}

std::uint32_t BuildMapShowRows(const UiRect& _zone, float _scale, const MapScreenTuning& _tuning,
                               std::span<MapShowRowRect> _outRows) noexcept
{
  const float scale = SafeScale(_scale);
  const float rowHeight = TargetHeightPixels(_tuning.showRowHeight, scale);
  const float rowGap = _tuning.showRowGap * scale;

  std::uint32_t written = 0;
  float pen = _zone.y;
  for (std::uint32_t index = 0; index < MAP_SHOW_TOGGLE_COUNT; ++index)
  {
    if (written >= _outRows.size() || pen + rowHeight > _zone.Bottom() + 0.001f)
    {
      break;
    }
    MapShowRowRect& row = _outRows[written];
    row.rect = UiRect{_zone.x, pen, _zone.width, rowHeight};
    row.toggle = static_cast<MapShowToggle>(index);
    pen += rowHeight + rowGap;
    ++written;
  }
  return written;
}

const MapShowRowRect* HitMapShowRow(std::span<const MapShowRowRect> _rows, float _x, float _y) noexcept
{
  for (const MapShowRowRect& row : _rows)
  {
    if (row.rect.Contains(_x, _y))
    {
      return &row;
    }
  }
  return nullptr;
}

std::uint32_t BuildMapHistorySpans(const UiRect& _zone, float _scale, const MapScreenTuning& _tuning,
                                   std::span<UiRect> _outSpans) noexcept
{
  const float scale = SafeScale(_scale);
  const float spanWidth = _tuning.historySpanWidth * scale;
  const float spanGap = _tuning.historySpanGap * scale;

  std::uint32_t written = 0;
  float pen = _zone.x;
  for (std::uint32_t index = 0; index < MAP_HISTORY_SPAN_COUNT; ++index)
  {
    if (written >= _outSpans.size() || pen + spanWidth > _zone.Right() + 0.001f)
    {
      break;
    }
    _outSpans[written] = UiRect{pen, _zone.y, spanWidth, _zone.height};
    pen += spanWidth + spanGap;
    ++written;
  }
  return written;
}

namespace
{

/// A node's dot, per level. It grows with the pinch because a pip that stayed
/// 6 px would be indistinguishable from a gate link at the level where the
/// player is choosing between two systems.
[[nodiscard]] float NodeDotFor(MapPinchLevel _level, float _scale, const MapScreenTuning& _tuning) noexcept
{
  switch (_level)
  {
  case MapPinchLevel::Region:
    return _tuning.nodeDotRegion * _scale;
  case MapPinchLevel::Constellation:
    return _tuning.nodeDotConstellation * _scale;
  case MapPinchLevel::System:
    return _tuning.nodeDotSystem * _scale;
  }
  return _tuning.nodeDotRegion * _scale;
}

[[nodiscard]] bool InsideMargin(const MapPoint& _point, const UiRect& _zone, float _margin) noexcept
{
  return _point.x >= _zone.x - _margin && _point.x <= _zone.Right() + _margin && _point.y >= _zone.y - _margin &&
         _point.y <= _zone.Bottom() + _margin;
}

} // namespace

MapGraphCounts BuildMapGraph(const MapTopology& _topology, const MapCamera& _camera, const UiRect& _graph,
                             MapPinchLevel _level, const MapShowFlags& _show, float _cellWidth, float _scale,
                             const MapScreenTuning& _tuning, std::span<MapNodeRect> _outNodes,
                             std::span<MapLinkSegment> _outLinks, std::span<MapGroupDisc> _outGroups) noexcept
{
  const float scale = SafeScale(_scale);
  const float margin = _tuning.cullMargin * scale;
  const float dot = NodeDotFor(_level, scale, _tuning);
  const float labelHeight = _tuning.labelHeight * scale;
  const float labelGap = _tuning.labelGap * scale;

  // Labels from CONSTELLATION in, which is the print's "region zoom: count and
  // brightness only" -- and off entirely when the checkbox is.
  const bool wantLabels = _show.systemNames && _level != MapPinchLevel::Region;

  // Hulls at the two levels where a constellation is a thing you are looking
  // *at* rather than a thing you are inside.
  const bool wantHulls = _show.constellationHulls && _level != MapPinchLevel::System;

  MapGraphCounts counts;

  for (std::uint32_t index = 0; index < _topology.nodes.size(); ++index)
  {
    if (counts.nodes >= _outNodes.size())
    {
      break;
    }
    const MapNode& node = _topology.nodes[index];
    const MapPoint point = MapProject(_camera, _graph, node.x, node.y);
    if (!InsideMargin(point, _graph, margin))
    {
      continue;
    }

    MapNodeRect& out = _outNodes[counts.nodes];
    out.point = point;
    out.node = index;
    out.dot = UiRect{point.x - dot * 0.5f, point.y - dot * 0.5f, dot, dot};
    out.labelled = false;
    out.label = UiRect{};

    /*
     * The label budget, spent in visible-node order.
     *
     * Not a de-confliction pass -- the print places each label against four
     * candidate positions and drops the collisions, which needs a glyph metric
     * and a look to tune, and lands with the visual checkpoint (U5b). What this
     * gives instead is *stability*: a static view labels the same nodes every
     * frame, and panning changes the labels because the view changed rather
     * than because a sort did.
     */
    if (wantLabels && counts.labels < _tuning.maxLabels && node.label != nullptr)
    {
      const auto cells = static_cast<float>(TextCellCount(node.label));
      out.label = UiRect{point.x + dot * 0.5f + labelGap, point.y - labelHeight * 0.5f, cells * _cellWidth,
                         labelHeight};
      out.labelled = true;
      ++counts.labels;
    }
    ++counts.nodes;
  }

  if (_show.gateLinks)
  {
    for (std::uint32_t index = 0; index < _topology.links.size(); ++index)
    {
      if (counts.links >= _outLinks.size())
      {
        break;
      }
      const MapLink& link = _topology.links[index];
      if (link.fromNode >= _topology.nodes.size() || link.toNode >= _topology.nodes.size())
      {
        continue; // A link into nothing is content damage, and drawing it would
                  // put a line from a system to the origin.
      }
      const MapNode& from = _topology.nodes[link.fromNode];
      const MapNode& to = _topology.nodes[link.toNode];
      const MapPoint a = MapProject(_camera, _graph, from.x, from.y);
      const MapPoint b = MapProject(_camera, _graph, to.x, to.y);

      // Either end on screen keeps the link: a gate whose far side is off the
      // viewport is exactly the one a player is looking for.
      if (!InsideMargin(a, _graph, margin) && !InsideMargin(b, _graph, margin))
      {
        continue;
      }
      _outLinks[counts.links] = MapLinkSegment{a, b, index};
      ++counts.links;
    }
  }

  if (wantHulls)
  {
    /*
     * The hulls, in **one pass over the nodes** rather than one per group.
     *
     * The obvious shape -- for each constellation, walk the systems looking for
     * its members -- is 250 x 2,500 at the corpus's cap, which is six hundred
     * thousand comparisons in a frame to draw 250 outlines. So the output array
     * doubles as the accumulator: every group is seeded, the node pass adds to
     * whichever group each node names, and the run is compacted in place
     * afterwards. `written <= index` holds throughout, which is what makes
     * compacting into the same array safe.
     */
    const auto groupCount = static_cast<std::uint32_t>(std::min(_topology.groups.size(), _outGroups.size()));
    for (std::uint32_t index = 0; index < groupCount; ++index)
    {
      MapGroupDisc& seed = _outGroups[index];
      seed.centre = MapProject(_camera, _graph, _topology.groups[index].x, _topology.groups[index].y);
      seed.group = index;
      seed.nodeCount = 0;
      seed.label = UiRect{};

      // Carried in *map units* until the compaction below converts it, so the
      // accumulation does not multiply by the zoom once per node.
      seed.radiusPixels = 0.0f;
    }

    /*
     * The radius is the furthest member from the *placed* centre, which is the
     * shape U1's clustering invariant is stated in: every system is nearer its
     * own constellation's placed centre than any other's. So two of these
     * overlapping on screen is the bake having broken its own invariant,
     * visibly -- and it is measured over **every** node rather than the visible
     * ones, because a hull whose radius shrank as its systems panned off screen
     * would be a hull that changed shape for a reason the bake did not.
     */
    for (const MapNode& node : _topology.nodes)
    {
      if (node.group >= groupCount)
      {
        continue;
      }
      const MapGroup& group = _topology.groups[node.group];
      const float dx = node.x - group.x;
      const float dy = node.y - group.y;
      MapGroupDisc& disc = _outGroups[node.group];
      disc.radiusPixels = std::max(disc.radiusPixels, std::sqrt(dx * dx + dy * dy));
      ++disc.nodeCount;
    }

    for (std::uint32_t index = 0; index < groupCount; ++index)
    {
      const MapGroupDisc candidate = _outGroups[index];
      if (candidate.nodeCount == 0)
      {
        continue; // A constellation with no systems is not drawn: an outline
                  // around nothing is a place the player would try to go.
      }

      const float radiusPixels = candidate.radiusPixels * _camera.pixelsPerUnit;
      if (!InsideMargin(candidate.centre, _graph, margin + radiusPixels))
      {
        continue;
      }

      MapGroupDisc& out = _outGroups[counts.groups];
      out = candidate;
      out.radiusPixels = radiusPixels;
      out.label = UiRect{};
      if (_topology.groups[index].label != nullptr)
      {
        const auto cells = static_cast<float>(TextCellCount(_topology.groups[index].label));
        out.label = UiRect{candidate.centre.x - cells * _cellWidth * 0.5f,
                           candidate.centre.y - radiusPixels - labelGap - labelHeight, cells * _cellWidth,
                           labelHeight};
      }
      ++counts.groups;
    }

    // Whatever the compaction left behind is not part of the answer, and the
    // count says so -- but a stale disc in the tail would be a disc a caller
    // that trusted the span rather than the count would draw.
    for (std::uint32_t index = counts.groups; index < groupCount; ++index)
    {
      _outGroups[index] = MapGroupDisc{};
    }
  }

  return counts;
}

const MapNodeRect* HitMapNode(std::span<const MapNodeRect> _nodes, float _x, float _y, float _scale,
                              const MapScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);

  // The finger's radius, floored: a 6 px pip still gets a 48 px target.
  const float target = TargetHeightPixels(_tuning.nodeTargetSize, scale);
  const float radius = target * 0.5f;
  const float radiusSquared = radius * radius;

  const MapNodeRect* best = nullptr;
  float bestDistance = radiusSquared;
  for (const MapNodeRect& node : _nodes)
  {
    const float dx = node.point.x - _x;
    const float dy = node.point.y - _y;
    const float distance = dx * dx + dy * dy;

    /*
     * Strictly nearer, so a tie keeps the first. Two systems at exactly the
     * same distance is a coin toss either way; what matters is that it is the
     * *same* coin toss twice, because a hit test that alternated would make a
     * double tap select two different places.
     */
    if (distance < bestDistance)
    {
      bestDistance = distance;
      best = &node;
    }
  }
  return best;
}

MapTopBarHit HitMapTopBar(const MapScreenLayout& _layout, float _x, float _y) noexcept
{
  if (_layout.back.Contains(_x, _y))
  {
    return MapTopBarHit::Back;
  }
  if (_layout.search.Contains(_x, _y))
  {
    return MapTopBarHit::Search;
  }
  if (_layout.menu.Contains(_x, _y))
  {
    return MapTopBarHit::Menu;
  }
  return MapTopBarHit::None;
}

MapActionHit HitMapAction(const MapScreenLayout& _layout, bool _canRoute, bool _canView, float _x,
                          float _y) noexcept
{
  /*
   * Drawn either way and refused here: `station-screen.png` §2's "disabled with
   * a reason rather than hidden", which is also what keeps the button in the
   * place the player last saw it.
   *
   * **Two gates rather than one**, because the two buttons ask different
   * questions. Routing needs a fleet and a destination; viewing needs ships
   * already standing where the player pressed. Sending a fleet somewhere they
   * have nothing is the ordinary case, so one flag for both would have made
   * VIEW live on every system a route could reach.
   */
  if (_canView && _layout.view.Contains(_x, _y))
  {
    return MapActionHit::View;
  }
  if (!_canRoute)
  {
    return MapActionHit::None;
  }
  if (_layout.setDestination.Contains(_x, _y))
  {
    return MapActionHit::SetDestination;
  }
  if (_layout.addWaypoint.Contains(_x, _y))
  {
    return MapActionHit::AddWaypoint;
  }
  return MapActionHit::None;
}

std::uint32_t BuildMapMarkerRects(std::span<const MapMarker> _markers, std::span<const MapNodeRect> _nodes,
                                  float _scale, const MapScreenTuning& _tuning,
                                  std::span<MapMarkerRect> _outRects) noexcept
{
  const float width = _tuning.markerBadgeWidth * _scale;
  const float height = _tuning.markerBadgeHeight * _scale;
  const float gap = _tuning.markerBadgeGap * _scale;

  std::uint32_t written = 0;
  for (std::uint32_t index = 0; index < _markers.size() && written < _outRects.size(); ++index)
  {
    /*
     * Binary search over the run `BuildMapGraph` left sorted by node index --
     * the same join, and for the same reason, as the route line's endpoint
     * lookup. A scan here would be 256 x 2,500 a frame.
     */
    const auto found = std::lower_bound(_nodes.begin(), _nodes.end(), _markers[index].node,
                                        [](const MapNodeRect& _rect, std::uint32_t _value)
                                        { return _rect.node < _value; });
    if (found == _nodes.end() || found->node != _markers[index].node)
    {
      continue; // Culled. Not drawn, so nothing to place.
    }

    /*
     * Above and right of the dot, which is the corner a label does not use --
     * `MapNodeRect::label` sits below its dot, so a badge here cannot cover the
     * one word that says which system this is.
     */
    MapMarkerRect& out = _outRects[written];
    out.marker = index;
    out.badge = UiRect{found->point.x + gap, found->point.y - height - gap, width, height};
    ++written;
  }
  return written;
}

} // namespace Neuron
