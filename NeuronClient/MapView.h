#pragma once

#include "HudPalette.h"

#include <cstdint>
#include <span>

/*
 * The strategic map's world, as neutral data (ADR-018 D14, ADR-016 §9, U5).
 *
 * `StationView.h` one surface along: the seam family for a screen, in one file
 * named for the family rather than for a type (R7's second exception). What
 * crosses is a graph, a handful of row types, and nothing that knows what any
 * of it means.
 *
 * D14 in one sentence: *"the baked universe topology crosses **once, at boot**,
 * as a neutral graph (nodes/edges/labels/badge classes) — the `OrderKinds`
 * asked-once pattern"*. This is that graph, and it is a seam type in the same
 * sense `RosterRow` is: the engine draws it and never learns what it means.
 *
 * **Asked once, because the bake does not change.** A universe is generated,
 * hashed and fixed before a session starts; asking for it every frame would be
 * asking a question whose answer is compiled in, and asking for it per node
 * would put 2,500 seam calls in a frame. What *does* change per frame is where
 * the camera is, and that is the client's own state.
 *
 * **What is deliberately not here.** No security, sovereignty or
 * station-service semantics -- that is ADR-014 §2c's leak test as D14 extends
 * it: *"labels, badge classes and colours arrive as data"*. So a node carries
 * the game's **word** for its security and the **colour** the game chose for
 * it, and this library never learns that 0.2 is dangerous or that green is
 * safe. A client that could compute a security colour would be a client with an
 * opinion about a second game's rules.
 *
 * **And no `UniversePos`.** Positions cross as floats in map units, converted
 * on the game's side. ADR-009 §2 keeps that type out of everything which is not
 * placement and CI enforces it by name -- so a map that borrowed it would be
 * asking for the exclusion list to be widened for a screen.
 */

namespace Neuron
{

/*
 * One system, as the map draws it.
 *
 * `id` and `group` are **opaque handles**, echoed rather than read: what a tap
 * on a node hands back to the game. That is `RosterRow::groupId`'s arrangement
 * and it is what keeps the client from indexing the game's tables.
 */
struct MapNode
{
  std::uint16_t id = 0;

  /*
   * Which `MapGroup` this node belongs to -- its constellation, to the game.
   *
   * **An index into `MapTopology::groups`, not the group's id**, which is the
   * rule this whole file follows once and states here: *ids go to the game;
   * indices come back*. What a tap hands over is `id`, opaque and never read;
   * what the game fills in for the client to walk with is an index, because the
   * game is the one that resolved its own ids when it built the graph. `MapLink`
   * and `MapRouteHop` are the same both ways round, and the alternative -- a
   * client searching a 2,500-entry table per node per frame to find the
   * constellation to draw a hull around -- is the search this saves.
   */
  std::uint16_t group = 0;

  /*
   * Where it is, in **map units**.
   *
   * Not pixels and not metres: an arbitrary space the game picks, which the
   * screen fits to its viewport. Floats because a strategic map is a picture
   * rather than a placement -- the precision that matters is the one that
   * survives being drawn at 2,500 nodes, and a `double` here would buy nothing
   * a viewport can show.
   */
  float x = 0.0f;
  float y = 0.0f;

  /// The game's word. `ROOT-N` today, and never parsed -- a client that read
  /// the number out of a name would be a client that knows how names are made.
  const char* label = nullptr;

  /// And the game's word for its badge: `SEC 0.2` on the print. Null for a node
  /// with nothing to badge, which draws no badge rather than an empty one.
  const char* badge = nullptr;

