#pragma once

#include "ByteWriter.h"
#include "HudRoster.h"
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
   * What to put in an intent the player has not qualified.
   *
   * The client turns a gesture into a place and a facing; which *command* that
   * is belongs to a surface that does not exist yet (the command wheel, S11),
   * and until it does the answer is the game's single default. Asking beats
   * leaving the fields zero, because zero meaning Move-in-Line is an accident
   * of two enumerations rather than an agreement.
   */
  [[nodiscard]] virtual OrderDefaults DefaultOrder() const = 0;

  /*
   * Which parameters a kind accepts, and what to call them.
   *
   * Writes at most `_outOptions.size()` and returns how many. The client offers
   * the list -- a key that steps it now, the command wheel's formation
   * sub-ring in S11 -- and copies the chosen number into `OrderIntent`.
   *
   * **This exists because the alternative is the client counting formations.**
   * A client that cycled `parameter` from 0 to 2 would have learned how many
   * formations this game has and that they are numbered contiguously; a client
   * that hard-coded "Line/Wedge/Claw" would have learned their names. Both are
   * game semantics arriving by the back door, and both break silently when a
   * second game ships on this engine with four stances instead (ADR-014).
   *
   * An empty answer is legitimate and means the kind takes no parameter.
   */
  [[nodiscard]] virtual std::uint32_t OrderOptions(std::uint16_t _kind, std::span<OrderOption> _outOptions) const = 0;

  /*
   * Which commands this game offers, in the order a surface should show them.
   *
   * The command row's buttons and, later, the wheel's sectors. The engine draws
   * a name and greys the ones marked unavailable, and never learns that the
   * first is a movement order -- which is the point: a second game on these
   * libraries has different verbs, and a row that spelled `MOVE` and `ATTACK`
   * in NeuronClient would be this game's vocabulary compiled into the engine.
   *
   * Writes at most `_outKinds.size()` and returns how many.
   */
  [[nodiscard]] virtual std::uint32_t OrderKinds(std::span<OrderKindOption> _outKinds) const = 0;

  /*
   * The fleet roster's rows, for the HUD's left column (`tactical-hud.png`).
   *
   * Writes at most `_outRows.size()` and returns how many. `_selectedIds` is
   * the client's current selection, so a row can report how much of itself is
   * selected without the engine matching ids against group membership.
   *
   * **The game aggregates, and that is the whole point of the call.** The
   * engine has `EntityRecord::groupId` and could group by it in four lines --
   * and would thereby have decided that groups are worth showing, that they are
   * named, that a group's health averages rather than takes a minimum, and that
   * an empty group disappears rather than showing as empty. Those are design
   * questions about a particular game, so they are answered on the side that is
   * allowed to answer them.
   *
   * An empty answer is legitimate: a game with no groups has no roster, and the
   * panel draws its frame with nothing in it.
   */
  [[nodiscard]] virtual std::uint32_t BuildRoster(std::span<const std::uint16_t> _selectedIds,
                                                  std::span<RosterRow> _outRows) const = 0;

  /*
   * What acting on this entity would mean, for this selection (ADR-017 2).
   *
   * The tactical gesture is "select ships, act on that thing", and the printed
   * command row stays exactly as printed -- so the verb is not a button and the
   * engine cannot pick it. It asks here about whatever is under the cursor and
   * is told a kind to send, a word to draw and an anchor to fill in.
   *
   * Answering "nothing" is the common case and is not a failure: most entities
   * afford nothing to most selections, and a client that treated absence as an
   * error would have to know which entities are special.
   *
   * Defaulted rather than pure, because a game with no context verbs is a real
   * thing and every client before this one was one.
   */
  [[nodiscard]] virtual bool ContextActionFor(std::uint16_t _entityId, std::span<const std::uint16_t> _selectedIds,
                                              ContextAction& _outAction) const
  {
    (void)_entityId;
    (void)_selectedIds;
    (void)_outAction;
    return false;
  }

  /*
   * The player's ships that are not in the scene, by where they are
   * (ADR-017 1).
   *
   * Docked ships despawn: they are a roster the authority keeps, not hulls with
   * positions, so nothing the engine has can list them. This is the one call
   * that can, and like `BuildRoster` it is the *game* that aggregates -- which
   * place, what to call it, and which ships count as the player's are all
   * questions the engine must not answer.
   *
   * Returns how many blocks were written, never more than the span holds.
   */
  [[nodiscard]] virtual std::uint32_t BuildDockedBlocks(std::span<DockedBlock> _outBlocks) const
  {
    (void)_outBlocks;
    return 0;
  }

  /*
   * What the authority has decided about orders already sent.
   *
   * Read out of the newest snapshot, which is the game's to parse. The client
   * uses it to promote a PENDING ghost and to retire one whose order has
   * finished; it reads the numbers and not their meanings (ADR-014 §5).
   *
   * Polled rather than pushed so the ghost list changes at a point in the frame
   * the client chose.
   */
  virtual void PollOrderFeedback(OrderFeedback& _outFeedback) = 0;

  /*
   * The diagnostic text for a reason code.
   *
   * The bounce toast has to say *why*, and the reason enum is the game's
   * (ADR-005 §4). The engine holds a code it cannot interpret, so the string
   * comes from the same side the code did -- which is also what keeps a local
   * refusal and a server refusal reading identically, since both codes travel
   * the same path to the same function (`puck-and-wheel.png` §4's BounceParity).
   *
   * Never null: an unknown code returns a string saying so, because a toast
   * with no text is the silent disappearance the print forbids.
   */
  [[nodiscard]] virtual const char* ReasonText(std::uint16_t _reasonCode) const = 0;

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

  [[nodiscard]] OrderDefaults DefaultOrder() const override { return OrderDefaults{}; }

  [[nodiscard]] std::uint32_t OrderOptions(std::uint16_t, std::span<OrderOption>) const override { return 0; }
  [[nodiscard]] std::uint32_t OrderKinds(std::span<OrderKindOption>) const override { return 0; }

  [[nodiscard]] std::uint32_t BuildRoster(std::span<const std::uint16_t>, std::span<RosterRow>) const override { return 0; }

  void PollOrderFeedback(OrderFeedback& _outFeedback) override { _outFeedback.Clear(); }

  [[nodiscard]] const char* ReasonText(std::uint16_t) const override { return "no world"; }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0; }

  [[nodiscard]] std::uint32_t LastPayloadBytes() const noexcept { return m_lastPayloadBytes; }

private:
  std::uint32_t m_lastPayloadBytes = 0;
};

} // namespace Neuron
