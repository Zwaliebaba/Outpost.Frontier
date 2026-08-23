#include "pch.h"

#include "GlyphAtlas.h"

#include "GpuDevice.h"
#include "Log.h"
#include "TaskPool.h"
#include "Telemetry.h"

#include <dwrite_3.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "dwrite.lib")

namespace Neuron
{
namespace
{

constexpr std::uint32_t GLYPH_PADDING_PIXELS = 1; // So bilinear sampling at a
                                                  // quad edge cannot reach into
                                                  // the neighbouring glyph.

/*
 * What the HUD can show: printable ASCII, plus the box and marker glyphs the
 * prints draw their panels and status rows with. Anything outside this set is
 * substituted by the caller rather than baked on demand -- a runtime bake would
 * mean a texture upload mid-frame, which is exactly what an atlas is for
 * avoiding.
 */
constexpr char32_t BOX_AND_MARKER_GLYPHS[] = {
    // Spelled as codepoints rather than as literals on purpose. The source
    // files here carry no byte-order mark, so a non-ASCII character literal
    // would be read in the compiler's current code page and silently become
    // the wrong glyph -- or a multi-character literal. A comment can afford
    // that risk; a table of glyph identities cannot.
    0x2500, 0x2502, 0x250c, 0x2510, 0x2514, 0x2518, // light box drawing
    0x251c, 0x2524, 0x252c, 0x2534, 0x253c,
    0x2550, 0x2551, 0x2554, 0x2557, 0x255a, 0x255d, // double box drawing
    0x2560, 0x2563, 0x2566, 0x2569, 0x256c,
    0x2580, 0x2584, 0x2588, 0x258c, 0x2590,         // block elements
    0x2591, 0x2592, 0x2593,
    0x2190, 0x2191, 0x2192, 0x2193,                 // arrows
    0x25b2, 0x25bc, 0x25c0, 0x25b6,                 // solid triangles
    0x25b8, 0x25be,                                 // small triangles: the ▸ token separator
                                                    // and the ▾ picker caret
    0x25cf, 0x25cb, 0x25a0, 0x25a1,                 // discs and squares
    0xfffd,                                         // U+FFFD REPLACEMENT CHARACTER: what a
                                                    // codepoint this bake has no glyph for is
                                                    // drawn as (ADR-020 §3's fail-soft). Baked
                                                    // rather than assumed, because a
                                                    // substitution the atlas cannot draw either
                                                    // is silence wearing a rule's clothes.
    0x25a3, 0x25a5, 0x25c8,                         // the summary's ▣, the menu's ▥ and the
                                                    // location's ◈ (tactical-hud.png top bar)
    0x00b0, 0x00b1, 0x00d7, 0x2022, 0x00b7,         // degree, plus-minus, times, bullet, and
                                                    // the · the order label separates with
    0x2316, 0x26a0, 0x238c,                         // the ⌖ ship-count glyph, the ⚠ alert
                                                    // triangle and the ⎌ undo chip
    0x231b, 0x23f3,                                 // hourglasses, for the pending chip --
                                                    // listed so a face that has one shows it;
                                                    // a face without bakes .notdef and the
                                                    // chip degrades to its count
    0x27e1, 0x2026};                                // the ⟡ the context bar's chips are marked
                                                    // with and the … the approach chip trails.
                                                    // Both were being *drawn* before they were
                                                    // baked, so both came out as the
                                                    // replacement box: a charset is only as
                                                    // complete as the strings that use it

/// One glyph, rasterised but not yet placed. Produced by the per-size tasks and
/// consumed by the single-threaded packer.
struct BakedGlyph
{
  char32_t codepoint = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int32_t bearingX = 0;
  std::int32_t bearingY = 0;
  float advancePixels = 0.0f;
  std::vector<std::uint8_t> coverage; // width * height, one byte per pixel.
};

struct BakedSize
{
  GlyphAtlasSize metrics;
  std::vector<BakedGlyph> glyphs;
};

std::wstring ToWide(const std::string& _utf8)
{
  const int count = MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), -1, nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), -1, result.data(), count);
  if (!result.empty())
  {
    result.pop_back(); // MultiByteToWideChar counted the terminator.
  }
  return result;
}

