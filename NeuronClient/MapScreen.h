#pragma once

#include "MapView.h"
#include "UiLayout.h"

#include <cstdint>
#include <span>

/*
 * Where everything on the strategic map goes, and where the map itself is
 * (`strategic-map.png` §1, ADR-020 §5, §7, U5).
 *
 * `SettingsScreen`'s shape applied to the largest screen in the corpus: **laid
 * out and hit-tested in one file**, so the thing you press is the thing you see.
 * The rule earns its keep here more than anywhere -- this screen has a top bar,
 * a rail of three stacked lists, two scrolling panels, a pair of primary
 * actions, an inert history rail, and a *viewport* with up to 2,500 pressable
 * things in it.
 *
 * **Numbers read off the print, authored at 1440×900** (ADR-020 §7). The zones
 * are exact: a 46 px top bar, a 240 px rail, a 280 px panel and a 120 px history
 * rail, with the graph taking what is left. Everything below is those numbers
 * times the UI scale, which is R9's entire permitted flexibility.
 *
 * **Two things on this screen are not a zone table, and they are the reason it
 * is a separate file rather than more rows in `UiLayout`.**
 *
 * The first is the **camera**. ADR-020 §7 declares this surface's overflow rule
 * as *"scroll the panels; the graph is a viewport -- the graph already pans and
 * zooms, and that *is* its overflow answer"*. So the graph is not laid out; it
 * is *projected*, and pan and pinch are arithmetic on three floats. This is the
 * first consumer of `GestureState::pinchScale` anywhere in the client: I1 built
 * the pinch and nothing had a use for a zoom until now.
 *
 * The second is **culling**. Every other screen draws what it has; this one has
 * a corpus cap of 2,500 systems (`MAX_MAP_NODES`) and a viewport that shows a
 * fraction of them. A node off screen emits nothing, which is the same
 * whole-row cull `UiScrollState` does for a list, in two dimensions.
 *
 * **The screen lays out; it does not know what any of it means.** Every word on
 * it that is about the *game* -- an overlay's name, a system's label and badge,
 * a legend row, a fact, a route's note -- arrives through `MapView.h` as data
 * (ADR-018 D14). What this file spells for itself is the three words that name
 * what the screen *draws*: gate links, system names and constellation hulls.
 * That is the line, and it is a clean one: what is on the screen is the
 * screen's; what it means is the game's.
 *
 * **The print predates the input reversal, and this file does not follow it
 * there.** `strategic-map.png` was authored 2026-08-08 with ~24 px rail rows and
 * ~20 px checkboxes -- mouse sizes. ADR-020's 2026-08-22 amendment made touch
 * primary and kept the 48 px floor as *"target-size discipline generalised to
 * every interactive widget"*. So every pressable thing here is sized through
 * `TargetHeightPixels` and the print's own row heights are a floor away from
 * what ships. Readouts are not: a legend row, a fact row and a route hop are
 * drawn and never pressed, and sizing them as targets would spend a finger's
 * worth of a 280 px panel on a line of text.
 */

namespace Neuron
{

/*
 * The three pinch levels the print names (`strategic-map.png` §1's hint box:
 * *"REGION · KESTREL REACH / pinch in ▸ CONSTELLATION ▸ SYSTEM"*).
 *
 * **A property of the camera, not a mode the player switches.** There is no
 * level button anywhere on the print, and that is the design: zooming *is* the
 * navigation, so the level is derived from how much of the graph is visible and
 * changes under the fingers. A mode would be a fourth control answering a
 * question the pinch already answers.
 *
 * What it governs is what is *legible*, not what exists: hulls and labels come
 * and go with it, and no node is ever removed from the graph by a level.
 */
enum class MapPinchLevel : std::uint8_t
{
  Region = 0,
  Constellation = 1,
  System = 2
};

/*
 * The three SHOW checkboxes (`strategic-map.png` §1).
 *
 * The one set of words on this screen the client owns, and it is worth naming
 * why: these describe **what the screen draws**, not what the game means. A gate
 * link is a line, a system name is a label and a constellation hull is an
 * outline -- three statements about this renderer, which is exactly what
 * ADR-014 §2c leaves on the engine's side of the seam. Compare the OVERLAY rail
 * one zone up, whose every word is the game's.
 */
enum class MapShowToggle : std::uint8_t
{
  GateLinks = 0,
  SystemNames = 1,
  ConstellationHulls = 2
};

inline constexpr std::uint32_t MAP_SHOW_TOGGLE_COUNT = 3;

/// The checkbox's word. Never null, for the same reason `OrderReasonText` is not.
[[nodiscard]] const char* MapShowToggleName(MapShowToggle _toggle) noexcept;

/// What the three checkboxes are set to. Retained by the caller across frames,
/// like a scroll offset, because it is the player's preference about a view
/// rather than a fact about the world.
struct MapShowFlags
{
  bool gateLinks = true;
  bool systemNames = true;
  bool constellationHulls = true;

  [[nodiscard]] bool Get(MapShowToggle _toggle) const noexcept;
  void Set(MapShowToggle _toggle, bool _on) noexcept;
};

/*
 * The history rail's spans (`strategic-map.png` §1).
 *
 * **Drawn, inert and labelled**, which is ADR-016 §9a.2's ruling in as many
 * words: the scrubber keeps its rail because *"the irreversible thing here is
 * the layout, not the feature"* -- adding a bottom rail to a finished screen
 * re-lays the screen out, while removing a reserved one reclaims its space
 * cleanly. Build-or-cut is decided when the strategic stream exists, and not by
 * U5.
 *
 * Which is why there is no `HitMapHistorySpan` below. An inert control with a
 * hit test is a control that will acquire a handler by accident; and it is also
 * why these are *not* sized through the 48 px floor, unlike every other row on
 * this screen. A target-sized thing that does nothing is a promise, and this
 * rail is explicitly not making one.
 */
inline constexpr std::uint32_t MAP_HISTORY_SPAN_COUNT = 5;

/// `24H`, `7D`, `30D`, `90D`, `ALL`. Never null.
[[nodiscard]] const char* MapHistorySpanName(std::uint32_t _span) noexcept;

/// Sizes at scale 1.0, off the print.
struct MapScreenTuning
{
  /// The top bar: `◀ TACTICAL | <region> ... [BAND]` left, SEARCH and MENU right.
  float topBarHeight = 46.0f;
  float topBarPaddingX = 18.0f;
  float topBarGap = 18.0f;

