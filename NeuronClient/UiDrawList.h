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

  /*
   * From two opposite corners in any order, which is what a drag produces.
   *
   * A selection box dragged up-and-left is as ordinary as one dragged
   * down-and-right, and subtracting in gesture order gives the second one a
   * negative width. `AddQuad` then declines it -- deliberately, because a
   * collapsed layout should cost no instance -- and the box is simply invisible
   * on half the gestures people make. (Not a winding problem: the Ui pipeline
   * culls nothing, and the shader would have drawn the mirrored rect over the
   * same pixels. It never gets that far.)
   */
  [[nodiscard]] static UiRect FromCorners(float _x0, float _y0, float _x1, float _y1) noexcept
  {
    const float left = _x0 < _x1 ? _x0 : _x1;
    const float top = _y0 < _y1 ? _y0 : _y1;
    const float right = _x0 < _x1 ? _x1 : _x0;
    const float bottom = _y0 < _y1 ? _y1 : _y0;
    return UiRect{left, top, right - left, bottom - top};
  }
};

/*
 * One filled quad. Packed 8:8:8:8 with **r in the low byte**, the same as
 * `OverlayMark::colourRgba` and for the same reason -- it is read by
 * `DXGI_FORMAT_R8G8B8A8_UNORM` out of a little-endian word.
 *
 * **Axis-aligned unless `oriented`,** in which case `rect` stops meaning
 * top-left-and-size and starts meaning centre-and-(length, thickness), swept
 * along `axis`. Two meanings in one field, which is worth the discomfort: the
 * alternative is a second quad array, a second upload and a second draw, for a
 * primitive that differs from the first only in how four corners are placed.
 *
 * ADR-006 §8a is why this exists at all -- "a dashed lane between two points is
 * not a quad around one" -- and it is not one feature's special case. Every
 * remaining item on `overlay-pass.png`'s mechanism-B list is an oriented
 * primitive: waypoint polylines, engagement arcs, off-screen indicators.
 */
struct UiQuad
{
  UiRect rect;
  std::uint32_t colourRgba = 0;

  /// Unit direction along the segment. Read only when `oriented`.
  float axisX = 0.0f;
  float axisY = 0.0f;

  /// A flag rather than "is the axis non-zero", because inferring it would make
  /// a degenerate zero-length segment silently become a top-left-anchored rect
  /// somewhere else entirely.
  bool oriented = false;
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

  /*
   * How many quads were already in the list when this run was added -- the
   * one thing that makes **build order the draw order** across both kinds.
   *
   * Quads and runs live in two arrays, so without this a pass can only emit
   * all of one then all of the other, and a panel added after a line of text
   * silently fails to cover it. That is invisible for a HUD that is a border
   * around a view and wrong the moment anything floats: the menu list drew its
   * ground over the ability rack and the rack's *labels* came through it.
   *
   * A cursor rather than a per-instance sort key, because the two arrays are
   * each already in order -- the merge is one walk, and the pass still issues
   * one draw over one stream.
   */
  std::uint32_t quadsBefore = 0;
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
  /// xy = top-left in pixels and zw = size -- or, for `UI_FLAG_ORIENTED`,
  /// xy = centre and zw = (length, thickness).
  float rect[4] = {};
  float uv[4] = {};        // Normalised atlas coordinates; zero for a panel.
  std::uint32_t colourRgba = 0;
  std::uint32_t flags = 0;

  /// Unit direction, for `UI_FLAG_ORIENTED`. **Appended rather than inserted**:
  /// every field above keeps the offset the input layout already declares for
  /// it, so growing the stream cannot move a panel or a glyph by a byte.
  float axis[2] = {};
};

static_assert(sizeof(UiInstance) == 48, "UiInstance is a per-instance vertex stream; its size is the stride the input "
                                        "layout declares, so a change here is a change in three places");

/// `flags` bit 0: sample the glyph atlas rather than drawing the colour flat.
/// Must match `UI_FLAG_GLYPH` in Ui.hlsli.
inline constexpr std::uint32_t UI_FLAG_GLYPH = 1u;

/*
 * `flags` bit 1: sweep the quad along `axis` instead of aligning it to the
 * screen. Must match `UI_FLAG_ORIENTED` in Ui.hlsli.
 *
 * Never set together with `UI_FLAG_GLYPH`. Not because the arithmetic would
 * fail -- a rotated glyph is perfectly well defined -- but because the atlas is
 * baked upright (ADR-006 §9) and a rotated sample of it would be a smear. The
 * pass sets one or the other and the shader branches once.
 */
inline constexpr std::uint32_t UI_FLAG_ORIENTED = 2u;

/*
 * One codepoint out of a UTF-8 string, advancing `_index` past it.
 *
 * The HUD's strings are UTF-8 because the atlas bakes more than ASCII -- the
 * icon sheet's marker glyphs (the alert triangle, the slot squares) are real
 * codepoints in the baked set, and a pass that read a byte as a codepoint
 * could never reach them. A malformed sequence yields U+FFFD and advances one
 * byte, so bad input costs a missing-glyph count rather than a stuck loop.
 */
[[nodiscard]] char32_t DecodeUtf8(std::string_view _text, std::size_t& _index) noexcept;

/// Codepoints in a UTF-8 string -- the number of monospace cells a run covers,
/// which is what every width measurement in the HUD needs. `strlen` counts
/// bytes and over-measures any string carrying a marker glyph.
[[nodiscard]] std::size_t TextCellCount(std::string_view _text) noexcept;

class UiDrawList
{
public:
  void AddQuad(const UiRect& _rect, std::uint32_t _colourRgba);

  /// A one-pixel-thick outline, as four quads. `_thickness` is in pixels and is
  /// drawn *inside* the rect, so a bordered box occupies exactly the rect it
  /// was given -- the prints' panels all butt against their neighbours.
  void AddBorder(const UiRect& _rect, float _thickness, std::uint32_t _colourRgba);

  /*
   * One straight segment of `_thickness` pixels, from one point to another at
   * any angle.
   *
   * A zero-length segment adds nothing rather than adding a quad with no
   * direction: the caller is a dash generator, and a dash that landed exactly
   * on the end of a lane is a rounding result, not a mark.
   */
  void AddSegment(float _x0, float _y0, float _x1, float _y1, float _thickness, std::uint32_t _colourRgba);

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
