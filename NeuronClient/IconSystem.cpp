#include "pch.h"

#include "IconSystem.h"

#include <algorithm>

namespace Neuron
{

std::uint8_t IconMarkersFor(const IconSubject& _subject) noexcept
{
  std::uint8_t markers = 0;

  if (_subject.selected)
  {
    markers |= ICON_MARKER_SELECTED;

    // §4: *"OFF-SCREEN -- edge chevron + range, selection only"*. Gated on the
    // selection here rather than at the draw, because a marker whose rule lives
    // in the composer is a rule the next composer has to be told about.
    if (_subject.offScreen)
    {
      markers |= ICON_MARKER_OFF_SCREEN;
    }
  }

  if (_subject.stale)
  {
    markers |= ICON_MARKER_STALE;
  }

  /*
   * The sweep and its completion, which are two marks and not one growing one:
   * §4 draws LOCKING as a sweep whose arc *is* the progress and LOCKED as four
   * corners that snap in on completion. A lock at exactly 1 is finished, so it
   * is the corners and not a full sweep -- otherwise the last frame of the
   * sweep and the first frame of the lock would both be drawn.
   */
  if (_subject.lockProgress >= 1.0f)
  {
    markers |= ICON_MARKER_LOCKED;
  }
  else if (_subject.lockProgress > 0.0f)
  {
    markers |= ICON_MARKER_LOCKING;
  }

  /*
   * And the one marker that leaks (§4, 04 §6.3).
   *
   * It reads `attackerLockComplete` and nothing else -- there is deliberately no
   * arithmetic here that could recover the sweep, because the sweep is exactly
   * what the disclosure rule withholds. A viewer who could see the attacker's
   * progress would have the free early-warning system the replication audience
   * refuses; a viewer who sees only the completion learns it at the moment the
   * attacker could already fire.
   */
  if (_subject.attackerLockComplete)
  {
    markers |= ICON_MARKER_TARGETING_ME;
  }

  return markers;
}

IconTier IconTierFor(float _longestAxisPixels, const IconTuning& _tuning) noexcept
{
  /*
   * The floor always bites first.
   *
   * A tuning whose bar-retire threshold sat *below* the legibility floor would
   * invert the middle rung -- glyphs would be merged away while their bars were
   * still being drawn -- so the two are ordered here rather than trusted to
   * arrive ordered. It costs one `max` and removes a whole class of
   * unreproducible tuning bug.
   */
  const float retire = std::max(_tuning.barRetirePixels, _tuning.legibilityFloorPixels);

  if (_longestAxisPixels >= retire)
  {
    return IconTier::Tactical;
  }
  if (_longestAxisPixels >= _tuning.legibilityFloorPixels)
  {
    return IconTier::Operational;
  }
  return IconTier::Strategic;
}

float IconLongestAxisPixels(float _silhouetteRadiusMetres, float _metresPerPixel) noexcept
{
  if (!(_metresPerPixel > 0.0f) || !(_silhouetteRadiusMetres > 0.0f))
  {
    // A hull with no stated size, or a camera with no scale. Zero reads as
    // `Strategic` one function up, which is the honest rung for a glyph nobody
    // can size: it is merged into a count rather than drawn at a guess.
    return 0.0f;
  }

  // The silhouette is a radius and a glyph is drawn across its diameter, which
  // is the one conversion in this file worth stating: `SilhouetteRadiusMetres`
  // answers "how far does this hull reach", and the plate's boxes are widths.
  return 2.0f * _silhouetteRadiusMetres / _metresPerPixel;
}

IconRing IconRingFor(IconFlag _flag) noexcept
{
  /*
   * The plate's own geometries, converted from its pixels to multiples of the
   * glyph -- it draws a 26 px cruiser inside rings of 38, 40 and 38 px.
   *
   * The claim these encode is §3's: *"Ring geometry alone separates the four
   * flag states with colour removed entirely -- swap the palette above to check
   * it."* So the three drawn rings differ from each other in at least one field
   * that is not a colour, and `IconTests` asserts exactly that rather than
   * trusting the numbers below to stay distinct through a retune.
   */
  IconRing ring;
  switch (_flag)
  {
  case IconFlag::None:
    // Nothing drawn. A zero radius rather than a flag on the struct, because a
    // ring of no size is already the whole of "there is no ring".
    return ring;

  case IconFlag::Engaged:
    // Thin, solid, and the only one that moves: the plate animates it on a
    // 1.4 s cycle, which is the shortest-lived of the three states (60 s) shown
    // as the least settled mark.
    ring.radiusPerLongestAxis = 19.0f / 26.0f;
    ring.strokePixels = 1.0f;
    ring.pulses = true;
    return ring;

  case IconFlag::Suspect:
    // Dashed, and the widest of the three -- the dash is this vocabulary's word
    // for anything unresolved, which is the same sentence `OverlayKind`'s stale
    // and status marks are written in.
    //
    // Twelve dashes is `OverlayTuning::statusMarkDashCount`, reused rather than
    // invented: both are a dashed ring at roughly glyph scale, and two dash
    // counts a player is meant to tell apart would be a channel nobody declared.
    ring.radiusPerLongestAxis = 20.0f / 26.0f;
    ring.strokePixels = 2.0f;
    ring.dashCount = 12;
    return ring;

  case IconFlag::Criminal:
    // Solid, thick, and haloed -- black then colour, so it survives being drawn
    // over a bright hull. It is the state that lasts longest (15 min) and the
    // one a player must never miss.
    ring.radiusPerLongestAxis = 19.0f / 26.0f;
    ring.strokePixels = 2.0f;
    ring.haloed = true;
    return ring;
  }
  return ring;
}

bool ResolveIcon(std::span<const IconGlyph> _table, const IconRequest& _request, float _metresPerPixel, const IconTuning& _tuning,
                 IconSpec& _outSpec) noexcept
{
  if (_request.slot == INVALID_ICON_SLOT || _request.slot >= _table.size())
  {
    // Draw nothing. `BuildScene`'s posture for a hull class with no mesh, one
    // channel over: substituting slot zero would put a glyph on screen that is
    // not the ship the server is simulating.
    return false;
  }

  const IconGlyph& glyph = _table[_request.slot];
  const float longestFraction = std::max(glyph.widthFraction, glyph.heightFraction);
  if (!(longestFraction > 0.0f))
  {
    return false; // A row with no extent. `ValidIconGlyphTable` refuses these.
  }

  IconSpec spec;
  spec.slot = _request.slot;
  spec.family = glyph.family;
  spec.headingOriented = IconHeadingOriented(glyph.family);

  spec.longestAxisPixels = IconLongestAxisPixels(_request.silhouetteRadiusMetres, _metresPerPixel);
  // The proportions applied to the size, so the longest of the two *is* the
  // stated longest axis by construction. Two multiplications rather than a
  // caller getting the normalisation right at each site is the same argument
  // `SceneEntity` makes for being one record instead of three arrays.
  spec.widthPixels = spec.longestAxisPixels * glyph.widthFraction / longestFraction;
  spec.heightPixels = spec.longestAxisPixels * glyph.heightFraction / longestFraction;

  spec.standing = _request.standing;
  spec.markers = IconMarkersFor(_request.subject);

  spec.flag = _request.flag;
  spec.drawsRing =
    _request.flag != IconFlag::None && IconDrawsFlagRing(_request.subject.selected, (spec.markers & ICON_MARKER_LOCKED) != 0);
  if (spec.drawsRing)
  {
    spec.ring = IconRingFor(_request.flag);
  }

  spec.tier = IconTierFor(spec.longestAxisPixels, _tuning);

  _outSpec = spec;
  return true;
}

} // namespace Neuron