  /// How wide `◀ TACTICAL`, `SEARCH ⌕` and `▥ MENU` are. Fixed rather than
  /// measured: they are three known words and a measured bar would move its own
  /// buttons when the region's name changed, which is the one thing in that bar
  /// that is data.
  float backWidth = 132.0f;
  float searchWidth = 120.0f;
  float menuWidth = 104.0f;

  /// The rail down the left, and the air inside it.
  float railWidth = 240.0f;
  float railPaddingX = 12.0f;
  float railPaddingY = 14.0f;
  float railGap = 16.0f;

  /// The panel down the right.
  float panelWidth = 280.0f;
  float panelPaddingX = 14.0f;
  float panelPaddingY = 14.0f;
  float panelGap = 14.0f;

  /// The history rail across the foot, and the scrub track inside it.
  float historyHeight = 120.0f;
  float historyPaddingX = 18.0f;
  float historyPaddingY = 12.0f;
  float historyGap = 10.0f;
  float historyHeadingHeight = 20.0f;
  float historyTrackHeight = 44.0f;
  float historySpanWidth = 54.0f;
  float historySpanGap = 16.0f;

  /*
   * A heading over a list -- `OVERLAY`, `SOVEREIGNTY`, `SHOW`, `ROUTE`.
   *
   * Not a target: a heading is a word, and the one thing this screen must not
   * do is make a caption look pressable on a surface where nearly everything
   * else is.
   */
  float headingHeight = 22.0f;

  /*
   * The pressable rows, at their print heights.
   *
   * The print draws them at 24 and 20; `TargetHeightPixels` raises both to 48
   * at every scale this client offers, which is the touch floor doing its job
   * against a print authored before touch was the primary input. They are
   * carried at the print's numbers rather than at 48 so that the *print* stays
   * the source and the floor stays visible as a correction.
   */
  float overlayRowHeight = 24.0f;
  float overlayRowGap = 4.0f;
  float showRowHeight = 20.0f;
  float showRowGap = 4.0f;

  /// The readouts, which are not targets and scale freely.
  float legendRowHeight = 22.0f;
  float factRowHeight = 26.0f;
  float routeHopHeight = 23.0f;

  /// The selected-system head: `SELECTED SYSTEM`, the big name, the badge line.
  float panelHeadHeight = 92.0f;

  /// The route's own summary line -- `SAFEST · +2 JUMPS` and the ETA.
  float routeSummaryHeight = 34.0f;

  /*
   * The two actions, and they are different heights on the print for a reason
   * worth keeping: 52 against 46 is what makes SET DESTINATION read as the
   * primary without a second colour doing the work.
   */
  float setDestinationHeight = 52.0f;
  float addWaypointHeight = 46.0f;

  /*
   * VIEW, which the print does not draw and ADR-016 §7 requires: *"fleet
   * markers and systems-with-presence carry a VIEW action"*. It takes ADD
   * WAYPOINT's height rather than SET DESTINATION's, because it is a navigation
   * verb rather than the panel's primary -- and it sits *above* both, since
   * the destructive-adjacent pair belongs at the foot where the print put them.
   */
  float viewHeight = 46.0f;
  float actionGap = 8.0f;

  /// The pinch hint, floated inside the graph's top-right corner.
  float hintWidth = 250.0f;
  float hintHeight = 52.0f;
  float hintInset = 14.0f;

  /// The graph's footer line: `48 SYSTEMS · 61 GATE LINKS · COMPLETE DEFINITION`.
  float graphFooterHeight = 26.0f;

  /*
   * A system's dot, per level it *has* one. It grows as you pinch in because a
   * pip that stayed 9 px would be a pip you could not tell from a gate link at
   * the level where you are choosing between two systems.
   *
   * **There is no REGION size, because REGION draws no dots.** It used to, at
   * 6 px -- which over the committed bake is ten of them inside two pixels, 250
   * times over. That rung is the counted chip now (`groupChipDot`), so the
   * smallest a system's own pip gets is the constellation's.
   */
  float nodeDotConstellation = 9.0f;
  float nodeDotSystem = 14.0f;

  /*
   * What a *finger* gets, which is not what the eye gets.
   *
   * The 48 px floor applied to the graph, and it is the one place on this screen
   * where the floor changes behaviour rather than a number: a 6 px pip with a
   * 48 px target means two systems can have overlapping targets, so `HitMapNode`
   * resolves to the **nearest** rather than to the first found. Draw order would
   * otherwise decide which of two adjacent systems a tap selected, and draw
   * order here is bake order.
   */
  float nodeTargetSize = 24.0f;

  /// Air between a node's dot and its label, and how tall a label's row is.
  float labelGap = 7.0f;

  /*
   * The fleet marker's badge, above and right of its node's dot.
   *
   * Not sized through `TargetHeightPixels`: it is a *readout* rather than a
   * target. The thing a finger presses to reach this system is the node
   * underneath it, which the 48 px floor already covers -- and a badge inflated
   * to 48 px would be a label three times the size of the system it labels, on
   * a screen that can hold 2,500 of them.
   */
  float markerBadgeWidth = 34.0f;
  float markerBadgeHeight = 16.0f;
  float markerBadgeGap = 3.0f;
  float labelHeight = 16.0f;

