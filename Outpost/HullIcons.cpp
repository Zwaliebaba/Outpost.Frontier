#include "pch.h"

#include "HullIcons.h"

#include <array>
#include <span>

namespace Outpost
{
namespace
{

using Neuron::IconFamily;
using Neuron::IconGlyph;

/*
 * The plate's own `sw`/`sh` pairs, in `Game::HullClass` order.
 *
 * *"Grouped by role so the eye learns four families, not eleven shapes: strike
 * craft are open deltas, line ships are closed chevrons and hexes that lengthen
 * with tonnage, support hulls are blunt and rectilinear, structures are the only
 * radially symmetric glyph."*
 *
 * The lengthening is visible in the numbers and is the size-adjacent half of the
 * silhouette channel: the four LINE hulls run 0.60, 0.78, 0.90, 1.00 down their
 * long axis, so a Corvette and a Battleship differ in *proportion* before either
 * is scaled by its tonnage. The `static_assert` below holds that ordering rather
 * than trusting a retune to keep it.
 *
 * **`Gate` is inferred and flagged as such.** The plate was drawn 2026-08-08 and
 * lists GATE in §2, among the entities *outside* `HullClass`; ADR-016 §10 then
 * appended `Gate` to the ship table two weeks later, on the precedent that a
 * station is a ship-table entry that never moves. So the taxonomy has a twelfth
 * class the plate never drew a glyph for, and this row is written from what §2
 * does say -- a graph edge and a route-planner anchor, never heading-oriented --
 * plus the family invariant, which forces it symmetric. It fills its box where
 * `Structure` sits inside one at 0.84, which is the only proportion difference
 * available between two symmetric marks; the rest of telling them apart is the
 * outline's job and the size channel's (168 m of gate against 253 m of station).
 * Recorded in ADR-027's manner: an inferred row, said to be inferred, so it is
 * correctable rather than authoritative by accident.
 */
constexpr std::array<IconGlyph, Game::HULL_CLASS_COUNT> HULL_GLYPHS = {{
  {IconFamily::Strike, 0.40f, 0.64f},  // Interceptor: a thin spike, the most robust silhouette in the set.
  {IconFamily::Strike, 0.54f, 0.58f},  // Fighter -- reserved: no mesh, no content, and it keeps its slot.
  {IconFamily::Strike, 0.72f, 0.48f},  // Bomber: the broadest of the open deltas.
  {IconFamily::Line, 0.56f, 0.60f},    // Corvette.
  {IconFamily::Line, 0.64f, 0.78f},    // Frigate: the twin-pronged stern is the Battleship's, not this one's.
  {IconFamily::Line, 0.72f, 0.90f},    // Cruiser -- reserved, and the hexagon of the accepted 20 px collision.
  {IconFamily::Line, 0.90f, 1.00f},    // Battleship: the longest hull, and the plate's unit box.
  {IconFamily::Support, 1.00f, 0.84f}, // Carrier: blunt and rectilinear, the widest.
  {IconFamily::Support, 0.78f, 0.70f}, // Hauler.
  {IconFamily::Support, 0.74f, 0.72f}, // Miner: the narrowest asymmetry in the plate, at two hundredths.
  {IconFamily::Static, 0.84f, 0.84f},  // Structure: the octagon, and the only glyph the plate draws symmetric.
  {IconFamily::Static, 1.00f, 1.00f},  // Gate -- inferred; see the note above.
}};

/*
 * The table is checked where it is written, at compile time.
 *
 * A `static_assert` rather than a test, because this file sits in an
 * `Application` project that no suite links -- `OutpostTests` reaches
 * `AppConfig.cpp` by naming it, and its own comment says a file added there that
 * needs more than JSON is *"the signal that the split has become the right
 * answer"*. A rule the build enforces is stronger than one a suite runs anyway:
 * it cannot be skipped, and it fails on the line that broke it.
 */
static_assert(HULL_GLYPHS.size() == Game::HULL_CLASS_COUNT,
              "every hull class draws through a slot; a class with no row would draw nothing at all");

// The message is ASCII because R31's guard walks narrow literals and the section
// signs this file's comments use freely would fail it. The rule is
// tactical-icon-system.png section 1's "nothing that moves is ever drawn
// symmetric" and section 2's "never heading-oriented", held as one.
static_assert(Neuron::ValidIconGlyphTable(std::span<const IconGlyph>{HULL_GLYPHS}),
              "a glyph is radially symmetric exactly when it is heading-less");

/*
 * *"Line ships are closed chevrons and hexes that lengthen with tonnage"* (§1).
 *
 * The plate says it in prose and its own numbers keep it: 0.60, 0.78, 0.90,
 * 1.00 down the long axis of the four LINE hulls. Worth an assertion rather than
 * a comment because it is the one part of the *silhouette* channel that also
 * reads as size -- a retune that broke the order would make a Corvette read as
 * the heavier ship while the size channel said otherwise, and nothing else here
 * would notice.
 */
static_assert(HULL_GLYPHS[static_cast<std::size_t>(Game::HullClass::Corvette)].heightFraction <
                HULL_GLYPHS[static_cast<std::size_t>(Game::HullClass::Frigate)].heightFraction,
              "the LINE family lengthens with tonnage: Corvette before Frigate");
static_assert(HULL_GLYPHS[static_cast<std::size_t>(Game::HullClass::Frigate)].heightFraction <
                HULL_GLYPHS[static_cast<std::size_t>(Game::HullClass::Cruiser)].heightFraction,
              "the LINE family lengthens with tonnage: Frigate before Cruiser");
static_assert(HULL_GLYPHS[static_cast<std::size_t>(Game::HullClass::Cruiser)].heightFraction <
                HULL_GLYPHS[static_cast<std::size_t>(Game::HullClass::Battleship)].heightFraction,
              "the LINE family lengthens with tonnage: Cruiser before Battleship");

/*
 * And the four standings, which are two closed sets that have to agree.
 *
 * `Game::Relationship`'s numbering is a wire encoding (ADR-022 §8b) and
 * `Neuron::StandingColour`'s is the set `ContrastAudit` proves clears the floor
 * in all three palettes. Nothing stops them drifting except this.
 */
static_assert(StandingOf(Game::Relationship::Own) == Neuron::StandingColour::Own);
static_assert(StandingOf(Game::Relationship::Allied) == Neuron::StandingColour::Allied);
static_assert(StandingOf(Game::Relationship::Neutral) == Neuron::StandingColour::Neutral);
static_assert(StandingOf(Game::Relationship::Hostile) == Neuron::StandingColour::Hostile,
              "a hostile hull drawn in the own-fleet phosphor is the one mistake this channel can make");

} // namespace

std::span<const Neuron::IconGlyph> HullIconGlyphs() noexcept
{
  return std::span<const Neuron::IconGlyph>{HULL_GLYPHS};
}

Neuron::IconSlot HullIconSlot(Game::HullClass _hullClass) noexcept
{
  const auto raw = static_cast<std::uint8_t>(_hullClass);
  if (raw >= HULL_GLYPHS.size())
  {
    // Unreachable for a value from the enumeration, and the honest answer if the
    // taxonomy ever grows past this table: no glyph rather than the wrong one.
    return Neuron::INVALID_ICON_SLOT;
  }
  return static_cast<Neuron::IconSlot>(raw);
}

} // namespace Outpost
