#include "pch.h"

#include "UiDrawList.h"

#include <algorithm>
#include <cmath>

namespace Neuron
{

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