  /*
   * How many labels the graph will place in one frame.
   *
   * A budget rather than a de-confliction pass, and the difference is named
   * rather than hidden: the print places labels against four candidate
   * positions per node and drops the ones that collide, which needs a glyph
   * metric and a look to tune. That is U5b's, with the visual checkpoint. What
   * this does instead is spend the budget **in visible-node order**, so a static
   * view labels the same nodes every frame and panning changes the labels
   * because the view changed rather than because the sort did.
   */
  std::uint32_t maxLabels = 200;

  /*
   * How far outside the viewport a thing is still built.
   *
   * A node exactly on the edge has a label that runs inwards and a dot that is
   * half in; culling on the centre alone would pop both. One label's width is
   * the honest margin.
   */
  float cullMargin = 96.0f;

  /*
   * Where the pinch levels change, stated in **pixels a constellation
   * occupies** rather than in fractions of the whole graph.
   *
   * The fractions this replaced (0.40 and 0.12 of the total extent) were
   * derived from the print's own footer -- `48 SYSTEMS · 61 GATE LINKS`, seven
   * constellations to a region -- which is to say from a graph that *was* one
   * region. Against the committed 2,500-system bake, 40 % of the whole universe
   * is about twenty regions, and the screen reported CONSTELLATION over a view
   * with two hundred constellations in it. A fraction of the extent is a
   * statement about the bake; what a level is actually about is what is
   * **legible**, and that is a number of pixels.
   *
   * So the level is read off one measurement -- the mean constellation's
   * projected diameter, from `MeasureMapGraph` -- and the two numbers below are
   * the two legibility questions:
   *
   * - Under `regionLevelDiameter`, a constellation is a smudge and its name
   *   would not fit inside it: REGION, where the print says *"count and
   *   brightness only"*.
   * - Once one constellation more than fills the viewport, there is no cluster
   *   left to see and the player is choosing between systems: SYSTEM.
   *   `systemLevelViewportSpans` states that against the viewport's shorter
   *   side, so it is one number at any window size.
   *
   * Between them is CONSTELLATION. A graph whose groups have no radius at all
   * -- one system each, or no groups -- has no constellation to measure, and
   * falls back to the fractions below, which is the only thing they are still
   * for.
   */
  float regionLevelDiameter = 90.0f;
  float systemLevelViewportSpans = 1.0f;
  float constellationLevelFraction = 0.40f;
  float systemLevelFraction = 0.12f;

  /*
   * How big a group's disc has to be before it is allowed a name.
   *
   * Not a de-confliction pass -- that is U5b's, with the visual checkpoint --
   * but the half of the problem that is not about glyph metrics: a label on a
   * disc smaller than its own word is a label that cannot be *placed* legibly
   * however cleverly it is nudged. At the fit over the committed bake that
   * suppresses all 250 of them, which is the print's rule and the difference
   * between a map and the stack of overlapping constellation names the screen
   * used to draw.
   */
  float groupLabelMinDiameter = 60.0f;

  /*
   * The counted chip a constellation becomes at REGION level (`CountedChip.h`'s
   * rung, `IconDensity.h`'s merge, `strategic-map.png` §2).
   *
   * `groupChipDot` is the pip at the smallest count and `groupChipDotMax` the
   * largest it grows to, because §2's rule is *count and brightness* -- so the
   * count reaches the player twice, as a size and as a number, and the number
   * is what makes it honest. `groupChipCountAtMax` is the membership that
   * reaches the top of that range; the shipped bake gives a constellation ten
   * systems, so a dozen is where growth stops meaning anything.
   */
  float groupChipDot = 5.0f;
  float groupChipDotMax = 10.0f;
  std::uint32_t groupChipCountAtMax = 12;

  /*
   * The region's own mark: its name over its band badge, at the centre of its
   * constellations. Drawn only at REGION, which is `MapRegion`'s whole role --
   * *"the coarsest pinch level, and the one the print badges"* -- and which the
   * screen did not draw at all until R15 sent somebody looking.
   */
  float regionMarkHeight = 18.0f;
  float regionMarkGap = 4.0f;

  /*
   * How much of the viewport a region's own content has to cover before the top
   * bar names it rather than the universe.
   *
   * The print's bar is `◀ TACTICAL | <region> [BAND]`, and the region on it is
   * the one you are *looking at*. At the fit over fifty regions there is no such
   * region, so the bar reads the universe's name; the moment one fills most of
   * the screen it reads that one. Two thirds rather than a half, because "most
   * of the viewport" should not flicker between two neighbours at the boundary.
   */
  float regionTitleViewportSpans = 0.66f;

  /*
   * Air between the fitted graph and the edge of its viewport.
   *
   * A fit with no padding puts the outermost systems' *centres* on the boundary,
   * so half of every edge dot and all of its label are outside the zone. This is
   * about a label's height either side, which is the smallest number that makes
   * the fit show what it fitted.
   */
  float fitPadding = 36.0f;

