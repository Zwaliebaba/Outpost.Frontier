#include "pch.h"

#include "StationScreen.h"

#include <algorithm>

namespace Neuron
{
namespace
{

[[nodiscard]] float SafeScale(float _scale) noexcept
{
  return _scale > 0.0f ? _scale : 1.0f;
}

/// The monospace cell, the same 8 px at 1.0 the HUD measures its text with.
/// Here rather than passed in, because a tab sized against a different cell
/// than the one it is drawn with would be a tab whose word does not fit.
[[nodiscard]] float Cell(float _scale) noexcept
{
  return 8.0f * _scale;
}

} // namespace

StationScreenLayout ResolveStationScreen(std::uint32_t _viewportWidth, std::uint32_t _viewportHeight, float _scale,
                                         const StationScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const auto width = static_cast<float>(_viewportWidth);
  const auto height = static_cast<float>(_viewportHeight);

  StationScreenLayout layout;
  layout.scale = scale;
  layout.viewport = UiRect{0.0f, 0.0f, width, height};

  const float statusHeight = std::min(_tuning.statusBarHeight * scale, height);
  layout.statusBar = UiRect{0.0f, 0.0f, width, statusHeight};

  const float tabHeight = std::min(_tuning.tabRowHeight * scale, std::max(0.0f, height - statusHeight));
  layout.tabRow = UiRect{0.0f, statusHeight, width, tabHeight};

  const float contentTop = statusHeight + tabHeight;
  const float contentHeight = std::max(0.0f, height - contentTop);

  // The composer takes its column and the roster takes what is left. Clamped so
  // a narrow window shrinks the composer rather than giving the roster a
  // negative width.
  const float composerWidth = std::min(_tuning.composerWidth * scale, width);
  layout.composer = UiRect{width - composerWidth, contentTop, composerWidth, contentHeight};
  layout.roster = UiRect{0.0f, contentTop, std::max(0.0f, width - composerWidth), contentHeight};

  /*
   * The composer's controls, top to bottom, in the print's order: what is
   * selected, the formation it will leave in, the action, the promise the
   * action makes, and the wing assignment under it.
   *
   * Laid out from the top for everything above the diagram and from the bottom
   * for everything below it, so the diagram absorbs the slack. It is the only
   * thing here that can be any size -- every control has a height a finger
   * needs, and a readout does not.
   */
  const float padX = _tuning.composerPaddingX * scale;
  const float padY = _tuning.composerPaddingY * scale;
  const float innerX = layout.composer.x + padX;
  const float innerWidth = std::max(0.0f, layout.composer.width - 2.0f * padX);
  const float rowHeight = _tuning.rowHeight * scale;
  const float rowGap = _tuning.rowGap * scale;

  float pen = layout.composer.y + padY + _tuning.headingHeight * scale;
  layout.selectionHead = UiRect{innerX, pen, innerWidth, rowHeight};
  pen += rowHeight + rowGap;
  layout.formation = UiRect{innerX, pen, innerWidth, rowHeight};
  pen += rowHeight + rowGap;
  layout.undock = UiRect{innerX, pen, innerWidth, _tuning.undockHeight * scale};
  pen += _tuning.undockHeight * scale + rowGap;
  const float diagramTop = pen;

  // Up from the bottom: NEW WING, then the wing dropdown above it.
  float floorPen = layout.composer.Bottom() - padY;
  layout.newWing = UiRect{innerX, floorPen - rowHeight, innerWidth, rowHeight};
  floorPen -= rowHeight + rowGap;
  layout.assignWing = UiRect{innerX, floorPen - rowHeight, innerWidth, rowHeight};
  floorPen -= rowHeight + _tuning.headingHeight * scale + rowGap;

  /*
   * The diagram, letterboxed into what is left (ADR-020 §7).
   *
   * Square, and centred in the slack rather than stretched to fill it: the
   * print calls it a readout of ADR-017 §4's deterministic scan, and a ring
   * system drawn out of round is a readout of something else.
   */
  const float diagramRoom = std::max(0.0f, floorPen - diagramTop);
  const float aspect = _tuning.parkingAspect > 0.0f ? _tuning.parkingAspect : 1.0f;

  // Widest that fits the column, and no taller than the room: taking the
  // narrower of the two constraints is the whole of letterboxing, and picking
  // it here means the height needs no second clamp.
  const float diagramWidth = std::min(innerWidth, diagramRoom * aspect);
  const float diagramHeight = diagramWidth / aspect;

  layout.parking = UiRect{innerX + (innerWidth - diagramWidth) * 0.5f,
                          diagramTop + (diagramRoom - diagramHeight) * 0.5f, diagramWidth, diagramHeight};

  return layout;
}

std::uint32_t BuildStationTabRow(std::span<const StationTab> _tabs, const UiRect& _row, float _scale,
                                 const StationScreenTuning& _tuning, std::span<StationTabButton> _outButtons)
{
  const float scale = SafeScale(_scale);
  const float cell = Cell(scale);
  const float gap = _tuning.tabGap * scale;
  const float inset = _tuning.tabInsetX * scale;
  const float padding = _tuning.tabPaddingCells * cell;

  const float right = _row.Right() - inset;
  float pen = _row.x + inset;
  std::uint32_t laid = 0;

  for (std::uint32_t index = 0; index < _tabs.size() && laid < _outButtons.size(); ++index)
  {
    const StationTab& tab = _tabs[index];

    // Sized to its own word, plus the tag when it carries one -- REPAIR reads
    // "REPAIR FREE" and has to have room for both.
    const float nameWidth = static_cast<float>(TextCellCount(tab.name != nullptr ? tab.name : "")) * cell;
    const float tagWidth = tab.tag != nullptr ? (static_cast<float>(TextCellCount(tab.tag)) + 1.0f) * cell : 0.0f;
    const float tabWidth = nameWidth + tagWidth + 2.0f * padding;

    if (pen + tabWidth > right)
    {
      // Dropped rather than shrunk or wrapped. The row is a fixed set of
      // siblings and a tab that reflowed would move under a player's finger
      // when a service landed.
      break;
    }

    StationTabButton& button = _outButtons[laid];
    button = StationTabButton{};
    button.rect = UiRect{pen, _row.y, tabWidth, _row.height};
    button.tab = index;
    button.id = tab.id;
    button.enabled = tab.enabled;

    ++laid;
    pen += tabWidth + gap;
  }

  return laid;
}

const StationTabButton* HitStationTab(std::span<const StationTabButton> _buttons, float _x, float _y) noexcept
{
  for (const StationTabButton& button : _buttons)
  {
    if (button.enabled && button.rect.Contains(_x, _y))
    {
      return &button;
    }
  }
  return nullptr;
}

std::uint32_t StationVisibleRows(const UiRect& _panel, float _scale, const StationScreenTuning& _tuning) noexcept
{
  const float scale = SafeScale(_scale);
  const float pitch = (_tuning.chipHeight + _tuning.chipGap) * scale;
  const float chipHeight = _tuning.chipHeight * scale;
  if (!(pitch > 0.0f))
  {
    return 0;
  }

  const float room = _panel.height - 2.0f * _tuning.rosterPaddingY * scale - _tuning.columnHeaderHeight * scale;
  if (room < chipHeight)
  {
    return 0;
  }

  /*
   * Rows fit on the chip, not on the pitch: the last one needs no gap under it.
   * `room / pitch` is the obvious spelling and it is wrong by one whenever the
   * slack at the bottom is at least a chip tall but short of a chip and a gap
   * -- and wrong in the direction that matters, because the scroll would then
   * believe in a row `BuildStationColumns` had already drawn.
   */
  return 1u + static_cast<std::uint32_t>((room - chipHeight) / pitch);
}

std::uint32_t StationColumnRows(std::span<const StationChip> _chips, std::uint32_t _groupCount) noexcept
{
  if (_groupCount == 0)
  {
    return 0;
  }

  // The tallest column is what the scroll is over: scrolling per column would
  // let two wings drift out of step and put a chip beside the wrong header.
  std::uint32_t tallest = 0;
  for (std::uint32_t group = 0; group < _groupCount; ++group)
  {
    std::uint32_t rows = 0;
    for (const StationChip& chip : _chips)
    {
      rows += chip.group == group ? 1u : 0u;
    }
    tallest = std::max(tallest, rows);
  }
  return tallest;
}

StationRosterCounts BuildStationColumns(std::span<const StationGroup> _groups, std::span<const StationChip> _chips,
                                        const UiRect& _panel, std::uint32_t _scrollRows, float _scale,
                                        const StationScreenTuning& _tuning,
                                        std::span<StationColumnRect> _outColumns,
                                        std::span<StationChipRect> _outChips)
{
  StationRosterCounts counts;

  const float scale = SafeScale(_scale);
  const float padX = _tuning.rosterPaddingX * scale;
  const float padY = _tuning.rosterPaddingY * scale;
  const float columnWidth = _tuning.columnWidth * scale;
  const float columnGap = _tuning.columnGap * scale;
  const float headerHeight = _tuning.columnHeaderHeight * scale;
  const float chipHeight = _tuning.chipHeight * scale;
  const float chipPitch = chipHeight + _tuning.chipGap * scale;

  const float right = _panel.Right() - padX;
  const float bottom = _panel.Bottom() - padY;
  float pen = _panel.x + padX;

  for (std::uint32_t group = 0; group < _groups.size() && counts.groups < _outColumns.size(); ++group)
  {
    if (pen + columnWidth > right)
    {
      // No room for another column. Dropped whole rather than half-drawn: a
      // column clipped down its middle reads as a chip list that ran out.
      break;
    }

    StationColumnRect& column = _outColumns[counts.groups];
    column = StationColumnRect{};
    column.header = UiRect{pen, _panel.y + padY, columnWidth, headerHeight};
    column.group = group;
    ++counts.groups;

    // The chips of this wing, in the order the game sorted them, minus the rows
    // scrolled past.
    const float chipsTop = column.header.Bottom();
    std::uint32_t row = 0;
    for (std::uint32_t index = 0; index < _chips.size(); ++index)
    {
      if (_chips[index].group != group)
      {
        continue;
      }

      const std::uint32_t thisRow = row;
      ++row;

      if (thisRow < _scrollRows)
      {
        continue; // Above the visible span.
      }
      if (counts.chips == _outChips.size())
      {
        break;
      }

      const float top = chipsTop + static_cast<float>(thisRow - _scrollRows) * chipPitch;
      if (top + chipHeight > bottom)
      {
        break; // Whole rows only: a chip straddling the edge is not emitted.
      }

      StationChipRect& laid = _outChips[counts.chips];
      laid = StationChipRect{};
      laid.rect = UiRect{pen, top, columnWidth, chipHeight};
      laid.chip = index;
      ++counts.chips;
    }

    pen += columnWidth + columnGap;
  }

  return counts;
}

const StationChipRect* HitStationChip(std::span<const StationChipRect> _chips, float _x, float _y) noexcept
{
  for (const StationChipRect& chip : _chips)
  {
    if (chip.rect.Contains(_x, _y))
    {
      return &chip;
    }
  }
  return nullptr;
}

const StationColumnRect* HitStationColumnHeader(std::span<const StationColumnRect> _columns, float _x,
                                                float _y) noexcept
{
  for (const StationColumnRect& column : _columns)
  {
    if (column.header.Contains(_x, _y))
    {
      return &column;
    }
  }
  return nullptr;
}

} // namespace Neuron
