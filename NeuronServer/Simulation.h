#pragma once

#include "ByteWriter.h"
#include "EntityRecord.h"
#include "OrderIntent.h"
#include "Wire.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/*
 * The engine/game seam (ADR-014 §2).
 *
 * NeuronServer hosts *a* simulation, never *this* one. It drives the tick,
 * owns sessions and moves bytes; what those bytes mean is GameLogic's business,
 * and the composition root is the only place that knows both.
 *
 * Everything crossing the seam is either a primitive or an opaque payload. The
 * moment a ship, an order or a formation appears in this file, the engine has
 * stopped being an engine.
 */

namespace Neuron
{

// `OrderVerdict` used to be declared here. It moved to NeuronCore with S5c,
// because `WorldView::PreCheck` (NeuronClient) has to return the same type this
// returns or ADR-014 §3's BounceParity is unverifiable -- and NeuronClient
// cannot see NeuronServer. A shared type belongs below both, not beside one.

/*
 * Where the simulation's world sits, for a client that must place itself before
 * the first snapshot arrives (ADR-009 §8's `worldMeta`).
 *
 * Named in engine terms deliberately -- "world", not "solar system". The engine
 * carries the numbers to the client and never reads them; what they mean is
 * GameLogic's business (ADR-014).
 *
 * The three strings are the session's display strings for the HUD's top bar
 * (`tactical-hud.png`): what to call the world, a secondary line, and a short
 * status badge. Display *data*, never parsed -- the extended leak test
 * (ADR-020 §D14) is exactly that labels arrive as data, so "Vesta-3" and
 * "SEC 0.4" are the game's words travelling through fields the engine could
 * carry for any networked sim. They are Public data and part of no hashed
 * state: replay hashes and the schema/content handshake do not cover them.
 */
struct WorldMeta
{
  std::uint16_t worldId = 0;

  /*
   * Which anchor this grid stands on, as a number the client can name back.
   *
   * `worldId` says *where in the universe* and this says *which grid* -- two
   * different questions that happened to have one field until a client needed
   * to address a grid rather than describe it. It is here because a station
   * command names the station it is addressed to (ADR-017 §3) and a Dock names
   * the anchor it is docking at (ADR-017 §2), and until this shipped the client
   * had no way to learn either: `ValidationView::stationAnchor` was filled by
   * `World` on the server and by nothing at all on the client, so a Dock could
   * be validated but never composed.
   *
   * Engine-neutral, like everything else here: "the id of the thing this grid
   * is anchored on". What an anchor *is* stays GameLogic's (ADR-014).
   */
  std::uint16_t gridAnchor = 0;

  std::int64_t anchorX = 0; // The tactical grid's origin, in whole world units.
  std::int64_t anchorY = 0;

  std::string worldName;   // The prime slot: where the player is.
  std::string worldDetail; // The dim line beside it: region-of-space and version.
  std::string worldBadge;  // The right cluster's badge, drawn verbatim.
};

/*
 * What the session knows about one viewer's feed, handed to the simulation so
 * it can serialise for that viewer (ADR-022 §1, U3d-a).
 *
 * A struct rather than four parameters, and the reason is the shape of what is
 * coming rather than tidiness: ADR-022's wire cluster adds the acked baseline,
 * the tick's byte budget and the ranked id list to exactly this argument, and a
 * parameter list that grows once per sub-slice grows at every call site each
 * time. The struct grows in one place and the call sites do not move.
 *
 * **Everything in it is session state, and that is the point** (§1). The sim
 * tier has no viewers; the session host is the only tier that knows which grid
 * a commander is watching, what their link is doing and what they last acked.
 * So the facts travel *in*, and nothing about a camera or a socket is stored
 * on a world.
 */
struct SnapshotRequest
{
  /// The durable player, never the connection (ADR-018 D5): a snapshot is owed
  /// to whoever is commanding, across the disconnect D5's grace window survives.
  PlayerId viewer = INVALID_PLAYER_ID;

  /// Which grid this viewer is watching. Already gated through `MayView`.
  std::uint16_t grid = 0;

  std::uint32_t tick = 0;