  /*
   * How far in a pinch may go, as a multiple of the scale that fits the graph.
   *
   * The floor is the fit itself -- there is nothing to see past the whole
   * universe, and letting the player zoom out into empty space is how a map
   * gets lost.
   *
   * **The ceiling was 24 and had the same defect the level fractions did.** It
   * was read off *"24× the fit of the print's region, where you are looking at
   * two systems"* -- and the print's region is 48 systems, where the committed
   * bake is 2,500. At 24× that bake the viewport still holds a third of one
   * region, so the deepest zoom the player could reach was short of SYSTEM
   * level entirely: the level existed and could not be arrived at.
   *
   * 120 is derived from the same legibility currency as the thresholds above:
   * a constellation fills the 1440×900 viewport at about 66× the fit of the
   * committed bake, and the rest is the room to spread the ten systems inside
   * it far enough apart to read their names. It is still the one number on this
   * screen stated against total extent rather than against a measured
   * constellation, and it is a ceiling rather than a threshold, which is why
   * that is tolerable: overshooting it costs a player nothing but empty space.
   */
  float maxZoomFactor = 120.0f;
};

/*
 * How far one wheel notch zooms.
 *
 * The mouse's pinch, and it goes through the same `PinchMapCamera` so the two
 * inputs cannot end up with different zoom behaviour -- which is what a separate
 * wheel path eventually produces. 1.2 per notch is about four notches to double,
 * which is the pace a desktop map moves at; `WHEEL_ROWS_PER_NOTCH`'s argument
 * applied to a viewport instead of a list.
 *
 * It is a development convenience rather than a second design: ADR-020's
 * 2026-08-22 amendment made touch primary and left the mouse a way to work
 * without a display in the room.
 */
/*
 * How often the screen re-asks for the fleet markers.
 *
 * A little under the summary family's ~1 Hz cadence, so a fresh summary is
 * never more than a frame or two from the screen while the seam call -- which
 * walks every place the commander has ships -- still runs about once a second
 * instead of sixty times.
 *
 * A period rather than a change notification because the client has no signal
 * for "the summaries moved": `ApplySummary` is the game's own business and a
 * dirty flag crossing the seam for this would be a second way of saying
 * something the rate already says.
 */
inline constexpr double MAP_MARKER_REFRESH_SECONDS = 0.5;

inline constexpr float MAP_WHEEL_ZOOM_PER_NOTCH = 1.2f;

/*
 * The graph's own bounds, in map units.
 *
 * Computed from the topology and held by the caller rather than recomputed per
 * frame, because the topology is asked once at boot and so is this. It is what
 * the camera is fitted and clamped against, and what the pinch levels are
 * fractions of.
 */
struct MapExtent
{
  float minX = 0.0f;
  float minY = 0.0f;
  float maxX = 0.0f;
  float maxY = 0.0f;

  [[nodiscard]] float Width() const noexcept { return maxX - minX; }
  [[nodiscard]] float Height() const noexcept { return maxY - minY; }
  [[nodiscard]] float CentreX() const noexcept { return (minX + maxX) * 0.5f; }
  [[nodiscard]] float CentreY() const noexcept { return (minY + maxY) * 0.5f; }

  /*
   * True for a graph with no nodes, and for one whose nodes are all at a point.
   * Both are drawable and neither is fittable, which is why the distinction is
   * asked here rather than assumed away in `FitMapCamera`.
   *
   * **`&&` rather than `||`, which is a caught defect.** A universe laid out on
   * a line has a real extent along one axis and none along the other, and the
   * first draft called that empty -- so a one-row bake fell through to the
   * 1-pixel-per-unit fallback and drew itself off the side of the viewport.
   * A graph is unfittable when it has no extent in *either* direction; with
   * extent in one, that is the axis the fit uses.
   */
  [[nodiscard]] bool Empty() const noexcept { return Width() <= 0.0f && Height() <= 0.0f; }
};

/// The bounds of every node in the topology. An empty topology gives an empty
/// extent rather than an inverted one.
[[nodiscard]] MapExtent MapExtentOf(const MapTopology& _topology) noexcept;

/*
 * Where the player is looking, in three floats.
 *
 * A centre in map units and a zoom in pixels per map unit -- not a rect,
 * because a rect would have to be kept consistent with the viewport's aspect on
 * every resize, and this way the aspect is simply never stored.
 */
struct MapCamera
{
  float centreX = 0.0f;
  float centreY = 0.0f;
  float pixelsPerUnit = 1.0f;
};

/// A point on the screen, in pixels.
struct MapPoint
{
  float x = 0.0f;
  float y = 0.0f;
};

/// Map units to screen pixels, and back. Exact inverses of each other, which is
/// what makes "pinch about the point under the fingers" expressible at all.
[[nodiscard]] MapPoint MapProject(const MapCamera& _camera, const UiRect& _graph, float _x, float _y) noexcept;
[[nodiscard]] MapPoint MapUnproject(const MapCamera& _camera, const UiRect& _graph, float _screenX,
                                    float _screenY) noexcept;

/*
 * The camera that shows the whole graph, centred, with air around it.
 *
 * The entry camera and the zoom floor both. An empty extent gets a camera at
 * the origin at 1 px per unit rather than a division by zero -- a client whose
 * game has no universe draws the chrome and an empty viewport rather than
 * refusing to open, which is `MapTopology::Empty`'s promise one level up.
 */
[[nodiscard]] MapCamera FitMapCamera(const MapExtent& _extent, const UiRect& _graph,
                                     const MapScreenTuning& _tuning) noexcept;

/*
 * What the map measures about its own graph, once.
 *
 * The pinch levels and the top bar are both statements about how big a *thing*
 * is on screen -- a constellation, a region -- and neither can be derived from
 * the graph's total extent, which is what the fractions this replaced tried to
 * do. So the two sizes are measured from the topology at boot and carried, for
 * `MapTopology`'s own reason: the bake does not change, so a per-frame walk over
 * 2,500 nodes would be re-answering a question whose answer is fixed.
 *
 * Both radii are in **map units**, like everything else that crosses.
 */
struct MapGraphMetrics
{
  /*
   * The mean constellation's radius: per group, the furthest member from its
   * *placed* centre, averaged over the groups that have members.
   *
   * The mean rather than the largest, because one sprawling constellation
   * should not decide what the other 249 read as -- and rather than the
   * smallest, because an empty or single-system group would drive it to zero.
   */
  float groupRadiusUnits = 0.0f;

  /*
   * And the mean region's: half the larger side of the box its constellation
   * centres occupy, grown by `groupRadiusUnits` so it covers what is drawn
   * rather than what is placed.
   */
  float regionRadiusUnits = 0.0f;

