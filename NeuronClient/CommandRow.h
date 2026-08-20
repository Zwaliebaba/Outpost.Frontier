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
 * **The parameter button is not a command.** `FORMATION` sits in the row and is
 * a different kind of thing -- the name of what the selected command varies by;
 * the chosen value itself is stated by the context bar's summary line. The
 * print gives it a dropdown caret for exactly that reason.
 *
 * **Where it goes is the print's order:** the immediate verbs stay contiguous
 * at the head of the row, and the parameter chip is deferred past any
 * parameterless commands so it opens the picker cluster -- `MOVE ATTACK |
 * FORMATION STANCE ABILITIES`, not `MOVE FORMATION ATTACK ...`. Muscle memory
 * forms early, and ATTACK is second on the print. A parameter whose own
 * command already sits in the picker cluster still lands right beside it,
 * because the next command along has a parameter name too.
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

  /// The currently chosen value's name, for the parameter button; null on a
  /// command button. Reported rather than drawn in the row -- every verb is
  /// one line, and the value is the context bar's summary to state
  /// (`FORMATION` in the row, `FORMATION LINE` in the summary).
  const char* value = nullptr;

  CommandAction action = CommandAction::SelectKind;
  std::uint16_t payload = 0;

  /// Drawn greyed and refused by the hit test. A command with no content, or a
  /// parameter button whose command has no options.
  bool enabled = false;

  /*
   * The verb whose input mode is currently engaged -- the command the puck will
   * issue -- and the only button that gets the filled treatment.
   *
   * **Never merely "enabled".** A fill says *you are in this mode*, so it
   * implies `enabled`: with nothing selected there is no mode to be in and no
   * button is filled. The parameter button is never this either; it names a
   * value rather than a mode.
   */
  bool active = false;

  /*
   * Pressing this opens a picker rather than arming the puck, and the drawing
   * pass hangs the print's `▾` caret on it -- without the caret, nothing
   * distinguishes an immediate verb from one that opens a mode.
   *
   * True for every parameter button, and for a command that has a named
   * parameter but no content: such a command cannot execute, so the only thing
   * selecting it can ever do is put its picker on the row. An *available*
   * command with a parameter (MOVE) stays uncarreted -- its picker is the
   * separate parameter chip, and the verb itself is immediate.
   */
  bool opensPicker = false;
};

/*
 * What the row needs to know beyond the game's own command list, for the one
 * predicate that decides whether a verb is live.
 *
 * **A verb with no subject is a lie about what a tap will do.** Availability
 * alone was not enough: with nothing selected the row still offered a filled
 * MOVE and a full-strength FORMATION, both of which would have done nothing to
 * nobody. Enablement is therefore `available && hasSelection` for every button
 * -- one line, applied uniformly, rather than a case per verb.
 *
 * `available` is still the game's half of it and still gates content (ADR-014
 * §2b: a command this build cannot simulate stays greyed whatever is selected).
 *
 * TODO: ATTACK wants a narrower predicate than the selection -- *a hostile
 * target context exists*, meaning at least one entity in the interest set the
 * selection is permitted to engage. That is not decidable client-side today:
 * `SceneEntity` carries no relationship field, so the client cannot tell a
 * hostile hull from an allied one (the renderer's `teamColorId` is a material
 * channel, not a stance). Until a relationship crosses the seam, ATTACK is
 * gated by the selection like the others and by `available`, which is false for
 * it in this build regardless.
 */
struct CommandContext
{
  /// Whether the player has anything selected for a verb to act on.
  bool hasSelection = false;
};

/// Sizes at scale 1.0, read off the print.
struct CommandRowTuning
{
  float buttonWidth = 132.0f;

  /*
   * 48 is the U2 touch floor, and it is a *floor* rather than only a size:
   * the layout clamps so a verb never resolves under 48 real pixels even at
   * 0.8x scale, because a target a thumb cannot hit is a control that does
   * not exist. The row's zone (64 at 1.0x) holds this plus 8 px above and
   * below; when the clamp wins, the button centres in whatever air is left.
   */
  float buttonHeight = 48.0f;
  float buttonGap = 8.0f;

  /// Inset from the row's left edge and from its top, so the buttons sit inside
  /// the zone rather than filling it -- the print leaves air above them.
  float paddingX = 18.0f;
  float paddingY = 8.0f;

  std::uint8_t labelSizeIndex = 1;
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
                                            const CommandContext& _context, const UiRect& _row, float _scale,
                                            const CommandRowTuning& _tuning, std::span<CommandButton> _outButtons);

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
