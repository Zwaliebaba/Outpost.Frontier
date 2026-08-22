#include "pch.h"

#include "HudRoster.h"

namespace Neuron
{
namespace
{

/// The scale guard every layout in this tree takes: a zero or negative scale
/// would collapse every rect to nothing and the column would be gone with no
/// error anywhere.
[[nodiscard]] float SafeScale(float _scale) noexcept
{
  return _scale > 0.0f ? _scale : 1.0f;
}

/// Where the first wing chip's top edge sits: under the panel's padding and its
/// `FLEET ROSTER` heading.
[[nodiscard]] float ChipsTop(const UiRect& _column, float _scale, const RosterColumnTuning& _tuning) noexcept
{
  return _column.y + _tuning.padding * _scale + _tuning.headingHeight * _scale;
}

} // namespace

UiRect RosterChipRect(const UiRect& _column, float _scale, const RosterColumnTuning& _tuning,
                      std::uint32_t _row) noexcept
{
  const float scale = SafeScale(_scale);
  const float pad = _tuning.padding * scale;
  const float height = _tuning.chipHeight * scale;
  const float pitch = height + _tuning.chipGap * scale;

  return UiRect{_column.x + pad * 0.5f, ChipsTop(_column, scale, _tuning) + static_cast<float>(_row) * pitch,
                _column.width - pad, height};
}

std::uint32_t RosterChipsThatFit(const UiRect& _column, std::uint32_t _rowCount, float _scale,
                                 const RosterColumnTuning& _tuning) noexcept
{
  std::uint32_t fitted = 0;
  while (fitted < _rowCount && RosterChipRect(_column, _scale, _tuning, fitted).Bottom() <= _column.Bottom())
  {
    ++fitted;
  }
  return fitted;
}

float RosterBlocksTop(const UiRect& _column, std::uint32_t _rowCount, float _scale,
                      const RosterColumnTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const std::uint32_t chips = RosterChipsThatFit(_column, _rowCount, _scale, _tuning);
  const float pitch = (_tuning.chipHeight + _tuning.chipGap) * scale;

  // A gap after the last chip, then the `ELSEWHERE` heading, then the blocks.
  return ChipsTop(_column, scale, _tuning) + static_cast<float>(chips) * pitch + _tuning.chipGap * scale +
         _tuning.headingHeight * scale;
}

std::uint32_t BuildLocationBlockColumn(std::span<const LocationBlock> _blocks, const UiRect& _column, float _top,
                                       float _scale, const RosterColumnTuning& _tuning,
                                       std::span<LocationBlockLayout> _outLayouts)
{
  const float scale = SafeScale(_scale);
  const float pad = _tuning.padding * scale;
  const float height = _tuning.blockHeight * scale;
  const float gap = _tuning.blockGap * scale;
  const float buttonHeight = _tuning.buttonHeight * scale;

  std::uint32_t laid = 0;
  float pen = _top;

  for (std::uint32_t index = 0; index < _blocks.size(); ++index)
  {
    if (laid == _outLayouts.size())
    {
      break;
    }

    const LocationBlock& block = _blocks[index];

    /*
     * Already on screen as hulls, so a row here would be one fleet counted
     * twice. The game said so; this file does not work it out -- comparing
     * `stateLabel` would be the engine reading a vocabulary it is not allowed
     * to have an opinion about.
     */
    if (block.inScene)
    {
      continue;
    }

    const UiRect panel{_column.x + pad * 0.5f, pen, _column.width - pad, height};
    if (panel.Bottom() > _column.Bottom())
    {
      // Dropped rather than shrunk: `Tactical`'s overflow rule is drop, and a
      // column that reflowed at some window height would be a layout engine.
      break;
    }

    LocationBlockLayout& out = _outLayouts[laid];
    out = LocationBlockLayout{};
    out.panel = panel;
    out.block = index;
    out.anchor = block.anchor;
    out.hasButton = block.buttonLabel != nullptr;
    if (out.hasButton)
    {
      out.button =
          UiRect{panel.x + pad * 0.5f, panel.Bottom() - pad * 0.5f - buttonHeight, panel.width - pad, buttonHeight};
    }

    ++laid;
    pen += height + gap;
  }

  return laid;
}

const LocationBlockLayout* HitLocationBlockButton(std::span<const LocationBlockLayout> _layouts, float _x,
                                                  float _y) noexcept
{
  for (const LocationBlockLayout& layout : _layouts)
  {
    if (layout.hasButton && layout.button.Contains(_x, _y))
    {
      return &layout;
    }
  }
  return nullptr;
}

} // namespace Neuron
