#include "pch.h"

#include "CountedChip.h"

#include <algorithm>
#include <cstdio>

namespace Neuron
{

const char* CountedChipText(std::uint32_t _culled, char* _buffer, std::size_t _bufferBytes) noexcept
{
  if (_buffer == nullptr || _bufferBytes == 0)
  {
    return "";
  }

  // Nothing withheld, nothing said. The rule ADR-022 §5d states is about never
  // claiming a grid is empty when it is not; it does not ask for a chip on
  // every frame of every grid that is fully sent.
  if (_culled == 0)
  {
    _buffer[0] = '\0';
    return _buffer;
  }

  // Clamped rather than wrapped: the wire cannot carry more than this, so a
  // larger number means somebody widened the header and forgot this line --
  // and a chip reading `+7 NOT SHOWN` for 65,543 would be a lie, where a
  // pinned maximum is merely imprecise.
  const std::uint32_t shown = std::min(_culled, MAX_COUNTED_CHIP_VALUE);
  std::snprintf(_buffer, _bufferBytes, "+%u NOT SHOWN", shown);
  return _buffer;
}

UiRect CountedChipRect(const UiRect& _zone, std::uint32_t _textCells, float _cellWidth, float _scale,
                       const CountedChipTuning& _tuning) noexcept
{
  const float scale = _scale > 0.0f ? _scale : 1.0f;
  const float height = std::min(_tuning.height * scale, std::max(0.0f, _zone.height));
  const float width = std::min(static_cast<float>(_textCells) * _cellWidth + _tuning.paddingX * 2.0f * scale,
                               std::max(0.0f, _zone.width));

  const float insetX = _tuning.insetX * scale;
  const float insetY = _tuning.insetY * scale;

  /*
   * Bottom-left, inset -- and clamped so a zone smaller than its own inset puts
   * the chip at the zone's edge rather than outside it.
   *
   * `std::max` against the zone's own origin rather than against zero: the
   * world rect does not start at the viewport's corner (the HUD is a border
   * around it), so clamping to zero would put the chip under the roster.
   */
  const float x = std::max(_zone.x, _zone.x + insetX);
  const float y = std::max(_zone.y, _zone.Bottom() - insetY - height);
  return UiRect{x, y, width, height};
}

} // namespace Neuron
