#pragma once

#include <cstdint>
#include <string_view>

/*
 * The HUD's colours (`tactical-hud.png`, `tactical-icon-system.png` §7).
 *
 * One struct, selected once at boot by the `client.ui.palette` config string,
 * and every colour the Ui pass draws resolves through it -- after this header
 * landed, a packed colour literal in `BuildHud` is a defect by definition. The
 * settings sheet's colour-vision palettes are the reason it is a runtime value
 * rather than a set of constants: swapping the table has to be a config edit,
 * because the icon sheet promises that flag *geometry* carries what colour
 * carries and a palette swap is therefore a swap rather than a redesign.
 *
 * **Packed r-in-the-low-byte (0xAABBGGRR), straight alpha**, the same as
 * `OverlayMark::colourRgba` and for the same reason: the word is read by
 * `DXGI_FORMAT_R8G8B8A8_UNORM` out of little-endian memory.
 *
 * **The chrome ramp is a hierarchy without a hue change.** Seven steps of one
 * green, and the steps are the design: a HUD that said everything at one
 * brightness would say nothing. The semantic colours each pair with a glyph or
 * a geometry change wherever they appear -- colour is never the only signal
 * (icon sheet §3, the whole argument for ring-based flags).
 *
 * Not in `UiTuning`, because a colour is not a size: the scale control retunes
 * sizes and the palette string retunes these, and the two must not travel
 * together.
 */

namespace Neuron
{

struct HudPalette
{
  // The chrome ramp -- hierarchy without hue. Do not collapse steps.
  std::uint32_t phosphor = 0xFF3EFF7Cu;      // #7CFF3E - default interactive text, own-fleet accent.
  std::uint32_t phosphorHot = 0xFF9EFFC9u;   // #C9FF9E - selected / focused / primary value.
  std::uint32_t phosphorBody = 0xFF798C7Du;  // #7D8C79 - descriptive text.
  std::uint32_t phosphorDim = 0xFF577A5Du;   // #5D7A57 - secondary label, inactive item.
  std::uint32_t phosphorLabel = 0xFF496A4Eu; // #4E6A49 - section label, unit.
  std::uint32_t phosphorGhost = 0xFF375A3Du; // #3D5A37 - separators, footer.
  std::uint32_t phosphorDead = 0xFF2A4A2Fu;  // #2F4A2A - disabled / illegal.

  // Semantic. Each carries a glyph or geometry change wherever it is used.
  std::uint32_t hostile = 0xFF4A5AFFu;  // #FF5A4A - alerts, the low hull band.
  std::uint32_t critical = 0xFF6F2DFFu; // #FF2D6F - the centre-top critical toast.
  std::uint32_t caution = 0xFF00B0FFu;  // #FFB000 - pending, queue chip, urgent toast.
  std::uint32_t allied = 0xFFFFE14Du;   // #4DE1FF - shield fill.
  std::uint32_t neutral = 0xFF8BA08Fu;  // #8FA08B - stale / empty states.

  /*
   * Surfaces and lines. The panel ground is deliberately translucent: the world
   * renders full-bleed beneath the chrome, and the 12% that reads through is
   * what keeps the HUD a border on a view rather than a frame around a hole.
   */
  std::uint32_t panel = 0xE0050804u;        // rgb(4,8,5) @ 0.88.
  std::uint32_t borderStrong = 0x4D3EFF7Cu; // Panel edges against the world.
  std::uint32_t border = 0x383EFF7Cu;       // Chips, buttons, toast frames.
  std::uint32_t rule = 0x243EFF7Cu;         // Row separators; the selected-chip fill.
  std::uint32_t chipBg = 0x8C060C06u;       // rgb(6,12,6) @ 0.55 - chip / slot ground.
  std::uint32_t trackHull = 0xFF0F2216u;    // #16220F - what an empty hull strip looks like.
  std::uint32_t trackShield = 0xFF201A0Du;  // #0D1A20 - and an empty shield strip.
};

/*
 * The table the config string names.
 *
 * "default" is the print's phosphor table above; the settings sheet's two
 * colour-vision palettes join this switch when they are authored. An unknown
 * name resolves to the default rather than failing -- a client that will not
 * start because a palette is mistyped would be the wrong failure, the same
 * judgement ADR-006 makes for a nebula block that describes no field.
 */
[[nodiscard]] constexpr HudPalette ResolveHudPalette(std::string_view _name) noexcept
{
  (void)_name; // One table so far; the name exists so config can already say it.
  return HudPalette{};
}

/// A colour at a different opacity, for the treatments the prints specify as
/// "half alpha" -- spelled as an operation on a palette entry rather than as a
/// second literal, so the entry stays the single source of the hue.
[[nodiscard]] constexpr std::uint32_t WithAlpha(std::uint32_t _colourRgba, std::uint8_t _alpha) noexcept
{
  return (_colourRgba & 0x00FFFFFFu) | (static_cast<std::uint32_t>(_alpha) << 24);
}

[[nodiscard]] constexpr std::uint32_t AtHalfAlpha(std::uint32_t _colourRgba) noexcept
{
  return WithAlpha(_colourRgba, static_cast<std::uint8_t>((_colourRgba >> 24) / 2));
}

/*
 * The hull strip's fill by gauge, on `RosterRow`'s own 0-255 scale.
 *
 * Three bands rather than a gradient, because the strip is a reading and not a
 * picture: healthy is the chrome's own green, worn is the caution amber, and
 * low is the hostile red -- the same three the world-space bars will adopt when
 * the overlay migrates. The thresholds are 70% and 40% of 255.
 */
[[nodiscard]] constexpr std::uint32_t HullGaugeFill(const HudPalette& _palette, std::uint8_t _gauge) noexcept
{
  if (_gauge >= 179)
  {
    return _palette.phosphor;
  }
  return _gauge >= 102 ? _palette.caution : _palette.hostile;
}

} // namespace Neuron