  /*
   * The highest order sequence this **viewer** has had accepted (ADR-022 §7).
   *
   * It was world-global state folded into the world hash, which with one
   * commander was invisible and with two is wrong twice over: one player's
   * order sequence perturbed the other's feedback loop, and a replay's hash
   * depended on which client happened to submit. It is session state and it
   * always read as per-viewer, so it lives here and travels in.
   *
   * The wire field did not move -- the game's snapshot header still carries it
   * (ADR-022 §7) -- and this is where the number it carries now comes from.
   */
  std::uint32_t lastOrderSeqProcessed = 0;
};

/*
 * One viewer's camera, for the relevance hook (ADR-022 §4, U3d-a).
 *
 * Named in engine terms like everything else on this seam: a viewer, a world, a
 * set of entity ids and a focus on a plane. That some games call the entities
 * ships and the world a grid is the game's business (ADR-014).
 *
 * Separate from `SnapshotRequest` because the two answer different questions
 * and one of them is asked speculatively: the engine may rank without sending,
 * and it certainly ranks before it knows how much of the answer it can afford.
 */
struct InterestQuery
{
  PlayerId viewer = INVALID_PLAYER_ID;
  std::uint16_t grid = 0;

  /// What the viewer has selected, as replicated ids (`EntityRecord::id`). A
  /// span, because the selection already exists in the session and this seam
  /// should not decide where.
  std::span<const std::uint32_t> selection;

  float focusXMetres = 0.0f;
  float focusYMetres = 0.0f;

  /// What the zoom actually shows, as a half-extent in plane units.
  float viewHalfExtentMetres = 0.0f;

  std::uint32_t tick = 0;
};

class Simulation
{
public:
  virtual ~Simulation() = default;

  /// Advances one fixed step. The tick index is the simulation's only clock
  /// (ADR-002 §1): implementations must not read a wall clock.
  virtual void AdvanceTick(std::uint32_t _tick) = 0;

  /*
   * Ranks one viewer's grid, most relevant first (ADR-022 §4).
   *
   * **It never truncates**, and that is the whole seam: the caller knows the
   * budget and the callee knows the game. The engine takes as much of this list
   * as this tick's bytes afford; the simulation says what order to spend them
   * in. A game rule change -- a new hostility tier, a new "always show" case --
   * is then a GameLogic edit that touches no engine code, and a budget change
   * is an engine edit that touches no game rule.
   *
   * **The return value is how many of the front are guaranteed** (§5a): the
   * viewer's own ships, anything selected, and the grid's landmarks. The engine
   * never truncates inside that prefix -- when it alone exceeds the budget, the
   * budget loses and the overrun is counted, because culling a player's own
   * fleet to hit a bandwidth number is the one outcome ADR-022 exists to
   * prevent. The engine cannot work the prefix out for itself: which ships are
   * guaranteed is game semantics, and this is the only place that knows.
   *
   * `_outRanked` is cleared and refilled. The caller owns the vector so the
   * per-tick, per-viewer ranking costs no allocation once it is warm.
   *
   * Defaulted to "rank nothing" rather than pure, for `World()`'s reason: a
   * simulation with nothing to rank is a real thing and `NullSimulation` is
   * one. An empty ranking is read by the engine as an empty grid, which is
   * exactly what such a simulation has.
   */
  [[nodiscard]] virtual std::uint32_t RankRelevance(const InterestQuery& _query, std::vector<std::uint32_t>& _outRanked)
  {
    (void)_query;
    _outRanked.clear();
    return 0;
  }

  /*
   * Fills the neutral records for exactly these entities, in this order
   * (ADR-022 §1, §8).
   *
   * The engine chose the ids -- it ranked through `RankRelevance` and truncated
   * to this tick's budget -- and the game says what each of them *is*. That is
   * the same split one step further along: relevance is game policy, budget is
   * link mechanism, and quantisation is game semantics again.
   *
   * Records rather than bytes, because the session host has to **compare** them
   * against the view it last sent (§2b) to work out what changed. Handing back
   * a serialised blob would put delta encoding inside the game, which §1 exists
   * to prevent.
   *
   * `_request.viewer` is what makes `statusBits`'s relationship bits fillable:
   * the same hull is OWN to one commander and NEUTRAL to the next (§8b).
   *
   * An id the simulation does not recognise is **skipped**, not filled with a
   * placeholder -- so `_outRecords` may be shorter than `_ids`. A ship can be
   * despawned between the ranking and this call, and a placeholder record would
   * be a hull the client draws at the origin.
   */
  virtual void WriteEntities(const SnapshotRequest& _request, std::span<const std::uint32_t> _ids,
                             std::vector<EntityRecord>& _outRecords)
  {
    (void)_request;
    (void)_ids;
    _outRecords.clear();
  }

