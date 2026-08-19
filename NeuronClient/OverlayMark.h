#pragma once

#include "OrderGhost.h"
#include "RenderWorld.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

/*
 * What the OverlayWorld pass draws, built on the CPU (ADR-006 §8).
 *
 * ADR-006 §8 splits the overlay in two: *(A)* per-entity instanced marks --
 * selection ellipses and hull/shield bars -- and *(B)* a client-authored draw
 * list for the order puck and ghost polylines. This file is (A), and S9 added
 * the half of the ghost that (A) can already express: the footprint ring and
 * its station ticks are circles on the plane, which is what this pass draws.
 * What is still (B) is the *polyline* -- the dashed lane from the fleet to the
 * destination and the per-leg ETA labels -- because a line between two points
 * is not a quad around one, and a label is text. Both belong with the Ui pass
 * (ADR-006 §10) and arrive with it.
 *
 * **One instance per mark, one quad per instance.** The vertex shader builds a
 * quad from `SV_VertexID` and the pixel shader decides what is inside it: a
 * ring is a distance test, a bar is a fill fraction. No index buffer, no
 * per-mark geometry, and adding a mark type is a branch rather than a mesh.
 *
 * **Rings and bars live in different spaces on purpose.** A selection ring is
 * a circle *on the plane*, so it foreshortens to the 2:1 ellipse ADR-006 §4
 * fixed the elevation to produce, and it depth-tests against hulls so a ring
 * behind a Carrier is occluded by it. A bar is screen-facing and never
 * occludes, because a health readout that hides behind the thing it describes
 * is not a readout. That is the `overlay-pass.png` rule, and it is why the
 * marks are ordered rings-then-bars: the two halves are two draws with
 * different depth state.
 *
 * **Device-free.** Which marks exist, where they are and how big they are is
 * arithmetic, so it is tested without a GPU. What a GPU has to confirm is that
 * they are drawn in the right order with the right depth state.
 */

namespace Neuron
{

/*
 * Plane-lying kinds first, screen-facing kinds after, and the numbering says
 * so: `OverlayKind::FIRST_SCREEN_FACING` is the boundary the shader branches on
 * and the point `OverlayMarkList::ringCount` splits the array at. Two orderings
 * that had to agree became one that cannot disagree.
 */
enum class OverlayKind : std::uint16_t
{
  SelectionRing = 0,
  OrderFootprint = 1,
  OrderStation = 2,

  HullBar = 3,
  ShieldBar = 4,

  /// The icon sheet's STALE state marker (§4): a dashed screen-facing ring on a
  /// ship whose extrapolation hit the 250 ms cap and froze (ADR-002 §4). Screen
  /// facing, because it is a readout about the *feed* and must never hide
  /// behind the hull it is warning about -- and dashed, the sheet's convention
  /// for anything unresolved.
  StaleMarker = 5,

  FIRST_SCREEN_FACING = HullBar
};

/*
 * One instanced mark. 32 bytes, and the fields a kind does not use are zero.
 *
 * The per-kind meaning is deliberate rather than sloppy: a ring is sized in
 * plane metres because it lies on the plane, and a bar is sized in pixels
 * because it faces the screen and must stay legible at every zoom. Packing
 * both into one "size" field would mean the shader guessing which one it had.
 */
struct OverlayMark
{
  DirectX::XMFLOAT2 anchorPlane{};    // Where on the plane this mark belongs.
  float radiusMetres = 0.0f;          // Rings and ticks: the radius on the plane.
  float halfWidthPixels = 0.0f;       // Bars: half the bar's screen width.
  float halfHeightPixels = 0.0f;      // Bars: half its screen height.
  float offsetUpPixels = 0.0f;        // Bars: how far above the anchor, on screen.
  std::uint32_t colourRgba = 0;       // Packed 8:8:8:8, r in the low byte.
  std::uint16_t kind = 0;             // OverlayKind.
  std::uint16_t fill = 0;             // Bars: 0-65535 of the full width.
                                      // Footprints: dashes around the ring, 0 solid.
};

static_assert(sizeof(OverlayMark) == 32, "OverlayMark is a per-instance vertex stream; its size is the stride the input "
                                         "layout declares, so a change here is a change in three places");

/*
 * Sizes and colours, all of them arguable and none of them scattered.
 *
 * Pixel quantities rather than metres wherever the mark faces the screen, so
 * the overlay stays the same size as the camera zooms -- which is the whole
 * reason the icon sheet specifies a scaling law rather than a world size.
 */
struct OverlayTuning
{
  /// A ring never shrinks below this on screen (ADR-006 §8's clamp). Without
  /// it a selected Interceptor at 40 km is a ring smaller than the cursor.
  float ringMinRadiusPixels = 14.0f;

