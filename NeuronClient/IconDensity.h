#pragma once

#include "HudPalette.h"
#include "UiLayout.h" // TARGET_FLOOR_PIXELS -- the merge grid's size, see below.

#include <DirectXMath.h>

#include <cstdint>
#include <span>

/*
 * §6's density ladder, at its last rung (`tactical-icon-system.png`).
 *
 * *"A glyph never shrinks below its legible floor; it is replaced."* `IconSystem.h`
 * decides which rung a glyph is on; this file is what `Strategic` replaces it
 * **with** -- *"extent outline plus a count -- 04 §6.3's honest aggregate. No
 * hull is drawn at a position the client cannot justify, and the chip says
 * exactly what is known: how many, roughly where."*
 *
 * **This is the rung `CountedChip` said did not exist.** ADR-022 §5d and the
 * universe build order both described the culled count as rendering through
 * *"the icon ladder's existing counted-chip rung"*; U3d-c found there was none,
 * built `CountedChip` as the first one, and recorded that it *could not have
 * been this rung anyway*. That distinction survives and is the reason there are
 * two files rather than one:
 *
 * - `CountedChip` counts entities the **server did not send**. It has a number
 *   and nothing else -- no position, no extent, not even a bearing -- so it is a
 *   screen-space statement about the feed, sited with the stale marker.
 * - `IconCluster` counts entities the client **has** and has chosen not to draw
 *   individually. It knows exactly where they are, so it states an extent on the
 *   plane and is entitled to.
 *
 * A player can see both at once and they mean different things: one says *"there
 * are more ships here than you are being shown"*, the other *"these are the ships
 * you are being shown, drawn as one"*.
 *
 * **A pure function of replicated fields**, which §6 requires in as many words
 * and which is what makes the merge safe to run per frame on every client: it
 * reads positions and a camera scale, takes no clock, draws no randomness, and
 * visits its input in order, so two clients handed the same snapshot produce the
 * same chips.
 *
 * **Device-free.** What the chips say and where they sit is arithmetic; what a
 * GPU has to confirm is that the outline and the count read as one statement.
 */

namespace Neuron
{

/*
 * One entity offered to the merge.
 *
 * Deliberately not a `SceneEntity`: the merge needs three facts and taking the
 * whole record would tie this file to the shape of the render seam, which is a
 * dependency in the wrong direction for something the tests want to drive with
 * hand-written rows.
 */
struct IconMergeCandidate
{
  DirectX::XMFLOAT2 planeMetres{};

  /*
   * Which chip this entity may join.
   *
   * The merge never mixes standings, and it is the plate that says so: it draws
   * `▲ 84` and `× 137` as two chips over overlapping ground rather than one
   * chip of 221. A count that spanned both would answer *"how many ships"* --
   * a question nobody in a fight is asking.
   */
  StandingColour standing = StandingColour::Neutral;

