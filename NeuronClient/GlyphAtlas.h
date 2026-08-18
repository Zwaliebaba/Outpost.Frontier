#pragma once

#include "GpuCom.h"

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * The text atlas (ADR-006 §9).
 *
 * DirectWrite bakes every glyph the HUD can show into one R8 texture at boot;
 * the Ui pass then draws instanced quads out of it. That is the whole
 * typography system, and it is a deliberate ceiling: no shaping, no
 * international text, no runtime layout engine.
 *
 * **Rejected, and worth restating here because it is the largest thing this
 * decision deletes:** D3D11On12 + D2D interop. It would give real typography
 * and cost a second device, wrapped-resource synchronisation every frame, and
 * the biggest boilerplate item in the codebase -- for text that the prints
 * render as a monospace grid.
 *
 * Baking is fanned out over the boot TaskPool, one task per size. DirectWrite's
 * factory and font face are thread-safe for the rasterisation calls used here,
 * and each task rasterises into storage only it can see: packing and blitting
 * happen afterwards, on one thread, so the atlas layout is identical from run
 * to run regardless of how the tasks interleaved.
 */

namespace Neuron
{

class GpuDevice;
class TaskPool;

/// Where one glyph sits in the atlas, and how to place it against a pen.
struct GlyphMetrics
{
  std::uint16_t atlasX = 0;
  std::uint16_t atlasY = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::int16_t bearingX = 0; // From the pen position to the left of the bitmap.
  std::int16_t bearingY = 0; // From the baseline up to the top of the bitmap.
  float advancePixels = 0.0f;
};

/// One baked size. The HUD asks for a size by index, never by number, so a
/// retune of the size list is a content change rather than a code change.
struct GlyphAtlasSize
{
  float sizePixels = 0.0f;
  float ascentPixels = 0.0f;
  float descentPixels = 0.0f;
  float lineHeightPixels = 0.0f;
  float cellWidthPixels = 0.0f; // The advance of 'M'; the face is monospace,
                                // so this is the grid the prints are drawn on.
};

class GlyphAtlas
{
public:
  struct Desc
  {
    /// A monospace face. Consolas ships with Windows; the fallback list exists
    /// because a face missing from a machine must not stop the client.
    std::string fontFamily = "Consolas";
    std::vector<float> sizesPixels = {13.0f, 16.0f, 22.0f};
    std::uint32_t textureWidth = 1024;
    std::uint32_t maxTextureHeight = 2048;
  };

  GlyphAtlas() = default;
  ~GlyphAtlas();

  GlyphAtlas(const GlyphAtlas&) = delete;
  GlyphAtlas& operator=(const GlyphAtlas&) = delete;

  /// _srvDestination is a slot in the client's one shader-visible CBV/SRV heap
  /// (ADR-006 §12); the atlas fills it and never owns a heap of its own.
  [[nodiscard]] bool Create(GpuDevice& _device, const Desc& _desc, TaskPool& _taskPool, D3D12_CPU_DESCRIPTOR_HANDLE _srvDestination);
  void Destroy();

  [[nodiscard]] std::uint32_t SizeCount() const noexcept { return static_cast<std::uint32_t>(m_sizes.size()); }
  [[nodiscard]] const GlyphAtlasSize& Size(std::uint32_t _sizeIndex) const noexcept { return m_sizes[_sizeIndex]; }

  /// Null for a codepoint the face has no glyph for -- the caller substitutes,
  /// because only it knows whether a missing glyph is worth a box or a space.
  [[nodiscard]] const GlyphMetrics* Find(std::uint32_t _sizeIndex, char32_t _codepoint) const noexcept;

  [[nodiscard]] std::uint32_t TextureWidth() const noexcept { return m_textureWidth; }
  [[nodiscard]] std::uint32_t TextureHeight() const noexcept { return m_textureHeight; }
  [[nodiscard]] std::uint32_t GlyphCount() const noexcept;
  [[nodiscard]] const std::string& FontFamily() const noexcept { return m_fontFamily; }
  [[nodiscard]] ID3D12Resource* Texture() const noexcept { return m_texture.get(); }

private:
  GpuPtr<ID3D12Resource> m_texture;
  std::vector<GlyphAtlasSize> m_sizes;
  std::vector<std::unordered_map<char32_t, GlyphMetrics>> m_glyphs; // One map per size.
  std::string m_fontFamily;
  std::uint32_t m_textureWidth = 0;
  std::uint32_t m_textureHeight = 0;
};

} // namespace Neuron
