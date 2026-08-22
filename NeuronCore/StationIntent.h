#pragma once

#include "OrderIntent.h"

#include <cstdint>

/*
 * The other half of the command seam: what a player asks of a *place* rather
 * than of ships in the world (ADR-014 §2, ADR-017 §8).
 *
 * It sits in NeuronCore beside `OrderIntent` for the identical reason, and the
 * reason is worth restating rather than referring to: **both seams speak it.**
 * `Neuron::WorldView::PreCheckStation` (NeuronClient) and the composition
 * root's `ApplyStationCommand` (behind `Neuron::Simulation`, NeuronServer) have
 * to reach the same verdict about the same request, and NeuronClient cannot see
 * NeuronServer -- so the type they both name has to sit below both.
 *
 * **The verdict is `OrderVerdict`, deliberately.** ADR-017 §8 put station
 * commands on the acked *order* stream: one sequence counter, one `OrderAck`,
 * one reason enum. A second verdict type would be a second vocabulary for
 * refusals, and the bounce a player sees would depend on which of two paths
 * their gesture happened to take.
 *
 * Like `OrderIntent`, it passes NeuronCore's zero-game-semantics test (ADR-004
 * ruling 4): a `StationIntent` is "the player asked that place to do something
 * with these ships", and the engine never learns what.
 */

namespace Neuron
{

/*
 * A station command the player has expressed but not yet sent.
 *
 * **Ship ids are 32-bit here while `OrderIntent`'s are 16.** Not an
 * inconsistency: they are different populations. `OrderIntent::entityIds` are
 * `EntityRecord::id`, which exist because the ship is in a snapshot -- and a
 * docked ship is not in any snapshot, because docking despawns it (ADR-017 §1).
 * A roster id is the durable one, u32 by ADR-018 A11/D6, and it is the only id
 * a docked ship has to be named by.
 */
struct StationIntent
{
  /*
   * GameLogic's station verb, opaque here. The engine's only interest is that
   * the number round-trips -- the same contract `OrderIntent::kind` has, and
   * the same reason a screen may offer a verb it cannot name.
   */
  std::uint16_t verb = 0;

  /*
   * Whatever the verb needs, one field for all of them.
   *
   * A formation for one verb and a wing number for another, exactly as
   * `OrderIntent::parameter` is a formation for one kind and a stance for the
   * next. One slot rather than one per verb, so a screen that has resolved a
   * gesture to a verb fills the same field whichever verb it resolved to.
   */
  std::uint16_t parameter = 0;

  /*
   * Which place is being asked. Never `INVALID_ANCHOR` for a real command:
   * unlike an order, a station command has nowhere to happen without one.
   */
  std::uint16_t anchor = INVALID_ANCHOR;

  /*
   * The client's own monotonic counter, shared with the order stream's.
   *
   * One counter across both message families, because they share one ack: two
   * counters would collide in `OrderVerdict::orderSeq` and a client could not
   * tell which of two pending things an ack was about.
   *
   * Zero on a pre-check, like `OrderIntent::orderSeq`, and for the same reason:
   * nothing has been sent, so nothing needs matching.
   */
  std::uint32_t orderSeq = 0;

  /// Which docked ships the command is for. A span in two fields rather than a
  /// container, the shape `OrderIntent` uses: the selection already exists
  /// somewhere and the seam should not decide where.
  const std::uint32_t* shipIds = nullptr;
  std::uint32_t shipCount = 0;
};

} // namespace Neuron
