#pragma once

#include <cstdint>

/*
 * What crosses the command half of the engine/game seam (ADR-014 §2).
 *
 * Three types, and they live in NeuronCore rather than beside either seam
 * because *both* seams speak them. `Neuron::WorldView::PreCheck` (NeuronClient)
 * and `Neuron::Simulation::ApplyOrderBytes` (NeuronServer) must return the same
 * verdict type or ADR-014 §3's BounceParity is a claim nothing can check --
 * and NeuronClient cannot see NeuronServer, so the shared type has to sit below
 * both.
 *
 * All three pass NeuronCore's zero-game-semantics test (ADR-004 ruling 4) the
 * same way `EntityRecord` does: they carry numbers whose meaning is GameLogic's.
 * An `OrderIntent` is "the player pointed at a place and asked for something";
 * the engine never learns what was asked for.
 */

namespace Neuron
{

/*
 * A command the player has expressed but not yet sent.
 *
 * The client builds one from a click (ADR-006 §11 turns the cursor into a plane
 * point), hands it to the game for a pre-check and a preview, and encodes it if
 * the game accepts. `kind` and `parameter` are opaque: GameLogic assigns the
 * numbers, and the engine's only interest is that they round-trip.
 *
 * Positions are metres on the plane, in the tactical grid's local frame -- the
 * same frame the renderer draws in and the wire quantises (ADR-009 §2). They
 * are floats and not the wire's centimetres on purpose: this is what the player
 * pointed at, and quantisation happens once, at `EncodeOrder`, so a pre-check
 * and the authority round the same value at the same moment.
 */
/// "No anchor": this order names a place and not a thing. Zero would be a
/// plausible id, so the sentinel is the top of the range rather than the bottom.
inline constexpr std::uint16_t INVALID_ANCHOR = 0xffffu;

struct OrderIntent
{
  std::uint16_t kind = 0;      // GameLogic's order enum; opaque here.
  std::uint16_t parameter = 0; // Formation, stance, whatever the kind needs.
  float targetXMetres = 0.0f;
  float targetYMetres = 0.0f;
  float facingRadians = 0.0f;
  bool queued = false; // Append to the queue rather than replace it.

  /*
   * The client's own monotonic counter, for the order it is about to send.
   *
   * The client allocates it, because the client is what has to match an ack to
   * a ghost, and `EncodeOrder` places it inside the payload -- which is the
   * only reason a number the engine assigns appears in a message the engine
   * cannot read. `OrderVerdict::orderSeq` is the same number coming back.
   *
   * Zero on a pre-check: nothing has been sent, so nothing needs matching.
   */
  std::uint32_t orderSeq = 0;

  /// Which entities the command is for, as replicated ids (`EntityRecord::id`).
  /// A span rather than a container: the selection already exists somewhere,
  /// and the seam should not decide where.
  const std::uint16_t* entityIds = nullptr;
  std::uint32_t entityCount = 0;

  /*
   * What the command acts *on*, when it acts on something rather than
   * somewhere. `INVALID_ANCHOR` for the orders that only name a place.
   *
   * Engine-neutral, like `Welcome::gridAnchor` and for the same reason: this
   * is "the id of the thing this order is about", and what that thing *is*
   * stays the game's (ADR-014). One field serves every such verb, so a client
   * whose gesture is "act on that structure" fills the same slot whichever
   * verb the game resolves it to -- which is what keeps a second one from
   * being added the first time a second verb needs it.
   */
  std::uint16_t anchor = INVALID_ANCHOR;
};

/*
 * What was decided about a submitted or proposed order.
 *
 * The reason code is GameLogic's enum and the engine passes the number through
 * without reading it, so a client's local bounce and a server's refusal cannot
 * say different things (ADR-014 §3). `serverOrderId` is zero for a pre-check --
 * nothing has been assigned an id until the authority sees it.
 */
struct OrderVerdict
{
  bool accepted = false;
  std::uint16_t reasonCode = 0;
  std::uint32_t serverOrderId = 0;

