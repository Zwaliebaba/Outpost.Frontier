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
 * **A queued order is one chain, not one ghost per waypoint** (S12,
 * `puck-and-wheel.png` §4: "waypoints render as a polyline with per-leg ETAs").
 * The lane walks the ghost's legs, so a single-leg order is this with two
 * points and there is no separate case for it. Only the *last* waypoint
 * retracts on a refusal, and only to the one before it: a refused append takes
 * back the leg that was refused rather than the plan the player already had.
 *
 * **The distance is fixed at commit; the ETA is not, any more.** The distance
 * is the plan's own length, walked leg by leg. The ETA of the leg the fleet is
 * *currently* flying is the authority's, replicated in the order state (S12a),
 * so it counts down; the legs ahead of it keep the game's prediction, because
 * the authority has not started them and has nothing measured to say.
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
  /*
   * One dash-plus-gap cycle, in **screen pixels**, scaled by the HUD's own
   * `scale` like every other piece of chrome.
   *
   * This was authored in world metres first, on the argument that a
   * screen-space period makes the dashes travel at different *world* speeds as
   * the camera zooms. That is true and it is not what matters: the lane is a
   * Ui-pass element drawn in screen space beside the labels it carries, and the
   * eye judges it against the screen rather than against the ground. In metres
   * the pattern was 90 px a cycle zoomed right in and about a pixel zoomed
   * right out -- the same order looking like four different things depending on
   * the camera. **Owner's call, 2026-08-19: the print's dash is a fixed size,
   * independent of camera distance.**
   *
   * The trade taken knowingly: the dashes now cover more ground per second when
   * zoomed out. Nobody reads a decorative crawl as a ground speed, and the
   * alternative was a pattern that only looked right at one zoom.
   *
   * 16.8 px at scale 1: a 13.4 px dash and a 3.4 px gap, three times the size
   * this was first tuned to (owner, 2026-08-19). The enlargement also bought
   * back a defect the small pattern had: at a 1.1 px gap roughly two fifths of
   * the lane rendered solid because the break fell below what anti-aliasing
   * could resolve, so the line only *looked* dashed in places. At 3.4 px every
   * gap survives.
   */
  float dashPeriodPixels = 16.8f;

  /*
   * How much of the period is dash rather than gap.
   *
   * 0.8 is the owner's "the gap should be about a quarter of the dash"
   * (2026-08-19): four parts mark to one part space. So the lane reads as a
   * near-continuous line that is *visibly travelling* rather than as a row of
   * separate ticks -- the gap is there to carry the motion, not to break the
   * line up.
   */
  float dashDutyCycle = 0.8f;

  /*
   * Seconds for one whole cycle to travel toward the destination -- the
   * design's `crawl`, linear and looping forever.
   *
   * **This is seconds per *cycle*, so it is not the speed** -- the speed is
   * `dashPeriodPixels / crawlSecondsPerCycle`, and changing the cycle's size
   * without touching this changes the speed with it. That caught this tuning
   * out once already, when shrinking the pattern silently slowed the march.
   *
   * 3.06 s over a 16.8 px cycle is **5.5 px/s, and at every zoom** -- the
   * screen-space period above is what makes that a single number rather than a
   * range.
   *
   * That 5.5 px/s has been held across two retunings of the pattern's size, and
   * holding it is why this number keeps moving: tripling the cycle in 2026-08-19
   * tripled this with it (1.02 -> 3.06 s), because leaving it alone would have
   * tripled the speed as a side effect of a change that was only about size.
   *
   * **Zero or less turns the crawl off and leaves a static dashed lane**, which
   * is deliberately the hook a reduce-motion accessibility setting will pull:
   * the crawl is decorative and must be able to stop, unlike the order-rejection
   * bounce, which carries information and must not. No such setting exists yet
   * (2026-08-19); when it does, it sets this to 0.
   */
  float crawlSecondsPerCycle = 3.06f;

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

/// Just the time -- `1m 41s` or `41s` -- for the per-leg labels a queued chain
/// hangs at its waypoints. **Empty** for a negative input rather than a printed
/// negative time: the game said it cannot tell, and the caller draws nothing.
/// `FormatLaneDetail` is written in terms of this, so the two cannot disagree
/// about what a minute and a half looks like.
void FormatEta(float _seconds, char* _out, std::size_t _capacity) noexcept;

/*
 * Appends every ghost's lane and label.
 *
 * Call **before** the HUD's panels are added, so they composite over it:
 * `overlay-pass.png` §1's rule is that panels and toasts always cover
 * world-space marks, and the Ui pass has one pipeline and no sort, so build
 * order is draw order.
 *
 * `_entities` is this frame's scene, and it is what makes a lane's tail follow
 * the fleet: the line is drawn from the ordering ship's *current* position, not
 * from where it stood when the order was given. The same list picking and the
 * overlay run over, so the line starts exactly where the hull is drawn.
 *
 * `_nowSeconds` is the same clock `OrderGhostList::Advance` was given, so a
 * refused ghost's lane retracts and fades in step with the footprint it points
 * at rather than a frame behind it. It is the **render** clock and never the
 * tick: the crawl below is presentation, and nothing here may reach anything
 * hashed.
 */
void BuildGhostLanes(std::span<const OrderGhost> _ghosts, std::span<const SceneEntity> _entities,
                     const GhostLaneView& _view, const OverlayTuning& _colours, const GhostLaneTuning& _tuning,
                     double _nowSeconds, UiDrawList& _outList);

/*
 * Where in its cycle the dash pattern is, as 0..1.
 *
 * Wrapped here, in doubles, rather than by handing an ever-growing elapsed time
 * to whatever consumes it. That is the whole reason this is a function: seconds
 * since start is a fine `double` after an hour and a *terrible* `float` -- at
 * 3,600 s a float's step is already a quarter of a millisecond, and the phase
 * built from it quantises into visible jerks. Wrapping first keeps the number
 * that leaves this function inside [0,1) for ever.
 *
 * A period of zero or less is "no crawl" and answers 0, which is what makes
 * turning the animation off a tuning change rather than a branch at the call
 * site. Negative times answer a positive phase, because `fmod` keeps the sign
 * of the dividend and a phase of -0.3 would step the pattern backwards.
 */
[[nodiscard]] float DashCrawlPhase(double _nowSeconds, double _periodSeconds) noexcept;

} // namespace Neuron
