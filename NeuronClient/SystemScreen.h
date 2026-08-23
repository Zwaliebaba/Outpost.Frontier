#pragma once

#include "SystemView.h"
#include "UiLayout.h"

#include <cstdint>
#include <span>

/*
 * The system view's zones, its rings and its hit tests (ADR-016 §9, U6).
 *
 * `MapScreen.h` one level down, and the contrast is the interesting part: the
 * map is *projected* through a camera because it is a viewport onto 2,500
 * systems, and this is **laid out** because it is one system and everything in
 * it fits. There is no camera here, no pan and no pinch — a system that needed
 * scrolling would be a system view that had become a second map.
 *
 * **Rings are placed, never measured.** ADR-016 §9's *"the layout cannot say
 * distance, so it says time"* is load-bearing: ring *r* sits at a radius this
 * file computes from the ring *count*, so the same system is drawn the same way
 * at any window size and two systems with different real extents look alike.
 * **There is no scale bar and no metre anywhere in this header**, because a
 * ruler over a non-linear layout is a lie with authority.
 *
 * Laid out and hit-tested in one file, which is ADR-020 §5.1: the reticle a
 * player presses is the reticle they see.
 */

namespace Neuron
{

/*
 * The numbers the print's geometry rests on, at its own 1440x900.
 *
 * Everything scales through `UiLayout::scale` and every *pressable* thing goes
 * through `TargetHeightPixels`, so ADR-020's 48 px floor is enforced rather than
 * scaled — which on this screen is the binding constraint, since an anchor
 * reticle is small and there can be fifteen of them.
 */
struct SystemScreenTuning
{
  float topBarHeight = 46.0f;

  /// The panel down the right, which names the selected anchor and carries its
  /// verbs. The map's is 280; this one is narrower because it lists one place.
  float panelWidth = 260.0f;
  float panelPadding = 14.0f;

  /// The star at the centre, and the innermost ring's clearance from it.
  float starRadius = 26.0f;
  float firstRingRadius = 78.0f;

  /*
   * How much of the remaining radius the outermost ring uses.
   *
   * Under one so the outer ring's labels have somewhere to go: a reticle at the
   * very edge of the disc is a label off the edge of the screen, and §9b.2 puts
   * the site ring — the one a mining player reads most — exactly there.
   */
  float outerRingFraction = 0.86f;

  /// The reticle, and the dot a backdrop body draws. Both at 1x.
  float anchorRadius = 13.0f;
  float backdropRadius = 9.0f;

  /// Where an anchor's word goes, under its reticle, and where the second line
  /// (§9b.4's `→ KIL-7`) goes under that.
  float labelGap = 8.0f;
  float detailGap = 4.0f;

  /// The split count's chip, up and right of the reticle — `MapMarker`'s badge
  /// one surface along, and the same reasoning about which corner is free.
  float countBadgeWidth = 42.0f;
  float countBadgeHeight = 16.0f;
  float countBadgeGap = 3.0f;
};

/*
 * The screen's fixed parts.
 *
 * Three zones and a disc: the print's top bar, the panel down the right, the
 * footer line, and what is left over is where the system is drawn.
 */
struct SystemScreenLayout
{
  UiRect viewport;
  UiRect topBar;

  /// The system's name and its band badge, in the bar.
  UiRect title;
  UiRect badge;

  /// The shared `◀ TACTICAL` chip, which is this corpus's one way back.
  UiRect backChip;

  /// The selected anchor's panel: its head, the body its facts go in, and the
  /// two verbs at its foot.
  UiRect panel;
  UiRect panelHead;
  UiRect panelBody;
  UiRect warpHere;
  UiRect dockHere;

  /// The disc the system is drawn in, and its centre and radius pre-resolved --
  /// every placement below is polar, and a caller re-deriving the centre from
  /// the rect is a caller that can disagree with the hit test.
  UiRect disc;
  float centreX = 0.0f;
  float centreY = 0.0f;
  float radius = 0.0f;

