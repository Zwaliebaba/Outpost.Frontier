#include "pch.h"

#include "SystemScreen.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

/// Keeps a rect inside the viewport rather than trusting the arithmetic, which
/// is `MapScreen.cpp`'s habit and for the same reason: a zone that went negative
/// at a small window would draw off-screen instead of failing.
[[nodiscard]] UiRect Clamped(float _x, float _y, float _width, float _height) noexcept
{
  return UiRect{_x, _y, std::max(0.0f, _width), std::max(0.0f, _height)};
}

/*
 * Where slot `_slot` of `_count` sits on its ring, in radians.
 *
 * Measured from straight up and going clockwise, which is a decision rather
 * than a default: the print's rings read like a clock face, and a fan that
 * started at three o'clock would put the first anchor of every system where a
 * reader's eye does not begin.
 *
 * `_count` of zero cannot happen from `PlaceSystemView` -- a ring with no slots
 * places nothing -- and is guarded anyway, because a divide here would be a
 * crash rather than a wrong picture.
 */
[[nodiscard]] float SlotAngle(std::uint16_t _slot, std::uint32_t _count) noexcept
{
  if (_count == 0)
  {
    return 0.0f;
  }
  constexpr float TWO_PI = 6.283185307179586f;
  return TWO_PI * static_cast<float>(_slot) / static_cast<float>(_count) - TWO_PI * 0.25f;
}

} // namespace

SystemScreenLayout ResolveSystemScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight, float _scale,
                                       const SystemScreenTuning& _tuning) noexcept
{
  SystemScreenLayout layout;
  const float width = static_cast<float>(_viewportWidth);
  const float height = static_cast<float>(_viewportHeight);
  layout.viewport = UiRect{0.0f, 0.0f, width, height};

  const float scale = _scale > 0.0f ? _scale : 1.0f;
  const float pad = _tuning.panelPadding * scale;

  /*
   * The top bar takes the floor, because the back chip lives in it and a chip
   * is pressable. Everything else on this screen may scale freely; a target may
   * not (ADR-020's 48 px floor).
   */
  const float barHeight = std::min(TargetHeightPixels(_tuning.topBarHeight, scale), height);
  layout.topBar = Clamped(0.0f, 0.0f, width, barHeight);

  const float chipWidth = std::min(160.0f * scale, width * 0.4f);
  layout.backChip = Clamped(pad, 0.0f, chipWidth, barHeight);
  layout.title = Clamped(layout.backChip.Right() + pad, 0.0f, std::max(0.0f, width * 0.4f), barHeight);
  layout.badge = Clamped(layout.title.Right() + pad, 0.0f,
                         std::max(0.0f, width - layout.title.Right() - pad * 2.0f), barHeight);

  /*
   * The panel, then the disc in what is left. That order is the same one the
   * map's rail and panel take, and for the same reason: the chrome's width is a
   * decision and the content's is a remainder.
   */
  const float panelWidth = std::min(_tuning.panelWidth * scale, width * 0.4f);
  layout.panel = Clamped(width - panelWidth, barHeight, panelWidth, std::max(0.0f, height - barHeight));

  const float headHeight = std::min(TargetHeightPixels(_tuning.topBarHeight, scale), layout.panel.height);
  layout.panelHead = Clamped(layout.panel.x, layout.panel.y, layout.panel.width, headHeight);

  /*
   * The two verbs, foot-anchored, WARP above DOCK -- the same stacking the map
   * gives SET DESTINATION and ADD WAYPOINT, and the same reason: the verb a
   * player reaches for most sits where their thumb already is, and the order
   * does not change when one of them is dark.
   */
  const float verbHeight = std::min(TargetHeightPixels(52.0f, scale), layout.panel.height);
  const float gap = 8.0f * scale;
  const float panelFloor = layout.panel.Bottom() - pad;
  layout.dockHere = Clamped(layout.panel.x + pad, panelFloor - verbHeight,
                            std::max(0.0f, layout.panel.width - pad * 2.0f), verbHeight);
  layout.warpHere = Clamped(layout.dockHere.x, layout.dockHere.y - gap - verbHeight, layout.dockHere.width,
                            verbHeight);
  layout.panelBody = Clamped(layout.dockHere.x, layout.panelHead.Bottom(), layout.dockHere.width,
                             std::max(0.0f, layout.warpHere.y - gap - layout.panelHead.Bottom()));

  const float footerHeight = std::min(TargetHeightPixels(28.0f, scale), std::max(0.0f, height - barHeight));
  layout.footer = Clamped(0.0f, height - footerHeight, width - panelWidth, footerHeight);

  layout.disc = Clamped(0.0f, barHeight, std::max(0.0f, width - panelWidth),
                        std::max(0.0f, height - barHeight - footerHeight));
  layout.centreX = layout.disc.x + layout.disc.width * 0.5f;
  layout.centreY = layout.disc.y + layout.disc.height * 0.5f;

  /*
   * The disc is inscribed in its zone, so a wide window gives a round system
   * rather than an oval one. `layout is legibility` cuts both ways: a system
   * stretched to fill a 21:9 monitor would imply an extent it does not have.
   */
  layout.radius = std::max(0.0f, std::min(layout.disc.width, layout.disc.height) * 0.5f);
  return layout;
}

