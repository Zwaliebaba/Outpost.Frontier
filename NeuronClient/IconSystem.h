#pragma once

#include "HudPalette.h"

#include <cstdint>
#include <span>

/*
 * The tactical icon system (`tactical-icon-system.png`, ADR-009 §6).
 *
 * The sheet's own first sentence is the whole architecture: **four independent
 * channels, never collapsed into one** -- silhouette carries hull class, size
 * carries tonnage, colour carries relationship, and a ring decoration carries
 * the PVP flag. This file is those four channels and the rules that bound them;
 * `IconDensity.h` is §6's ladder over them.
 *
 * **Nothing here names a ship class**, and that is the seam rather than
 * squeamishness. The renderer's mesh table is already a list of anonymous
 * indices the engine draws without knowing which one is a Carrier (ADR-014,
 * `MESH_FOR_HULL` in the composition root), and the glyph table is the same
 * shape: `IconSlot` is an index, `IconGlyph` says what the glyph at that index
 * *looks like*, and the composition root is what says a Battleship takes slot
 * six. That is ADR-018 D14's reading applied one surface further along -- **the
 * game says which class, the engine owns what a class looks like** -- and it is
 * what keeps the eleven-value taxonomy a decision in `Design/` rather than an
 * enum in an engine library.
 *
 * **What this file does not contain, deliberately:**
 *
 * - **The glyph geometry.** §7 authors the set as a one-channel **SDF atlas**,
 *   resident `Critical`, so a single source resolves 12 px to 96 px across every
 *   UI scale without a mip set. That is art plus a GPU, and the pass that
 *   samples it is the one thing here that cannot be tested without a display.
 *   `IconGlyph` carries a glyph's *proportions*, which is what every rule below
 *   needs and what the plate's own numbers state.
 * - **The flag durations** -- 60 s engaged, 5 min suspect, 15 min criminal. They
 *   are the combat phase's rule about when a flag is *set*, not this system's
 *   about how one is drawn, and a countdown living here would be an engine
 *   library holding a balance number (ADR-014 §2c).
 * - **Anything that decides.** A tap is not a selection (`Gesture.h`'s rule) and
 *   a glyph is not a decision either: these are pure functions over replicated
 *   fields, which is also what makes §6's ladder deterministic.
 *
 * **Device-free.** Every rule below is arithmetic over numbers the wire already
 * carries, so the taxonomy, the floor and the ladder are all testable before
 * anyone has looked at a screen. What a screen has to confirm is §5's own
 * question -- whether eleven silhouettes really do separate at 20 px on a real
 * panel -- and §7 leaves that open as R3 F13.
 */

