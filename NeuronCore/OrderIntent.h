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

  /// Radius of the whole arrangement, for the ring the puck draws around it.
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

} // namespace Neuron