[[nodiscard]] std::vector<char32_t> BuildCodepointSet()
{
  std::vector<char32_t> codepoints;
  codepoints.reserve(95 + std::size(BOX_AND_MARKER_GLYPHS));
  for (char32_t c = U' '; c <= U'~'; ++c)
  {
    codepoints.push_back(c);
  }
  for (char32_t c : BOX_AND_MARKER_GLYPHS)
  {
    codepoints.push_back(c);
  }
  return codepoints;
}

/// Finds the family, or the first fallback that exists. Returns null if none
/// do, which on a Windows install would mean something is very wrong.
[[nodiscard]] GpuPtr<IDWriteFontFace> CreateFontFace(IDWriteFactory* _factory, const std::string& _requested, std::string& _outUsed)
{
  GpuPtr<IDWriteFontCollection> collection;
  if (FAILED(_factory->GetSystemFontCollection(collection.put(), FALSE)))
  {
    return {};
  }

  // Consolas first because it is what the prints are set in; the rest are
  // monospace faces that ship with Windows, in descending order of similarity.
  const std::string candidates[] = {_requested, "Consolas", "Cascadia Mono", "Courier New", "Segoe UI"};
  for (const std::string& candidate : candidates)
  {
    if (candidate.empty())
    {
      continue;
    }

    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(collection->FindFamilyName(ToWide(candidate).c_str(), &index, &exists)) || exists == FALSE)
    {
      continue;
    }

    GpuPtr<IDWriteFontFamily> family;
    GpuPtr<IDWriteFont> font;
    GpuPtr<IDWriteFontFace> face;
    if (FAILED(collection->GetFontFamily(index, family.put())) ||
        FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            font.put())) ||
        FAILED(font->CreateFontFace(face.put())))
    {
      continue;
    }

    _outUsed = candidate;
    return face;
  }

  return {};
}

/*
 * Rasterises one glyph to 8-bit coverage.
 *
 * CLEARTYPE_3x1 and then averaging the three subpixels, rather than
 * ALIASED_1x1: aliased gives hard 0-or-255 coverage, and a 13-pixel monospace
 * face rendered without antialiasing is unreadable at the sizes the HUD uses.
 * The subpixel triplet is averaged rather than kept because the atlas is one
 * channel and the quads are drawn over arbitrary backgrounds, where subpixel
 * ordering is wrong anyway.
 */