  /*
   * Which of the four standing colours this node is drawn in.
   *
   * D14's *"badge classes and colours arrive as data"*, and it is the **class**
   * half rather than a packed colour, which is a correction the first draft
   * needed. A baked `0xFF00FF7Cu` would be a colour that ignores the player's
   * colour-vision palette -- so the map would be the one screen in the client
   * that does not answer the accessibility setting N3 built, on a surface whose
   * whole subject is a coloured overlay.
   *
   * `StandingColour` rather than a set of the map's own for the reason it is
   * four: it is the closed set `ContrastAudit` *proves* clears the floor in
   * every palette. A map that invented a fifth would be a map the audit no
   * longer covers.
   *
   * The gradient becomes bands, and that is a gain rather than a loss:
   * `strategic-map.png` §2's whole argument is that a categorical fill and a
   * continuous one must not be confused on the same screen -- and the number
   * itself is still exact, in `badge`, because colour is never the only carrier.
   */
  StandingColour tint = StandingColour::Neutral;
};

/*
 * A cluster of nodes: a constellation, to the game.
 *
 * Carried rather than derived from its members' centroid, which is the same
 * distinction the bake itself draws -- a constellation has a *placed* centre,
 * and the clustering invariant that makes the map legible is only true against
 * that centre rather than against wherever the systems ended up.
 */
struct MapGroup
{
  std::uint16_t id = 0;

  /// Which region -- an **index** into `MapTopology::regions`, for `MapNode::group`'s
  /// reason. The map's coarsest pinch level groups by this.
  std::uint16_t region = 0;

  float x = 0.0f;
  float y = 0.0f;

  const char* label = nullptr;
};

/*
 * A region: the coarsest pinch level, and the one the print badges.
 *
 * `badge` is `FRONTIER` on the print -- the game's word for a security band,
 * carried rather than computed for the reason above.
 */
struct MapRegion
{
  std::uint16_t id = 0;
  float x = 0.0f;
  float y = 0.0f;
  const char* label = nullptr;
  const char* badge = nullptr;

  /// The region's band, as a standing colour. See `MapNode::tint`.
  StandingColour tint = StandingColour::Neutral;
};

/*
 * A gate link, by node index rather than by node id.
 *
 * Indices because the map draws links by looking both ends up, and an index is
 * that lookup already done -- at 61 links over 48 systems the difference is
 * nothing, and at the corpus's full size it is a search per link per frame.
 * The game fills them, so it is the game that resolves its own ids.
 *
 * **Undirected, and stored once.** A gate pair is one line on the screen, and
 * two entries would draw it twice at double the alpha -- which is exactly the
 * kind of thing that reads as a deliberate highlight.
 */
struct MapLink
{
  std::uint16_t fromNode = 0;
  std::uint16_t toNode = 0;
};

/*
 * How many links a client will ask for: `MAX_MAP_LINKS`, in `UploadBudget.h`
 * beside `MAX_MAP_NODES`.
 *
 * It lives there rather than here because the number the seam will *accept* and
 * the number the renderer has *room* for are one number, and two declarations of
 * it are two numbers waiting to drift. Named rather than included: this header
 * is the seam's vocabulary and `UploadBudget.h` carries the whole overlay and
 * ghost budget with it, which is a weight every implementer of `WorldView` would
 * then inherit to look up one integer it does not need. A gate graph stays sparse by construction
 * -- ADR-009 §4 gives a system a handful of gates, not a fan to everywhere -- so
 * it is `MAX_MAP_NODES`' companion rather than its square.
 */

/*
 * The whole graph, as spans into storage the *game* owns.
 *
 * Spans rather than vectors for `RosterRow`'s reason one level up: the client
 * is a reader here, and the bake outlives every frame that draws it. The
 * lifetime rule is the one `WorldView` already sets -- valid until the next
 * call that could rebuild it, which for a topology asked once at boot means
 * "for the session".
 *
 * Empty is a legal answer and means *no map*: a client whose game has no
 * universe draws the surface's chrome and an empty viewport rather than
 * refusing to open.
 */
struct MapTopology
{
  std::span<const MapNode> nodes;
  std::span<const MapLink> links;
  std::span<const MapGroup> groups;
  std::span<const MapRegion> regions;