  /// How far outside the hull the ring sits, so it reads as *around* the ship
  /// rather than *on* it.
  float ringPadMetres = 8.0f;

  /// The print's world-space bars: 24 px wide, 2 px hull over 2 px shield
  /// (`tactical-hud.png`), so half-sizes of 12 and 1.
  float barHalfWidthPixels = 12.0f;
  float barHalfHeightPixels = 1.0f;

  /// The hull bar's height above the ship, and the shield bar's above that.
  float barOffsetUpPixels = 24.0f;
  float barGapPixels = 5.0f;

  /*
   * The puck: a fixed mark on screen at the point the order was given.
   *
   * **A size, not a floor, and that is the correction.** It was the formation's
   * extent clamped up to a minimum, which is defensible for a compact Claw and
   * badly wrong for a Line: the extent of a line is *half its length*, so a
   * circle drawn at that radius circumscribes a formation it barely touches at
   * two points, and eleven ships put a green ellipse across the whole viewport.
   *
   * The footprint is the **station ticks** -- one per ship, at the exact place
   * that ship is going (ADR-005 §3). They already say what shape the fleet will
   * take and how much room it needs, and they say it truthfully for every
   * formation. The ring's job is the other half of `puck-and-wheel.png` §2: it
   * marks where the player pointed. A shape that hugs the formation is the
   * print's enclosing outline, and that is a polyline -- draw list *(B)*, with
   * the Ui pass.
   */
  float puckRadiusPixels = 22.0f;

  /// One tick per ship at the station it is being sent to (ADR-005 §3: "one
  /// station tick per ship, never decorative"). Small, because at fleet scale
  /// there are as many of these as there are ships.
  float stationRadiusPixels = 5.0f;

  /// Dashes around a footprint the authority has not confirmed. The print's
  /// pending and rejected ghosts are both dashed and its accepted one is solid,
  /// which is the whole visual difference between a promise and a fact.
  std::uint16_t ghostDashCount = 24;

  /// The STALE marker (icon sheet §4): a dashed screen-facing ring, drawn on
  /// *every* frozen ship rather than only selected ones -- a ship holding
  /// position because the feed stopped must never read as a ship that chose to
  /// stop, and the player has not necessarily selected the one that froze.
  float staleMarkerRadiusPixels = 11.0f;
  std::uint16_t staleMarkerDashCount = 8;

  /*
   * Colours, packed as `OverlayMark::colourRgba` says: 8:8:8:8 with **r in the
   * low byte**, which is what `DXGI_FORMAT_R8G8B8A8_UNORM` reads out of a
   * little-endian `uint32`.
   *
   * Two of these used to be written the other way round -- 0xAARRGGBB, out of
   * habit -- so the selection ring rendered amber and the shield bar blue.
   * Neither had ever been looked at: S8b shipped the marks and said in writing
   * that its visual half was outstanding because a depth state needs a GPU, and
   * a swapped colour needs exactly the same frame to notice. The green hull bar
   * survived only because 0x50e050 is a palindrome in bytes.
   */
  /*
   * The selection ring is the **own-fleet phosphor**, not the allied cyan it
   * shipped as: allied cyan is reserved for allied assets and shield fills
   * (icon sheet §7), and a player reading colour fast would parse their own
   * selection as someone else's ship. The defaults below are stand-ins for a
   * caller with no palette; `ClientApp::Initialise` overwrites every ghost and
   * selection colour here from the resolved `HudPalette`, so the ring, the
   * drag box and the roster's selected chip are one colour from one table.
   */
  std::uint32_t ringColourRgba = 0xff3eff7cu;      // Selection: the own-fleet phosphor.
  std::uint32_t staleRingColourRgba = 0xff4080ffu; // A frozen ship's ring, dimmer and warmer: (255, 128, 64).