  /*
   * Which order this is about -- the client's own monotonic counter, echoed.
   *
   * It travels *back out of the game* rather than being read by the engine,
   * and that is the whole reason it is here. The sequence lives inside the
   * submitted payload, which the engine frames and never parses (ADR-004
   * ruling 4); an engine that dug four bytes out of it to fill in the ack
   * would have started reading game semantics for the sake of one field. The
   * game already parsed it, so the game hands it back.
   *
   * Zero on a pre-check, where the client has not assigned one yet.
   */
  std::uint32_t orderSeq = 0;
};

/// How many marks a preview may carry. One per selected entity at the icon
/// sheet's de-clutter cap would be 1,024; a footprint is drawn per *group*, and
/// beyond this many marks the overlay is noise rather than information.
inline constexpr std::uint32_t MAX_ORDER_PREVIEW_MARKS = 64;

/*
 * Where a pending order would put things, for the overlay to draw.
 *
 * ADR-014 §2 called this `FormationPreview`. Renamed, and the ADR's own §4 is
 * why: `EntityRecord` earns its place in the engine by naming "no ship, order,
 * formation or hull class", and a type called `FormationPreview` fails that
 * test in the library the test was written for. What the engine needs to know
 * is that a proposed command has marks and an extent worth drawing; that some
 * games call the arrangement a formation is the game's business.
 *
 * Marks are plane metres in the grid's local frame, like `OrderIntent`.
 */
struct OrderPreview
{
  float markXMetres[MAX_ORDER_PREVIEW_MARKS] = {};
  float markYMetres[MAX_ORDER_PREVIEW_MARKS] = {};
  std::uint32_t markCount = 0;

  /*
   * Radius of the whole arrangement -- how much room the formation needs.
   *
   * **Not what the puck's ring is sized from**, and that is worth saying here
   * because this field's own comment used to claim it was. A circle at this
   * radius circumscribes a Line rather than enclosing it, so at fleet scale it
   * became an ellipse across the viewport touching the formation at two points.
   * The marks the client draws are one tick per station, which describe the
   * shape truthfully for every formation (`OverlayTuning::puckRadiusPixels`).
   *
   * It stays because it answers a question the ticks do not: *how big is this
   * order*, as one number. The HUD's context bar wants that (S11), and so does
   * any later check about whether an arrangement fits where it is being sent.
   */
  float extentMetres = 0.0f;

  /*
   * Whether this order happens somewhere the client is drawing.
   *
   * **Most orders do, and one kind does not.** A Move, a Dock or a Mine ends at
   * a place on the grid in view, so the promise can be drawn there -- a
   * footprint, one tick per ship, a lane from the fleet to it. An order whose
   * destination is *another grid* has no such place: there is no point on this
   * plane that means "the far side of a gate", and the nearest thing to one is
   * wherever the player's cursor happened to be, which is not the same thing at
   * all.
   *
   * So the game says which it is, and the client draws a mark on the plane or a
   * chip in the chrome. Without this the ghost drew a formation where the
   * gesture landed and promised the fleet would assemble there, while the fleet
   * was in fact leaving the system -- a promise about the wrong world.
   *
   * True by default, because every order that existed before this field did was
   * one that happens here, and a default that changed their behaviour would be
   * the field rewriting history to introduce itself.
   */
  bool onThisGrid = true;

  void Clear() noexcept
  {
    markCount = 0;
    extentMetres = 0.0f;
    onThisGrid = true;
    etaSeconds = -1.0f;
    label[0] = '\0';
    workingLabel[0] = '\0';
  }

  /// Appends a mark, or reports that the preview is full. Returning false
  /// rather than silently dropping keeps a truncated footprint from reading as
  /// a complete one.
  [[nodiscard]] bool AddMark(float _xMetres, float _yMetres) noexcept
  {
    if (markCount >= MAX_ORDER_PREVIEW_MARKS)
    {
      return false;
    }
    markXMetres[markCount] = _xMetres;
    markYMetres[markCount] = _yMetres;
    ++markCount;
    return true;
  }

  /*
   * How long the whole arrangement takes to form, in seconds, or negative when
   * the game will not say.
   *
   * The ghost's lane label is `18.4 km - ETA 41s` (`tactical-hud.png`), and the
   * engine can compute the kilometres from two points it already holds. It
   * cannot compute the seconds: that needs a movement model, and a movement
   * model is exactly the game semantics NeuronCore is charted not to have
   * (ADR-004 ruling 4). So the number crosses.
   *
   * **A prediction, not a replicated fact,** and the distinction is load-
   * bearing rather than pedantic. `overlay-pass.png` §2 puts the whole
   * client-authored draw list on the pre-check rather than on replication, so
   * the label can exist before the order has been sent -- which is the point of
   * previewing it. S12 replicates the authority's own per-leg ETA in the order
   * state; when it does, the ghost prefers that and this stays as what the
   * preview says before there is an authority to ask.
   */
  float etaSeconds = -1.0f;

