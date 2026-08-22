#pragma once

#include "StationView.h"
#include "UiDrawList.h"

#include <cstdint>
#include <span>

/*
 * Where everything on the station surface goes (`station-screen.png` §1,
 * ADR-020 §5.1, §7).
 *
 * `CommandRow`'s shape applied to a whole screen: **laid out and hit-tested in
 * one file**, so the thing you press is the thing you see. That rule cost
 * nothing while a HUD had one clickable row; a screen with a tab row, sixty
 * chips, two dropdowns and a primary action is where it starts paying, because
 * every one of those is a rect that a draw and an input handler could disagree
 * about independently.
 *
 * **Numbers read off the print, which is authored at 1440×900** and is what
 * ADR-020 §7 means by "the prints are normative at 1440×900". The zones there
 * are exact: a 46 px status bar, a 52 px tab row under it, and the rest split
 * 1032 / 408 between the roster and the composer. Everything below is those
 * numbers times the UI scale, which is R9's entire permitted flexibility.
 *
 * **Overflow follows §7's declared rule for this surface: the wing columns
 * scroll, and the parking diagram letterboxes.** The diagram is a readout
 * rather than a control -- it is on the screen because ADR-017 §4 made the
 * player a promise about self-parking that they have to be able to see -- so
 * holding its aspect is the honest degradation, where stretching it would be
 * drawing a ring system that is not the one the fleet flies.
 */

namespace Neuron
{

/// Sizes at scale 1.0, off the print.
struct StationScreenTuning
{
  /// The status bar, the same height the tactical HUD's is: one bar across the
  /// client, whichever surface is under it.
  float statusBarHeight = 46.0f;

  /// The tab row: 52 tall, 2 between tabs, inset 26 from the edges.
  float tabRowHeight = 52.0f;
  float tabGap = 2.0f;
  float tabInsetX = 26.0f;

  /// Air either side of a tab's word, in cells rather than pixels, because the
  /// face is monospace and a tab is sized to what it says.
  float tabPaddingCells = 2.0f;

  /// The composer's column, and the roster gets what is left (1032 at 1440).
  float composerWidth = 408.0f;

  float rosterPaddingX = 26.0f;
  float rosterPaddingY = 22.0f;
  float composerPaddingX = 22.0f;
  float composerPaddingY = 20.0f;

  /// A wing column and the chips down it. The print fits sixty ships without a
  /// scrollbar, which is what a chip this size in a panel this wide buys.
  float columnWidth = 196.0f;
  float columnGap = 14.0f;
  float columnHeaderHeight = 30.0f;
  float chipHeight = 26.0f;
  float chipGap = 4.0f;

  /// The composer's controls, top to bottom under its heading.
  float headingHeight = 18.0f;
  float rowHeight = 30.0f;
  float rowGap = 10.0f;

  /*
   * UNDOCK: "68 px, always visible, disabled with a reason rather than hidden".
   * The print's number, and the reason it is the largest control on the screen
   * is that it is the only primary action.
   */
  float undockHeight = 68.0f;

  /// The parking diagram's aspect, letterboxed into whatever room is left.
  /// Square, because it draws two concentric rings around a point.
  float parkingAspect = 1.0f;
};

/// The screen's zones, resolved for one viewport at one scale.
struct StationScreenLayout
{
  UiRect viewport;
  UiRect statusBar;
  UiRect tabRow;

  /// The wing columns live here, and this is what scrolls (§7).
  UiRect roster;

  /// The composer panel down the right.
  UiRect composer;

  /// The composer's controls, top to bottom. Every one of these is pressed, so
  /// every one is resolved here rather than in the draw.
  UiRect selectionHead;
  UiRect formation;
  UiRect undock;
  UiRect parking;
  UiRect assignWing;
  UiRect newWing;

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
[[nodiscard]] StationScreenLayout ResolveStationScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight,
                                                       float _scale, const StationScreenTuning& _tuning) noexcept;

/// One laid-out tab.
struct StationTabButton
{
  UiRect rect;

  /// Which `StationTab` this is, by index into the span it was built from.
  std::uint32_t tab = 0;

  /// Echoed from the game, and handed back on a press.
  std::uint16_t id = 0;

  /// Drawn dimmed and refused by the hit test, exactly as a greyed command is.
  bool enabled = false;
};

/*
 * Lays the tab row out, left to right, each tab sized to its own word.
 *
 * A tab that would run past the row's right edge is **dropped**, not shrunk or
 * wrapped -- `CommandRow`'s rule, and the same argument: a row that reflowed at
 * some window width would be a layout engine, which R9 says this pass must not
 * become. The count returned is what fitted.
 */
[[nodiscard]] std::uint32_t BuildStationTabRow(std::span<const StationTab> _tabs, const UiRect& _row, float _scale,
                                               const StationScreenTuning& _tuning,
                                               std::span<StationTabButton> _outButtons);

/// Which tab a press landed on, or null. A disabled tab returns null rather
/// than itself, so a caller cannot forget to check.
[[nodiscard]] const StationTabButton* HitStationTab(std::span<const StationTabButton> _buttons, float _x,
                                                    float _y) noexcept;

/// One laid-out wing column: its header, and where its chips begin.
struct StationColumnRect
{
  UiRect header;

  /// Which `StationGroup` this is, by index.
  std::uint32_t group = 0;
};

/// One laid-out chip.
struct StationChipRect
{
  UiRect rect;

  /// Which `StationChip` this is, by index into the span it was built from.
  std::uint32_t chip = 0;
};

/*
 * Lays the roster out: a column per group, chips down each.
 *
 * `_scrollRows` is the first chip row to draw in every column -- the scrolling
 * list applied to the one zone §7 says scrolls. Chips above it emit nothing,
 * and a chip that would run past the panel's bottom is not emitted either,
 * which is `UiScrollState`'s whole-row cull rather than a second one.
 *
 * A column that would run past the panel's right edge is dropped with its
 * chips, for the tab row's reason.
 */
[[nodiscard]] StationRosterCounts BuildStationColumns(std::span<const StationGroup> _groups,
                                                      std::span<const StationChip> _chips, const UiRect& _panel,
                                                      std::uint32_t _scrollRows, float _scale,
                                                      const StationScreenTuning& _tuning,
                                                      std::span<StationColumnRect> _outColumns,
                                                      std::span<StationChipRect> _outChips);

/// Which chip a press landed on, or null. The print's first gesture: tap
/// toggles.
[[nodiscard]] const StationChipRect* HitStationChip(std::span<const StationChipRect> _chips, float _x,
                                                    float _y) noexcept;

/// Which column header a press landed on, or null. The print's second gesture:
/// holding a header takes the whole wing.
[[nodiscard]] const StationColumnRect* HitStationColumnHeader(std::span<const StationColumnRect> _columns, float _x,
                                                              float _y) noexcept;

/// How many chip rows the tallest column would need, so the caller can tell
/// `UiScrollState` what it is scrolling over.
[[nodiscard]] std::uint32_t StationColumnRows(std::span<const StationChip> _chips,
                                              std::uint32_t _groupCount) noexcept;

/// How many chip rows fit the panel at this scale -- `UiScrollState`'s visible
/// count, derived from the same numbers the layout uses.
[[nodiscard]] std::uint32_t StationVisibleRows(const UiRect& _panel, float _scale,
                                               const StationScreenTuning& _tuning) noexcept;

} // namespace Neuron