namespace Neuron
{

/*
 * §1's four role families.
 *
 * *"Grouped by role so the eye learns four families, not eleven shapes: strike
 * craft are open deltas, line ships are closed chevrons and hexes that lengthen
 * with tonnage, support hulls are blunt and rectilinear, structures are the only
 * radially symmetric glyph."*
 *
 * The engine names these four and not the eleven, which is the same line
 * `LampRig` draws one pass over: three rigs shared across every hull is visual
 * vocabulary, and eleven hull names would be the game's taxonomy in engine code.
 *
 * **`Static` is the heading-less family, and that is load-bearing** rather than
 * descriptive. §1 says *"nothing that moves is ever drawn symmetric, so heading
 * is readable at every size"*, and §2 says its non-hull entities are *"never
 * heading-oriented"*. Those are one rule stated twice, and stating it as an
 * equivalence -- **a glyph is radially symmetric exactly when it is `Static`**
 * -- is what turns it into something `ValidIconGlyphTable` can check. The
 * decision this slice took rather than looked up is the reverse implication: the
 * sheet only says movers are asymmetric, and a heading-less glyph drawn
 * asymmetric would read as pointing somewhere it cannot point, so the taxonomy
 * holds it both ways.
 */
enum class IconFamily : std::uint8_t
{
  Strike = 0,
  Line = 1,
  Support = 2,
  Static = 3
};

inline constexpr std::uint32_t ICON_FAMILY_COUNT = 4;

/// Which glyph, as an index into the table the composition root supplies. A
/// plain alias for `EntityId`'s reason -- it is an array-ish handle, and what is
/// wanted is a name that moves if the width does, not a wrapper at every call.
using IconSlot = std::uint8_t;

/// A hull the table has no row for. Draws nothing, which is the same answer
/// `BuildScene` gives a hull class with no mesh: substituting another glyph
/// would put a ship on screen that is not the ship the server is simulating.
inline constexpr IconSlot INVALID_ICON_SLOT = 0xffu;

/*
 * One glyph's shape, as the plate states it.
 *
 * `widthFraction` and `heightFraction` are the sheet's own `sw`/`sh`: the
 * glyph's extent inside a unit box, so an Interceptor is 0.40 x 0.64 (a thin
 * spike) and a Carrier 1.00 x 0.84 (blunt and rectilinear). They are
 * proportions and not sizes -- the **size** channel is `IconRequest`'s
 * silhouette radius, which is the game's own answer to how big a hull is.
 *
 * Nothing here carries a polygon. The outline is the atlas's (§7), and a
 * client that had the vertices would still need the atlas to draw them.
 */
struct IconGlyph
{
  IconFamily family = IconFamily::Line;
  float widthFraction = 1.0f;
  float heightFraction = 1.0f;
};

/// Does this family's glyph carry a heading? The equivalence above, asked
/// rather than remembered -- and the only place the rule is spelled, so a
/// second reader cannot spell it differently.
[[nodiscard]] constexpr bool IconHeadingOriented(IconFamily _family) noexcept
{
  return _family != IconFamily::Static;
}

/*
 * How close two proportions have to be to call a glyph radially symmetric.
 *
 * A tolerance rather than an equality because the numbers are authored art
 * direction -- the plate states them to two decimals -- and a table that had to
 * spell 0.84 twice as the identical float would be a table one careless edit
 * away from failing a rule it still obeys. A hundredth is half the smallest
 * difference any asymmetric glyph in the plate carries (`Miner`, at
 * 0.74 x 0.72), so the tolerance cannot swallow one.
 */
inline constexpr float ICON_SYMMETRY_TOLERANCE = 0.01f;

/*
 * Is this table one the rules above can be applied to?
 *
 * Checks the one invariant the plate states and the numbers have to keep: a
 * glyph is radially symmetric exactly when it is heading-less. Everything else
 * about a table is art direction; this is the part that would silently break a
 * reader who has learned that a symmetric mark does not move.
 *
 * Not an assert, because a table arrives from the composition root and a
 * malformed one is content rather than a bug in this library (AGENTS §5's rule
 * for anything reading content). The caller decides what a false means.
 *
 * **`constexpr` so a table authored as a constant can be checked at compile
 * time**, which is what the composition root's own does: a rule enforced by a
 * `static_assert` cannot be skipped, cannot go unrun, and does not need a test
 * project to reach a file in `Outpost`. The runtime call stays available for a
 * table that ever arrives from a file.
 */
[[nodiscard]] constexpr bool ValidIconGlyphTable(std::span<const IconGlyph> _table) noexcept
{
  for (const IconGlyph& glyph : _table)
  {
    // A proportion outside the unit box is not a proportion. Rejected rather
    // than clamped: a row that says 1.4 meant something this file cannot guess.
    if (!(glyph.widthFraction > 0.0f) || !(glyph.heightFraction > 0.0f) || glyph.widthFraction > 1.0f || glyph.heightFraction > 1.0f)
    {
      return false;
    }

    // The one invariant the sheet states and the numbers have to keep: symmetric
    // exactly when heading-less. See `IconHeadingOriented`'s note for why the
    // reverse implication is this taxonomy's decision rather than the plate's.
    //
    // Spelled as two comparisons rather than `std::fabs`, which is not constexpr
    // before C++26 -- and being usable in a `static_assert` is the whole point.
    const float difference = glyph.widthFraction - glyph.heightFraction;
    const bool symmetric = difference <= ICON_SYMMETRY_TOLERANCE && -difference <= ICON_SYMMETRY_TOLERANCE;
    if (symmetric == IconHeadingOriented(glyph.family))
    {
      return false;
    }
  }
  return true;
}

/*
 * §3's PVP flag, as a **ring** and never as a hue.
 *
 * *"Flags are drawn as rings around the glyph rather than as a hue change,
 * because a hostile Criminal and a neutral Criminal must both read as attackable
 * while still reading as different parties."* The two channels are orthogonal
 * on purpose, and `IconRingFor` is where that stops being a claim: the four
 * geometries separate with colour removed entirely, which is a property a test
 * can assert and a palette swap cannot break.
 *
 * The values are the flag's, set by a combat phase that does not exist yet.
 * `None` is what every entity carries until it does.
 */
enum class IconFlag : std::uint8_t
{
  None = 0,
  Engaged = 1,
  Suspect = 2,
  Criminal = 3
};

inline constexpr std::uint32_t ICON_FLAG_COUNT = 4;

/*
 * A flag ring's geometry, in multiples of the glyph it surrounds.
 *
 * Relative rather than absolute, which is the correction the plate's own numbers
 * need: it draws its rings at 38-40 px around a 26 px glyph, and a ring fixed in
 * pixels would swallow a 20 px glyph at the floor and be lost inside a 48 px one.
 * A multiple keeps *"around the glyph"* true at every size on the ladder.
 *
 * `dashCount` of zero is a solid ring. `haloed` is the criminal's black-then-
 * colour outline, which is the one geometry that survives being drawn over a
 * bright hull.
 */
struct IconRing
{
  float radiusPerLongestAxis = 0.0f;
  float strokePixels = 0.0f;
  std::uint16_t dashCount = 0;
  bool haloed = false;
  bool pulses = false;
};

/*
 * §4's state markers -- *"composed over the glyph -- additive, never mutually
 * exclusive"*, which is why they are a mask and not an enum with one value.
 *
 * A ship can be selected, stale and targeted at once, and the sheet draws all
 * three. Anything that made them exclusive would have to rank them, and the
 * ranking would be a decision nobody took.
 */
inline constexpr std::uint8_t ICON_MARKER_SELECTED = 1u << 0;
inline constexpr std::uint8_t ICON_MARKER_LOCKING = 1u << 1;
inline constexpr std::uint8_t ICON_MARKER_LOCKED = 1u << 2;
inline constexpr std::uint8_t ICON_MARKER_TARGETING_ME = 1u << 3;
inline constexpr std::uint8_t ICON_MARKER_STALE = 1u << 4;
inline constexpr std::uint8_t ICON_MARKER_OFF_SCREEN = 1u << 5;

/*
 * What is true about one entity, for the marker rules to read.
 *
 * Two of these have producers today -- `selected` is the client's own set and
 * `stale` is `SceneEntity::stale`, the 250 ms extrapolation cap (ADR-002 §4).
 * The three lock fields have none: locking is the combat phase's, which has no
 * ADR and no build order. They are here rather than added later because §4's
 * one *disclosure* rule has to be enforced by whoever composes the markers, and
 * a rule written after the composer exists is a rule somebody has to remember to
 * apply.
 */
struct IconSubject
{
  bool selected = false;
  bool stale = false;