  [[nodiscard]] bool Empty() const noexcept { return nodes.empty(); }
};

/*
 * --- what the game says about the graph, rather than about its shape ---------
 *
 * Everything below is a *word* or a *number* the game supplies and this library
 * draws. They are separate from the topology because they arrive at different
 * rates -- the graph once at boot, these on a selection, a press or a summary --
 * and ADR-020 §6's screen-data contract is three shapes rather than three
 * methods, so a family that shared one call would be a family that had to be
 * rebuilt to answer any of its questions.
 */

/*
 * One entry in the OVERLAY rail (`strategic-map.png` §1, §2).
 *
 * `StationTab` exactly, and not by coincidence: a tab and an overlay are the
 * same problem -- a fixed list where some entries have nothing behind them yet,
 * *drawn and disabled with a reason* rather than hidden. `StationTab`'s own
 * comment already names this screen as the precedent it borrowed from.
 *
 * **This is how the four stub overlays stay stubs without the engine knowing
 * they are stubs.** ADR-016 §9 lists sovereignty, activity heat, intel pings and
 * the site layer as visible stubs whose content does not exist; the engine
 * learns that overlay 3 is disabled and what tag to draw beside it, and never
 * that "intel" is a thing this game has not built. A client that shipped the
 * word `SOVEREIGNTY` in a table would have failed D14's leak test on the first
 * screen it was written for.
 *
 * **Exclusive**, which is §2's ruling and a real one: sovereignty is a
 * categorical fill and activity is a continuous one, and stacking them makes a
 * contested busy system indistinguishable from a quiet one under a different
 * owner. The rail keeps one selection, not a set.
 */
struct MapOverlayOption
{
  /// The word on it, from the game. Never null in an overlay it reports.
  const char* name = nullptr;

  /// Why it is disabled, when it is. Null on a live one -- `StationTab::tag`'s
  /// contract, and drawn and only drawn for its reason.
  const char* tag = nullptr;

  /// What a press hands back. Opaque, echoed, never read here.
  std::uint16_t id = 0;

  bool enabled = false;
};

/*
 * Where the player's ships are, at the map's resolution (ADR-016 §6, §7, U3b).
 *
 * **A count at a place, and never a fleet.** The summary family this comes from
 * has no fleet id and nothing to keep in step with a snapshot, because a fleet
 * is emergent -- the player's ships sharing a location. So a marker is a
 * *system* and two numbers, and the client could not track "the same fleet"
 * across two frames even if it wanted to.
 *
 * **One marker per system rather than per anchor**, which is a resolution
 * decision and not a simplification. This map draws systems; an anchor is
 * finer than anything on it, and two markers a pixel apart on one node would be
 * a distinction the player cannot act on. The system view is where anchors
 * become separate things, and ADR-016 §7 has an open ruling there about
 * exactly that.
 */
struct MapMarker
{
  /*
   * Which node it sits on -- an **index** into `MapTopology::nodes`, following
   * this file's one rule: ids go to the game; indices come back.
   */
  std::uint16_t node = 0;

  /*
   * Where a VIEW would point. Opaque, echoed, never read here.
   *
   * A *grid* rather than the system, because watching is a subscription to one
   * grid and the game is what knows which of a system's anchors the player has
   * ships on. `INVALID_MAP_ANCHOR` when there is nowhere to watch -- which is
   * the ordinary state for a system the player is only *arriving* at.
   */
  std::uint16_t anchor = 0xffffu;

  /// Ships in this system now: standing on a grid or docked at a station. The
  /// two are one number here because both mean "already there".
  std::uint16_t shipCount = 0;

  /*
   * Ships crossing *to* this system, counted separately and deliberately.
   *
   * Adding them to `shipCount` would draw a fleet in a system it has not
   * reached, on the one screen a player uses to decide where things are. They
   * are drawn as an arrival rather than as a presence, and `etaLabel` says
   * when.
   */
  std::uint16_t incomingCount = 0;