  /*
   * What to call this order, for the ghost's label -- `MOVE - CLAW`.
   *
   * A buffer rather than a `const char*`, which is what `OrderOption::name` and
   * `RosterRow::name` are. Those are read and drawn inside the frame that asked
   * for them, so pointing at storage the world view owns is safe. A preview is
   * **kept by the ghost**, for as long as the order is in flight, and a pointer
   * into the world view would by then be describing whatever order was previewed
   * next.
   *
   * The engine copies these bytes and never parses them. It cannot: `MOVE` is a
   * kind and `CLAW` is a formation, and ADR-014 §2b is the rule that the engine
   * may name neither.
   */
  static constexpr std::uint32_t LABEL_CAPACITY = 32;
  char label[LABEL_CAPACITY] = {};

  /*
   * What to call this order once it has stopped travelling and started
   * *working* -- `MINING` -- or empty for an order whose whole life is a
   * journey.
   *
   * **Most orders end when their last leg does; one does not**, and this field
   * is how the engine is told which without being told why. A group under way
   * with no leg left is either about to be retired or doing something the plane
   * cannot show as motion, and only the game knows which -- so the game answers
   * by having written a word here, or not.
   *
   * That split is the whole reason this is not derived from `legIndex` and
   * `legCount`. The engine reads both already -- it draws `3 LEGS` from them --
   * so it could compute "no leg left" perfectly well; what it must not do is
   * conclude from that arithmetic that a fleet is *mining*, which is a fact
   * about a game that has fields in it (ADR-014 2b).
   *
   * Filled at preview time and kept by the ghost, exactly like `label` and for
   * the same lifetime reason: by the time an order is working, whatever storage
   * the world view was pointing at has been previewed over several times.
   */
  char workingLabel[LABEL_CAPACITY] = {};
};

/*
 * The opaque numbers a client needs before it has a command surface.
 *
 * `OrderIntent::kind` and `parameter` are GameLogic's, and a client that filled
 * them in itself would have started choosing game semantics. Until the command
 * wheel exists there is one kind and one formation, and the honest way to get
 * them is to ask the side that knows. That the answer is currently `{0, 0}` is
 * the point: a zeroed intent meaning Move-in-Line is a coincidence of two
 * enumerations, and coincidences are what renumber.
 */
struct OrderDefaults
{
  std::uint16_t kind = 0;
  std::uint16_t parameter = 0;
};

/*
 * One value `OrderIntent::parameter` may take, with a word for it.
 *
 * A formation, a stance, whatever the kind's parameter means -- the engine does
 * not know, and this is what lets it offer the choice anyway. The number is
 * copied into an intent and the name is drawn; neither is interpreted, which is
 * the same arrangement `ReasonText` has and for the same reason (ADR-014 §2c).
 *
 * `name` points at storage the world view owns and is valid for its lifetime.
 * In practice these are string literals compiled into the game.
 */
struct OrderOption
{
  std::uint16_t parameter = 0;
  const char* name = nullptr;
};

/// How many options a client will ask for. Eight is the command wheel's sector
/// count (`puck-and-wheel.png` §3), which is the surface these will be drawn on
/// -- a game with more would need a different surface, not a bigger array.
inline constexpr std::uint32_t MAX_ORDER_OPTIONS = 8;

/*
 * One command the game offers, for the command row to draw (S11d).
 *
 * `OrderOption` reports the values one command's parameter may take;
 * this reports **the commands themselves**. Without it a client drawing the
 * print's `MOVE | ATTACK | FORMATION | STANCE | ABILITIES` row would be
 * spelling those five words in the engine -- which is this game's command
 * vocabulary living in a library that is meant to serve a second game with
 * different verbs (ADR-014 §2b). No CI rule would have caught it either: a
 * string is not an include.
 *
 * The same list is what the command wheel's eight sectors will be built from
 * (`puck-and-wheel.png` §3), which is why it arrives shaped for that rather
 * than for a row of five.
 */
struct OrderKindOption
{
  std::uint16_t kind = 0;
  const char* name = nullptr;