  /// How many groups and regions actually had members. Zero of either means
  /// there is nothing to measure and the fallbacks take over.
  std::uint32_t groups = 0;
  std::uint32_t regions = 0;
};

/// Measures the graph. Cheap, but linear in the nodes -- ask it once, beside
/// `MapExtentOf`, and keep the answer for the session.
[[nodiscard]] MapGraphMetrics MeasureMapGraph(const MapTopology& _topology) noexcept;

/*
 * Which level this camera is at.
 *
 * `_metrics` is what decides it and `_extent` is only the fallback for a graph
 * with no constellation to measure -- see `MapScreenTuning::regionLevelDiameter`
 * for why the measurement replaced the fractions.
 */
[[nodiscard]] MapPinchLevel MapLevelOf(const MapCamera& _camera, const MapExtent& _extent, const UiRect& _graph,
                                       const MapGraphMetrics& _metrics, const MapScreenTuning& _tuning) noexcept;

/*
 * And the scale that reads as `_level`, for a caller that wants to *jump* to a
 * level rather than pinch to it -- the transit view frames a crossing that way
 * (U6's focus polish, D16).
 *
 * Stated in the level's own currency so the jump lands where the readout agrees
 * with it: this is `MapLevelOf` solved for `pixelsPerUnit` rather than a second
 * derivation that could disagree with the first. Falls back to the fit's own
 * scale times the fraction, for `MapLevelOf`'s reason.
 */
[[nodiscard]] float MapScaleForLevel(MapPinchLevel _level, const MapExtent& _extent, const UiRect& _graph,
                                     const MapGraphMetrics& _metrics, const MapScreenTuning& _tuning) noexcept;

/// The level's word, for the hint box. Never null.
[[nodiscard]] const char* MapPinchLevelName(MapPinchLevel _level) noexcept;

/*
 * Drags the map by a screen delta.
 *
 * The delta is the finger's, so the map follows it: dragging right moves the
 * world right, which means the camera centre moves *left*. Clamped so the
 * camera's centre stays inside the graph's own extent -- a map that can be
 * flung into empty space is a map the player has to find their way back on.
 */
[[nodiscard]] MapCamera PanMapCamera(const MapCamera& _camera, float _deltaX, float _deltaY, const MapExtent& _extent,
                                     const UiRect& _graph, const MapScreenTuning& _tuning) noexcept;

/*
 * Zooms about a point on the screen, which is what a pinch is.
 *
 * `_scaleFactor` is `GestureState::pinchScale`'s ratio: 1 is unchanged, above 1
 * is spreading, below 1 is closing. The point under `(_anchorX, _anchorY)` is
 * held still, so the system between the fingers stays between the fingers --
 * without that a pinch drifts, and a player pinching towards a system watches
 * it slide away from them.
 *
 * Clamped to `[fit, fit × maxZoomFactor]`, and the centre re-clamped after,
 * because zooming out about a corner would otherwise walk the camera off the
 * graph one pinch at a time.
 *
 * **Where the centre clamp bites, it wins and the anchor slides.** Pinching
 * about a point near the edge of the graph asks for a centre outside it, and
 * the two promises cannot both be kept. The clamp is the one worth keeping: a
 * pinch that slides a little is a pinch, and a camera that has walked off the
 * universe is a screen the player has to find their way back from.
 */
[[nodiscard]] MapCamera PinchMapCamera(const MapCamera& _camera, float _scaleFactor, float _anchorX, float _anchorY,
                                       const MapExtent& _extent, const UiRect& _graph,
                                       const MapScreenTuning& _tuning) noexcept;

/// The screen's zones, resolved for one viewport at one scale.
struct MapScreenLayout
{
  UiRect viewport;

  /// The top bar and its three controls. `title` is what is left between them,
  /// and it is the one part of the bar that is data.
  UiRect topBar;
  UiRect back;
  UiRect title;
  UiRect search;
  UiRect menu;

  /// The rail and the panel, both of which scroll (ADR-020 §7). Their internal
  /// splits are data-dependent and come from `SplitMapRail` / `SplitMapPanel`.
  UiRect rail;
  UiRect panel;

  /// The panel's fixed parts: its head, the body the rows go in, and the two
  /// actions anchored to its foot.
  UiRect panelHead;
  UiRect panelBody;
  UiRect setDestination;
  UiRect addWaypoint;

  /// VIEW: switch the watched grid to this system (ADR-016 §7). Above the other
  /// two, and drawn dark on a system the player has nothing in.
  UiRect view;

  /// The viewport the camera projects into, the hint floated in its corner, and
  /// the count line under it.
  UiRect graph;
  UiRect hint;
  UiRect graphFooter;

  /// The history rail, drawn and inert.
  UiRect history;
  UiRect historyHeading;
  UiRect historySpans;
  UiRect historyTrack;

  float scale = 1.0f;
};

/*
 * Resolves the zones.
 *
 * A viewport smaller than the chrome is not an error -- the zones clamp and the
 * panels overlap rather than inverting, which is `ResolveUiLayout`'s posture and
 * for the same reason: dragging a window small must not put a quad with a
 * flipped winding on screen.
 */
[[nodiscard]] MapScreenLayout ResolveMapScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight,
                                               float _scale, const MapScreenTuning& _tuning) noexcept;

/// The rail's three stacked lists, split for the counts it actually holds.
struct MapRailZones
{
  UiRect overlayHeading;
  UiRect overlayRows;
  UiRect legendHeading;
  UiRect legendRows;
  UiRect showHeading;
  UiRect showRows;
};

/*
 * Splits the rail.
 *
 * SHOW is anchored to the rail's foot and the legend sits just above it, which
 * is the print's grouping; the overlay list takes its natural height at the top
 * and the slack falls between the two, which is the print's own `flex:1` on the
 * overlay block. Separate from `ResolveMapScreen` because it depends on how many
 * overlays and legend rows the *game* reported, and a zone resolver that took
 * data would have to be re-run when the data changed rather than when the window
 * did.
 *
 * **Under pressure the legend gives way first, and that order is a decision.**
 * At 1.6x on a 900-pixel window the print's rail wants more height than there
 * is -- the floor has raised every row and the chrome has not shrunk -- so
 * something has to lose. The overlay list is the rail's reason for existing
 * (`strategic-map.png` §2's exclusive layer switch) and SHOW is three fixed
 * toggles; the legend is a readout *of* the overlay, so it is the block that
 * clamps and scrolls. A legend showing its heading and no rows is a visible
 * stub; an overlay list missing SECURITY BAND is a control the player cannot
 * reach.
 */
