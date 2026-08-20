#pragma once

#include "Universe.h"

#include <array>
#include <cstdint>

/*
 * Angles, in integers (ADR-016 §2's integer-only rule).
 *
 * A quarter wave of sine at 2^30, sampled 64 times, mirrored into 256 steps
 * around the circle. Everything that places something at a bearing uses this
 * table, because content is compared byte for byte across compilers (U1's
 * accept) and `std::sin` is not a promise any two of them make identically.
 *
 * 256 steps is 1.4 degrees, which is finer than anything here needs: an orbit's
 * bearing is flavour, and an anchor's warp-in bearing is a direction to stand
 * off in, not a tolerance to hit.
 *
 * **This lived in `UniverseGen.cpp` until E1b**, where it was correct and
 * private. It moved because a second consumer arrived that is not the bake:
 * a site's bearing is re-derived every epoch at *runtime* (ADR-024 §3a), on
 * both halves, so the table stopped being the generator's business alone. The
 * alternative was a second copy of a 65-entry table that had to stay equal to
 * the first by everyone remembering, which is the kind of duplication that is
 * only ever discovered by two machines disagreeing about where a field is.
 */

namespace Game
{

inline constexpr std::int32_t SIN_SCALE_BITS = 30;
inline constexpr std::uint32_t ANGLE_STEPS = 256;

/// Turns16 is the wire's angular unit (ADR-004): a full turn in 65,536 steps.
/// A bearing step converts exactly, because 65,536 / 256 is a whole number --
/// which is why the table has 256 entries rather than a rounder-looking count.
inline constexpr std::uint32_t TURNS16_PER_ANGLE_STEP = 65536u / ANGLE_STEPS;

inline constexpr std::array<std::int32_t, 65> SIN_QUARTER = {
    0,          26350943,   52686014,   78989349,   105245103,  131437462,  157550647,  183568930,
    209476638,  235258165,  260897982,  286380643,  311690799,  336813204,  361732726,  386434353,
    410903207,  435124548,  459083786,  482766489,  506158392,  529245404,  552013618,  574449320,
    596538995,  618269338,  639627258,  660599890,  681174602,  701339000,  721080937,  740388522,
    759250125,  777654384,  795590213,  813046808,  830013654,  846480531,  862437520,  877875009,
    892783698,  907154608,  920979082,  934248793,  946955747,  959092290,  970651112,  981625251,
    992008094,  1001793390, 1010975242, 1019548121, 1027506862, 1034846671, 1041563127, 1047652185,
    1053110176, 1057933813, 1062120190, 1065666786, 1068571464, 1070832474, 1072448455, 1073418433,
    1073741824};

/// sin(2*pi*_step/256), scaled by 2^30.
[[nodiscard]] inline std::int64_t SinStep(std::uint32_t _step) noexcept
{
  const std::uint32_t step = _step % ANGLE_STEPS;
  const std::uint32_t quadrant = step / 64;
  const std::uint32_t index = step % 64;
  switch (quadrant)
  {
  case 0:
    return SIN_QUARTER[index];
  case 1:
    return SIN_QUARTER[64 - index];
  case 2:
    return -SIN_QUARTER[index];
  default:
    return -SIN_QUARTER[64 - index];
  }
}

[[nodiscard]] inline std::int64_t CosStep(std::uint32_t _step) noexcept
{
  return SinStep(_step + ANGLE_STEPS / 4);
}

/// `_radius` metres at `_step`/256 of a turn, rounded to the metre. The
/// multiply is int64 throughout: a radius of 30 AU is 4.5e12, and 4.5e12 * 2^30
/// overflows nothing in 64 bits but would silently ruin 32.
[[nodiscard]] inline UniversePos PolarOffset(std::int64_t _radiusMetres, std::uint32_t _step) noexcept
{
  return UniversePos{(_radiusMetres * CosStep(_step)) >> SIN_SCALE_BITS, (_radiusMetres * SinStep(_step)) >> SIN_SCALE_BITS};
}

} // namespace Game