  /// Off the viewport, which §4 draws as an edge chevron plus a range. Read
  /// only when `selected`: the sheet says *"selection only"*, and a chevron per
  /// off-screen hull at the 1,024-entity cap is a border of chevrons.
  bool offScreen = false;

  /// This viewer's lock on the entity, 0..1. Below 1 draws the sweep; at 1 the
  /// four corners snap in.
  float lockProgress = 0.0f;

  /*
   * Whether an attacker's lock **on the viewer** has completed.
   *
   * §4: *"Targeting-me is the one marker that leaks information, and 04 §6.3
   * already bounds it: it may appear only once the attacker's lock has
   * completed, never during the sweep. Drawn earlier it becomes a free
   * early-warning system that the replication audience explicitly refuses."*
   *
   * A separate field from `lockProgress` because they are opposite directions
   * of the same relationship, and a single number would let the sweep leak by
   * arithmetic.
   */
  bool attackerLockComplete = false;
};

/*
 * The additive marker set for one entity, with §4's two gates applied.
 *
 * Pure, so the disclosure rule is a property of a function rather than a
 * discipline at each call site -- which is the whole reason it is ruled now,
 * before the combat phase writes anything against it.
 */
[[nodiscard]] std::uint8_t IconMarkersFor(const IconSubject& _subject) noexcept;

/*
 * §6's ladder, named. What an icon degrades to as the view widens.
 *
 * `Tactical` is the full glyph with its hull and shield bars. `Operational`
 * retires the bars and keeps the glyph -- *"per-entity detail nobody reads at
 * this range"*. `Strategic` stops drawing hulls at all and merges them into
 * counted chips, which is `IconDensity.h`'s half.
 */
enum class IconTier : std::uint8_t
{
  Tactical = 0,
  Operational = 1,
  Strategic = 2
};

/*
 * The numbers, in one place, in `OverlayTuning`'s spirit: arguable, and not
 * scattered.
 */
struct IconTuning
{
  /*
   * §5's floor: **20 px, and not ADR-020 §8's 48**.
   *
   * The sheet is explicit about why the two coexist: *"U2 governs what a finger
   * must be able to hit ... this is what an eye must be able to tell apart, and
   * it is the tighter constraint in a 1,024-entity view."* Two floors measuring
   * two different things, so neither is derivable from the other.
   *
   * **Not multiplied by the UI scale**, which is a decision rather than an
   * oversight. The scale is this client's proxy for how big the chrome should be
   * on a given panel; the floor is a statement about an eye and a pixel, and §7
   * already answers density the other way -- an SDF source that resolves *"12 px
   * to 96 px and every UI scale from 0.8x to 1.6x"*. A floor that scaled would
   * be a floor that means something different on each machine, and R3 F13 is
   * open precisely because nobody has measured this one on a real panel yet.
   */
  float legibilityFloorPixels = 20.0f;