  /*
   * The game's own per-tick tail for one viewer (ADR-022 §3b).
   *
   * Whatever a viewer is owed beside the entity records: for this game, the
   * order-state records that promote a client's ghost and the order high-water
   * mark beside them. **Opaque**, exactly as `WriteSummaries` is and for the
   * same reason -- the engine frames the bytes and has no opinion about what an
   * order is (ADR-014 §5).
   *
   * It rides in part zero of the tick's update, and the engine reserves room for
   * it before it starts packing records, so a busy tick cannot be the reason a
   * ghost loses its ETA.
   *
   * False when there is nothing to say, and the engine then sends no tail
   * rather than an empty one.
   */
  [[nodiscard]] virtual bool WriteTickTail(const SnapshotRequest& _request, ByteWriter& _writer)
  {
    (void)_request;
    (void)_writer;
    return false;
  }

  /*
   * May this viewer watch that grid? (ADR-016 §7 — U3b.)
   *
   * Returns zero for yes and the game's own reason code for no, which is
   * `OrderAck`'s arrangement and exists for the same reason: the refusal a
   * player reads must be the same words whichever half produced it.
   *
   * The engine asks rather than decides, and the split is ADR-014's usual one.
   * *Whether* a view is legal is game semantics -- ADR-016 §7 gates it on
   * presence, ADR-017 §7 counts docked ships as presence, and both of those are
   * facts about ships the engine may not know. *Enforcing* the answer is the
   * session role's job, which is this library's.
   *
   * Defaulted to "yes" rather than pure, for `World()`'s reason: a simulation
   * with one grid has nothing to gate, and `NullSimulation` is one.
   */
  [[nodiscard]] virtual std::uint16_t MayView(PlayerId _viewer, std::uint16_t _grid)
  {
    (void)_viewer;
    (void)_grid;
    return 0;
  }

  /*
   * Serializes what one viewer is owed at the **summary cadence** -- the slow,
   * per-viewer feed that answers questions a snapshot deliberately does not
   * (ADR-016 §6). Returns false when this viewer has nothing to say, and the
   * engine sends nothing rather than an empty frame.
   *
   * Opaque, like the snapshot, and framed under one wire type for the whole
   * family: which member a payload carries is a byte inside these bytes, under
   * the game's own schema hash. The engine decides *when* -- cadence is a link
   * decision -- and the game decides *what*, which is ADR-022 §4's split
   * applied to the one part of replication that exists today.
   *
   * Defaulted rather than pure, for `World()`'s reason: a simulation with no
   * summaries is a real thing, and `NullSimulation` is one.
   */
  [[nodiscard]] virtual bool WriteSummaries(PlayerId _viewer, std::uint32_t _tick, ByteWriter& _writer)
  {
    (void)_viewer;
    (void)_tick;
    (void)_writer;
    return false;
  }

  /*
   * Validates and applies one order payload. Returning a verdict rather than a
   * bool keeps the refusal reason with the decision that produced it.
   *
   * **Both ids, and they are not the same question.** `_clientId` is which
   * socket said it; `_player` is whose property it is about. They coincide
   * today -- one player, one session each -- and the pair is here because the
   * moment they stop coinciding is the moment a command that moved somebody
   * else's goods would have looked correct. A simulation that owns per-player
   * state (a station Bay, ADR-024 §5b) cannot be handed a command without
   * being told whose it is, and asking it to look the answer up would put the
   * engine's session table inside the game.
   *
   * This is the command half of what `WriteSnapshot` and `WriteSummaries`
   * already do on the way out, and ADR-018 D5's note applies unchanged: with
   * one player it filters nothing, and the shape does not change when there
   * are two.
   */
  [[nodiscard]] virtual OrderVerdict ApplyOrderBytes(PlayerId _player, std::uint32_t _clientId,
                                                     std::span<const std::uint8_t> _payload) = 0;

  /*
   * Everything the simulation calls durable, as bytes it alone can read
   * (ADR-025 §2).
   *
   * Opaque, like the snapshot and the summaries, and for a stronger reason: the
   * store one library out owns files, framing and checksums and **must not
   * learn what it is storing**. A record has a kind, a length and a checksum;
   * that one of those kinds is a station Bay is a fact for the composition root
   * to know.
   *
   * False when it did not fit, which is the `ByteWriter` contract -- the caller
   * grows the buffer and asks again rather than persisting half a shard.
   *
   * Defaulted rather than pure, for `World()`'s reason: a simulation with
   * nothing durable is a real thing, and `NullSimulation` is one.
   */
  [[nodiscard]] virtual bool WriteDurableState(ByteWriter& _writer)
  {
    (void)_writer;
    return false;
  }

