#include "pch.h"

#include "ContrastAudit.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

/// One sRGB byte to its linear value, per WCAG 2.x. The branch is the standard's
/// own: the curve has a linear toe near black, and dropping it would overstate
/// the darkest grounds -- which are exactly the ones this file measures against.
[[nodiscard]] float Linearise(std::uint32_t _byte) noexcept
{
  const float channel = static_cast<float>(_byte) / 255.0f;
  return channel <= 0.04045f ? channel / 12.92f : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

/// The three weights, once. They are the standard's and not a preference: green
/// carries most of perceived brightness, which is why a green-on-black chrome
/// ramp reads as high contrast and a blue-on-black one does not.
[[nodiscard]] float Weigh(float _red, float _green, float _blue) noexcept
{
  return 0.2126f * _red + 0.7152f * _green + 0.0722f * _blue;
}

} // namespace

const char* StandingColourName(StandingColour _standing) noexcept
{
  switch (_standing)
  {
  case StandingColour::Allied:
    return "Allied";
  case StandingColour::Neutral:
    return "Neutral";
  case StandingColour::Hostile:
    return "Hostile";
  case StandingColour::Own:
  default:
    return "Own";
  }
}

float RelativeLuminance(std::uint32_t _colourRgba) noexcept
{
  // Red is the low byte and alpha the high one, which is the packing
  // `GroundOf` already reads and the one the atlas expects.
  return Weigh(Linearise(_colourRgba & 0xFFu), Linearise((_colourRgba >> 8) & 0xFFu),
               Linearise((_colourRgba >> 16) & 0xFFu));
}

float RelativeLuminance(const ClearColour& _linearColour) noexcept
{
  // No linearisation: `SpaceClearColour` states its values in linear space
  // because the render target is `_SRGB` and the hardware encodes them. Running
  // them through the curve a second time would report the void as darker than
  // it is drawn, which would flatter every ratio here.
  return Weigh(_linearColour.red, _linearColour.green, _linearColour.blue);
}

float ContrastRatio(float _luminanceA, float _luminanceB) noexcept
{
  const float lighter = std::max(_luminanceA, _luminanceB);
  const float darker = std::min(_luminanceA, _luminanceB);
  return (lighter + 0.05f) / (darker + 0.05f);
}

std::uint32_t AuditPalette(const HudPalette& _palette, std::span<ContrastRow> _outRows) noexcept
{
  float luminance[STANDING_COLOUR_COUNT] = {};
  for (std::uint32_t index = 0; index < STANDING_COLOUR_COUNT; ++index)
  {
    luminance[index] = RelativeLuminance(StandingColourOf(_palette, static_cast<StandingColour>(index)));
  }
  const float voidLuminance = RelativeLuminance(SpaceClearColour());

  std::uint32_t written = 0;
  const auto emit = [&](ContrastRow _row) {
    if (written < _outRows.size())
    {
      _outRows[written] = _row;
      ++written;
    }
  };

  // Void first, in standing order, so the print's panel is a prefix.
  for (std::uint32_t index = 0; index < STANDING_COLOUR_COUNT; ++index)
  {
    ContrastRow row;
    row.first = static_cast<StandingColour>(index);
    row.againstVoid = true;
    row.ratio = ContrastRatio(luminance[index], voidLuminance);
    emit(row);
  }

  // Then every pair once, as readings rather than verdicts (see the header).
  // `second` starts past `first` because separation is symmetric, and listing
  // both directions would double a table nobody is comparing against anything.
  for (std::uint32_t first = 0; first < STANDING_COLOUR_COUNT; ++first)
  {
    for (std::uint32_t second = first + 1; second < STANDING_COLOUR_COUNT; ++second)
    {
      ContrastRow row;
      row.first = static_cast<StandingColour>(first);
      row.second = static_cast<StandingColour>(second);
      row.againstVoid = false;
      row.ratio = ContrastRatio(luminance[first], luminance[second]);
      emit(row);
    }
  }
  return written;
}

float WorstGroundContrast(const HudPalette& _palette) noexcept
{
  ContrastRow rows[CONTRAST_ROW_COUNT] = {};
  const std::uint32_t count = AuditPalette(_palette, rows);

  float worst = 0.0f;
  bool seen = false;
  for (std::uint32_t index = 0; index < count; ++index)
  {
    if (!rows[index].Floored())
    {
      continue;
    }
    worst = seen ? std::min(worst, rows[index].ratio) : rows[index].ratio;
    seen = true;
  }
  return worst;
}

float ClosestPairSeparation(const HudPalette& _palette) noexcept
{
  ContrastRow rows[CONTRAST_ROW_COUNT] = {};
  const std::uint32_t count = AuditPalette(_palette, rows);

  float closest = 0.0f;
  bool seen = false;
  for (std::uint32_t index = 0; index < count; ++index)
  {
    if (rows[index].Floored())
    {
      continue;
    }
    closest = seen ? std::min(closest, rows[index].ratio) : rows[index].ratio;
    seen = true;
  }
  return closest;
}

} // namespace Neuron
