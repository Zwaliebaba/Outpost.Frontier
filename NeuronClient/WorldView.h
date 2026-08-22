#pragma once

#include "ByteWriter.h"
#include "DeltaReceiver.h"
#include "HudRoster.h"
#include "OrderIntent.h"
#include "StationIntent.h"
#include "StationView.h"
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
   * Hands the game one assembled tick and asks it to take it. False if the
   * frame was rejected.
   *
   * **This used to be `ApplySnapshot(payload) -> tick`, and both halves of that
   * changed with ADR-022.** The payload was opaque and the game read the tick
   * out of it, because the whole snapshot was the game's from its first byte.
   * Interest and delta made the envelope the engine's: reassembling a
   * multi-part tick, holding the acked baseline and deciding when a tick is
   * whole are decisions about a viewer and a link (§1), and `EntityRecord` is
   * this library's type (ADR-014 §4). So the engine now *knows* the tick --
   * `DeltaHeader` carries it -- and passing it in is no longer the engine
   * guessing a number from somewhere else.
   *
   * What is still the game's, and still travels unread, is `ReplicatedFrame::tail`:
   * the order-state records and the order high-water mark, under the game's own
   * schema hash, exactly as a summary is.
   *
   * The tick is the only clock either side agrees on (ADR-002 §1).
   */
  [[nodiscard]] virtual bool ApplyFrame(const ReplicatedFrame& _frame) = 0;

  /*
   * Hands the game one summary payload, the same way and with the same
   * ignorance (ADR-016 6).
   *
   * The summary family answers questions a snapshot deliberately does not --
   * where a commander's ships are when they are nowhere on this grid, and what
   * is sitting in a hangar. The engine frames those bytes, orders them and does
   * not read them; which member of the family arrived is a byte inside, and a
   * connection that looked would have learned what a station is.
   *
   * No tick comes back, unlike `ApplySnapshot`, because there is nothing to
   * time: a summary describes a state rather than an instant and never drives
   * the render clock. True when the payload was understood, false when it was
   * refused -- which the caller uses for a diagnostic and nothing else, because
   * a dropped summary is a stale panel rather than a broken world.
   *
   * Defaulted rather than pure: a game with no summary family is a real thing,
   * and every client before this one was one.
   */
  [[nodiscard]] virtual bool ApplySummary(std::span<const std::uint8_t> _payload)
  {
    (void)_payload;
    return false;
  }

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
   * **It takes the selection, and is asked every frame.** It used to be asked
   * once at boot, because "which verbs does this game have" is a question whose
   * answer is compiled in. That is not the only question the row needs
   * answering: a verb can be real and still not offerable *now* -- MINE with no
   * field on this grid, or with no Miner among the selected ships -- and a row
   * that offered it anyway would be promising a refusal. So the game is handed
   * what is selected and answers about this moment, which is the arrangement
   * `BuildRoster` already has and for the same reason: the client must not be
   * the one deciding which verbs a selection qualifies for.
   *
   * A verb the game turns off carries `reasonCode`, so the row can say *why*
   * in the game's own words through `ReasonText` -- the same code the authority
   * would have bounced the order with, which is ADR-005 4's parity rule
   * reaching a button before it ever reaches a toast.
   *
   * Writes at most `_outKinds.size()` and returns how many.
   */
  [[nodiscard]] virtual std::uint32_t OrderKinds(std::span<const EntityId> _selectedIds,
                                                 std::span<OrderKindOption> _outKinds) const = 0;

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
  [[nodiscard]] virtual std::uint32_t BuildRoster(std::span<const EntityId> _selectedIds,
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
  [[nodiscard]] virtual bool ContextActionFor(EntityId _entityId, std::span<const EntityId> _selectedIds,
                                              ContextAction& _outAction) const
  {
    (void)_entityId;
    (void)_selectedIds;
    (void)_outAction;
    return false;
  }

  /*
   * The player's ships that are not in the scene, by where they are
   * (ADR-017 1, ADR-016 9).
   *
   * Three ways a ship can be absent from what the client draws, and the engine
   * can distinguish none of them: docked ships despawn into a roster the
   * authority keeps, a fleet on another grid is in a world this client is not
   * watching, and a fleet mid-warp is in no world at all. Like `BuildRoster` it
   * is the *game* that aggregates -- which place, what to call it, what to call
   * its state, and which ships count as the player's are all questions the
   * engine must not answer.
   *
   * **Every place, including the one being watched.** A client that draws the
   * world it is looking at will not want to *draw* that row, and dropping it is
   * that client's business: the same list answers "does where I am looking still
   * hold anything of mine", which is what makes following a fleet possible, and
   * a game that had already removed the answer would have decided what the
   * client may notice.
   *
   * Returns how many blocks were written, never more than the span holds.
   */
  [[nodiscard]] virtual std::uint32_t BuildLocationBlocks(std::span<LocationBlock> _outBlocks) const
  {
    (void)_outBlocks;
    return 0;
  }

  /*
   * How much is left of the place the fleet is standing in (ADR-024 §3d).
   *
   * False when the grid is not that kind of place, which is the common case and
   * not a failure -- most grids are not fields, and a client that treated
   * absence as an error would have to know which grids are special.
   *
   * Defaulted, like every call added after the first client shipped.
   */
  [[nodiscard]] virtual bool BuildFieldReadout(FieldReadout& _outReadout) const
  {
    (void)_outReadout;
    return false;
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
   * Things the game wants said, since the last time it was asked (ADR-011 12).
   *
   * The toast stack is the engine's surface and its levels, its dwell times and
   * its coalescing are presentation -- but the *words* are not. "A fleet
   * finished docking" is a sentence only the game can write, and a client that
   * composed it would have learned that ships dock, that stations are places
   * and what to call the moment one arrives (ADR-014 2b). So the game hands
   * over a title, a body and a key, and the engine decides how loudly to say
   * it and for how long.
   *
   * Polled and **drained**: what is returned has been handed over, and asking
   * again returns what has happened since. That is deliberate -- a notice the
   * client dropped because its stack was full must not come back next frame as
   * though it had just happened.
   *
   * The strings point at storage the world view owns and are valid until the
   * next call, which is long enough: the caller copies them into the stack.
   * Returns how many were written, never more than the span holds.
   */
  [[nodiscard]] virtual std::uint32_t PollNotices(std::span<Notice> _outNotices)
  {
    (void)_outNotices;
    return 0;
  }

  /*
   * --- the station surface (ADR-017 §6, ADR-020 §6) ------------------------
   *
   * Three calls, which is ADR-020 §6's seam budget for a screen spent exactly:
   * one asked-once builder (the tab row), one row builder at the roster's own
   * rate, and pure query functions. A screen wanting a fourth shape is that
   * section's tripwire rather than a fourth method.
   *
   * All three carry defaults rather than being pure, so a view that has no
   * station to describe -- `NullWorldView`, a test's stub -- inherits an honest
   * nothing instead of having to write one.
   */

  /*
   * The tab row for this station.
   *
   * Asked once on entry rather than per frame: the row is fixed, and a service
   * arriving is a build rather than an event. The words and the reasons are
   * the game's, which is the whole reason this is a call and not a table in
   * the client (ADR-020 §6's leak test).
   *
   * Returns how many were written, never more than the span holds.
   */
  [[nodiscard]] virtual std::uint32_t BuildStationTabs(std::uint16_t _anchor, std::span<StationTab> _outTabs) const
  {
    (void)_anchor;
    (void)_outTabs;
    return 0;
  }

  /*
   * The docked roster: group rows and chip rows, together.
   *
   * One call rather than two because it is one walk over one roster, and two
   * would be two chances to disagree about which group a chip is in.
   *
   * `_selectedIds` are the composer's, and the game counts them into
   * `StationGroup::selectedCount` for the reason `BuildRoster` does: the
   * engine matching ids against group membership would be the aggregation it
   * is not supposed to be doing.
   *
   * Sorted by the game, and the order is a decision rather than a convenience
   * -- ADR-017 §6a.4 puts class first and **ship id** second, not name, because
   * names are client-side and two clients must not read the same hangar in two
   * orders.
   */
  [[nodiscard]] virtual StationRosterCounts BuildStationRoster(std::uint16_t _anchor,
                                                               std::span<const std::uint32_t> _selectedIds,
                                                               std::span<StationGroup> _outGroups,
                                                               std::span<StationChip> _outChips) const
  {
    (void)_anchor;
    (void)_selectedIds;
    (void)_outGroups;
    (void)_outChips;
    return StationRosterCounts{};
  }

  /*
   * Which ships are in a roster group, by the id the row handed back.
   *
   * `RosterRow::groupId` has always been documented as "what a click on the row
   * would hand back to the game, once rows are clickable" -- this is the other
   * end of that sentence. The engine echoes an opaque number and gets entity
   * ids; it never learns that a group is a wing, or that a wing is a number
   * between 1 and 255.
   *
   * A pure query, asked on the press rather than per frame, which is the third
   * of ADR-020 §6's three shapes. Per-frame would put a walk over the fleet in
   * every frame to answer a question nobody asked.
   *
   * **Counted from the same population `BuildRoster` counts**, which is the
   * frame's own sampled fleet rather than the newest snapshot. A row saying
   * three and a press selecting four would be a row about a different fleet
   * from the one on screen.
   *
   * Returns how many were written, never more than the span holds.
   */
  [[nodiscard]] virtual std::uint32_t BuildGroupMembers(std::uint16_t _groupId,
                                                        std::span<EntityId> _outIds) const
  {
    (void)_groupId;
    (void)_outIds;
    return 0;
  }

  /*
   * The composer's actions for this station: what the primary button says, and
   * which verb it sends.
   *
   * `OrderKinds`' asked-once shape, and ADR-020 §6's budget is three *shapes*
   * rather than three methods -- this is the first of them again, for a screen
   * rather than for a row. It exists because a screen that could not ask would
   * have to spell `StationVerb::Undock` itself, which is the leak test's whole
   * subject: the client sends the verb and must not know the word.
   *
   * Zero from a view with nothing docked here, which greys the composer rather
   * than offering an action against an empty hangar.
   */
  [[nodiscard]] virtual std::uint32_t BuildStationActions(std::uint16_t _anchor,
                                                          std::span<StationAction> _outActions) const
  {
    (void)_anchor;
    (void)_outActions;
    return 0;
  }

  /*
   * The values a station verb's parameter may take, with words for them.
   *
   * `OrderOptions` exactly -- same `OrderOption`, same contract: the number is
   * copied into a `StationIntent` and the name is drawn, and neither is
   * interpreted. Undock's parameter is a formation, which is why the type is
   * the order side's rather than one of this file's.
   *
   * An empty answer is legitimate and means the verb takes no parameter, the
   * path `OrderOptions` already keeps open for a kind with none.
   */
  [[nodiscard]] virtual std::uint32_t StationActionOptions(std::uint16_t _verb,
                                                           std::span<OrderOption> _outOptions) const
  {
    (void)_verb;
    (void)_outOptions;
    return 0;
  }

  /*
   * Whether this command would be taken, and why not.
   *
   * The station half of ADR-014 §3's parity claim, and the print asks for it in
   * as many words: "the pre-check runs against the same `RosterView` the server
   * validates with, so a tappable UNDOCK is a promise". The verdict type is the
   * order stream's because the reason enum is (ADR-017 §8).
   *
   * The default refuses. A view that cannot judge must not wave a command
   * through -- the posture `ValidationView`'s optional fields already take.
   */
  [[nodiscard]] virtual OrderVerdict PreCheckStation(const StationIntent& _intent)
  {
    (void)_intent;
    return OrderVerdict{};
  }

  /*
   * The bytes for a station command, or false and nothing written.
   *
   * `EncodeOrder`'s twin, and it exists because until it did the format had a
   * validator, a server path and a `selfTest` that hand-assembled it, and no
   * line in the client that could send one -- so UNDOCK could be validated,
   * acked and replayed, and never issued.
   */
  [[nodiscard]] virtual bool EncodeStationCommand(const StationIntent& _intent, ByteWriter& _writer)
  {
    (void)_intent;
    (void)_writer;
    return false;
  }

  /*
   * How many ships one station command may name.
   *
   * The game's cap, asked rather than assumed, because the screen has to
   * declare the consequence *before* the player commits: a selection past it
   * undocks as waves, and `station-screen.png` §2 is explicit that the cap "is
   * a wire constant the player should never meet as an error".
   *
   * Zero from a view with no game, which the caller reads as "nothing can be
   * sent" rather than as "no limit".
   */
  [[nodiscard]] virtual std::uint32_t ShipsPerStationCommand() const { return 0; }

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
  /// Records the size and takes nothing: a view with no world has nowhere to
  /// put a frame, and pretending otherwise would give the clock estimate
  /// something to track that nothing is producing.
  [[nodiscard]] bool ApplyFrame(const ReplicatedFrame& _frame) override
  {
    m_lastPayloadBytes = static_cast<std::uint32_t>(_frame.entities.size() * ENTITY_RECORD_BYTES + _frame.tail.size());
    return false;
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
  [[nodiscard]] std::uint32_t OrderKinds(std::span<const EntityId>, std::span<OrderKindOption>) const override
  {
    return 0;
  }

  [[nodiscard]] std::uint32_t BuildRoster(std::span<const EntityId>, std::span<RosterRow>) const override { return 0; }

  void PollOrderFeedback(OrderFeedback& _outFeedback) override { _outFeedback.Clear(); }

  [[nodiscard]] const char* ReasonText(std::uint16_t) const override { return "no world"; }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return 0; }

  [[nodiscard]] std::uint32_t LastPayloadBytes() const noexcept { return m_lastPayloadBytes; }

private:
  std::uint32_t m_lastPayloadBytes = 0;
};

} // namespace Neuron
