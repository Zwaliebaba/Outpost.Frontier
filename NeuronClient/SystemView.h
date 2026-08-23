#pragma once

#include "HudPalette.h"

#include <cstdint>
#include <span>

/*
 * One solar system, as neutral data (ADR-018 D14, ADR-016 §9, U6).
 *
 * `MapView.h` one level down. The strategic map draws the graph *between*
 * systems; this draws the inside of one, and it is the surface where a warp is
 * clicked — so what crosses is places, labels and two counts, and nothing that
 * knows what any of it means.
 *
 * **The rule this file follows is `MapView.h`'s, stated once there and again
 * here because it is what makes the seam work: ids go to the game; indices come
 * back.** `SystemAnchor::id` is the opaque handle a click hands over;
 * `ring` and `slot` are the game's own placement, resolved before it crossed.
 *
 * **Ring and slot are the whole of ADR-016 §9b's first two rulings, and they are
 * on this side of the seam on purpose.** §9b.1 rules that a ring holds about
 * eight and overflows outward; §9b.2 puts sites on their own outermost ring.
 * Both are decisions about *which anchors go where*, and both need to know what
 * an anchor is — so the game computes them and hands over a ring index. The
 * client draws ring N at a radius and never learns that the outer one is
 * mining fields, that the rule exists, or that 70.6 % of systems need it.
 *
 * **There are two lists because the print draws two kinds of thing.** *"Anchors
 * are targets and everything else is backdrop"* — an anchor gets a reticle and
 * a label, a star or a moon is dim, unlabelled and inert, and the player never
 * clicks something and is told no. That is a hit-test difference rather than a
 * flag, so it is two spans rather than one with a `clickable` bit: a list you
 * cannot hit-test cannot be hit-tested by mistake.
 *
 * **And no distances.** ADR-016 §9's *"the layout cannot say distance, so it
 * says time"*: rings are evenly spaced by bake order, there is **no scale bar
 * ever**, and what a trip costs arrives as an ETA the game already prints for
 * the tactical ghost. Nothing here carries a metre.
 */

namespace Neuron
{

/*
 * One place a fleet can be sent: a planet, a station, a gate or a field, and
 * this file cannot tell which.
 *
 * The two counts are ADR-016 §9b.3's ruling — *one marker with a split count*
 * rather than two markers. Both are the player's own ships, and they are two
 * numbers because at this resolution the difference is actionable: docked and
 * on-grid take different verbs (ADR-017). The strategic map merges them into
 * one, which is the same fact at a resolution where it is not.
 */
struct SystemAnchor
{
  /// What a click hands back. Opaque, echoed, never read here.
  std::uint16_t id = 0;

  /*
   * Which ring, counting outward from the star, and which position on it.
   *
   * The game's placement rather than the client's, per this file's header: a
   * ring index already encodes §9b.1's capacity rule and §9b.2's site ring, and
   * a client that computed them would have to know what an anchor *is*.
   *
   * `slot` is a position within the ring rather than an angle, so the screen
   * decides how a ring is fanned and the game decides what is on it.
   *
   * **Slots on one ring are contiguous from zero and the two lists share them**
   * -- an anchor and a backdrop body on ring 2 never hold the same slot. That
   * is what lets the screen fan a ring by counting what is on it, and it is the
   * difference between a moon sitting *between* two planets and a moon sitting
   * *on top of* one.
   */
  std::uint16_t ring = 0;
  std::uint16_t slot = 0;

  /// The game's word for the place. Never parsed.
  const char* label = nullptr;

  /*
   * A second line under the label, or null.
   *
   * ADR-016 §9b.4's ruling arrives through this and nothing else: a gate reads
   * `→ KIL-7` because the game wrote those characters, and this file does not
   * learn that gates exist or that the far side is a system. A planet has
   * nothing to say here and draws one line.
   */
  const char* detail = nullptr;

  /// The player's ships standing on this anchor's grid, and docked at it. Two
  /// numbers, per §9b.3 — see the struct comment.
  std::uint16_t shipCount = 0;
  std::uint16_t dockedCount = 0;

  /// Which standing colour the reticle takes. A class rather than a packed
  /// value, for `MapNode::tint`'s reason: a baked colour ignores the player's
  /// colour-vision palette.
  StandingColour tint = StandingColour::Neutral;
};

/*
 * Scenery: a star, a moon, a belt — drawn and inert.
 *
 * Placed the same way an anchor is so one function serves both, and carried in
 * its own list so that *"the player never clicks something and is told no"* is
 * a property of the data rather than a branch somebody has to remember.
 */
struct SystemBackdrop
{
  std::uint16_t ring = 0;
  std::uint16_t slot = 0;