  /*
   * What this command's parameter is called -- `Formation` for a Move -- or
   * null when it has none.
   *
   * The print draws `FORMATION` as a button in the row with a dropdown caret,
   * beside the commands rather than inside one. It is not a command: it is the
   * *name of the thing the command varies by*, and a client can offer the
   * choice through `OrderOptions` but cannot say what is being chosen without
   * this.
   */
  const char* parameterName = nullptr;

  /*
   * False for a command this build has no content for.
   *
   * Drawn greyed rather than omitted, which is the sheet's rule and not a
   * convenience: `puck-and-wheel.png` §3 keeps the wheel's eight sectors in
   * fixed positions "so the ring stays learnable as a shape rather than a
   * lookup", and a row whose buttons moved as content arrived would be the same
   * mistake in a line. It also means a player can see what the game *will*
   * offer, which is worth more than a tidier row.
   */
  bool available = false;

  /*
   * Whether this command still needs a gesture to say *where*.
   *
   * True for a Move, which goes to a place the player chooses. **False for a
   * command the game can decide from the selection alone**, and the engine uses
   * that one bit for both halves of the same thing: such a command is greyed or
   * lit *before* any gesture, and its button **issues it on the press** rather
   * than arming the puck -- because there is nothing a second gesture could
   * add, and a lit button that then waited would be asking the player for
   * something they have nothing left to say.
   *
   * Which commands those are is entirely the game's answer. The engine never
   * learns *why* one needs pointing at and another does not; it reads the bit
   * and lays out a different kind of button.
   *
   * Defaults to true, which is the safe direction: a view that does not answer
   * leaves every verb armed-then-gestured rather than firing one on a press.
   */
  bool namesDestination = true;

  /*
   * Why not, when `available` is false -- the game's own reason code, the same
   * number a refused order comes back with, and null-meaning zero when the
   * command is simply offerable.
   *
   * **Two different kinds of unavailable share this field on purpose.** A
   * command this build has no content for is permanently greyed and has no
   * reason to give; a command that is real but not offerable *right now* -- no
   * field on this grid, no Miner in the selection -- has one, and it is the
   * same sentence the authority would have bounced the order with. The engine
   * reads neither: it draws `ReasonText(reasonCode)` beside a greyed verb, so
   * a player learns why the button is dark without the client knowing what any
   * of it means (ADR-014 2b).
   *
   * That this is a *code* and not a string is the parity rule doing its usual
   * work: the same enum, the same table, the same words whether the refusal was
   * predicted here or arrived from the server (ADR-005 4).
   */
  std::uint16_t reasonCode = 0;
};

/// How many commands a client will ask for -- the wheel's eight sectors again,
/// and for the same reason.
inline constexpr std::uint32_t MAX_ORDER_KINDS = 8;

/*
 * What acting on one entity means (ADR-017 2, `tactical-hud.png`).
 *
 * The gesture is "select ships, act on that thing", and the printed command row
 * stays exactly as printed -- so the verb cannot be a button, and the engine
 * cannot decide what it is. It asks the game about the entity under the cursor
 * and gets back a kind to send, a word to draw, and the anchor to fill in.
 *
 * The engine never learns *why* an entity affords a verb. A station affords
 * docking because of what a station is, which is the game's business; all this
 * carries is that something is afforded and what to call it. Future station
 * verbs -- trade, repair, missions -- arrive as different `kind`s through the
 * same call, and no engine code changes.
 */
struct ContextAction
{
  /// The game's order enum, opaque here. Only meaningful when `available`.
  std::uint16_t kind = 0;

  /// The word to draw, from the game -- `DOCK`. Null when nothing is afforded.
  const char* label = nullptr;

  /// What to put in `OrderIntent::anchor`. Usually the entity asked about, but
  /// the game says so rather than the engine assuming it.
  std::uint16_t anchor = INVALID_ANCHOR;

