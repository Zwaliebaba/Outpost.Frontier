#pragma once

#include "UiDrawList.h"

#include <cstdint>

/*
 * "There are more ships here than you are being shown" (`tactical-icon-system.png`
 * §6, ADR-022 §5d, U3d-c).
 *
 * The icon system's density ladder ends at a **counted chip**: at the strategic
 * tier a merged group stops being hulls and becomes *"an extent outline plus a
 * count -- the honest aggregate. No hull is drawn at a position the client
 * cannot justify, and the chip says exactly what is known: how many, roughly
 * where."*
 *
 * This is that rung, and U3d-c is its first caller. It is built here rather
 * than found: ADR-022 §5d and the build order both say `culledCount` *"renders
 * through the icon ladder's existing counted-chip rung"*, and the rung did not
 * exist -- the density ladder is not built, so there was nothing to reuse.
 *
 * **What this chip counts has no position at all, and that is the one place it
 * departs from the print.** A density merge knows where its group is and draws
 * an extent; a culled entity is one the server *did not send*, so the client
 * has its count and nothing else -- not a position, not an extent, not even a
 * bearing. Drawing it on the plane would be inventing exactly the "position the
 * client cannot justify" the print forbids in the same sentence. So it is a
 * **screen-space statement about the feed**, sited with the other readouts that
 * are about the connection rather than about the world (`StaleMarker`'s
 * reasoning, one level up).
 *
 * **It never says "empty".** That is the whole rule ADR-022 §5d states: *"The
 * player is never told a grid is empty when it is not; they are told how many
 * they are not being shown."* A zero count draws nothing, because nothing is
 * being withheld and a chip reading zero is a chip that has to be read.
 *
 * **And it is never the player's own fleet.** ADR-022 §5a guarantees owned and
 * selected ships are never culled -- it is a correctness requirement, not a
 * quality-of-service one, because a culled owned ship is one the client cannot
 * pre-check an order for. So a player seeing this chip knows without being told
 * that the hulls behind it are somebody else's.
 *
 * **Device-free**, so the sentence and the geometry are both testable before
 * anyone has looked at a screen.
 */

namespace Neuron
{

/*
 * The largest count the wire can carry.
 *
 * `DeltaHeader.culledCount` is a `u16`, so this is a fact about the protocol
 * rather than a choice made here -- and it is what sizes the buffer the text
 * needs. A grid cannot hold more than 1,024 entities anyway (ADR-018 D4), so
 * the header's range is generous by a factor of sixty and the chip will never
 * see four digits in practice.
 */
inline constexpr std::uint32_t MAX_COUNTED_CHIP_VALUE = 65535;

/// `+65535 NOT SHOWN` and a terminator, which is the longest this can be.
inline constexpr std::size_t COUNTED_CHIP_TEXT_BYTES = 24;

/// Sizes at scale 1.0.
struct CountedChipTuning
{
  /// Air inside the chip, around its word.
  float paddingX = 10.0f;
  float paddingY = 6.0f;

  /// And air between the chip and the corner of the zone it sits in.
  float insetX = 14.0f;
  float insetY = 14.0f;

  /*
   * The chip's own height, which is *not* the 48 px target floor.
   *
   * ADR-020 §8's floor governs what a finger must be able to **hit**, and this
   * is a readout -- nothing presses it, so sizing it as a target would take a
   * finger's worth of the world zone to say a number. The floor's own sentence
   * is about touch targets; a chip is the same class of thing as the stale
   * marker, which is also drawn and never pressed.
   */
  float height = 26.0f;
};

/*
 * The sentence, into `_buffer`, returning it.
 *
 * Empty for a zero count -- see the header note: silence is correct when
 * nothing is being withheld, and it is the caller's cue to draw nothing.
 *
 * `+` because the count is *in addition to* what is on screen rather than a
 * total. A player who reads `12 NOT SHOWN` beside eight visible hulls should
 * not have to work out whether twelve includes the eight.
 */
[[nodiscard]] const char* CountedChipText(std::uint32_t _culled, char* _buffer, std::size_t _bufferBytes) noexcept;

/*
 * Where the chip goes inside a zone: its bottom-left corner, inset.
 *
 * Bottom-left of the world zone rather than a corner of the viewport, because
 * the statement is about *this view* and the HUD is a border around it -- and
 * bottom-left specifically because the sheet's other corners are spoken for
 * (toasts stack bottom-right, the critical toast is centre-top, and the top bar
 * is the session's).
 *
 * `_textCells` is how many monospace cells the word occupies, which the caller
 * has already measured with the same `TextCellCount` it will draw with -- so
 * the chip cannot be sized against a different string than the one inside it.
 *
 * A zone too small for the chip gets a clamped rect rather than a negative one,
 * which is `ResolveUiLayout`'s posture and for its reason.
 */
[[nodiscard]] UiRect CountedChipRect(const UiRect& _zone, std::uint32_t _textCells, float _cellWidth, float _scale,
                                     const CountedChipTuning& _tuning) noexcept;

} // namespace Neuron