float SystemRingRadius(std::uint16_t _ring, std::uint16_t _ringCount, const SystemScreenLayout& _layout, float _scale,
                       const SystemScreenTuning& _tuning) noexcept
{
  const float scale = _scale > 0.0f ? _scale : 1.0f;
  const float first = _tuning.firstRingRadius * scale;
  const float outer = _layout.radius * _tuning.outerRingFraction;

  /*
   * One ring sits at `first` rather than at the rim.
   *
   * A lone ring pushed to the edge is a system that looks empty in the middle,
   * which is the opposite of what the print draws -- and it is the common case
   * at the smallest systems, which have five anchors.
   */
  if (_ringCount <= 1 || outer <= first)
  {
    return std::min(first, std::max(0.0f, outer));
  }

  /*
   * Otherwise evenly between the first and the outer, so a system that grows a
   * ring gets tighter rather than clipped. That is what makes ADR-016 §9b.1's
   * overflow safe at 15 anchors, which is the committed bake's worst case.
   */
  const float step = (outer - first) / static_cast<float>(_ringCount - 1);
  const std::uint16_t ring = std::min(_ring, static_cast<std::uint16_t>(_ringCount - 1));
  return first + step * static_cast<float>(ring);
}

SystemPlacement PlaceSystemView(const SystemViewData& _system, const SystemScreenLayout& _layout, float _scale,
                                const SystemScreenTuning& _tuning, std::span<SystemAnchorRect> _outAnchors,
                                std::span<SystemBackdropRect> _outBackdrop) noexcept
{
  SystemPlacement placed;
  const float scale = _scale > 0.0f ? _scale : 1.0f;

  /*
   * How many slots each ring has, counted before anything is placed.
   *
   * A slot's *angle* is a function of how many share its ring, so nothing can
   * be placed until every ring's population is known -- placing as we walk
   * would fan the first anchor as though it were alone.
   */
  std::uint32_t ringSlots[MAX_SYSTEM_RINGS] = {};
  const std::uint16_t ringCount = std::max<std::uint16_t>(_system.ringCount, 1);
  for (const SystemAnchor& anchor : _system.anchors)
  {
    if (anchor.ring < MAX_SYSTEM_RINGS)
    {
      ++ringSlots[anchor.ring];
    }
  }

  /*
   * **Both lists, into one count.** `SystemAnchor::slot` and
   * `SystemBackdrop::slot` share a ring's slot space, so a ring fanned by its
   * anchors alone would put every backdrop body exactly on top of an anchor --
   * the two lists exist to separate what is *pressable*, not to separate what
   * occupies the ring.
   */
  for (const SystemBackdrop& body : _system.backdrop)
  {
    if (body.ring < MAX_SYSTEM_RINGS)
    {
      ++ringSlots[body.ring];
    }
  }

  const float reticle = _tuning.anchorRadius * scale;
  const float labelGap = _tuning.labelGap * scale;
  const float detailGap = _tuning.detailGap * scale;
  const float badgeWidth = _tuning.countBadgeWidth * scale;
  const float badgeHeight = _tuning.countBadgeHeight * scale;
  const float badgeGap = _tuning.countBadgeGap * scale;
  const float lineHeight = 14.0f * scale;

  for (std::uint32_t index = 0; index < _system.anchors.size(); ++index)
  {
    if (placed.anchors >= _outAnchors.size())
    {
      break;
    }
    const SystemAnchor& anchor = _system.anchors[index];
    const std::uint32_t slots = anchor.ring < MAX_SYSTEM_RINGS ? ringSlots[anchor.ring] : 0;
    const float radius = SystemRingRadius(anchor.ring, ringCount, _layout, scale, _tuning);
    const float angle = SlotAngle(anchor.slot, slots);

    SystemAnchorRect& out = _outAnchors[placed.anchors];
    out = SystemAnchorRect{};
    out.anchor = index;
    out.x = _layout.centreX + std::cos(angle) * radius;
    out.y = _layout.centreY + std::sin(angle) * radius;
    out.reticle = UiRect{out.x - reticle, out.y - reticle, reticle * 2.0f, reticle * 2.0f};
    out.label = UiRect{out.x - badgeWidth, out.reticle.Bottom() + labelGap, badgeWidth * 2.0f, lineHeight};

    // The second line only exists when the game wrote one -- §9b.4's far-side
    // name for a gate, and nothing for a planet.
    if (anchor.detail != nullptr)
    {
      out.detail = UiRect{out.label.x, out.label.Bottom() + detailGap, out.label.width, lineHeight};
    }

    /*
     * The count chip, up and right of the reticle -- the corner the label does
     * not use, which is `BuildMapMarkerRects`' reasoning one surface along.
     * Collapsed when the player has nothing here, so a caller that forgot to
     * check draws nothing rather than drawing a zero.
     */
    if (anchor.shipCount > 0 || anchor.dockedCount > 0)
    {
      out.countBadge = UiRect{out.x + badgeGap, out.y - reticle - badgeHeight - badgeGap, badgeWidth, badgeHeight};
    }
    ++placed.anchors;
  }

  for (std::uint32_t index = 0; index < _system.backdrop.size(); ++index)
  {
    if (placed.backdrop >= _outBackdrop.size())
    {
      break;
    }
    const SystemBackdrop& body = _system.backdrop[index];

    // The same shared count the anchors were fanned by -- see above.
    const std::uint32_t slots = body.ring < MAX_SYSTEM_RINGS ? ringSlots[body.ring] : 0;
    const float radius = SystemRingRadius(body.ring, ringCount, _layout, scale, _tuning);
    const float angle = SlotAngle(body.slot, slots);

    SystemBackdropRect& out = _outBackdrop[placed.backdrop];
    out.x = _layout.centreX + std::cos(angle) * radius;
    out.y = _layout.centreY + std::sin(angle) * radius;
    out.radius = _tuning.backdropRadius * scale * (static_cast<float>(body.scale) / 128.0f);
    ++placed.backdrop;
  }
  return placed;
}

