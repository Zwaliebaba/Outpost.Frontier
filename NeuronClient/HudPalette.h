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

/// A colour at a different opacity, for the treatments the prints specify as
/// "half alpha" -- spelled as an operation on a palette entry rather than as a
/// second literal, so the entry stays the single source of the hue.
[[nodiscard]] constexpr std::uint32_t WithAlpha(std::uint32_t _colourRgba, std::uint8_t _alpha) noexcept
{
  return (_colourRgba & 0x00FFFFFFu) | (static_cast<std::uint32_t>(_alpha) << 24);
}

/*
 * A chip ground in a table's own hue.
 *
 * The default table spells its ground as a literal because it was authored
 * beside the print; every table after it derives one, so a new palette cannot
 * ship a ground that belongs to a different hue than its text. Five per cent of
 * the colour at the ground's own opacity: dark enough to read as a surface, and
 * tinted enough to be *this* table's surface rather than a neutral black that
 * would make every palette's chips identical.
 */
[[nodiscard]] constexpr std::uint32_t GroundOf(std::uint32_t _colourRgba) noexcept
{
  const std::uint32_t r = (_colourRgba & 0xFFu) / 20u;
  const std::uint32_t g = ((_colourRgba >> 8) & 0xFFu) / 20u;
  const std::uint32_t b = ((_colourRgba >> 16) & 0xFFu) / 20u;
  return 0x8Cu << 24 | b << 16 | g << 8 | r;
}

/*
 * The two colour-vision tables (`settings.png`, ADR-020 §8's token doc).
 *
 * **They are not filters over the default.** A deuteranopic table produced by
 * rotating hues would keep the default's *structure* -- five semantics that
 * differ by hue and nothing else -- which is the property that fails. Each is
 * authored: the semantics are chosen to differ in **lightness and saturation as
 * well as hue**, so they separate for a viewer who cannot use the hue at all.
 *
 * **Deuteranopia moves the chrome ramp too**, and that is the larger change:
 * own-fleet green and hostile red are the pair that collapses, so the own
 * colour becomes a blue and the whole seven-step ramp is re-anchored on it.
 * Tritanopia leaves the ramp alone -- green/red separate fine for it -- and
 * only moves the semantics that lean on blue and yellow.
 *
 * The lines, chips and grounds are **derived** from each table's own phosphor at
 * the alphas the default uses, so a table cannot drift into borders from one
 * hue and text from another. That is the single rule that keeps a palette a
 * table rather than a set of forty independent decisions.
 */
[[nodiscard]] constexpr HudPalette DeuteranopiaPalette() noexcept
{
  HudPalette table;
  // Chrome, re-anchored on #7CD4FF: own-fleet stops being green.
  table.phosphor = 0xFFFFD47Cu;      // #7CD4FF
  table.phosphorHot = 0xFFFFECC9u;   // #C9ECFF
  table.phosphorBody = 0xFF8C8679u;  // #79868C
  table.phosphorDim = 0xFF7A7057u;   // #57707A
  table.phosphorLabel = 0xFF6A6049u; // #49606A
  table.phosphorGhost = 0xFF5A5037u; // #37505A
  table.phosphorDead = 0xFF4A422Au;  // #2A424A

  table.hostile = 0xFF00B0FFu;  // #FFB000 - amber, not red.
  table.critical = 0xFFC75EFFu; // #FF5EC7
  table.caution = 0xFF66E0FFu;  // #FFE066
  table.allied = 0xFFFF8CB9u;   // #B98CFF
  table.neutral = 0xFF9AA39Au;  // #9AA39A

  table.trackHull = 0xFF221C0Fu;   // #0F1C22
  table.trackShield = 0xFF200D17u; // #170D20

  table.borderStrong = WithAlpha(table.phosphor, 0x4Du);
  table.border = WithAlpha(table.phosphor, 0x38u);
  table.rule = WithAlpha(table.phosphor, 0x24u);
  table.chipBg = GroundOf(table.phosphor);
  return table;
}

[[nodiscard]] constexpr HudPalette TritanopiaPalette() noexcept
{
  HudPalette table; // The chrome ramp stands: green and red separate for this one.
  table.hostile = 0xFF2D2DFFu;  // #FF2D2D
  table.critical = 0xFF0000D4u; // #D40000
  table.caution = 0xFFC07AFFu;  // #FF7AC0
  table.allied = 0xFFE09DFFu;   // #FF9DE0
  table.neutral = 0xFFA6B0A8u;  // #A8B0A6

  table.trackShield = 0xFF180D20u; // #200D18

  // Derived from the same own colour the default uses, so these are the default
  // values -- spelled as derivations anyway, because the *rule* is what a third
  // table will copy, and a table that inherited literals would copy the wrong
  // thing the first time its phosphor moved.
  table.borderStrong = WithAlpha(table.phosphor, 0x4Du);
  table.border = WithAlpha(table.phosphor, 0x38u);
  table.rule = WithAlpha(table.phosphor, 0x24u);
  return table;
}

/*
 * The table the config string names.
 *
 * An unknown name resolves to the default rather than failing -- a client that
 * will not start because a palette is mistyped would be the wrong failure, the
 * same judgement ADR-006 makes for a nebula block that describes no field.
 *
 * Resolved once, at boot. Live re-application belongs with the settings surface
 * that offers the choice; until that exists, a client picks its table at start
 * and every colour on screen comes from it.
 */
[[nodiscard]] constexpr HudPalette ResolveHudPalette(std::string_view _name) noexcept
{
  if (_name == "deuteranopia")
  {
    return DeuteranopiaPalette();
  }
  if (_name == "tritanopia")
  {
    return TritanopiaPalette();
  }
  return HudPalette{};
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
 * low is the hostile red. The thresholds are 70% and 40% of 255, and they are
 * named so the world-space bars (`BuildOverlayMarks`) band on the same numbers
 * -- a wing's roster strip and its ships' bars must not disagree about where
 * worn begins.
 */
inline constexpr std::uint8_t HULL_GAUGE_WORN_BELOW = 179;
inline constexpr std::uint8_t HULL_GAUGE_LOW_BELOW = 102;

[[nodiscard]] constexpr std::uint32_t HullGaugeFill(const HudPalette& _palette, std::uint8_t _gauge) noexcept
{
  if (_gauge >= HULL_GAUGE_WORN_BELOW)
  {
    return _palette.phosphor;
  }
  return _gauge >= HULL_GAUGE_LOW_BELOW ? _palette.caution : _palette.hostile;
}

} // namespace Neuron