  /// False when this entity affords nothing to this selection -- which is the
  /// common answer, and is not a failure.
  bool available = false;
};

/*
 * One order the authority is still working on, as the client's ghost needs it.
 *
 * Every field is a number: two identities and three counters. `state` is the
 * game's enum and the engine only compares it for change, exactly as it only
 * forwards `reasonCode` -- the client draws a different ghost when the state
 * moves and never learns what "2" is called.
 *
 * This is what promotes a PENDING ghost when the ack was lost. The ack is
 * reliable (ADR-003's control channel), so in practice it arrives first; the
 * snapshot is the path that cannot be lost, because a snapshot the ghost's
 * order is missing from is itself the signal that the order is over.
 */
struct OrderProgress
{
  std::uint32_t serverOrderId = 0;
  std::uint32_t clientOrderSeq = 0;
  std::uint8_t state = 0;
  std::uint8_t legIndex = 0;
  std::uint8_t legCount = 0;
  std::uint8_t memberCount = 0;

  /*
   * Whether the game considers this order over.
   *
   * A bool rather than "the engine compares `state` to 2", and that is the seam
   * rather than fussiness (ADR-014 §2): `state` is a number the game chose and
   * the engine forwards without ever learning what its values mean, so an
   * engine that decoded one would be an engine with an opinion about this
   * game's order model. The *game* knows which of its states is finished, and
   * says so here.
   *
   * The ghost that was drawing the order retires on this. Without it a promise
   * that has been kept stays on screen for ever, pointing at a destination the
   * ships already reached.
   */
  bool finished = false;

  /*
   * Seconds until this order's current leg completes, or negative when the
   * game will not say (S12).
   *
   * The *authority's* number, which is what makes it different from
   * `OrderPreview::etaSeconds`. That one is a prediction the game made before
   * the order was sent, from a fleet standing still; this one is measured from
   * where the ships have actually got to and how fast they are actually going.
   * A ghost prefers this the moment it has one, and the engine never learns
   * that either is a *time* -- it formats a number the game handed it.
   *
   * A float here and whole seconds on the wire: the seam is not the budget, and
   * a client that had to remember the quantisation to divide by would be doing
   * the arithmetic the wire already did.
   */
  float etaSeconds = -1.0f;
};

/*
 * How many legs of one order a client will draw (S12).
 *
 * The queue's real cap is GameLogic's -- `puck-and-wheel.png` §4 says four
 * slots and the game enforces it -- and this is the engine's buffer for
 * whatever the game reports, the same arrangement `MAX_ORDER_PROGRESS` has.
 * Eight is twice the sheet's four, so a game with a deeper queue draws what it
 * can rather than overrunning, and `OrderProgress::legCount` is a `uint8_t` in
 * any case.
 */
inline constexpr std::uint32_t MAX_GHOST_LEGS = 8;

/// How many live orders the game may report in one poll. The snapshot's order
/// area is the real cap and it is GameLogic's; this is the engine's buffer for
/// it, asserted equal where the two meet so a game that reports more than the
/// client can hold fails to compile rather than to draw.
inline constexpr std::uint32_t MAX_ORDER_PROGRESS = 16;

/*
 * What the authority says about the orders this client has outstanding.
 *
 * Polled once a frame rather than pushed, for the reason `BuildScene` is
 * polled: the game already has the newest snapshot decoded, and a callback
 * would mean the game deciding when the client's ghost list is allowed to
 * change.
 */
struct OrderFeedback
{
  /*
   * The highest sequence the authority has finished deciding about.
   *
   * A ghost whose sequence is at or below this has been answered -- accepted
   * and present in `orders`, or accepted and already finished, or refused. A
   * ghost above it is still in flight. One monotonic number closes the loop for
   * every order at once, which is why it is in the snapshot header rather than
   * being derivable from the records (ADR-004 §6).
   */
  std::uint32_t lastOrderSeqProcessed = 0;

  OrderProgress orders[MAX_ORDER_PROGRESS] = {};
  std::uint32_t orderCount = 0;

  void Clear() noexcept
  {
    lastOrderSeqProcessed = 0;
    orderCount = 0;
  }

  /// Appends, or reports the buffer is full. A dropped progress record means a
  /// ghost draws one frame behind; silently overwriting one would mean a ghost
  /// that never promotes.
  [[nodiscard]] bool Add(const OrderProgress& _progress) noexcept
  {
    if (orderCount >= MAX_ORDER_PROGRESS)
    {
      return false;
    }
    orders[orderCount] = _progress;
    ++orderCount;
    return true;
  }
};

} // namespace Neuron