  /*
   * Where the hull and shield bars retire, which is the `Tactical` /
   * `Operational` boundary.
   *
   * **Derived, not picked: it is the bar's own width.** `OverlayTuning` draws
   * the print's 24 px bar as two 12 px halves, and a readout wider than the
   * thing it describes has stopped being attached to it -- so bars retire when
   * the glyph they sit over falls under their own width.
   *
   * Written out here rather than reached for across a header, because the icon
   * system does not otherwise depend on the overlay and one `#include` to share
   * a number would be the wrong direction for a dependency. What stops the two
   * drifting is a test that asserts the arithmetic (`IconTests`), which is the
   * same job a wiring line would do and does not require a caller that does not
   * exist yet.
   *
   * It lands on §5's own table, which is the check that this derivation is the
   * sheet's rather than a coincidence: 24 px is the row the plate labels *"dense
   * engagement"* and 20 px the one it labels *"legibility floor"*.
   */
  float barRetirePixels = 24.0f;
};

/*
 * Which rung of the ladder a glyph of this size is on.
 *
 * A pure function of one number, which is what §6 means by *"a pure function of
 * replicated fields"*: the size comes from a hull's silhouette and the camera's
 * scale, both of which every client computes identically from the same
 * snapshot. Nothing here reads a clock or a preference.
 */
[[nodiscard]] IconTier IconTierFor(float _longestAxisPixels, const IconTuning& _tuning) noexcept;

/*
 * The size channel: how many pixels the glyph's longest axis gets.
 *
 * The hull's silhouette on the plane, projected. `SilhouetteRadiusMetres` is the
 * game's own presentation-only answer to how big a hull is -- the same number
 * the composition root already scales meshes by -- so the size channel carries
 * tonnage without this library learning what a tonne is.
 *
 * **Not the pick radius**, which is deliberately larger than the hull so a
 * moving ship stays clickable. Sizing a glyph by it would make the icon a
 * picking number wearing a shape.
 *
 * Zero metres per pixel, or zero radius, gives zero -- a glyph with no size,
 * which `IconTierFor` reads as `Strategic` and the merge then handles.
 */
[[nodiscard]] float IconLongestAxisPixels(float _silhouetteRadiusMetres, float _metresPerPixel) noexcept;

/*
 * §3's *"drawn on the selection and on locked targets only (decided)"*.
 *
 * The plate argues it from a screen it had already seen fail: flags are
 * `Public`, *"so every ring in view could legally be drawn -- but a lawless
 * region at the 1,024-entity cap would then be a screen of rings, and the states
 * that matter are attached to something you are about to shoot or be shot by."*
 *
 * Small enough to inline at each call and written once anyway, because the two
 * halves of a decision -- what is drawn and what is merely knowable -- drift the
 * moment they are spelled twice.
 */
[[nodiscard]] constexpr bool IconDrawsFlagRing(bool _selected, bool _lockedTarget) noexcept
{
  return _selected || _lockedTarget;
}

/// The ring for a flag. `None` returns a zero radius, which draws nothing.
[[nodiscard]] IconRing IconRingFor(IconFlag _flag) noexcept;

/// One entity's inputs to the four channels, in the shape the seam supplies
/// them: a slot from the game, a world size from the game, a standing from the
/// game's relationship bits, a flag from a combat phase that does not exist, and
/// the client's own view of the entity's state.
struct IconRequest
{
  IconSlot slot = INVALID_ICON_SLOT;
  float silhouetteRadiusMetres = 0.0f;
  StandingColour standing = StandingColour::Neutral;
  IconFlag flag = IconFlag::None;
  IconSubject subject;
};

/*
 * One resolved icon: all four channels and the ladder rung, ready to draw.
 *
 * A single record for `SceneEntity`'s reason one file over -- these could be
 * five parallel answers from five calls, and the first edit that changed one and
 * not the others would put a Carrier's flag ring around an Interceptor.
 */
struct IconSpec
{
  IconSlot slot = INVALID_ICON_SLOT;
  IconFamily family = IconFamily::Line;
  bool headingOriented = true;

