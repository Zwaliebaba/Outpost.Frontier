#pragma once

#include "UiDrawList.h"

#include <cstdint>

/*
 * Where the HUD's zones are (ADR-006 §10, `tactical-hud.png`).
 *
 * The print is the spec, and the numbers below are read off it: a top status
 * row, a roster down the left, an ability rack down the right, and a context
 * bar with a command row across the bottom. What is left in the middle is the
 * world, and the world is what everything else must not cover.
 *
 * **Zones, not widgets.** This file decides where things go and nothing about
 * what they say -- so the layout can be asserted against the print's
 * proportions without a device, a font, or a fleet. R9 is the risk it is shaped
 * against: "layouts hardcoded to print zones, UI-scale multiplier the only
 * flexibility" is the whole permitted surface, and a zone table is the cheapest
 * thing that cannot grow into a layout engine.
 *
 * **UI scale is a multiplier from day one** (ADR-006 §10, the settings sheet
 * makes 0.8-1.6x a requirement). Everything here is a constant times the scale,
 * which is why the constants are pixels at 1.0 rather than fractions of the
 * viewport: a HUD that scaled with the window would shrink its own text on a
 * small screen, which is the opposite of what a scale control is for.
 */

namespace Neuron
{

/// Sizes at scale 1.0, in pixels. Arguable, and all in one place for exactly
/// that reason -- these are the numbers a pass over the print retunes.
struct UiTuning
{
  float topBarHeight = 34.0f;
  float rosterWidth = 176.0f;
  float abilityWidth = 128.0f;

  /// The context bar (`N SHIPS : WING -> FORMATION`) and the command row of
  /// buttons under it. Two zones because the print draws two, and because a
  /// toast may never cover either (`alerts-and-toasts.png` §2).
  float contextBarHeight = 34.0f;
  float commandRowHeight = 62.0f;

  /// Gap between a panel's edge and its contents.
  float padding = 8.0f;

  /// The bottom-right toast stack. Rows are one head line and one detail line
  /// plus padding, and the sheet caps the stack at five of them.
  float toastWidth = 268.0f;
  float toastHeight = 46.0f;
  float toastGap = 6.0f;

  /// The centre-top critical surface, which is its own zone and not part of the
  /// stack: the sheet puts it near the eye's resting point during a fight.
  float criticalWidth = 420.0f;
  float criticalHeight = 52.0f;
  float criticalTopMargin = 24.0f;

  /// Which baked size each band of text uses. Indices into `GlyphAtlas`, never
  /// point sizes -- retuning the size list is then a content change.
  std::uint8_t smallSizeIndex = 0;
  std::uint8_t bodySizeIndex = 1;
  std::uint8_t headSizeIndex = 2;
};

/*
 * The zones, resolved for one viewport at one scale.
 *
 * `world` is the interesting one: it is what is left after the chrome, and it
 * is what picking and the camera would have to be told about if the HUD ever
 * stopped being a border around the view. It is reported rather than used for
 * now, so that the day something needs it there is one answer rather than four
 * subtractions in four files.
 */
struct UiLayout
{
  UiRect viewport;
  UiRect topBar;
  UiRect roster;
  UiRect abilityRack;
  UiRect contextBar;
  UiRect commandRow;
  UiRect world;

  /// Centre-top, for the one critical toast the sheet allows to interrupt.
  UiRect criticalToast;

  float scale = 1.0f;

  /// Where the `_index`-th bottom-right toast goes, stacking upward from just
  /// above the context bar. Index 0 is the top slot, which is the sheet's
  /// place for an Urgent row.
  [[nodiscard]] UiRect ToastSlot(std::size_t _index, const UiTuning& _tuning) const noexcept;
};

/*
 * Resolves the zones.
 *
 * A viewport smaller than the chrome is not an error: the zones clamp, `world`
 * collapses to nothing, and the HUD draws over itself rather than inverting.
 * That case is reachable by dragging a window small, and a layout that produced
 * negative sizes there would put a quad with a flipped winding on screen.
 */
[[nodiscard]] UiLayout ResolveUiLayout(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight, float _scale,
                                       const UiTuning& _tuning) noexcept;

} // namespace Neuron