const SystemAnchorRect* HitSystemAnchor(std::span<const SystemAnchorRect> _placed, float _x, float _y, float _scale,
                                        const SystemScreenTuning& _tuning) noexcept
{
  const float scale = _scale > 0.0f ? _scale : 1.0f;

  /*
   * The target is the floored one, not the reticle.
   *
   * A 13 px reticle at 1x is a quarter of what a finger needs, so the pressable
   * area is `TargetHeightPixels` and the drawn mark is smaller -- which is the
   * 48 px floor doing its job rather than the print's geometry being wrong.
   */
  const float target = TargetHeightPixels(_tuning.anchorRadius * 2.0f, scale) * 0.5f;
  const float targetSquared = target * target;

  /*
   * Nearest, never first. Two anchors on neighbouring slots can both be inside
   * one floored target, and first-found would let the order the game happened
   * to write them in decide which one a tap selects.
   */
  const SystemAnchorRect* best = nullptr;
  float bestSquared = 0.0f;
  for (const SystemAnchorRect& rect : _placed)
  {
    const float dx = rect.x - _x;
    const float dy = rect.y - _y;
    const float distance = dx * dx + dy * dy;
    if (distance > targetSquared)
    {
      continue;
    }
    if (best == nullptr || distance < bestSquared)
    {
      best = &rect;
      bestSquared = distance;
    }
  }
  return best;
}

SystemActionHit HitSystemAction(const SystemScreenLayout& _layout, bool _canWarp, bool _canDock, float _x,
                                float _y) noexcept
{
  /*
   * Two gates rather than one, for `HitMapAction`'s reason: the verbs ask
   * different questions. A fleet can warp to a planet it cannot dock at, and
   * one flag for both would offer DOCK on open space.
   */
  if (_canWarp && _layout.warpHere.Contains(_x, _y))
  {
    return SystemActionHit::WarpHere;
  }
  if (_canDock && _layout.dockHere.Contains(_x, _y))
  {
    return SystemActionHit::DockHere;
  }
  return SystemActionHit::None;
}

} // namespace Neuron