  /// The size channel, in pixels. `widthPixels`/`heightPixels` are the glyph's
  /// proportions applied to it, so the longest of the two is `longestAxisPixels`
  /// by construction rather than by the caller multiplying correctly.
  float longestAxisPixels = 0.0f;
  float widthPixels = 0.0f;
  float heightPixels = 0.0f;

  /// The colour channel, as a **class** and not a packed value: the palette that
  /// resolves it is the player's, and a baked colour would be the one part of
  /// this system that ignores the colour-vision setting (ADR-018 D14's reading,
  /// the correction U5a had to make on the map).
  StandingColour standing = StandingColour::Neutral;

  /// The flag channel, and whether §3's rule lets it be drawn on this entity.
  IconFlag flag = IconFlag::None;
  IconRing ring;
  bool drawsRing = false;

  std::uint8_t markers = 0;
  IconTier tier = IconTier::Tactical;
};

/*
 * The four channels, resolved together.
 *
 * False for a slot the table has no row for, and `_outSpec` is left alone --
 * "draw nothing" rather than "draw slot zero", which is the same posture
 * `BuildScene` takes for a hull class with no mesh.
 */
[[nodiscard]] bool ResolveIcon(std::span<const IconGlyph> _table, const IconRequest& _request, float _metresPerPixel,
                               const IconTuning& _tuning, IconSpec& _outSpec) noexcept;

} // namespace Neuron
