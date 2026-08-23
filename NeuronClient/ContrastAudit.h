#pragma once

#include "ClearColour.h"
#include "HudPalette.h"

#include <cstdint>
#include <span>

/*
 * Whether a palette's glyph colours can actually be read (`settings.png` §1,
 * ADR-020 §8).
 *
 * The print states the rule and the reason in one sentence: *"Floor is 4.5:1 on
 * every glyph pair. The audit runs live so a palette can never ship below it."*
 * That is the difference between an accessibility claim and an accessibility
 * proof, and it is why this is arithmetic rather than a note in a design
 * document -- a palette offered from a menu labelled "Deuteranopia" asks the
 * player to trust a word, and this is what lets the screen show them a number.
 *
 * **What a "pair" is, and why it cannot be two glyphs.** Contrast ratio is a
 * statement about *luminance*, so requiring 4.5:1 between two glyph colours and
 * 4.5:1 between each of them and the void is not merely hard, it is
 * arithmetically impossible: clearing the floor against a void at 0.0037
 * forces every glyph to a luminance of at least 0.19, and clearing it between
 * two such glyphs then forces the brighter one past 1.04 -- brighter than
 * white. Two glyphs cannot satisfy both, let alone four. So the floor is read
 * here as the only reading that is satisfiable and that WCAG's metric actually
 * measures: **a glyph against the ground it is drawn on.**
 *
 * The print says the same thing one panel higher, which is what settles it:
 * *"Flag rings and hull glyphs stay distinguishable by shape in every palette
 * -- colour is never the only carrier."* Telling two glyphs apart is the icon
 * system's job and it is answered with geometry
 * (`tactical-icon-system.png` §3). Telling a glyph from the space behind it is
 * a contrast question, and that is this file.
 *
 * Pair readings are still measured and still reported -- see `ContrastRow` --
 * because a pair at 1.02:1 is worth knowing about. It does not fail anything;
 * it says that pair rests its whole weight on shape.
 *
 * **Void is `SpaceClearColour()` and nothing else.** A glyph on the tactical
 * view is drawn over the world, so the ground the audit measures against is the
 * one the frame actually clears to rather than a black this file picked. That
 * also settles a question the arithmetic would otherwise have: those values are
 * *linear* already (the RTV is `_SRGB`, so the hardware encodes them), which is
 * the space relative luminance is defined in.
 *
 * The nebula draws over the void and is brighter than it, so a glyph that
 * clears the floor here can still sit on a lit backdrop. That is not a hole:
 * a floor measured against the darkest ground is the one that binds, and the
 * brighter case is again what geometry answers.
 *
 * **Device-free**, like `ClearColour` beside it and for the same reason: this is
 * presentation maths, so it is testable without a window, a device, or a palette
 * anybody has looked at.
 */

namespace Neuron
{

/*
 * The floor, from `settings.png` §1, applied to a glyph against its ground.
 *
 * WCAG 2.x's AA threshold for normal text, applied to glyphs -- the stricter
 * reading, since the standard would allow 3:1 for large ones. One number for
 * every glyph rather than a table of exceptions, because a floor with
 * exceptions is a floor nobody can check at a glance.
 */
inline constexpr float CONTRAST_FLOOR = 4.5f;

/*
 * The colours a standing glyph can be drawn in.
 *
 * Four, and not by coincidence: the wire spends exactly two bits on the
 * relationship channel (ADR-022 §8b), so four is what a viewer can ever be told
 * about a hull. These are palette entries rather than a second copy of that
 * enum -- `HudPalette` already carries the words -- and the mapping lives in
 * `StandingColourOf` so a palette swap moves all of them together.
 */
enum class StandingColour : std::uint8_t
{
  Own = 0,
  Allied = 1,
  Neutral = 2,
  Hostile = 3
};

inline constexpr std::uint32_t STANDING_COLOUR_COUNT = 4;

/// The palette entry a standing is drawn in. `Own` is the phosphor rather than
/// an entry of its own, which is the same statement `OverlayTuning` makes when
/// it takes its ring colour from there: own-fleet *is* the chrome's accent.
[[nodiscard]] constexpr std::uint32_t StandingColourOf(const HudPalette& _palette, StandingColour _standing) noexcept
{
  switch (_standing)
  {
  case StandingColour::Allied:
    return _palette.allied;
  case StandingColour::Neutral:
    return _palette.neutral;
  case StandingColour::Hostile:
    return _palette.hostile;
  case StandingColour::Own:
  default:
    return _palette.phosphor;
  }
}

/// What to call one on the screen. Never null, for the same reason
/// `OrderReasonText` is not.
[[nodiscard]] const char* StandingColourName(StandingColour _standing) noexcept;

/*
 * One measured row, of one of two kinds.
 *
 * **A ground row has a floor and a verdict**: `second` is meaningless, and
 * `ratio` is the glyph against the void. This is what "may this palette ship"
 * is decided on.
 *
 * **A pair row has neither.** `ratio` is the luminance separation between two
 * glyph colours, reported because it is worth knowing and *not* tested against
 * `CONTRAST_FLOOR` -- see the header note for why no palette could pass such a
 * test. A low number here means the pair is carried entirely by shape, which is
 * the design's stated intent rather than a defect.
 *
 * One struct with a discriminant rather than two, which is the arrangement
 * `StationCommand`'s per-verb fields already use: a member that means something
 * for one shape and nothing for the other beats two types that differ by one
 * field and have to be walked twice.
 */
struct ContrastRow
{
  StandingColour first = StandingColour::Own;
  StandingColour second = StandingColour::Allied;
  bool againstVoid = false;
  float ratio = 0.0f;