  /*
   * The game's word for when the soonest crossing lands, or null.
   *
   * A word rather than a number, for `MapRouteSummary::eta`'s reason: the
   * client would have to invent a format, and the format is the thing the two
   * surfaces must agree on rather than the seconds.
   */
  const char* etaLabel = nullptr;
};

/// What `MapMarker::anchor` holds when the player has nothing to watch in a
/// system -- one they are only arriving at. Zero is a legal anchor id.
inline constexpr std::uint16_t INVALID_MAP_ANCHOR = 0xffffu;

/*
 * How many places a client will mark.
 *
 * The client's statement of what it is built to draw, the way `MAX_MAP_NODES`
 * is -- and it is this size because a commander's summary can describe up to
 * 256 places. Named here rather than included from the game: this header is the
 * neutral seam, and reaching into the game's cap to size an engine array would
 * be the leak ADR-018 D14 is about.
 */
inline constexpr std::uint32_t MAX_MAP_MARKERS = 256;

/// How many overlays a client will ask for. The print draws five and this is
/// that plus room, because a layer arriving is a row and nothing else.
inline constexpr std::uint32_t MAX_MAP_OVERLAYS = 8;

/*
 * One line of the active overlay's key (`strategic-map.png` §1's legend block).
 *
 * A swatch, a word and a count -- and all three come from the game, including
 * the count, for `RosterRow`'s reason: the engine has the nodes and could tally
 * them by tint in four lines, and would thereby have decided that a legend
 * counts *systems* rather than jumps, or hulls, or days held. That is a design
 * question about one game's overlay.
 *
 * **Zero rows is the common answer today and is not a failure.** The one
 * overlay with content is a gradient over a per-system number, which has no
 * categories to list; the four with categories have no content. So the zone
 * draws its heading and nothing under it, which is what a visible stub looks
 * like.
 */
struct MapLegendRow
{
  const char* name = nullptr;
  std::uint32_t count = 0;

  /// The swatch, as a standing colour. See `MapNode::tint`.
  StandingColour tint = StandingColour::Neutral;
};

inline constexpr std::uint32_t MAX_MAP_LEGEND_ROWS = 12;

/*
 * One line of the selected-system panel (`strategic-map.png` §1).
 *
 * The print draws five -- SOVEREIGNTY, HELD SINCE, VULNERABLE, STATIONS, GATES
 * -- and every one of them is a game fact with a game word in front of it, so
 * both halves cross as text. A `label`/`value` pair rather than a typed record
 * because the panel *prints* them: a client that received `heldDays = 41` would
 * have to decide that days are how this is said.
 *
 * A game that can answer only some of them reports only those, and the panel is
 * shorter. It does not report a row with an empty value -- a label with nothing
 * beside it is a promise the game has not kept, where a missing row is a fact
 * this game does not have.
 */
struct MapFactRow
{
  const char* label = nullptr;
  const char* value = nullptr;
};

inline constexpr std::uint32_t MAX_MAP_FACT_ROWS = 12;

/*
 * One jump of a planned route (`strategic-map.png` §1's ROUTE panel, §3).
 *
 * **By node index, like `MapLink`, and for the same reason**: the map draws the
 * route as a line through nodes it has already projected, so an index is that
 * lookup already done. The game fills it against the topology it built and
 * handed over, which is asked once and lives for the session -- so there is no
 * moment when an index here refers to a graph that has moved.
 *
 * `badge` is the print's per-leg security, and §3 is emphatic about why it is
 * per leg: *"a single 0.1 system in an otherwise safe chain is the whole risk,
 * and an average erases it"*. So the engine draws a badge per hop and never
 * averages anything, because it could not -- it has a word and a colour.
 */
struct MapRouteHop
{
  std::uint32_t node = 0;
  const char* name = nullptr;
  const char* badge = nullptr;

  /// The leg's security, as a standing colour. See `MapNode::tint`.
  StandingColour tint = StandingColour::Neutral;
};

/*
 * How many hops a client will ask for.
 *
 * The print draws eleven and names fourteen in §3's worked problem, and this is
 * generous against both -- a route is bounded by the graph's diameter rather
 * than by its size, and a bake that produced a longer one would be a bake whose
 * route panel scrolls, which it already does.
 */
inline constexpr std::uint32_t MAX_MAP_ROUTE_HOPS = 64;

/*
 * What the route panel says about the route as a whole.
 *
 * The print's `SAFEST · +2 JUMPS` and `ETA 4m 10s`, which are two sentences the
 * game writes: one about which of §3's three routes this is and what it cost,
 * one about time. Both are the game's because both are about its rules -- what
 * makes a route "safest" is a security model, and an ETA is spool and transit
 * arithmetic (ADR-016 §9's *"the same spool-and-transit arithmetic the tactical
 * ghost prints, so the two surfaces cannot disagree about one warp"*).
 *
 * Either may be null, which draws nothing rather than an empty line.
 */
struct MapRouteSummary
{
  const char* note = nullptr;
  const char* eta = nullptr;
};

} // namespace Neuron
