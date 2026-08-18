#pragma once

#include <cstdint>
#include <vector>

/*
 * The nebula field (ADR-006 §1's reserved `Nebula` node, built).
 *
 * The corpus target frame is
 * `CullClear -> GpuCull -> DepthPre -> Opaque -> Effects -> Nebula -> Tonemap
 * -> Overlay -> Ui -> Present`, and `Nebula` sits after the geometry because it
 * composites *over* the scene rather than behind it: it is the ambient field
 * `tactical-hud.png` shows the fleet sitting inside, which is why
 * `overlay-pass.png` §1 worries about "HDR drift between a bright nebula and
 * empty space". It is not a skybox and it draws no celestial body -- celestials
 * are data, not geometry (ADR-009 §9a).
 *
 * **The field is baked on the CPU, not evaluated in the shader.** That is the
 * decision this file exists to make, and it buys one thing: the maths is
 * testable without a GPU, like `ObjMesh`, `IsoCamera` and `ClearColour` before
 * it. A procedural shader would put the only copy of this function somewhere no
 * test in this tree can reach, and a second copy in C++ to test would be two
 * implementations free to disagree. `GlyphAtlas` already established the
 * pattern: bake at boot, upload once, let the shader do nothing but sample.
 *
 * **The tile is periodic**, so it wraps seamlessly and covers the plane at any
 * zoom. The alternative -- one tile sized to the play area -- looks correct
 * until someone zooms out: `IsoCamera::MAX_ZOOM_METRES` is a 40 km ortho
 * half-height against a 40 km grid, so the camera can see well past the play
 * area and a clamped tile would smear its edge texels across everything beyond.
 * Periodicity removes the failure instead of sizing around it, and "does it
 * wrap" is a property a test can check.
 *
 * Free of D3D12 and Windows on purpose. Nothing here allocates a GPU resource;
 * `GpuNebula` does that with what this produces.
 */

namespace Neuron
{

/*
 * What the field looks like. All of it is content (ADR-012): art direction
 * belongs in `Outpost.json`, not in a rebuild.
 *
 * The tint is *not* baked in. It multiplies at draw time, so recolouring the
 * haze costs a config edit and no re-bake -- and the baked texture stays a
 * single channel, which is what `GlyphAtlas` learned to do for the same reason.
 */
struct NebulaSettings
{
  /// Linear rgb. The default is the green the tactical print's field carries.
  float tintRed = 0.06f;
  float tintGreen = 0.22f;
  float tintBlue = 0.10f;

  /// Peak added brightness. Additive over the scene, so this is the ceiling on
  /// how much the haze can wash out a hull -- the readability budget, really.
  float intensity = 0.35f;

  /// How much plane one period of the field covers. Bigger is a broader, calmer
  /// field; smaller repeats sooner when zoomed out.
  float tileMetres = 96000.0f;

  /// Texels per side. 256 over a 96 km tile is ~375 m per texel, which is
  /// smooth under bilinear at every zoom the tactical tier offers.
  std::uint32_t resolution = 256;

  /// Lattice cells across the tile at the lowest octave.
  std::uint32_t basePeriod = 4;

  /// Each octave doubles the frequency and halves the amplitude. Four gives
  /// features down to a twelfth of the tile -- two or three across the view at
  /// the default zoom, which is structure rather than fog.
  std::uint32_t octaves = 4;

  /// Fraction of the field pushed to black before shaping. "Sparse space" is
  /// the art direction (ADR-006 context); an even fog is the opposite of it.
  float coverage = 0.55f;

  /// Falloff exponent above the coverage floor. Higher concentrates the field
  /// into fewer, softer-edged clouds.
  float contrast = 2.0f;

  /// Changes the field without changing anything else about it.
  std::uint32_t seed = 1;
};

/// One baked tile: single-channel intensity, `resolution` squared, row-major.
struct NebulaField
{
  std::uint32_t resolution = 0;
  float tileMetres = 0.0f;
  std::vector<std::uint8_t> intensity;

  [[nodiscard]] bool Valid() const noexcept
  {
    return resolution > 0 && intensity.size() == static_cast<std::size_t>(resolution) * resolution;
  }

  /// Wrapping fetch, so callers can read across the seam without repeating the
  /// modulo -- and so a test can assert the seam is continuous.
  [[nodiscard]] std::uint8_t At(std::int32_t _x, std::int32_t _y) const noexcept;
};

/// Bakes the field. Returns false and empties the output if the settings could
/// not describe a field -- a zero resolution, no octaves, a degenerate tile.
[[nodiscard]] bool BakeNebulaField(const NebulaSettings& _settings, NebulaField& _outField);

/*
 * The field, sampled in tile space, before quantisation.
 *
 * Exposed because it is the function under test: the baked texture is this
 * evaluated on a grid, so testing this tests what ships. `_u` and `_v` are
 * periodic with period 1 and may be any real value.
 */
[[nodiscard]] float SampleNebulaField(const NebulaSettings& _settings, float _u, float _v) noexcept;

} // namespace Neuron
