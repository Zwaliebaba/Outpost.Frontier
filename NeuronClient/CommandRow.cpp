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
   * The selected command's parameter button. Only the selected command gets
   * one, because the parameter *values* come from `OrderOptions(kind)` and the
   * caller has asked for exactly one kind's -- five dropdowns would need five
   * asks a frame.
   *
   * **Its slot is fixed, and that took a bug to get right.** The chip used to
   * be held until "the next command with a parameter of its own", which
   * reproduced the print (`MOVE ATTACK FORMATION STANCE ABILITIES`) only
   * because Move was once the only early verb that had one. Warp and Dock now
   * vary by formation too (ADR-018 D7 makes Dock's radius a function of the
   * solved formation), so the chip started landing after *whichever of the
   * three was selected* -- and clicking WARP or DOCK shifted every button to
   * its right.
   *
   * That is precisely the failure this row's own header forbids: the sectors
   * keep fixed positions "so the ring stays learnable as a shape rather than a
   * lookup", and a row whose buttons move under the player is the same mistake
   * in a line. A player reaching for DOCK must not find MINE there because
   * their last click changed what the row looks like.
   *
   * So the slot is a property of the **kind list** and of nothing else: the
   * chip goes at the door of the picker cluster, which is the first command
   * after the leading one that carries a parameter name. What the chip *says*
   * still follows the selection; where it sits does not.
   */
  std::size_t chipSlot = _kinds.size();
  {
    std::size_t index = 0;
    for (const OrderKindOption& kind : _kinds)
    {
      if (kind.name == nullptr)
      {
        break;
      }
      if (index > 0 && kind.parameterName != nullptr)
      {
        chipSlot = index;
        break;
      }
      ++index;
    }
  }

  const OrderKindOption* pendingParameter = nullptr;
  bool chipPlaced = false;

  /*
   * `_kind` is null when the armed command varies by nothing -- Attack takes a
   * target rather than a parameter, and says so with a null name.
   *
   * **The slot is still filled**, with a dash and nothing to press. Leaving it
   * empty would move every button to its right the moment such a command was
   * armed, which is the whole defect this layout was fixed for; and the dash is
   * this HUD's existing word for an absent value, the one the roster draws for
   * a wing with no ships. It is punctuation rather than vocabulary, so the
   * engine is not naming anything the game did not.
   */
  const auto emitParameter = [&](const OrderKindOption* _kind) {
    CommandButton& parameter = _outButtons[count];
    parameter = CommandButton{};
    parameter.rect = UiRect{pen, top, width, height};
    parameter.label = _kind != nullptr ? _kind->parameterName : "-";
    parameter.action = CommandAction::CycleParameter;
    parameter.payload = _kind != nullptr ? _kind->kind : _selectedKind;
    parameter.opensPicker = _kind != nullptr;

    // The same predicate, plus something to cycle *to*: one option is a
    // constant rather than a choice, and a button that visibly does nothing
    // when pressed is worse than one that is visibly not for pressing.
    parameter.enabled = _kind != nullptr && _kind->available && _context.hasSelection && _options.size() > 1;
    if (_kind != nullptr && _optionIndex < _options.size())
    {
      parameter.value = _options[_optionIndex].name;
    }
    ++count;
    pen += width + gap;
  };

  // Which command's parameter the chip will show. Resolved before the row is
  // laid out, because the chip's slot comes before the selected command as
  // often as after it.
  for (const OrderKindOption& kind : _kinds)
  {
    if (kind.name == nullptr)
    {
      break;
    }
    if (kind.kind == _selectedKind && kind.parameterName != nullptr)
    {
      pendingParameter = &kind;
      break;
    }
  }

  std::size_t kindIndex = 0;
  for (const OrderKindOption& kind : _kinds)
  {
    if (kind.name == nullptr)
    {
      break;
    }

    // The chip goes in its fixed slot, whoever is selected and whether or not
    // that command has anything to vary by.
    if (kindIndex == chipSlot)
    {
      if (count >= _outButtons.size() || pen + width > right)
      {
        return count; // No room. Dropped rather than shrunk: see the header.
      }
      emitParameter(pendingParameter);
      chipPlaced = true;
    }
    ++kindIndex;

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
    button.action = CommandAction::SelectKind;
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
  }

  // A kind list whose picker cluster never opened -- one command, or none with
  // a parameter after the first -- still gets its chip, at the end.
  // `count > 0` because a row with no commands has no cluster to open and
  // nothing to vary: a chip alone on an empty row would be a control for a verb
  // that is not there.
  if (!chipPlaced && count > 0 && count < _outButtons.size() && pen + width <= right)
  {
    emitParameter(pendingParameter);
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