  /// Drawn at a size the screen picks from this, 0..255 — a moon against a gas
  /// giant. A ratio rather than a radius, because a radius would be a distance
  /// and this surface has none.
  std::uint8_t scale = 128;
};

/*
 * How many anchors one system will draw.
 *
 * The committed bake's worst case is 15 and its mean is 9.9 (ADR-016 §9b), so
 * this is that with room. A system past it is a content change rather than a
 * rebuild, and the builder says when it drops one.
 */
inline constexpr std::uint32_t MAX_SYSTEM_ANCHORS = 32;

/*
 * And how much scenery, which is far less than one would guess.
 *
 * The committed bake's worst case is **two**: nearly every celestial a system
 * has is a planet with an anchor on it, and what is left over is the handful
 * that carry no warp destination. The cap is headroom for the moons
 * `CelestialKind` authors and the MVP content does not bake, not a measured
 * ceiling -- so it is stated as headroom rather than fitted to a two.
 */
inline constexpr std::uint32_t MAX_SYSTEM_BACKDROP = 32;

/*
 * And how many rings one can hold.
 *
 * The committed bake needs two or three (never one -- every system has sites,
 * and §9b.2 puts those on a ring of their own). This is that with room for a
 * system twice the size, and it exists as a named number because the screen
 * counts a ring's population into a fixed array before it places anything.
 */
inline constexpr std::uint32_t MAX_SYSTEM_RINGS = 12;

/*
 * The system, as spans into storage the game owns.
 *
 * Asked when the screen opens and when the view changes rather than per frame:
 * the bake does not move, and what does move — the two counts — moves at the
 * summary family's ~1 Hz. That makes it ADR-020 §6's second shape, the same one
 * `BuildMapLegend` and `BuildMapMarkers` use.
 */
struct SystemViewData
{
  /// The game's word for the system, and for its security band. Both drawn,
  /// neither parsed.
  const char* label = nullptr;
  const char* badge = nullptr;
  StandingColour tint = StandingColour::Neutral;

  std::span<const SystemAnchor> anchors;
  std::span<const SystemBackdrop> backdrop;

  /*
   * How many rings there are, which the screen needs before it places anything
   * -- the outermost ring has to fit the disc, so the radius step is a function
   * of the count rather than a constant.
   *
   * Carried rather than derived from the anchors' maximum, because a ring can
   * legitimately be *empty of anchors* and still exist: §9b.2's site ring is
   * outermost, so a system whose sites all sit on it leaves no anchor naming
   * the ring below when that ring holds only scenery.
   *
   * Never zero from a game that filled either list, and clamped to one by the
   * screen if it is -- a ring count of nothing is a divide, and this file's
   * contract is not worth a crash.
   */
  std::uint16_t ringCount = 0;
};

/*
 * One of the two verbs at the panel's foot (ADR-016 §9, U6).
 *
 * `OrderKindOption`'s shape at a different resolution, and deliberately the
 * same one: a word the game wrote, an opaque kind the client echoes back, and
 * an availability carrying the reason the authority would have bounced it with.
 * The engine draws the word, greys on the bool and prints the reason through
 * `ReasonText`, exactly as the command row does -- which is what makes a verb
 * greyed on this screen and the same verb greyed on that row say the same thing
 * in the same words (ADR-014 §3).
 *
 * **`name` is the game's and it is not "WARP".** The screen has two verb rects
 * because the print drew two; what they are called is a sentence about this
 * game, and a client that spelled them would be a client whose second game
 * could not rename them.
 */
struct SystemVerb
{
  /// The game's word for it, drawn and never parsed. Null means this game has
  /// no such verb, which draws as a rect with nothing in it rather than as a
  /// gap the other verb slides into.
  const char* name = nullptr;

  /// The order kind to send. Opaque, echoed, never read here.
  std::uint16_t kind = 0;

  /// Whether it would be taken *right now*, and why not.
  bool available = false;
  std::uint16_t reasonCode = 0;
};

/*
 * The two verbs, for one anchor and one selection.
 *
 * A second call rather than fields on `SystemAnchor`, because the two questions
 * change at different rates: the rings are a fact about the bake and are asked
 * when the screen opens, while whether this fleet may warp there changes with
 * every tap on the plane behind. Folding them together would mean rebuilding
 * 15 anchors' worth of labels to find out that a button greyed.
 */
struct SystemAnchorVerbs
{
  SystemVerb warp;
  SystemVerb dock;
};

/*
 * "No place is picked", for asking what the verbs are *called* when there is
 * nothing to ask about.
 *
 * `INVALID_MAP_ANCHOR`'s twin one screen down, and it exists for the panel's
 * sake: both verbs are drawn at all times, their words belong to the game, and
 * a screen with no selection still has to be told them -- so the absence is
 * spelled rather than expressed by not asking.
 */
inline constexpr std::uint16_t INVALID_SYSTEM_ANCHOR = 0xffffu;

} // namespace Neuron
