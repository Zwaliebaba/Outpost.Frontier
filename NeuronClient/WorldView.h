#pragma once

#include "ByteWriter.h"
#include "OrderIntent.h"
#include "RenderWorld.h"

#include <cstdint>
#include <span>

/*
 * The engine/game seam, client side (ADR-014 §2).
 *
 * `ClientApp` owns the window, the device, the pass list, the camera, picking
 * and the HUD. It does not own meaning. Everything on this interface is the
 * game answering a question the presentation layer cannot answer for itself:
 * what does this snapshot say, what should be on screen at this instant, would
 * this order be refused, what would it look like, and what bytes does it send.
 *
 * The mirror of `Neuron::Simulation` (NeuronServer), and deliberately shaped
 * the same way: opaque payloads in and out, neutral records in between, and no
 * type in this file that names a ship, a wing or a hull class. The moment one
 * appears, NeuronClient has stopped being an engine and become this game's
 * client -- which matters because the sibling repository ships these same
 * libraries around an entirely different game.
 *
 * **Who implements it.** ADR-014 §2 says GameLogic does, and ADR-014 §1 says
 * GameLogic depends on NeuronCore only. Both cannot be true: this header is
 * NeuronClient's, so a GameLogic class implementing it would need NeuronClient
 * on its include path, and GameLogic's freedom from Windows, D3D12 and file IO
 * would become a convention instead of a structural fact. It is resolved the
 * way ADR-008 already points: **the composition root holds the vtable.**
 * `Outpost.exe` implements this by forwarding to GameLogic's pure functions,
 * which is wiring rather than logic (ADR-014 §6). GameLogic keeps its charter,
 * the engine keeps its ignorance, and the adapter lives in the one project that
 * was always allowed to know both. ADR-014 §2a records it.
 */

namespace Neuron
{

class WorldView
{
public:
  virtual ~WorldView() = default;

  /*
   * Hands the game one snapshot payload, exactly as it came off the wire, and
   * asks what tick it described. Returns 0 if the payload was rejected.
   *
   * The tick is a *return* value rather than a parameter, and that is the whole
   * design of this call. The engine has framed and ordered the payload and has
   * not looked inside, so it cannot know which tick the bytes describe -- only
   * the game can read that. Passing a tick in would mean the engine supplying a
   * number it had guessed from somewhere else (the last `Pong`, say), and the
   * clock estimate would then be built on a value that drifts from the payload
   * it is supposed to time. Putting the tick in the framing as well would fix
   * that and create two copies of one number, which is the arrangement S5b
   * already refused for the content hash.
   *
   * The tick is the only clock either side agrees on (ADR-002 §1).
   */
  [[nodiscard]] virtual std::uint32_t ApplySnapshot(std::span<const std::uint8_t> _payload) = 0;

  /*
   * Fills the scene for a presentation instant.
   *
   * `_renderTick` is fractional on purpose: the client draws between snapshots
   * and the game owns the interpolation, because only the game knows which
   * quantities interpolate and which snap (ADR-002 §4). The scene is cleared
   * and refilled rather than diffed -- it is rebuilt every frame anyway, and a
   * partially updated scene is worse than a rebuilt one.
   */
  virtual void BuildScene(double _renderTick, RenderScene& _outScene) = 0;

  /*
   * Would this order be refused, and why?
   *
   * The client asks before sending so a refusal is immediate rather than a
   * round trip away. ADR-014 §3 is the requirement this exists for: the same
   * validation runs here and on the authority, so a local bounce and a server
   * refusal carry the same reason code. Reached through this interface instead
   * of a link-time symbol -- same function, same answer.
   */
  [[nodiscard]] virtual OrderVerdict PreCheck(const OrderIntent& _intent) = 0;

  /*
   * Where the order would put things, for the overlay to draw.
   *
   * The footprint under the cursor has to be the real arrangement rather than a
   * decorative ellipse, or the preview is a lie the player will discover by
   * issuing the order (ADR-006 §8's client-authored draw list is what renders
   * the result).
   */
  virtual void SolvePreview(const OrderIntent& _intent, OrderPreview& _outPreview) = 0;

  /*
   * Encodes the order for the wire.
   *
   * The game owns the message layout (ADR-004 ruling 1); the engine writes the
   * bytes into a datagram and never inspects them. Returns false if the order
   * does not fit, which the caller must treat as "not sent" rather than
   * "sent empty".
   */
  [[nodiscard]] virtual bool EncodeOrder(const OrderIntent& _intent, ByteWriter& _writer) = 0;

  /*
   * The game's message layout and authored content, for the handshake.
   *
   * The same two numbers `Simulation` reports, from the same content -- and
   * asked of the game rather than passed in as configuration, because a client
   * that is told its own content hash cannot detect that its content changed.
   */
  [[nodiscard]] virtual std::uint64_t SchemaHash() const = 0;
  [[nodiscard]] virtual std::uint64_t ContentHash() const = 0;
};

/*
 * A world view with no world, for running the client before a game exists and
 * for tests that care about the frame rather than the fleet.
 *
 * Game-free by construction, so it belongs to the engine -- the same argument
 * that puts `NullSimulation` in NeuronServer. It builds an empty scene rather
 * than a placeholder one: a client wired to nothing should look like a client
 * wired to nothing.
 */
class NullWorldView final : public WorldView
{
public:
  /// Records the size and reports no tick: a view with no world cannot say
  /// when it is, and claiming a tick would give the clock estimate something
  /// to track that nothing is producing.
  [[nodiscard]] std::uint32_t ApplySnapshot(std::span<const std::uint8_t> _payload) override
  {
    m_lastPayloadBytes = static_cast<std::uint32_t>(_payload.size());
    return 0;
  }

  void BuildScene(double, RenderScene& _outScene) override { _outScene.Clear(); }

  [[nodiscard]] OrderVerdict PreCheck(const OrderIntent&) override
  {
    return OrderVerdict{}; // Refuses everything: there is nothing to command.
  }

  void SolvePreview(const OrderIntent&, OrderPreview& _outPreview) override { _outPreview.Clear(); }

  [[nodiscard]] bool EncodeOrder(const OrderIntent&, ByteWriter&) override { return false; }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0; }

  [[nodiscard]] std::uint32_t LastPayloadBytes() const noexcept { return m_lastPayloadBytes; }

private:
  std::uint32_t m_lastPayloadBytes = 0;
};

} // namespace Neuron