[[nodiscard]] MapRailZones SplitMapRail(const UiRect& _rail, std::uint32_t _overlayCount, std::uint32_t _legendCount,
                                        float _scale, const MapScreenTuning& _tuning) noexcept;

/// The panel body's parts, split for the counts it holds.
struct MapPanelZones
{
  UiRect facts;
  UiRect routeHeading;
  UiRect routeHops;
  UiRect routeSummary;
};

/*
 * Splits the panel body.
 *
 * Facts take what they need; the route takes the rest and scrolls. That order
 * is a decision: a route can be fourteen hops and the facts are five lines, so
 * giving the facts the slack would leave a route panel that scrolls with a
 * screen of air above it.
 */
[[nodiscard]] MapPanelZones SplitMapPanel(const UiRect& _body, std::uint32_t _factCount, std::uint32_t _hopCount,
                                          float _scale, const MapScreenTuning& _tuning) noexcept;

/*
 * A run of equal rows down a zone, starting at `_firstRow`.
 *
 * One function for the legend, the facts and the route hops, because all three
 * are the same thing: a scrolling list of readouts whose only difference is a
 * row height the caller picks from the tuning. `UiScrollState`'s whole-row cull
 * in both directions -- a row above the zone emits nothing and a row that would
 * run past the bottom is not emitted either, since there is no clip rectangle
 * to hide the half of it hanging out (`UiScrollState.h`'s argument, and the
 * fixed 48-byte `UiInstance` stride behind it).
 *
 * The count returned is what fitted, and `_outRows[i]` is the rect for row
 * `_firstRow + i`.
 */
[[nodiscard]] std::uint32_t BuildMapRows(std::uint32_t _rowCount, const UiRect& _zone, float _rowHeight,
                                         std::uint32_t _firstRow, std::span<UiRect> _outRows) noexcept;

/// One laid-out overlay row.
struct MapOverlayRowRect
{
  UiRect rect;

  /// Which overlay this is, by index into the span it was built from.
  std::uint32_t overlay = 0;

  /// Drawn dimmed and refused by the hit test, exactly as a greyed command is.
  /// Four of the five are false today and the game says why (ADR-016 §9).
  bool enabled = false;
};

/*
 * Lays the OVERLAY rail out, top to bottom, one row per overlay.
 *
 * Every overlay always gets a row, including the disabled ones: they are
 * *visible stubs* by ruling (ADR-016 §9, §9a.3), and a rail that hid them would
 * grow entries later for no visible reason -- the same argument
 * `BuildSettingsNav` makes for the blocked ACCOUNT section.
 *
 * **This list drops rather than scrolls, which is the one place this screen
 * departs from ADR-020 §7's "scroll the panels".** §7's rule is about the
 * panels' *content*, and the two lists here that carry content -- the legend and
 * the route -- do scroll, through `BuildMapRows`' first-row argument. The
 * overlay list is a fixed five-entry switch, and a switch you have to scroll to
 * find an entry of is worse than one that has run out of room visibly. So it
 * takes `CommandRow`'s drop rule, and `SplitMapRail` makes sure the drop is
 * unreachable at every scale the slider offers.
 */
[[nodiscard]] std::uint32_t BuildMapOverlayRows(std::span<const MapOverlayOption> _overlays, const UiRect& _zone,
                                                float _scale, const MapScreenTuning& _tuning,
                                                std::span<MapOverlayRowRect> _outRows) noexcept;

/// Which overlay a press landed on, or null. A disabled row returns null rather
/// than itself, so a caller cannot forget to check.
[[nodiscard]] const MapOverlayRowRect* HitMapOverlayRow(std::span<const MapOverlayRowRect> _rows, float _x,
                                                        float _y) noexcept;

/// One laid-out SHOW checkbox.
struct MapShowRowRect
{
  UiRect rect;
  MapShowToggle toggle = MapShowToggle::GateLinks;
};

/// Lays the three checkboxes out. Always three: they are the screen's own, not
/// the game's, so there is nothing to report and nothing to be absent.
[[nodiscard]] std::uint32_t BuildMapShowRows(const UiRect& _zone, float _scale, const MapScreenTuning& _tuning,
                                             std::span<MapShowRowRect> _outRows) noexcept;

/// Which checkbox a press landed on, or null.
[[nodiscard]] const MapShowRowRect* HitMapShowRow(std::span<const MapShowRowRect> _rows, float _x, float _y) noexcept;

/// The history rail's spans, left to right. Drawn and inert -- see
/// `MAP_HISTORY_SPAN_COUNT`, and note the absence of a hit test.
[[nodiscard]] std::uint32_t BuildMapHistorySpans(const UiRect& _zone, float _scale, const MapScreenTuning& _tuning,
                                                 std::span<UiRect> _outSpans) noexcept;

/// One system, projected.
struct MapNodeRect
{
  /// The dot, centred on the projected point.
  UiRect dot;

  /// Where its word goes. Collapsed when `labelled` is false, so a caller that
  /// forgot to check draws nothing rather than drawing at the origin.
  UiRect label;

  /// The projected point itself, which is what a route line joins and what the
  /// hit test measures against. Carried rather than re-derived from `dot`
  /// because the dot's size depends on the level and the point does not.
  MapPoint point;

  /// Which node this is, by index into `MapTopology::nodes`.
  std::uint32_t node = 0;

  bool labelled = false;
};

/// One gate link, projected. Both ends, because a link with one end off screen
/// still draws the part that is on.
struct MapLinkSegment
{
  MapPoint from;
  MapPoint to;
  std::uint32_t link = 0;
};