  /*
   * The same bytes, back into a simulation that has just been built.
   *
   * False refuses the load, and a refusal stops the shard rather than starting
   * an amnesiac one (ADR-025 §6). The engine cannot judge these bytes and does
   * not try: it hands them over and believes the answer.
   */
  [[nodiscard]] virtual bool ReadDurableState(std::span<const std::uint8_t> _state)
  {
    (void)_state;
    return false;
  }

  /*
   * The reload proof (ADR-025 §1a).
   *
   * **Not the replay hash.** That one folds the order queues persistence
   * deliberately forgets, so a correct reload cannot reproduce it. This folds
   * exactly what was written down, and it is what a snapshot records and a
   * checkpoint verifies.
   */
  [[nodiscard]] virtual std::uint64_t DurableHash() const { return 0; }

  /// Message layout of the game's own wire types. Exchanged at the handshake so
  /// mismatched builds refuse each other at the door.
  /*
   * A commander the shard has not seen before (ADR-018 D5, U3c-b).
   *
   * Called once, on the Sim thread, when a `Hello` mints a new `PlayerId`
   * rather than resuming a lapsed one -- so a resume never fires it, which is
   * the difference between coming back and arriving.
   *
   * The engine has NO opinion about what a new commander is given: a starting
   * fleet, a single hull, or nothing at all because a reloaded shard already
   * holds their ships. That is a game question and this library must not have
   * an answer to it (ADR-014 §3), which is why this hands over an id and
   * returns nothing. A default that does nothing is the honest base case: a
   * simulation with no notion of joining is not broken, it is a replay.
   */
  virtual void PlayerJoined(PlayerId _player) { (void)_player; }

  /*
   * The grid a session opens on, described for this commander
   * (ADR-018 D5, U3c-b).
   *
   * A whole `WorldMeta` rather than an anchor id, because the two cannot
   * disagree: the `Welcome` carries the grid's number AND where it sits on the
   * universe plane, and a client handed one grid's id with another grid's
   * origin would place every position it drew in the wrong part of space.
   *
   * Asked after `PlayerJoined`, so a brand-new commander is described whatever
   * joining gave them. It defaults to `World()`, which is what every session
   * got when every session was the same commander -- and which is exactly
   * wrong once one is not: a second commander would open on a grid they have no
   * presence on and be refused a view of their own fleet.
   *
   * A game question like `PlayerJoined`, and asked rather than assumed for the
   * same reason.
   */
  [[nodiscard]] virtual WorldMeta WorldFor(PlayerId _player)
  {
    (void)_player;
    return World();
  }

  [[nodiscard]] virtual std::uint64_t SchemaHash() const = 0;

  /// The content the simulation was built from -- the universe definition and
  /// anything else authored. Same handshake, different failure.
  [[nodiscard]] virtual std::uint64_t ContentHash() const = 0;

  /// Where this simulation's world is anchored. Defaulted rather than pure
  /// because a simulation with no world is a real thing -- NullSimulation is
  /// one -- but anything built from authored content must answer.
  [[nodiscard]] virtual WorldMeta World() const { return {}; }
};

/*
 * A simulation that does nothing, for hosting a server before a game exists and
 * for tests that care about the loop rather than the world. It is game-free by
 * construction, so it belongs to the engine rather than to GameLogic.
 */
class NullSimulation final : public Simulation
{
public:
  void AdvanceTick(std::uint32_t _tick) override { m_lastTick = _tick; }
  // Writes nothing and ranks nothing: there is no world here to have entities
  // in, which is a real thing for a simulation to be rather than a stub.

  [[nodiscard]] OrderVerdict ApplyOrderBytes(PlayerId, std::uint32_t, std::span<const std::uint8_t>) override
  {
    return OrderVerdict{}; // Refuses everything: there is nothing to command.
  }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0; }

  [[nodiscard]] std::uint32_t LastTick() const noexcept { return m_lastTick; }

private:
  std::uint32_t m_lastTick = 0;
};

} // namespace Neuron
