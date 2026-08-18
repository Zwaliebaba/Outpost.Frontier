#pragma once

#include <cstdint>

/*
 * The fleet roster, as the game hands it over (`tactical-hud.png`, ADR-014).
 *
 * The print draws a column of rows down the left: a name, a count, and a pair
 * of strips for the group's health. The engine draws exactly that and learns
 * nothing from it -- **a row is a name and four numbers**, and the word for
 * what a row *is* never crosses the seam. This game means a wing; another game
 * on these libraries can mean a squad, a platoon or a convoy, and the pass that
 * draws the panel does not change.
 *
 * **Why the game aggregates rather than the engine.** The engine has the
 * replicated entities and could group them by `EntityRecord::groupId` itself --
 * it is one byte and the field is right there. It must not: it would then have
 * decided that groups are worth showing, that they are named, that the two
 * gauges average rather than take a minimum, and that a group with no members
 * disappears rather than showing as empty. Every one of those is a design
 * question about *this* game, and the seam exists so that they are answered on
 * the side that is allowed to answer them (ADR-014 §2c).
 */

namespace Neuron
{

/// One row. `name` points at storage the world view owns and is valid for its
/// lifetime -- in practice a string the composition root loaded once, the same
/// arrangement `OrderOption::name` and `ReasonText` have.
struct RosterRow
{
  const char* name = nullptr;

  /// Opaque, and echoed rather than read: it is what a click on the row would
  /// hand back to the game, once rows are clickable.
  std::uint16_t groupId = 0;

  std::uint16_t shipCount = 0;

  /// How many of them the player currently has selected. The print highlights
  /// a row that the selection covers, and this is the only thing the engine
  /// needs in order to decide that -- the alternative is the engine matching
  /// selected ids against group membership, which is the aggregation it is not
  /// supposed to be doing.
  std::uint16_t selectedCount = 0;

  /// 0-255, not a percentage -- `EntityRecord`'s own scale, so a full group is
  /// 255 and nothing has to remember a divide by a hundred.
  std::uint8_t hullGauge = 0;
  std::uint8_t shieldGauge = 0;
};

/*
 * How many rows a client will ask for.
 *
 * The print draws eight and a "8/8" footer that implies scrolling past that.
 * Sixteen is twice what the sheet shows and still a fixed array on the stack;
 * a fleet organised into more groups than this wants a scrolling roster, which
 * is a surface rather than a bigger number.
 */
inline constexpr std::uint32_t MAX_ROSTER_ROWS = 16;

} // namespace Neuron
