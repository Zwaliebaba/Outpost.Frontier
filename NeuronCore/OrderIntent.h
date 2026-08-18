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

  void Clear() noexcept
  {
    markCount = 0;
    extentMetres = 0.0f;
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
};

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
