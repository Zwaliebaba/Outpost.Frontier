#include "pch.h"

#include "CommandRow.h"

#include <algorithm>

namespace Neuron
{

std::uint32_t BuildCommandRow(std::span<const OrderKindOption> _kinds, std::uint16_t _selectedKind,
                              std::span<const OrderOption> _options, std::uint32_t _optionIndex,
                              const CommandContext& _context, const UiRect& _row, float _scale,
                              const CommandRowTuning& _tuning, std::span<CommandButton> _outButtons)
{
  const float scale = _scale > 0.0f ? _scale : 1.0f;
  const float width = _tuning.buttonWidth * scale;

  // The U2 floor: a verb never resolves under its 1.0x height, whatever the
  // scale control says. Below 1.0x the clamp wins and the top inset gives way
  // instead -- the button centres in the air the zone still has.
  const float height = std::max(_tuning.buttonHeight * scale, _tuning.buttonHeight);
  const float gap = _tuning.buttonGap * scale;

  const float top =
      _row.y + std::min(_tuning.paddingY * scale, std::max(0.0f, (_row.height - height) * 0.5f));
  const float right = _row.Right() - _tuning.paddingX * scale;
  float pen = _row.x + _tuning.paddingX * scale;

  std::uint32_t count = 0;

  /*
   * The selected command's parameter button, once the row reaches the picker
   * cluster. Only the selected command gets one, because the parameter *values*
   * come from `OrderOptions(kind)` and the caller has asked for exactly one
   * kind's -- five dropdowns would need five asks a frame. It is *deferred*
   * rather than emitted beside its command: the print keeps the immediate
   * verbs contiguous (`MOVE ATTACK`) and opens the picker cluster with the
   * parameter chip (`FORMATION STANCE ABILITIES`), so the chip is held until
   * the next command that has a parameter name of its own, or the end of the
   * row.
   */
  const OrderKindOption* pendingParameter = nullptr;
  const auto emitParameter = [&](const OrderKindOption& _kind) {
    CommandButton& parameter = _outButtons[count];
    parameter = CommandButton{};
    parameter.rect = UiRect{pen, top, width, height};
    parameter.label = _kind.parameterName;
    parameter.action = CommandAction::CycleParameter;
    parameter.payload = _kind.kind;
    parameter.opensPicker = true;

    // The same predicate, plus something to cycle *to*: one option is a
    // constant rather than a choice, and a button that visibly does nothing
    // when pressed is worse than one that is visibly not for pressing.
    parameter.enabled = _kind.available && _context.hasSelection && _options.size() > 1;
    if (_optionIndex < _options.size())
    {
      parameter.value = _options[_optionIndex].name;
    }
    ++count;
    pen += width + gap;
  };

  for (const OrderKindOption& kind : _kinds)
  {
    if (kind.name == nullptr)
    {
      break;
    }

    // The held parameter chip goes down at the door of the picker cluster --
    // just before the next command that has a parameter of its own.
    if (pendingParameter != nullptr && kind.parameterName != nullptr)
    {
      if (count >= _outButtons.size() || pen + width > right)
      {
        return count; // No room. Dropped rather than shrunk: see the header.
      }
      emitParameter(*pendingParameter);
      pendingParameter = nullptr;
    }

    if (count >= _outButtons.size())
    {
      return count;
    }
    if (pen + width > right)
    {
      return count; // No room. Dropped rather than shrunk: see the header.
    }

    const bool selected = kind.kind == _selectedKind;

    CommandButton& button = _outButtons[count];
    button = CommandButton{};
    button.rect = UiRect{pen, top, width, height};
    button.label = kind.name;
    /*
     * Arm, or act.
     *
     * The one bit the game sent, doing the only thing this pass can do with it.
     * A verb that still needs a place to go makes the press *arm the puck*; a
     * verb the game could already judge from the selection makes the press
     * *issue it*, because there is nothing a second gesture would add and a lit
     * button that then waited would be asking for something the player has
     * nothing left to say.
     */
    button.action = kind.namesDestination ? CommandAction::SelectKind : CommandAction::IssueNow;
    button.payload = kind.kind;
    // The one predicate, for every verb: the game says it has content, and the
    // player has given it something to act on.
    button.enabled = kind.available && _context.hasSelection;
    // A fill means "this mode is engaged", so it implies enabled -- with an
    // empty selection nothing is filled, because there is no mode to be in.
    button.active = selected && button.enabled;
    // A command with a named parameter and no content cannot execute: the only
    // thing selecting it does is open its picker, and the caret says so. An
    // available command's picker is its separate parameter chip.
    button.opensPicker = kind.parameterName != nullptr && !kind.available;
    ++count;
    pen += width + gap;

    if (selected && kind.parameterName != nullptr)
    {
      pendingParameter = &kind;
    }
  }

  // A selected command at the tail of the list still gets its chip.
  if (pendingParameter != nullptr && count < _outButtons.size() && pen + width <= right)
  {
    emitParameter(*pendingParameter);
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