/*
 * One constellation, as a disc.
 *
 * A disc rather than the print's blob outline, and that is the *bake's* own
 * shape rather than a simplification of the drawing: U1's clustering invariant
 * is "every system is nearer its own constellation's placed centre than any
 * other's", which is a statement about radii. So the hull this draws is exactly
 * the region the invariant guarantees is disjoint from its neighbours -- and if
 * two of these ever overlap on screen, the bake has broken its own invariant
 * and the map is the place that shows it.
 */
struct MapGroupDisc
{
  MapPoint centre;
  float radiusPixels = 0.0f;
  UiRect label;
  std::uint32_t group = 0;

  /*
   * How many systems belong to it -- **membership, not the cull**.
   *
   * Counted over the whole topology rather than over the visible nodes, for the
   * same reason the radius is: a disc whose shape or number changed as its
   * systems panned off screen would be a disc reporting something about the
   * viewport while looking like it reported something about the universe. It is
   * also what the chip below prints, and a count that shrank when you panned
   * would be the one number on this screen a player could catch lying.
   */
  std::uint32_t nodeCount = 0;

  /*
   * The counted chip, at REGION level: the pip, centred on `centre`.
   *
   * This is the density ladder's last rung applied to the map
   * (`IconDensity.h`, `strategic-map.png` §2's *"region zoom: count +
   * brightness only"*). At the fit over the committed bake, ten systems inside
   * one constellation are two pixels apart -- so they are not drawn as ten
   * smeared pips but as one chip that says how many, roughly where. The
   * individual dots start at CONSTELLATION.
   *
   * Collapsed when `chipped` is false, so a caller that forgot to check draws
   * nothing rather than drawing at the origin -- `MapNodeRect::label`'s posture.
   */
  UiRect chip;
  bool chipped = false;

  /// Whether the disc earned a name this frame: big enough to hold one, and
  /// inside the label budget. See `MapScreenTuning::groupLabelMinDiameter`.
  bool labelled = false;
};

/*
 * One region, marked: its name over its band badge, at the centre of its
 * constellations.
 *
 * Built only at REGION level, which is the role `MapView.h` gives the type --
 * *"the coarsest pinch level, and the one the print badges"* -- and which the
 * screen did not draw at all until R15 was found on it. A separate builder
 * rather than a fourth span on `BuildMapGraph` because it answers a different
 * question at a different rate: the graph is projected and culled per frame at
 * 2,500 nodes, and this is fifty labels that only exist at one level.
 */
struct MapRegionMark
{
  MapPoint centre;
  UiRect label;
  UiRect badge;

  /// Which region this is, by index into `MapTopology::regions`.
  std::uint32_t region = 0;
};

/*
 * Places the region marks for one camera. Returns what it wrote; nothing at all
 * at the two finer levels, where a region is bigger than the screen and a name
 * at its centre would sit on top of the systems it contains.
 *
 * **The name and the badge are gated separately, each on its own width**, which
 * is the group label's rule one level up and was learned the same way: fifty
 * region names at the universe fit are 112 pixels of word laid over 47 pixels
 * of region, which is the label soup R15 names, arriving one tier higher than
 * the constellations it was found on. So a word is drawn only where it fits the
 * thing it names -- which at the fit is the short band badges and not the long
 * names, and one pinch in is both.
 *
 * `_metrics` supplies how wide a region is; a graph it could not measure gets
 * no marks rather than unmeasured ones.
 */
[[nodiscard]] std::uint32_t BuildMapRegionMarks(const MapTopology& _topology, const MapCamera& _camera,
                                                const UiRect& _graph, MapPinchLevel _level,
                                                const MapGraphMetrics& _metrics, float _cellWidth, float _scale,
                                                const MapScreenTuning& _tuning,
                                                std::span<MapRegionMark> _outMarks) noexcept;

/// What one graph build produced.
struct MapGraphCounts
{
  std::uint32_t nodes = 0;
  std::uint32_t links = 0;
  std::uint32_t groups = 0;
  std::uint32_t labels = 0;
};

/*
 * Projects and culls the graph for one camera.
 *
 * Rebuilt every frame from the topology, like every other build on this client
 * -- the camera moves continuously and a diffed graph would be a cache that has
 * to be told when the camera moved.
 *
 * **What the level and the checkboxes govern**, which is the density ladder
 * `strategic-map.png` §2 states and this builder did not have:
 *
 * - **REGION** builds *no node rects at all*. Every constellation becomes one
 *   counted chip (`MapGroupDisc::chip`), because 2,500 six-pixel pips two
 *   pixels apart are a smear rather than a graph -- §2's *"region zoom: count
 *   and brightness only"* taken literally instead of applied to labels only.
 * - **CONSTELLATION** builds the dots, their names, and the hulls: this is the
 *   level a constellation is a thing you are looking *at*.
 * - **SYSTEM** builds the dots and their names and no hulls, because inside one
 *   constellation its own outline is the whole screen.
 *
 * A group's **name** is not automatic at either level it is drawn: it needs a
 * disc big enough to hold one (`groupLabelMinDiameter`) and a slot in the same
 * `maxLabels` budget the node labels spend from. Emitting 250 group labels
 * unconditionally, outside the budget, is what put the whole constellation
 * vocabulary on top of itself at the fit.
 *
 * Everything is further gated by its SHOW checkbox, and a checkbox that is off
 * means zero built rather than built-and-not-drawn: the cost this saves is the
 * whole point of the control at 2,500 nodes.
 *
 * `_cellWidth` measures labels, and it is the caller's *drawn* cell width, so
 * the label rect cannot be sized against a different face than the one inside
 * it -- `CountedChipRect`'s arrangement.
 *
 * Writes at most what each span holds and reports what it wrote. A span too
 * small truncates rather than overruns, and a truncated graph is a graph
 * missing its tail rather than a corrupted one.
 */