  /// Does this row have a floor to meet at all? Only a ground row does.
  [[nodiscard]] bool Floored() const noexcept { return againstVoid; }

  /// And does it meet it. Meaningless -- and deliberately not callable as a
  /// verdict -- for a pair row, which is why `Floored` is the question a
  /// caller asks first.
  [[nodiscard]] bool MeetsFloor() const noexcept { return againstVoid && ratio >= CONTRAST_FLOOR; }
};

/*
 * Every row: four glyphs against the void, then the six pairs among them.
 *
 * The print's panel draws four and this measures ten -- so the screen shows
 * what the print draws and a *test* covers the set. Sizing the audit to the
 * panel would have made the four visible rows the only ones anything checked.
 */
inline constexpr std::uint32_t CONTRAST_ROW_COUNT = STANDING_COLOUR_COUNT + 6u;

/*
 * WCAG 2.x relative luminance of a packed palette colour.
 *
 * The bytes are sRGB and are linearised here; alpha is ignored, because a glyph
 * is drawn at full opacity and a translucent one is a different question about
 * a different ground.
 */
[[nodiscard]] float RelativeLuminance(std::uint32_t _colourRgba) noexcept;

/// And of the clear colour, whose channels are already linear -- so this is the
/// weighted sum without the linearisation step, and that difference is the whole
/// reason both overloads exist rather than one taking three floats.
[[nodiscard]] float RelativeLuminance(const ClearColour& _linearColour) noexcept;

/// The ratio between two luminances, brighter over darker, offset per WCAG. 1.0
/// for a colour against itself and about 21 for white on black.
[[nodiscard]] float ContrastRatio(float _luminanceA, float _luminanceB) noexcept;

/*
 * Measures a palette and fills `_outRows`, returning how many it wrote.
 *
 * Ground rows first in standing order, then the pairs, so the print's panel is
 * a prefix of the result and the screen can draw the first four without a
 * search. Writes nothing past the span it was given, which is `UiDrawList`'s
 * contract applied to a smaller thing.
 */
std::uint32_t AuditPalette(const HudPalette& _palette, std::span<ContrastRow> _outRows) noexcept;

/*
 * The worst *ground* ratio in a palette -- the number that decides whether it
 * may ship, and the one a test asserts against `CONTRAST_FLOOR`.
 *
 * Ground rows only, deliberately. Folding the pair readings in would make this
 * return about 1.0 for every palette ever authored and would turn the one
 * number worth watching into a constant.
 */
[[nodiscard]] float WorstGroundContrast(const HudPalette& _palette) noexcept;

/// The closest two glyph colours sit in luminance. Not a verdict -- see
/// `ContrastRow` -- but the reading that says how hard the icon geometry is
/// working in this palette.
[[nodiscard]] float ClosestPairSeparation(const HudPalette& _palette) noexcept;

} // namespace Neuron
