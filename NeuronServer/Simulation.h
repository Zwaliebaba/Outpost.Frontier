#pragma once

#include "ByteWriter.h"
#include "OrderIntent.h"
#include "Wire.h"

#include <cstdint>
#include <span>
#include <string>

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
  std::int64_t anchorX = 0; // The tactical grid's origin, in whole world units.
  std::int64_t anchorY = 0;

  std::string worldName;   // The prime slot: where the player is.
  std::string worldDetail; // The dim line beside it: region-of-space and version.
  std::string worldBadge;  // The right cluster's badge, drawn verbatim.
};

class Simulation
{
public:
  virtual ~Simulation() = default;

  /// Advances one fixed step. The tick index is the simulation's only clock
  /// (ADR-002 §1): implementations must not read a wall clock.
  virtual void AdvanceTick(std::uint32_t _tick) = 0;

  /*
   * Serializes the state **one viewer** needs for this tick.
   *
   * The viewer is here from this method's first line rather than added when it
   * first matters (ADR-018 A13, ADR-022 §1). A simulation is free to ignore it
   * -- and today's does, because there is one grid and one player -- but the
   * *seam* may not, because every replication decision after this one is a
   * decision about a viewer: which grid they are watching, what they own, what
   * they last acked. A signature without a viewer is a signature that has to
   * change at every call site on the day two clients see different worlds.
   *
   * `PlayerId` and not `clientId`: the durable player, never the connection
   * (ADR-018 D5). A snapshot is owed to whoever is commanding, across the
   * disconnect that D5's grace window is designed to survive.
   *
   * Returns false if it could not -- which at MVP scale means the fleet
   * outgrew one datagram, the point at which ADR-004 §6's growth path stops
   * being optional. A bool rather than a silent short write, because a
   * truncated snapshot is worse than a missing one: the client would read the
   * absent ships as despawned and resurrect them on the next tick.
   * [ADR-022](../Design/ADR/ADR-022-interest-and-delta.md) §6 replaces this
   * refusal with priority truncation; until that slice lands it stays exactly
   * as loud as it is.
   */
  [[nodiscard]] virtual bool WriteSnapshot(PlayerId _viewer, std::uint32_t _tick, ByteWriter& _writer) = 0;

  /// Validates and applies one order payload. Returning a verdict rather than a
  /// bool keeps the refusal reason with the decision that produced it.
  [[nodiscard]] virtual OrderVerdict ApplyOrderBytes(std::uint32_t _clientId, std::span<const std::uint8_t> _payload) = 0;

  /// Message layout of the game's own wire types. Exchanged at the handshake so
  /// mismatched builds refuse each other at the door.
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
  [[nodiscard]] bool WriteSnapshot(PlayerId, std::uint32_t, ByteWriter&) override { return false; }

  [[nodiscard]] OrderVerdict ApplyOrderBytes(std::uint32_t, std::span<const std::uint8_t>) override
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