  /// The count line under the disc: how many anchors and how many rings, which
  /// is this screen's version of the map's graph footer.
  UiRect footer;
};

/// Lays the screen out. Pure arithmetic on a viewport and a scale, so a test
/// asserts the print's geometry without a device.
[[nodiscard]] SystemScreenLayout ResolveSystemScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight,
                                                     float _scale, const SystemScreenTuning& _tuning) noexcept;

/*
 * Where one ring sits, as a radius.
 *
 * Ring 0 is `firstRingRadius` and the outermost is `outerRingFraction` of the
 * disc, with the rest spaced evenly between — so adding a ring moves them all
 * inward rather than pushing the outer one off the screen. That is what makes
 * §9b.1's overflow safe: a system that grows a third ring gets tighter, never
 * clipped.
 *
 * A single ring sits at `firstRingRadius` rather than at the outer edge,
 * because a lone ring pushed to the rim is a system that looks empty in the
 * middle.
 */
[[nodiscard]] float SystemRingRadius(std::uint16_t _ring, std::uint16_t _ringCount, const SystemScreenLayout& _layout,
                                     float _scale, const SystemScreenTuning& _tuning) noexcept;

/// One placed anchor: where its reticle, its word and its count chip go.
struct SystemAnchorRect
{
  /// Which anchor -- an index into the caller's list.
  std::uint32_t anchor = 0;

  /// The reticle's centre, which is what the hit test measures against and what
  /// a ring line is drawn through.
  float x = 0.0f;
  float y = 0.0f;

  UiRect reticle;
  UiRect label;
  UiRect detail;

  /// Collapsed when the anchor has none of the player's ships, so a caller that
  /// forgot to check draws nothing rather than drawing at the origin.
  UiRect countBadge;
};

/// And one placed backdrop body. No label, no badge, nothing to hit.
struct SystemBackdropRect
{
  float x = 0.0f;
  float y = 0.0f;
  float radius = 0.0f;
};

/*
 * Places every anchor and every body.
 *
 * Slots are fanned evenly around their ring **from a fixed bearing**, so the
 * same system is drawn identically in two sessions — the print's rings are
 * ordered by bake order precisely so a player learns where things are, and a
 * fan that started wherever the first slot happened to be would undo that.
 *
 * A ring is fanned by **everything on it**, anchors and backdrop together,
 * because the two lists share one slot space per ring (`SystemView.h`). The
 * split between them is about what may be *pressed*, not about what takes up
 * room on a ring -- a fan that counted only the anchors would put every
 * backdrop body exactly on top of one.
 *
 * Returns how many of each were written; a list longer than the caller's span
 * is filled to the span and the remainder dropped, which the caller is expected
 * to notice rather than to discover.
 */
struct SystemPlacement
{
  std::uint32_t anchors = 0;
  std::uint32_t backdrop = 0;
};

[[nodiscard]] SystemPlacement PlaceSystemView(const SystemViewData& _system, const SystemScreenLayout& _layout,
                                              float _scale, const SystemScreenTuning& _tuning,
                                              std::span<SystemAnchorRect> _outAnchors,
                                              std::span<SystemBackdropRect> _outBackdrop) noexcept;

/*
 * Which anchor a press landed on, or null.
 *
 * The **nearest** within a target, never the first, for `HitMapNode`'s reason
 * exactly: the 48 px floor is wider than a 13 px reticle, so two anchors on
 * neighbouring slots can both be under one finger and first-found would let
 * bake order decide which one a tap selects.
 *
 * Backdrop is not offered because it is not passed: *"the player never clicks
 * something and is told no"* is enforced by this function having no way to
 * return one.
 */
[[nodiscard]] const SystemAnchorRect* HitSystemAnchor(std::span<const SystemAnchorRect> _placed, float _x, float _y,
                                                      float _scale, const SystemScreenTuning& _tuning) noexcept;

/// Which of the panel's verbs a press landed on, or `None`. Both are drawn
/// either way and refused here, per `station-screen.png` §2.
enum class SystemActionHit : std::uint8_t
{
  None = 0,
  WarpHere,
  DockHere
};

[[nodiscard]] SystemActionHit HitSystemAction(const SystemScreenLayout& _layout, bool _canWarp, bool _canDock, float _x,
                                              float _y) noexcept;

} // namespace Neuron