[[nodiscard]] bool RasteriseGlyph(IDWriteFactory* _factory, IDWriteFontFace* _face, UINT16 _glyphIndex, float _sizePixels,
                                  BakedGlyph& _outGlyph)
{
  DWRITE_GLYPH_OFFSET offset{};
  FLOAT advance = 0.0f;

  DWRITE_GLYPH_RUN run{};
  run.fontFace = _face;
  run.fontEmSize = _sizePixels;
  run.glyphCount = 1;
  run.glyphIndices = &_glyphIndex;
  run.glyphAdvances = &advance;
  run.glyphOffsets = &offset;

  GpuPtr<IDWriteGlyphRunAnalysis> analysis;
  if (FAILED(_factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
                                              0.0f, 0.0f, analysis.put())))
  {
    return false;
  }

  RECT bounds{};
  if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds)))
  {
    return false;
  }

  const auto width = static_cast<std::uint32_t>(std::max<LONG>(0, bounds.right - bounds.left));
  const auto height = static_cast<std::uint32_t>(std::max<LONG>(0, bounds.bottom - bounds.top));
  if (width == 0 || height == 0)
  {
    // A space has no coverage and still has an advance; it is a glyph with an
    // empty bitmap, not a failure.
    _outGlyph.width = 0;
    _outGlyph.height = 0;
    _outGlyph.bearingX = 0;
    _outGlyph.bearingY = 0;
    return true;
  }

  std::vector<std::uint8_t> subpixels(static_cast<std::size_t>(width) * height * 3);
  if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, subpixels.data(),
                                          static_cast<UINT32>(subpixels.size()))))
  {
    return false;
  }

  _outGlyph.width = width;
  _outGlyph.height = height;
  _outGlyph.bearingX = bounds.left;
  // The analysis was run with the baseline at y = 0 and y grows downward, so a
  // bitmap above the baseline has a negative top. The HUD wants the height
  // above the baseline, which is that negated.
  _outGlyph.bearingY = -bounds.top;
  _outGlyph.coverage.resize(static_cast<std::size_t>(width) * height);
  for (std::size_t i = 0; i < _outGlyph.coverage.size(); ++i)
  {
    const std::uint32_t sum = static_cast<std::uint32_t>(subpixels[i * 3]) + subpixels[i * 3 + 1] + subpixels[i * 3 + 2];
    _outGlyph.coverage[i] = static_cast<std::uint8_t>((sum + 1) / 3);
  }
  return true;
}

void BakeOneSize(IDWriteFactory* _factory, IDWriteFontFace* _face, const DWRITE_FONT_METRICS& _fontMetrics,
                 IDWriteFontFace* _symbolFace, const std::vector<char32_t>& _codepoints, float _sizePixels,
                 BakedSize& _outBaked)
{
  NEURON_SPAN("GlyphBake");

  const float unitsPerEm = static_cast<float>(_fontMetrics.designUnitsPerEm);
  const float scale = _sizePixels / unitsPerEm;

  _outBaked.metrics.sizePixels = _sizePixels;
  _outBaked.metrics.ascentPixels = static_cast<float>(_fontMetrics.ascent) * scale;
  _outBaked.metrics.descentPixels = static_cast<float>(_fontMetrics.descent) * scale;
  _outBaked.metrics.lineHeightPixels =
      (static_cast<float>(_fontMetrics.ascent) + _fontMetrics.descent + _fontMetrics.lineGap) * scale;

  std::vector<UINT32> asUint;
  asUint.reserve(_codepoints.size());
  for (char32_t codepoint : _codepoints)
  {
    asUint.push_back(static_cast<UINT32>(codepoint));
  }

  std::vector<UINT16> glyphIndices(asUint.size(), 0);
  if (FAILED(_face->GetGlyphIndices(asUint.data(), static_cast<UINT32>(asUint.size()), glyphIndices.data())))
  {
    return;
  }

  std::vector<DWRITE_GLYPH_METRICS> designMetrics(glyphIndices.size());
  if (FAILED(_face->GetDesignGlyphMetrics(glyphIndices.data(), static_cast<UINT32>(glyphIndices.size()), designMetrics.data(), FALSE)))
  {
    return;
  }

  _outBaked.glyphs.reserve(_codepoints.size());
  std::vector<std::size_t> missing;
  for (std::size_t i = 0; i < _codepoints.size(); ++i)
  {
    if (glyphIndices[i] == 0)
    {
      missing.push_back(i); // .notdef -- the face may still borrow one below.
      continue;
    }

    BakedGlyph glyph;
    glyph.codepoint = _codepoints[i];
    glyph.advancePixels = static_cast<float>(designMetrics[i].advanceWidth) * scale;
    if (!RasteriseGlyph(_factory, _face, glyphIndices[i], _sizePixels, glyph))
    {
      continue;
    }

    if (_codepoints[i] == U'M')
    {
      _outBaked.metrics.cellWidthPixels = glyph.advancePixels;
    }
    _outBaked.glyphs.push_back(std::move(glyph));
  }

  /*
   * The marker glyphs the main face does not carry, borrowed from the symbol
   * face -- Consolas has no ⌖, ⎌ or ⚠, and the prints draw all three. The
   * borrowed bitmap is **forced onto the monospace cell**: the pen advances by
   * a glyph's own advance, and a proportional symbol advance in the middle of
   * a run would break the grid every layout multiplication assumes. Centred in
   * the cell for the same reason, so a wide symbol overhangs both sides evenly
   * rather than shouldering into the next column.
   */
  if (_symbolFace == nullptr || missing.empty() || _outBaked.metrics.cellWidthPixels <= 0.0f)
  {
    return; // No face, nothing missing, or a face with no 'M' to size a cell by.
  }
  for (const std::size_t i : missing)
  {
    const auto asUint32 = static_cast<UINT32>(_codepoints[i]);
    UINT16 symbolIndex = 0;
    if (FAILED(_symbolFace->GetGlyphIndices(&asUint32, 1, &symbolIndex)) || symbolIndex == 0)
    {
      continue; // Missing everywhere: the caller substitutes, as it always did.
    }

    BakedGlyph glyph;
    glyph.codepoint = _codepoints[i];
    if (!RasteriseGlyph(_factory, _symbolFace, symbolIndex, _sizePixels, glyph))
    {
      continue;
    }
    glyph.advancePixels = _outBaked.metrics.cellWidthPixels;
    glyph.bearingX = static_cast<std::int32_t>((_outBaked.metrics.cellWidthPixels - static_cast<float>(glyph.width)) * 0.5f);
    _outBaked.glyphs.push_back(std::move(glyph));
  }
}

} // namespace

