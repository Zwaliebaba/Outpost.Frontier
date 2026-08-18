#include "pch.h"

#include "UiDrawList.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{

char32_t DecodeUtf8(std::string_view _text, std::size_t& _index) noexcept
{
  const auto lead = static_cast<unsigned char>(_text[_index]);
  if (lead < 0x80u)
  {
    ++_index;
    return lead;
  }

  std::size_t length = 0;
  char32_t codepoint = 0;
  if ((lead & 0xe0u) == 0xc0u)
  {
    length = 2;
    codepoint = lead & 0x1fu;
  }
  else if ((lead & 0xf0u) == 0xe0u)
  {
    length = 3;
    codepoint = lead & 0x0fu;
  }
  else if ((lead & 0xf8u) == 0xf0u)
  {
    length = 4;
    codepoint = lead & 0x07u;
  }
  else
  {
    ++_index; // A stray continuation byte. One replacement per byte, so a
              // damaged string stays the same length it claims.
    return 0xFFFD;
  }

  if (_index + length > _text.size())
  {
    ++_index;
    return 0xFFFD;
  }
  for (std::size_t i = 1; i < length; ++i)
  {
    const auto follow = static_cast<unsigned char>(_text[_index + i]);
    if ((follow & 0xc0u) != 0x80u)
    {
      ++_index;
      return 0xFFFD;
    }
    codepoint = (codepoint << 6) | (follow & 0x3fu);
  }

  _index += length;
  return codepoint;
}

std::size_t TextCellCount(std::string_view _text) noexcept
{
  std::size_t count = 0;
  std::size_t index = 0;
  while (index < _text.size())
  {
    (void)DecodeUtf8(_text, index);
    ++count;
  }
  return count;
}

void UiDrawList::AddQuad(const UiRect& _rect, std::uint32_t _colourRgba)
{
  // A zero-area quad is not an error and not worth an instance: layouts that
  // collapse to nothing are how an empty roster or a zero-width bar reads.
  if (_rect.width <= 0.0f || _rect.height <= 0.0f)
  {
    return;
  }
  m_quads.push_back(UiQuad{_rect, _colourRgba, 0.0f, 0.0f, false});
}

void UiDrawList::AddSegment(float _x0, float _y0, float _x1, float _y1, float _thickness, std::uint32_t _colourRgba)
{
  if (_thickness <= 0.0f)
  {
    return;
  }

  const float dx = _x1 - _x0;
  const float dy = _y1 - _y0;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (!(length > 0.0f))
  {
    return; // Zero length, or a NaN that made the comparison false.
  }

  UiQuad quad;
  // Centre and extent, which is what an oriented quad's `rect` means. The
  // shader sweeps from -0.5 to +0.5 of each, so the centre is the anchor and
  // neither end is privileged -- a segment drawn backwards is the same segment.
  quad.rect = UiRect{(_x0 + _x1) * 0.5f, (_y0 + _y1) * 0.5f, length, _thickness};
  quad.colourRgba = _colourRgba;
  quad.axisX = dx / length;
  quad.axisY = dy / length;
  quad.oriented = true;
  m_quads.push_back(quad);
}

void UiDrawList::AddBorder(const UiRect& _rect, float _thickness, std::uint32_t _colourRgba)
{
  if (_thickness <= 0.0f || _rect.width <= 0.0f || _rect.height <= 0.0f)
  {
    return;
  }

  // Four bars rather than a rect with a hole, because the Ui pass draws filled
  // quads and nothing else. Top and bottom run the full width; the sides fill
  // only what is left between them, so no pixel is written twice -- which
  // matters the moment a border is drawn with alpha.
  const float thickness = std::min(_thickness, std::min(_rect.width, _rect.height) * 0.5f);
  const float innerHeight = _rect.height - 2.0f * thickness;

  AddQuad(UiRect{_rect.x, _rect.y, _rect.width, thickness}, _colourRgba);
  AddQuad(UiRect{_rect.x, _rect.Bottom() - thickness, _rect.width, thickness}, _colourRgba);
  AddQuad(UiRect{_rect.x, _rect.y + thickness, thickness, innerHeight}, _colourRgba);
  AddQuad(UiRect{_rect.Right() - thickness, _rect.y + thickness, thickness, innerHeight}, _colourRgba);
}

void UiDrawList::AddText(float _x, float _y, std::uint8_t _sizeIndex, std::uint32_t _colourRgba, std::string_view _text)
{
  if (_text.empty())
  {
    return;
  }

  UiTextRun run;
  run.x = _x;
  run.y = _y;
  run.colourRgba = _colourRgba;
  run.sizeIndex = _sizeIndex;
  run.textOffset = static_cast<std::uint32_t>(m_text.size());
  run.textLength = static_cast<std::uint32_t>(_text.size());

  m_text.append(_text);
  m_runs.push_back(run);
}

void UiDrawList::Clear() noexcept
{
  // Every one of these keeps its capacity, which is the point: the HUD is
  // rebuilt every frame and must not allocate to do it.
  m_quads.clear();
  m_runs.clear();
  m_text.clear();
}

} // namespace Neuron
