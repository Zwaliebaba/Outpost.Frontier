#include "pch.h"

#include "CommandRow.h"

namespace Neuron
{

std::uint32_t BuildCommandRow(std::span<const OrderKindOption> _kinds, std::uint16_t _selectedKind,
                              std::span<const OrderOption> _options, std::uint32_t _optionIndex, const UiRect& _row,
                              float _scale, const CommandRowTuning& _tuning, std::span<CommandButton> _outButtons)
{
  const float scale = _scale > 0.0f ? _scale : 1.0f;
  const float width = _tuning.buttonWidth * scale;
  const float height = _tuning.buttonHeight * scale;
  const float gap = _tuning.buttonGap * scale;

  const float top = _row.y + _tuning.paddingY * scale;
  const float right = _row.Right() - _tuning.paddingX * scale;
  float pen = _row.x + _tuning.paddingX * scale;

  std::uint32_t count = 0;
  for (const OrderKindOption& kind : _kinds)
  {
    if (count >= _outButtons.size() || kind.name == nullptr)
    {
      break;
    }
    if (pen + width > right)
    {
      break; // No room. Dropped rather than shrunk: see the header.
    }

    const bool selected = kind.kind == _selectedKind;

    CommandButton& button = _outButtons[count];
    button = CommandButton{};
    button.rect = UiRect{pen, top, width, height};
    button.label = kind.name;
    button.action = CommandAction::SelectKind;
    button.payload = kind.kind;
    button.enabled = kind.available;
    button.active = selected && kind.available;
    ++count;
    pen += width + gap;

    /*
     * The selected command's parameter button, immediately after it.
     *
     * Only the selected one gets a parameter button, because the parameter
     * *values* come from `OrderOptions(kind)` and the caller has asked for
     * exactly one kind's. Drawing a FORMATION button beside a command the
     * player has not selected would mean asking the game for every kind's
     * options every frame to fill in the current value -- and would put five
     * dropdowns in a row that has space for five buttons.
     */
    if (!selected || kind.parameterName == nullptr)
    {
      continue;
    }
    if (count >= _outButtons.size() || pen + width > right)
    {
      break;
    }

    CommandButton& parameter = _outButtons[count];
    parameter = CommandButton{};
    parameter.rect = UiRect{pen, top, width, height};
    parameter.label = kind.parameterName;
    parameter.action = CommandAction::CycleParameter;
    parameter.payload = kind.kind;

    // Enabled only when there is something to cycle *to*. One option is a
    // constant, not a choice, and a button that visibly does nothing when
    // pressed is worse than one that is visibly not for pressing.
    parameter.enabled = kind.available && _options.size() > 1;
    if (_optionIndex < _options.size())
    {
      parameter.value = _options[_optionIndex].name;
    }
    ++count;
    pen += width + gap;
  }
  return count;
}

const CommandButton* HitCommandRow(std::span<const CommandButton> _buttons, float _x, float _y) noexcept
{
  for (const CommandButton& button : _buttons)
  {
    if (button.enabled && button.rect.Contains(_x, _y))
    {
      return &button;
    }
  }
  return nullptr;
}

} // namespace Neuron