[[nodiscard]] MapGraphCounts BuildMapGraph(const MapTopology& _topology, const MapCamera& _camera,
                                           const UiRect& _graph, MapPinchLevel _level, const MapShowFlags& _show,
                                           float _cellWidth, float _scale, const MapScreenTuning& _tuning,
                                           std::span<MapNodeRect> _outNodes, std::span<MapLinkSegment> _outLinks,
                                           std::span<MapGroupDisc> _outGroups) noexcept;

/*
 * Which system a tap selected, or null.
 *
 * **The nearest within the target radius, not the first**, which is the one
 * place the 48 px floor changes behaviour rather than a number: a 6 px pip with
 * a finger-sized target means two systems at REGION level can have overlapping
 * targets, and first-found would let *bake order* decide which of two adjacent
 * systems a tap picked. Nearest is the answer a player would predict.
 *
 * Measured against `MapNodeRect::point` rather than against `dot`, so the
 * target does not change size with the pinch level while the finger does not.
 */
[[nodiscard]] const MapNodeRect* HitMapNode(std::span<const MapNodeRect> _nodes, float _x, float _y, float _scale,
                                            const MapScreenTuning& _tuning) noexcept;

/*
 * Which constellation a tap selected at REGION level, or null.
 *
 * **The counterpart to the chip, and the reason the chip is worth building.**
 * At the fit over the committed bake a constellation's ten systems sit inside
 * two pixels of each other, so ten 48 px targets are stacked on one point and
 * `HitMapNode` would resolve between them on sub-pixel proximity -- a coin toss
 * dressed as a choice. A player up there is not choosing a system; they are
 * choosing where to go and look. So the tap lands on the cluster, and what it
 * means is select-and-zoom rather than select.
 *
 * Nearest within the target radius, like `HitMapNode`, and measured against the
 * disc's centre so the target does not change size with the pinch.
 */
[[nodiscard]] const MapGroupDisc* HitMapGroup(std::span<const MapGroupDisc> _groups, float _x, float _y, float _scale,
                                              const MapScreenTuning& _tuning) noexcept;

/*
 * Which region the top bar names, or `INVALID_MAP_REGION` for the universe.
 *
 * `strategic-map.png`'s bar is `◀ TACTICAL | <region> [BAND]`, and the region on
 * it is the one you are **looking at**. What the screen drew instead was the
 * first region in the bake beside whole-universe counts -- a name that was
 * right only by accident and never changed as the camera moved across fifty
 * regions.
 *
 * The rule is what the print's own sentence says: at the fit there is no region
 * you are looking at, so the bar reads the universe; once one region's content
 * covers most of the viewport, it reads that one. The region nearest the camera
 * centre is the one it means -- regions do not overlap (U1's own invariant says
 * so, mechanically, since R15), so nearest-centre and contains-the-centre are
 * the same answer wherever they differ by less than a region's own width.
 */
inline constexpr std::uint32_t INVALID_MAP_REGION = 0xffffffffu;

[[nodiscard]] std::uint32_t MapTitleRegion(const MapTopology& _topology, const MapCamera& _camera,
                                           const UiRect& _graph, const MapGraphMetrics& _metrics,
                                           const MapScreenTuning& _tuning) noexcept;

/// The three controls in the top bar, and nothing.
enum class MapTopBarHit : std::uint8_t
{
  None = 0,
  Back,
  Search,
  Menu
};

[[nodiscard]] MapTopBarHit HitMapTopBar(const MapScreenLayout& _layout, float _x, float _y) noexcept;

/// The panel's two actions, and nothing.
enum class MapActionHit : std::uint8_t
{
  None = 0,
  SetDestination,
  AddWaypoint,

  /*
   * Watch this system's grid.
   *
   * Gated separately from the other two, because the question it asks is a
   * different one: SET DESTINATION needs a fleet *and* somewhere to send it,
   * and VIEW needs ships already standing in the system pressed. A player can
   * easily have one and not the other -- routing a fleet to a system they have
   * nothing in is the ordinary case.
   */
  View
};

/*
 * Which action a press landed on.
 *
 * `_canRoute` is the caller's answer to "is there a destination to set" -- a
 * system is selected and it is not the one the fleet is in. Both buttons are
 * *drawn* either way and refused here, which is this corpus's standing posture:
 * `station-screen.png` §2's *"disabled with a reason rather than hidden"*, and
 * the reason a player can read is why the button stays where it was.
 */
[[nodiscard]] MapActionHit HitMapAction(const MapScreenLayout& _layout, bool _canRoute, bool _canView, float _x,
                                        float _y) noexcept;

/*
 * One fleet marker, placed against the node it belongs to.
 *
 * Separate from `MapNodeRect` rather than a field on it, and the reason is
 * rate: the graph is projected every frame and the markers arrive at about
 * 1 Hz, so a marker folded into the node run would be rebuilt sixty times a
 * second to say what it said last frame. This is the join, done per frame
 * because the camera moves, over a list that does not.
 */
struct MapMarkerRect
{
  /// Where the count badge goes, above and right of its node's dot.
  UiRect badge;

  /// Which marker this is -- an index into the caller's marker list.
  std::uint32_t marker = 0;
};

/*
 * Places each marker against its node, dropping the ones the cull removed.
 *
 * `_nodes` must be the run `BuildMapGraph` wrote, which is sorted by node index
 * -- so this is a binary search per marker rather than a scan. At the corpus's
 * cap a scan would be 256 x 2,500 comparisons a frame to draw at most 256
 * badges, which is the same arithmetic the route line's endpoint lookup already
 * refused.
 *
 * A marker whose node is off screen emits nothing: it is not drawn, so there is
 * nothing to place. The count line under the graph is where "there are more
 * than you can see" belongs, the way it already is for nodes.
 */
[[nodiscard]] std::uint32_t BuildMapMarkerRects(std::span<const MapMarker> _markers, const MapTopology& _topology,
                                                std::span<const MapNodeRect> _nodes,
                                                std::span<const MapGroupDisc> _groups, float _scale,
                                                const MapScreenTuning& _tuning,
                                                std::span<MapMarkerRect> _outRects) noexcept;

} // namespace Neuron