GlyphAtlas::~GlyphAtlas()
{
  Destroy();
}

std::uint32_t GlyphAtlas::GlyphCount() const noexcept
{
  std::uint32_t total = 0;
  for (const std::unordered_map<char32_t, GlyphMetrics>& perSize : m_glyphs)
  {
    total += static_cast<std::uint32_t>(perSize.size());
  }
  return total;
}

const GlyphMetrics* GlyphAtlas::Find(std::uint32_t _sizeIndex, char32_t _codepoint) const noexcept
{
  if (_sizeIndex >= m_glyphs.size())
  {
    return nullptr;
  }
  const auto found = m_glyphs[_sizeIndex].find(_codepoint);
  return found == m_glyphs[_sizeIndex].end() ? nullptr : &found->second;
}

bool GlyphAtlas::Create(GpuDevice& _device, const Desc& _desc, TaskPool& _taskPool, D3D12_CPU_DESCRIPTOR_HANDLE _srvDestination)
{
  NEURON_SPAN("AtlasBake");
  Destroy();

  if (_desc.sizesPixels.empty() || _desc.textureWidth == 0)
  {
    return false;
  }

  GpuPtr<IDWriteFactory> factory;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(factory.put()))))
  {
    NEURON_LOG_ERROR("DirectWrite is unavailable; there is no text without it");
    return false;
  }

  GpuPtr<IDWriteFontFace> face = CreateFontFace(factory.get(), _desc.fontFamily, m_fontFamily);
  if (!face)
  {
    NEURON_LOG_ERROR("no usable font face found (asked for '%s')", _desc.fontFamily.c_str());
    return false;
  }
  if (m_fontFamily != _desc.fontFamily)
  {
    NEURON_LOG_WARNING("font '%s' is not installed; using '%s'", _desc.fontFamily.c_str(), m_fontFamily.c_str());
  }

  DWRITE_FONT_METRICS fontMetrics{};
  face->GetMetrics(&fontMetrics);

  const std::vector<char32_t> codepoints = BuildCodepointSet();

  /*
   * The symbol fallback, for marker glyphs the text face lacks. Segoe UI
   * Symbol ships with Windows and carries the whole marker set; asked for
   * directly rather than through `CreateFontFace`, whose candidate list would
   * quietly hand back the same text face that was missing the glyph. Null is
   * fine -- the bake then degrades exactly as it did before the fallback
   * existed.
   */
  std::string symbolUsed;
  GpuPtr<IDWriteFontFace> symbolFace = CreateFontFace(factory.get(), "Segoe UI Symbol", symbolUsed);
  if (symbolFace && symbolUsed != "Segoe UI Symbol")
  {
    symbolFace = nullptr; // A text face is not a symbol fallback.
  }

  // One task per size. Each writes only its own slot, and packing happens after
  // the wait, so the atlas layout does not depend on the thread schedule.
  std::vector<BakedSize> baked(_desc.sizesPixels.size());
  WaitGroup group;
  for (std::size_t i = 0; i < _desc.sizesPixels.size(); ++i)
  {
    _taskPool.Submit(
        [&, i]() {
          BakeOneSize(factory.get(), face.get(), fontMetrics, symbolFace.get(), codepoints, _desc.sizesPixels[i], baked[i]);
        },
        &group);
  }
  _taskPool.Wait(group);

  // Shelf packing, largest size first so the tall rows are laid down before the
  // short ones and the last shelf is the only ragged one. Good enough for a few
  // hundred glyphs that are all within a factor of two of each other.
  std::vector<std::uint32_t> order(baked.size());
  for (std::uint32_t i = 0; i < order.size(); ++i)
  {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](std::uint32_t _a, std::uint32_t _b) { return baked[_a].metrics.sizePixels > baked[_b].metrics.sizePixels; });

  m_textureWidth = _desc.textureWidth;
  m_sizes.resize(baked.size());
  m_glyphs.resize(baked.size());

  std::uint32_t penX = GLYPH_PADDING_PIXELS;
  std::uint32_t penY = GLYPH_PADDING_PIXELS;
  std::uint32_t shelfHeight = 0;

  for (std::uint32_t sizeIndex : order)
  {
    m_sizes[sizeIndex] = baked[sizeIndex].metrics;
    for (const BakedGlyph& glyph : baked[sizeIndex].glyphs)
    {
      GlyphMetrics placed;
      placed.bearingX = static_cast<std::int16_t>(glyph.bearingX);
      placed.bearingY = static_cast<std::int16_t>(glyph.bearingY);
      placed.advancePixels = glyph.advancePixels;

      if (glyph.width != 0 && glyph.height != 0)
      {
        if (penX + glyph.width + GLYPH_PADDING_PIXELS > m_textureWidth)
        {
          penX = GLYPH_PADDING_PIXELS;
          penY += shelfHeight + GLYPH_PADDING_PIXELS;
          shelfHeight = 0;
        }

        placed.atlasX = static_cast<std::uint16_t>(penX);
        placed.atlasY = static_cast<std::uint16_t>(penY);
        placed.width = static_cast<std::uint16_t>(glyph.width);
        placed.height = static_cast<std::uint16_t>(glyph.height);

        penX += glyph.width + GLYPH_PADDING_PIXELS;
        shelfHeight = std::max(shelfHeight, glyph.height);
      }

      m_glyphs[sizeIndex].emplace(glyph.codepoint, placed);
    }
  }

  const std::uint32_t neededHeight = penY + shelfHeight + GLYPH_PADDING_PIXELS;
  if (neededHeight > _desc.maxTextureHeight)
  {
    NEURON_LOG_ERROR("the glyph atlas needs %u rows and the cap is %u; drop a size or widen the texture", neededHeight,
                     _desc.maxTextureHeight);
    Destroy();
    return false;
  }

  // Rounded to a power of two so the texture is a shape every driver is happy
  // with, and never below 64 so a tiny set still produces a valid resource.
  m_textureHeight = 64;
  while (m_textureHeight < neededHeight)
  {
    m_textureHeight *= 2;
  }

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(m_textureWidth) * m_textureHeight, 0);
  for (std::uint32_t sizeIndex = 0; sizeIndex < baked.size(); ++sizeIndex)
  {
    for (const BakedGlyph& glyph : baked[sizeIndex].glyphs)
    {
      const GlyphMetrics& placed = m_glyphs[sizeIndex].at(glyph.codepoint);
      for (std::uint32_t row = 0; row < glyph.height; ++row)
      {
        std::memcpy(&pixels[(static_cast<std::size_t>(placed.atlasY) + row) * m_textureWidth + placed.atlasX],
                    &glyph.coverage[static_cast<std::size_t>(row) * glyph.width], glyph.width);
      }
    }
  }

  // Upload. R8_UNORM: coverage is one channel, and the Ui pass multiplies it
  // into whatever colour the HUD asked for.
  D3D12_HEAP_PROPERTIES defaultHeap{};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC textureDesc{};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  textureDesc.Width = m_textureWidth;
  textureDesc.Height = m_textureHeight;
  textureDesc.DepthOrArraySize = 1;
  textureDesc.MipLevels = 1;
  textureDesc.Format = DXGI_FORMAT_R8_UNORM;
  textureDesc.SampleDesc.Count = 1;
  textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  check_hresult(_device.Device()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(m_texture.put())));
  NAME_D3D12_OBJECT(m_texture);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rowCount = 0;
  UINT64 rowBytes = 0;
  UINT64 uploadBytes = 0;
  _device.Device()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rowCount, &rowBytes, &uploadBytes);

  D3D12_HEAP_PROPERTIES uploadHeap{};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC uploadDesc{};
  uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  uploadDesc.Width = uploadBytes;
  uploadDesc.Height = 1;
  uploadDesc.DepthOrArraySize = 1;
  uploadDesc.MipLevels = 1;
  uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
  uploadDesc.SampleDesc.Count = 1;
  uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  GpuPtr<ID3D12Resource> upload;
  check_hresult(_device.Device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload.put())));

  std::uint8_t* mapped = nullptr;
  const D3D12_RANGE readRange{0, 0};
  check_hresult(upload->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
  for (std::uint32_t row = 0; row < m_textureHeight; ++row)
  {
    // Row pitch in an upload buffer is aligned to 256 bytes and the source is
    // tightly packed, so this is a row at a time and not one memcpy.
    std::memcpy(mapped + footprint.Offset + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                &pixels[static_cast<std::size_t>(row) * m_textureWidth], m_textureWidth);
  }
  upload->Unmap(0, nullptr);

  GpuPtr<ID3D12CommandAllocator> allocator;
  GpuPtr<ID3D12GraphicsCommandList> commandList;
  check_hresult(_device.Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())));
  check_hresult(_device.Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr,
                                                    IID_PPV_ARGS(commandList.put())));

  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = m_texture.get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = upload.get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = footprint;

  commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = m_texture.get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  commandList->ResourceBarrier(1, &barrier);

  check_hresult(commandList->Close());
  ID3D12CommandList* lists[] = {commandList.get()};
  _device.Queue()->ExecuteCommandLists(1, lists);
  _device.WaitForIdle(); // The upload buffer dies at the end of this scope.

  D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc{};
  viewDesc.Format = DXGI_FORMAT_R8_UNORM;
  viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  viewDesc.Texture2D.MipLevels = 1;
  _device.Device()->CreateShaderResourceView(m_texture.get(), &viewDesc, _srvDestination);

  NEURON_LOG_INFO("glyph atlas: %s, %u sizes, %u glyphs, %ux%u", m_fontFamily.c_str(), static_cast<unsigned>(m_sizes.size()),
                  GlyphCount(), m_textureWidth, m_textureHeight);
  return true;
}

void GlyphAtlas::Destroy()
{
  m_texture = nullptr;
  m_sizes.clear();
  m_glyphs.clear();
  m_fontFamily.clear();
  m_textureWidth = 0;
  m_textureHeight = 0;
}

} // namespace Neuron
