#pragma once

#include "IconSystem.h"
#include "Relevance.h" // Game::Relationship -- ADR-022 §8b's two status bits, decoded.
#include "ShipClass.h"

#include <span>

/*
 * Which glyph a hull class wears (`tactical-icon-system.png` §1, 07b).
 *
 * The composition root's half of the icon seam, and it is here for the reason
 * `MESH_FOR_HULL` is: the engine draws slot six and has no business knowing that
 * slot six is a Battleship (ADR-014, ADR-018 D14). `Neuron::IconSystem` owns the
 * four channels, the legibility floor and the ladder; this file owns the eleven
 * shapes -- twelve, with the gate -- and the mapping from the game's taxonomy to
 * them.
 *
 * **The proportions are the plate's own numbers**, its `sw`/`sh` pairs, copied
 * rather than re-derived. They are the *shape* channel and not the size one: how
 * big a hull is drawn comes from `Game::SilhouetteRadiusMetres`, which is where
 * this tree already states it.
 *
 * **The slot is `HullClass`'s own value today and is still asked for through a
 * function**, because the two are about to diverge. §2 draws five entities
 * *outside* `HullClass` -- probe, wreck, site, gate, celestial -- and each of
 * them needs a glyph the day it has a producer. Gate has one already, by a route
 * the plate did not anticipate (ADR-016 §10 appended it to the ship table), and
 * the other four will arrive as slots past the end of the hull classes. A cast
 * at each call site is a cast somebody has to find again then.
 */

namespace Outpost
{

/*
 * The glyph table, indexed by `Neuron::IconSlot`, in `Game::HullClass` order.
 *
 * Twelve rows: the plate's eleven and the gate. **The two reserved hull classes
 * keep their rows** on `ShipClass`'s own argument -- the enum is a wire value,
 * an icon index and a palette index, so a class with no content still occupies
 * its slot and a renumber later would move every glyph at once.
 */
[[nodiscard]] std::span<const Neuron::IconGlyph> HullIconGlyphs() noexcept;

/// Which slot this hull class draws through. Always valid: every class has a
/// row, including the two with no content and the one the plate never drew.
[[nodiscard]] Neuron::IconSlot HullIconSlot(Game::HullClass _hullClass) noexcept;

/*
 * The relationship channel, from ADR-022 §8b's two status bits.
 *
 * A translation and not a lookup: `Game::Relationship` is a wire encoding whose
 * numbering is fixed at four values, and `Neuron::StandingColour` is the engine's
 * closed set of four that `ContrastAudit` proves clears the contrast floor in
 * every palette. They agree value for value today and are written out anyway,
 * because one is a fact about the wire and the other a fact about a palette, and
 * a cast between them would be a promise that those two never move apart.
 *
 * `constexpr` so the four rows can be checked by `static_assert` beside the
 * glyph table, on the same argument: a mapping nothing verifies is a mapping
 * that silently draws somebody else's fleet in your own colour.
 */
[[nodiscard]] constexpr Neuron::StandingColour StandingOf(Game::Relationship _relationship) noexcept
{
  switch (_relationship)
  {
  case Game::Relationship::Own:
    return Neuron::StandingColour::Own;
  case Game::Relationship::Allied:
    return Neuron::StandingColour::Allied;
  case Game::Relationship::Neutral:
    return Neuron::StandingColour::Neutral;
  case Game::Relationship::Hostile:
    return Neuron::StandingColour::Hostile;
  }
  // Every two-bit value is a legal `Relationship` (that is the point of spending
  // exactly two bits on a four-valued channel), so this is unreachable from the
  // wire. Neutral is the safe direction anyway: it is what the registry already
  // answers for a ship it has no owner for.
  return Neuron::StandingColour::Neutral;
}

} // namespace Outpost
