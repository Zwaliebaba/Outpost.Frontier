#include "pch.h"

#include "NebulaField.h"

#include "Hash.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{
namespace
{

/// Guards against a config that would ask for a gigabyte of haze.
constexpr std::uint32_t MAX_RESOLUTION = 2048;
constexpr std::uint32_t MAX_OCTAVES = 8;

[[nodiscard]] std::int32_t FloorToInt(float _value) noexcept
{
  const auto truncated = static_cast<std::int32_t>(_value);
  return _value < static_cast<float>(truncated) ? truncated - 1 : truncated;
}

[[nodiscard]] std::int32_t WrapIndex(std::int32_t _value, std::int32_t _period) noexcept
{
  const std::int32_t remainder = _value % _period;
  return remainder < 0 ? remainder + _period : remainder;
}

/*
 * One lattice corner's value, in [0,1).
 *
 * FNV-1a rather than a hand-rolled integer scramble: it is the tree's hash
 * already (NeuronCore/Hash.h), it is stable across builds by construction --
 * which is what makes a baked field reproducible -- and nothing here needs a
 * hash to be anything more than well-mixed.
 *
 * The wrap happens *before* hashing, which is the whole trick behind a seamless
 * tile: corner (period, y) and corner (0, y) are literally the same lattice
 * point, so the field on the two edges is identical rather than merely similar.
 */
[[nodiscard]] float LatticeValue(std::int32_t _x, std::int32_t _y, std::int32_t _period, std::uint32_t _seed) noexcept
{
  struct Key
  {
    std::int32_t x;
    std::int32_t y;
    std::uint32_t seed;
  };

  const Key key{WrapIndex(_x, _period), WrapIndex(_y, _period), _seed};
  const std::uint64_t hash = HashValue(key);

  // The top bits are the best-mixed, and 24 of them is far more resolution than
  // an 8-bit texture can carry.
  constexpr float SCALE = 1.0f / 16777216.0f;
  return static_cast<float>((hash >> 40) & 0xffffff) * SCALE;
}

/// Hermite ease. Value noise interpolated linearly shows its lattice as a grid
/// of creases; this is the cheapest thing that does not.
[[nodiscard]] float Smoothstep(float _t) noexcept
{
  return _t * _t * (3.0f - 2.0f * _t);
}

[[nodiscard]] float Lerp(float _a, float _b, float _t) noexcept
{
  return _a + (_b - _a) * _t;
}

/// Periodic value noise at one octave. `_u`/`_v` are in tile space.
[[nodiscard]] float PeriodicNoise(float _u, float _v, std::int32_t _period, std::uint32_t _seed) noexcept
{
  const float x = _u * static_cast<float>(_period);
  const float y = _v * static_cast<float>(_period);

  const std::int32_t x0 = FloorToInt(x);
  const std::int32_t y0 = FloorToInt(y);
  const float fx = Smoothstep(x - static_cast<float>(x0));
  const float fy = Smoothstep(y - static_cast<float>(y0));

  const float v00 = LatticeValue(x0, y0, _period, _seed);
  const float v10 = LatticeValue(x0 + 1, y0, _period, _seed);
  const float v01 = LatticeValue(x0, y0 + 1, _period, _seed);
  const float v11 = LatticeValue(x0 + 1, y0 + 1, _period, _seed);

  return Lerp(Lerp(v00, v10, fx), Lerp(v01, v11, fx), fy);
}

[[nodiscard]] float Saturate(float _value) noexcept
{
  return std::clamp(_value, 0.0f, 1.0f);
}

[[nodiscard]] NebulaSettings Sanitised(const NebulaSettings& _settings) noexcept
{
  NebulaSettings settings = _settings;
  settings.resolution = std::clamp(settings.resolution, 1u, MAX_RESOLUTION);
  settings.basePeriod = std::max(1u, settings.basePeriod);
  settings.octaves = std::clamp(settings.octaves, 1u, MAX_OCTAVES);
  settings.coverage = std::clamp(settings.coverage, 0.0f, 0.99f);
  settings.contrast = std::max(0.01f, settings.contrast);
  return settings;
}

} // namespace

std::uint8_t NebulaField::At(std::int32_t _x, std::int32_t _y) const noexcept
{
  if (!Valid())
  {
    return 0;
  }
  const auto period = static_cast<std::int32_t>(resolution);
  const std::size_t index = static_cast<std::size_t>(WrapIndex(_y, period)) * resolution + WrapIndex(_x, period);
  return intensity[index];
}

float SampleNebulaField(const NebulaSettings& _settings, float _u, float _v) noexcept
{
  const NebulaSettings settings = Sanitised(_settings);

  float sum = 0.0f;
  float amplitude = 1.0f;
  float total = 0.0f;
  auto period = static_cast<std::int32_t>(settings.basePeriod);

  for (std::uint32_t octave = 0; octave < settings.octaves; ++octave)
  {
    // The seed advances per octave so the octaves are independent fields rather
    // than the same field at different scales, which would beat against itself.
    sum += amplitude * PeriodicNoise(_u, _v, period, settings.seed + octave);
    total += amplitude;
    amplitude *= 0.5f;
    period *= 2;
  }

  const float value = total > 0.0f ? sum / total : 0.0f;

  // Lift the coverage floor to black and curve what is left. Without this the
  // field is an even grey fog everywhere, which reads as a fault in the monitor
  // rather than as space.
  const float lifted = (value - settings.coverage) / (1.0f - settings.coverage);
  return std::pow(Saturate(lifted), settings.contrast);
}

bool BakeNebulaField(const NebulaSettings& _settings, NebulaField& _outField)
{
  _outField = NebulaField{};

  if (_settings.resolution == 0 || _settings.resolution > MAX_RESOLUTION || _settings.octaves == 0 ||
      _settings.octaves > MAX_OCTAVES || !(_settings.tileMetres > 0.0f))
  {
    return false;
  }

  const NebulaSettings settings = Sanitised(_settings);
  const std::uint32_t resolution = settings.resolution;

  NebulaField field;
  field.resolution = resolution;
  field.tileMetres = settings.tileMetres;
  field.intensity.resize(static_cast<std::size_t>(resolution) * resolution);

  const float step = 1.0f / static_cast<float>(resolution);
  for (std::uint32_t y = 0; y < resolution; ++y)
  {
    // Texel centres, not corners: sampling the corner would put the tile's seam
    // half a texel out and give the wrap a visible crease.
    const float v = (static_cast<float>(y) + 0.5f) * step;
    for (std::uint32_t x = 0; x < resolution; ++x)
    {
      const float u = (static_cast<float>(x) + 0.5f) * step;
      const float value = Saturate(SampleNebulaField(settings, u, v));
      field.intensity[static_cast<std::size_t>(y) * resolution + x] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
    }
  }

  _outField = std::move(field);
  return true;
}

} // namespace Neuron