  /*
   * This entity is drawn as itself whatever the tier says.
   *
   * **The policy is the caller's and the mechanism is here**, which is the same
   * split `OverlayTuning::statusMarkBits` makes: this library holds a flag and
   * `Outpost.exe` holds the sentence that sets it. Two things set it today, and
   * they are set for different reasons:
   *
   * - **A flagged hull**, because §3 says so outright: *"the de-clutter merge
   *   never folds a flagged entity into a chip -- a flagged hull always survives
   *   as its own glyph."* The information is deliberately not withheld at the
   *   one moment it decides whether to shoot.
   * - **The player's own selection**, on ADR-022 §5a's argument rather than the
   *   plate's: a selected ship the client cannot place is a ship it cannot
   *   pre-check an order for, and an order surface opened over a chip has
   *   nothing to aim at. §5a makes the same guarantee one layer down for
   *   culling, and for the same reason.
   */
  bool alwaysDrawn = false;
};

/*
 * One merged group: *how many, roughly where*.
 *
 * `halfExtentMetres` is the group's own bounding box rather than a radius, which
 * is the honest shape for a fleet: a line formation is long and thin, and a
 * circle drawn around it would claim ground the ships are nowhere near. It is
 * the same correction `OverlayTuning::puckRadiusPixels` records for the puck.
 */
struct IconCluster
{
  DirectX::XMFLOAT2 centreMetres{};
  DirectX::XMFLOAT2 halfExtentMetres{};
  std::uint32_t count = 0;
  StandingColour standing = StandingColour::Neutral;
};

struct IconDensityTuning
{
  /*
   * How coarse the merge is, in pixels of screen.
   *
   * **The 48 px target floor, and derived rather than picked.** A chip is a
   * thing a player presses -- it is how they ask what is inside it -- so the
   * smallest group worth making a chip of is one whose chip a finger can hit.
   * Merging finer than that produces two chips a finger cannot separate, which
   * is the failure ADR-020 §8's floor exists to prevent, arriving through a
   * different door.
   *
   * It is deliberately a *different* floor from `IconTuning`'s 20 px, and the
   * plate is what separates them: *"U2 governs what a finger must be able to
   * hit ... this is what an eye must be able to tell apart."* The glyph floor is
   * read; the merge grid is pressed. Two measurements, two numbers, neither
   * derivable from the other.
   */
  float mergeCellPixels = TARGET_FLOOR_PIXELS;
};

/*
 * What one merge did.
 *
 * `dropped` is the no-silent-caps clause: a caller whose spans were too small
 * gets told how many entities went nowhere rather than a screen that quietly
 * omits them. Size `_outClusters` and `_outSurvivors` at the candidate count and
 * it is always zero, which is what `ClientApp` does -- a cluster cannot outnumber
 * the entities in it.
 */
struct IconMergeResult
{
  std::uint32_t clusters = 0;
  std::uint32_t survivors = 0;
  std::uint32_t dropped = 0;
};

/*
 * Fold the candidates into chips.
 *
 * `_outSurvivors` receives **indices into `_candidates`** rather than copies:
 * the caller has the entity beside the candidate and wants to draw the real
 * glyph, so handing back a position would make it match the two up again.
 *
 * A group of one is never a chip. It is returned as a survivor instead, because
 * a chip reading `1` has replaced a glyph with a worse drawing of the same fact
 * -- the merge is only a gain where it is actually aggregating.
 *
 * Visits `_candidates` in order and appends in the order it first sees a group,
 * so the chip list is stable frame to frame for a stable input. A merge that
 * reordered its output would make the chips flicker between two arrangements
 * that are both correct.
 */
[[nodiscard]] IconMergeResult MergeIcons(std::span<const IconMergeCandidate> _candidates, float _metresPerPixel,
                                         const IconDensityTuning& _tuning, std::span<IconCluster> _outClusters,
                                         std::span<std::uint32_t> _outSurvivors) noexcept;

/// `<marker> 65535` and a terminator, which is the longest a chip can be: a grid
/// holds at most 1,024 entities (ADR-018 D4), so five digits is generous by a
/// factor of sixty and the chip will never see four in practice.
inline constexpr std::size_t ICON_CLUSTER_TEXT_BYTES = 16;

/*
 * The chip's sentence, into `_buffer`, returning it.
 *
 * `▲ 84` and `× 137` are the plate's own two chips. The other two standings take
 * `●` and `○`, which is this slice's decision rather than the sheet's: the plate
 * draws own and hostile because those are the two a screenshot needs, and a chip
 * carrying **only** a colour would be the one place in this system that breaks
 * §1's rule that *"colour is never the only carrier of a state"*. A filled disc
 * for allied and a hollow one for neutral keep the marker channel closed over
 * all four, in glyphs the atlas already bakes.
 *
 * Empty for a zero count, on `CountedChipText`'s rule: a chip of nothing is a
 * chip that has to be read before it can be ignored.
 */
[[nodiscard]] const char* IconClusterText(StandingColour _standing, std::uint32_t _count, char* _buffer, std::size_t _bufferBytes) noexcept;

} // namespace Neuron
