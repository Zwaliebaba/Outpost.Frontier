#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/*
 * What the Ui pass draws, built on the CPU (ADR-006 §10).
 *
 * Screen-space quads and runs of text, in pixels with the origin top-left --
 * the space the prints are drawn in, so a layout constant read off a sheet is
 * the number that goes in the code.
 *
 * **Text stays text until the last moment.** A run carries a string and a
 * position; the pass turns it into glyph quads against the atlas. That keeps
 * every decision about *what the HUD says* device-free and testable, and puts
 * the only atlas-dependent code in one loop. It also means a test can assert
 * the words rather than the quads, which is the difference between checking the
 * HUD and checking the renderer.
 *
 * **The face is monospace and that is load-bearing** (ADR-006 §9). A run's
 * width is its length times the cell width, so layout needs no per-glyph
 * metrics and no measuring pass -- which is most of why this file has no
 * dependency on `GlyphAtlas` at all.
 *
 * **Allocation-free after the first frame.** Text goes into one pooled buffer
 * and a run holds an offset and a length, so `Clear` keeps every capacity. A
 * HUD rebuilt sixty times a second must not be sixty allocations a second.
 */

namespace Neuron
{

/// A rectangle in screen pixels, origin top-left. Half-open: `x + width` is the
/// first column *not* covered, which is what makes adjacent rects abut without
/// overlapping by one.
struct UiRect
{
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  [[nodiscard]] float Right() const noexcept { return x + width; }
  [[nodiscard]] float Bottom() const noexcept { return y + height; }

  [[nodiscard]] bool Contains(float _x, float _y) const noexcept
  {
    return _x >= x && _x < Right() && _y >= y && _y < Bottom();
  }

  /// Shrunk on every side. A negative inset grows it; nothing clamps, because a
  /// rect inset past its own size is a layout mistake worth seeing.
  [[nodiscard]] UiRect Inset(float _by) const noexcept
  {
    return UiRect{x + _by, y + _by, width - 2.0f * _by, height - 2.0f * _by};
  }
};

/// One filled rectangle. Packed 8:8:8:8 with **r in the low byte**, the same as
/// `OverlayMark::colourRgba` and for the same reason -- it is read by
/// `DXGI_FORMAT_R8G8B8A8_UNORM` out of a little-endian word.
struct UiQuad
{
  UiRect rect;
  std::uint32_t colourRgba = 0;
};

/// One run of text on one line. `x` is the left of the first cell and `y` is
/// the *top* of the line rather than its baseline, because every layout number
/// on the prints is a box and converting once here beats converting at every
/// call site.
struct UiTextRun
{
  float x = 0.0f;
  float y = 0.0f;
  std::uint32_t colourRgba = 0;
  std::uint8_t sizeIndex = 0;

  /// Into the list's own text buffer. Not a `string_view`, because the buffer
  /// grows and a view into it would dangle the moment another run was added.
  std::uint32_t textOffset = 0;
  std::uint32_t textLength = 0;
};

/*
 * One HUD quad as the GPU takes it: 40 bytes, and it stays 40 bytes.
 *
 * Panels and glyphs share this stream and differ only by `flags`, because they
 * are the same quad in the same space blended the same way -- two pipelines
 * would mean two draws over one upload and a sort to separate them, for a HUD
 * whose natural build order is panel, text, panel, text.
 *
 * Here rather than beside the pass for the same reason `OverlayMark` is here:
 * it is a layout, the input-element table in `GpuPipelines` is a second copy of
 * it, and the static assert below is what keeps the two honest.
 */
struct UiInstance
{
  float rect[4] = {};      // xy = top-left in pixels, zw = size.
  float uv[4] = {};        // Normalised atlas coordinates; zero for a panel.
  std::uint32_t colourRgba = 0;
  std::uint32_t flags = 0;
};

static_assert(sizeof(UiInstance) == 40, "UiInstance is a per-instance vertex stream; its size is the stride the input "
                                        "layout declares, so a change here is a change in three places");

/// `flags` bit 0: sample the glyph atlas rather than drawing the colour flat.
/// Must match `UI_FLAG_GLYPH` in Ui.hlsli.
inline constexpr std::uint32_t UI_FLAG_GLYPH = 1u;

class UiDrawList
{
public:
  void AddQuad(const UiRect& _rect, std::uint32_t _colourRgba);

  /// A one-pixel-thick outline, as four quads. `_thickness` is in pixels and is
  /// drawn *inside* the rect, so a bordered box occupies exactly the rect it
  /// was given -- the prints' panels all butt against their neighbours.
  void AddBorder(const UiRect& _rect, float _thickness, std::uint32_t _colourRgba);

  void AddText(float _x, float _y, std::uint8_t _sizeIndex, std::uint32_t _colourRgba, std::string_view _text);

  [[nodiscard]] std::span<const UiQuad> Quads() const noexcept { return m_quads; }
  [[nodiscard]] std::span<const UiTextRun> Runs() const noexcept { return m_runs; }

  /// The characters a run points into. Returned as a view over the whole buffer
  /// so a caller slices it with the run's own offset and length.
  [[nodiscard]] std::string_view Text(const UiTextRun& _run) const noexcept
  {
    return std::string_view{m_text}.substr(_run.textOffset, _run.textLength);
  }

  /// Total characters across every run -- what the glyph quad buffer has to
  /// hold in the worst case, before the pass drops the ones with no glyph.
  [[nodiscard]] std::size_t CharacterCount() const noexcept { return m_text.size(); }

  void Clear() noexcept;

private:
  std::vector<UiQuad> m_quads;
  std::vector<UiTextRun> m_runs;
  std::string m_text;
};

} // namespace Neuron
