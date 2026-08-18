#pragma once

#include "OrderIntent.h"
#include "UiDrawList.h"

#include <cstdint>
#include <span>

/*
 * The command row across the bottom (`tactical-hud.png`, ADR-006 §10).
 *
 * `MOVE | ATTACK | FORMATION | STANCE | ABILITIES`, with the commands this
 * build has no content for drawn greyed rather than hidden. The row is a
 * *layout and a hit test* and nothing else: which buttons exist and what they
 * are called comes from the game through `WorldView::OrderKinds`, and what
 * pressing one does is the caller's business.
 *
 * **The parameter button is not a command.** `FORMATION` sits in the row beside
 * the verbs and is a different kind of thing -- the name of what the selected
 * command varies by, with the current choice under it. The print gives it a
 * dropdown caret for exactly that reason. It is placed immediately after the
 * command it belongs to, so the pair reads as one control.
 *
 * **Device-free, and that is what makes the row clickable at all.** A button is
 * a rect and a decision; putting both here means the hit test and the drawing
 * cannot disagree about where the button is, which is the failure mode of a
 * HUD that lays out in the renderer and hit-tests in the input handler.
 */

namespace Neuron
{

/// What a button does when it is pressed. The row reports it; the caller acts.
enum class CommandAction : std::uint8_t
{
  /// Make this the command the puck will issue. `payload` is the kind.
  SelectKind = 0,

  /// Step the selected command's parameter to its next value. `payload` is the
  /// kind the parameter belongs to.
  CycleParameter = 1
};

/// One laid-out button.
struct CommandButton
{
  UiRect rect;

  /// The word on it, from the game. Never null in a slot the row reports.
  const char* label = nullptr;

  /// The second line, for the parameter button: the currently chosen value's
  /// name. Null on a command button, which has one line.
  const char* value = nullptr;

  CommandAction action = CommandAction::SelectKind;
  std::uint16_t payload = 0;

  /// Drawn greyed and refused by the hit test. A command with no content, or a
  /// parameter button whose command has no options.
  bool enabled = false;

  /// The command the puck is currently set to. Exactly one command button is
  /// this, and the parameter button never is.
  bool active = false;
};

/// Sizes at scale 1.0, read off the print.
struct CommandRowTuning
{
  float buttonWidth = 132.0f;
  float buttonHeight = 44.0f;
  float buttonGap = 8.0f;

  /// Inset from the row's left edge and from its top, so the buttons sit inside
  /// the zone rather than filling it -- the print leaves air above them.
  float paddingX = 12.0f;
  float paddingY = 9.0f;

  std::uint8_t labelSizeIndex = 1;
  std::uint8_t valueSizeIndex = 0;
};

/*
 * How many buttons a row can hold: every command, plus one parameter button.
 *
 * The parameter button is `+ 1` rather than sharing a slot because it is not a
 * command -- a build whose game offered eight commands would still want to name
 * the parameter of whichever one is selected.
 */
inline constexpr std::uint32_t MAX_COMMAND_BUTTONS = MAX_ORDER_KINDS + 1;

/*
 * Lays the row out.
 *
 * `_selectedKind` is the command the puck will issue, `_options` are that
 * command's parameter values and `_optionIndex` which is chosen -- the same
 * three the caller already holds to build an intent.
 *
 * Buttons that would run past the row's right edge are dropped rather than
 * shrunk or wrapped: a HUD that reflowed at some window width would be a layout
 * engine, which is exactly what R9 says this pass must not become. The count
 * returned is what fitted.
 */
[[nodiscard]] std::uint32_t BuildCommandRow(std::span<const OrderKindOption> _kinds, std::uint16_t _selectedKind,
                                            std::span<const OrderOption> _options, std::uint32_t _optionIndex,
                                            const UiRect& _row, float _scale, const CommandRowTuning& _tuning,
                                            std::span<CommandButton> _outButtons);

/*
 * Which button a click at these pixels landed on, or null.
 *
 * Disabled buttons return null rather than themselves: a caller that had to
 * check `enabled` after asking would eventually forget, and a greyed ATTACK
 * that silently switched the puck to an unsubmittable command is a fleet that
 * stops responding with no message.
 */
[[nodiscard]] const CommandButton* HitCommandRow(std::span<const CommandButton> _buttons, float _x, float _y) noexcept;

} // namespace Neuron