  /*
   * The bars band exactly as the roster strips do -- healthy, worn, low at
   * `HULL_GAUGE_WORN_BELOW`/`HULL_GAUGE_LOW_BELOW` -- so a wing's row and its
   * ships' bars cannot disagree about what worn means. Shield is always the
   * allied cyan, the same fill the roster draws.
   */
  std::uint32_t hullColourRgba = 0xff3eff7cu;     // Healthy: the phosphor.
  std::uint32_t hullWornColourRgba = 0xff00b0ffu; // Worn: the caution amber.
  std::uint32_t hullLowColourRgba = 0xff4a5affu;  // Low: the hostile red.
  std::uint32_t shieldColourRgba = 0xffffe14du;   // The allied cyan shield fill.

  std::uint32_t ghostPendingColourRgba = 0xa03eff7cu;  // The selection phosphor, translucent: a promise.
  std::uint32_t ghostUnderWayColourRgba = 0xff3eff7cu; // The same phosphor, opaque: the world agreed.
  std::uint32_t ghostRejectedColourRgba = 0xff4a5affu; // The hostile red. Refused, and on its way back.
};

/*
 * The marks, in draw order.
 *
 * `ringCount` splits the array: `[0, ringCount)` are plane-lying and
 * depth-tested, `[ringCount, size())` are screen-facing and are not. The pass
 * issues two instanced draws over the two halves, so the split has to be a
 * contiguous range rather than a per-mark flag.
 */
struct OverlayMarkList
{
  std::vector<OverlayMark> marks;
  std::uint32_t ringCount = 0;

  void Clear() noexcept
  {
    marks.clear();
    ringCount = 0;
  }

  [[nodiscard]] std::uint32_t BarCount() const noexcept { return static_cast<std::uint32_t>(marks.size()) - ringCount; }
};

/*
 * Builds the marks for one frame's selection.
 *
 * Only selected ships get marks. Rings for everything would make the plane
 * unreadable at fleet scale, and bars for everything is the same problem with
 * more pixels -- the icon sheet's channel separation is about *what a player
 * asked to look at* (ADR-006 §7).
 *
 * `_metresPerPixel` converts the ring's screen-space floor into plane metres;
 * it is `IsoCamera::MetresPerPixel()`, unforeshortened, because a ring's radius
 * is measured along the plane's screen-right axis where there is no
 * foreshortening to undo.
 *
 * Selected ids that are not in `_entities` are skipped rather than drawn at the
 * origin. `Selection::Retain` should have removed them the same frame; this is
 * the second line of defence, because a ring at (0,0) is a bug that looks like
 * a feature.
 */
void BuildOverlayMarks(std::span<const SceneEntity> _entities, std::span<const std::uint16_t> _selectedIds,
                       const OverlayTuning& _tuning, float _metresPerPixel, OverlayMarkList& _outMarks);

/*
 * Appends the ghosts' plane-lying marks: one footprint ring per ghost and one
 * tick per station inside it (`puck-and-wheel.png` §4).
 *
 * **Inserted rather than appended.** Both kinds lie on the plane, so they
 * belong in `[0, ringCount)` -- the half the pass draws depth-tested -- and
 * pushing them onto the end would put a ring in the bars' draw, where it would
 * never be occluded by the hull it is lying under. They go in at `ringCount`,
 * which moves the bars along; a frame has a handful of each, and the move is
 * cheaper than the alternative of building the two halves in a fixed order and
 * making `BuildOverlayMarks` know that ghosts exist.
 *
 * Call after `BuildOverlayMarks`, which clears the list.
 *
 * A refused ghost is drawn part-way back toward the fleet, by
 * `OrderGhostList::BounceFraction` -- hence `_nowSeconds`, which is the same
 * clock the list was advanced with.
 */
void BuildGhostMarks(std::span<const OrderGhost> _ghosts, const OverlayTuning& _tuning, float _metresPerPixel,
                     double _nowSeconds, OverlayMarkList& _outMarks);

} // namespace Neuron
