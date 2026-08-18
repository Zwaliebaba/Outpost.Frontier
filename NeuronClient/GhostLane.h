#pragma once

#include "OverlayMark.h"
#include "Picking.h"
#include "UiDrawList.h"

#include <cstdint>
#include <span>

/*
 * The ghost's lane and its label -- ADR-006 §8's draw list *(B)*, at last.
 *
 * `tactical-hud.png` draws a dashed line from the fleet into the footprint and
 * two lines of text under it: `MOVE - CLAW`, then `18.4 km - ETA 41s`. S9
 * shipped the footprint and its station ticks as mechanism *(A)* marks -- they
 * are circles on the plane, which is what that pass draws -- and left this
 * half owed, because "a line between two points is not a quad around one and a
 * label is text" (ADR-006 §8a).
 *
 * **Screen space, and therefore never occluded.** `overlay-pass.png`'s
 * retirement matrix carries the order ghost as `MECH B - UIDRAWLIST` with no
 * OCCLUDES badge, unlike the footprint beside it. So the lane is projected on
 * the CPU and drawn as Ui quads: it passes in front of a Carrier rather than
 * behind it. That is the sheet's decision, not a shortcut -- a route the hull
 * you are flying around can hide is a route you cannot follow.
 *
 * **It is a summary of the order, not a live countdown.** Both numbers are
 * fixed when the order commits: the distance is the lane's own length and the
 * ETA came back from `SolvePreview` with the footprint. Neither can update,
 * because a ghost knows how many members it has and not *which* -- there is no
 * way from here to ask where those ships are now. S12 replicates the
 * authority's per-leg ETA in the order state and that is what makes the label
 * tick.
 *
 * **Device-free.** Projection, clipping, dash placement and label text are all
 * arithmetic over a mapping and a viewport, so the whole file is tested without
 * a GPU -- which matters more here than usual, because the primitive it draws
 * on is the one thing in the Ui pass that no test on this machine can run.
 */

namespace Neuron
{

/// Where the lane is being drawn. Bundled because five loose parameters is how
/// a caller passes the height as the width.
struct GhostLaneView
{
  PlaneMapping mapping;
  std::uint32_t viewportWidth = 0;
  std::uint32_t viewportHeight = 0;

  /*
   * The zone world-space content may draw in -- the HUD's `world` rect.
   *
   * A clip rather than a courtesy. The panels are drawn after the lane and
   * cover it, so overlap is already invisible; what this prevents is *cost*.
   * A target 40 km away with the camera zoomed in is a lane megapixels long,
   * and dashing its whole length would be tens of thousands of quads for the
   * few hundred pixels of it anybody can see.
   */
  UiRect worldRect;

  /// Monospace cell width in pixels, for centring a label under the footprint.
  /// The face is fixed-pitch (ADR-006 §9), so a line's width is its length
  /// times this and no measuring pass is needed.
  float cellPixels = 8.0f;

  /// The UI scale, so the lane's dashes and its label gap grow with the HUD
  /// rather than staying at 1.0x while everything around them scales.
  float scale = 1.0f;
};

/// Pixel geometry. Colours are **not** here: they come from `OverlayTuning`,
/// because the lane and the footprint it runs into are the same ghost and two
/// colour tables would eventually disagree about what PENDING looks like.
struct GhostLaneTuning
{
  float dashPixels = 10.0f;
  float gapPixels = 8.0f;
  float thicknessPixels = 2.0f;

  /// Below the footprint's ring, where the print puts the two label lines.
  float labelGapPixels = 8.0f;
  float labelLineHeightPixels = 13.0f;

  std::uint8_t labelSizeIndex = 0;

  /*
   * A backstop, not a budget.
   *
   * The clip already bounds a lane to the viewport's diagonal, so a sane frame
   * produces at most a couple of hundred dashes per ghost. This catches the
   * case where a tuning edit sets the pitch to something near zero and turns
   * one lane into a million quads before anybody notices.
   */
  std::uint32_t maxDashesPerLane = 512;
};

/*
 * Clips a screen-space segment to a rect, in place.
 *
 * Liang-Barsky. Returns false when the segment is wholly outside, in which case
 * the endpoints are untouched -- a caller that ignored the return would then
 * draw the *unclipped* line rather than a garbage one, which is the failure
 * that is obvious on screen rather than subtle.
 *
 * Exposed because it is the part most worth testing on its own: every
 * interesting case (both ends in, one end in, crossing corner to corner,
 * parallel and outside) is a two-line test here and a screenshot otherwise.
 */
[[nodiscard]] bool ClipSegmentToRect(const UiRect& _rect, DirectX::XMFLOAT2& _start, DirectX::XMFLOAT2& _end) noexcept;

/// `18.4 km` / `940 m`, and `ETA 1m 41s` / `ETA 41s`. Written into `_out`,
/// which is never left unterminated. A negative `_etaSeconds` omits the ETA
/// half rather than printing a negative time -- the game says it cannot tell,
/// and inventing a number would be the client answering for it.
void FormatLaneDetail(float _distanceMetres, float _etaSeconds, char* _out, std::size_t _capacity) noexcept;

/*
 * Appends every ghost's lane and label.
 *
 * Call **before** the HUD's panels are added, so they composite over it:
 * `overlay-pass.png` §1's rule is that panels and toasts always cover
 * world-space marks, and the Ui pass has one pipeline and no sort, so build
 * order is draw order.
 *
 * `_nowSeconds` is the same clock `OrderGhostList::Advance` was given, so a
 * refused ghost's lane retracts and fades in step with the footprint it points
 * at rather than a frame behind it.
 */
void BuildGhostLanes(std::span<const OrderGhost> _ghosts, const GhostLaneView& _view, const OverlayTuning& _colours,
                     const GhostLaneTuning& _tuning, double _nowSeconds, UiDrawList& _outList);

} // namespace Neuron
