#pragma once

#include "World.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "EntityRecord.h"
// For the datagram cap. GameLogic does not touch the transport, but the
// snapshot has to fit one datagram, so the budget below is derived from the one
// definition of that number rather than restating it (ADR-004 §6). The header
// is a pure interface -- no Windows, no sockets.
#include "Transport.h"

#include <cstdint>
#include <vector>

/*
 * State replication, server to client (ADR-004 §6).
 *
 * **Full snapshots every tick, no deltas.** Any snapshot completely replaces
 * the previous one, so loss is not a case to handle: a dropped datagram costs
 * one tick of freshness and nothing else. There is no baseline to track, no
 * acknowledgement to wait on, and no way for the client's view to drift out of
 * agreement with the server's. `baselineTick` is on the wire and always zero,
 * reserved so a delta-against-acked-baseline scheme slots in later without a
 * format break.
 *
 * **The ship record is `Neuron::EntityRecord` and not a type of ours.** ADR-004
 * §6 specifies exactly the twenty bytes NeuronCore already defines, and ADR-014
 * §4 is why they live there: the engine has to buffer, interpolate and draw
 * without knowing what a ship is. Declaring a `ShipRecord` here that happened to
 * match would be two layouts to keep in step and one of them would eventually
 * lose. What GameLogic owns is the *meaning* -- `typeId` is a `HullClass`,
 * `gaugeA` is hull, `gaugeB` is shield -- and the engine never reads it.
 *
 * Quantisation is the wire contract, and it is what both sides validate against
 * so a client pre-check and the authority cannot disagree on a rounding
 * boundary: position in centimetres, velocity in centimetres per second,
 * heading in 1/65,536 of a turn.
 */

namespace Game
{

/// `{tick, baselineTick, gridAnchor, shipCount, orderCount, lastOrderSeqProcessed}`.
struct SnapshotHeader
{
  std::uint32_t tick = 0;

  /// Zero means "full". Reserved for the delta path (ADR-004 §6); nothing reads
  /// it yet, and it is on the wire from the first snapshot so that adding
  /// deltas is a behaviour change rather than a schema change.
  std::uint32_t baselineTick = 0;

  /*
   * Which grid these ships are standing on (ADR-016 §6, U3b) -- **the smear
   * guard**.
   *
   * A session hosts many grids and a client views one at a time (ADR-016 §4),
   * so the moment a view can switch, "a snapshot" stops being self-evidently
   * about the same world as the last one. Without this field a client that
   * switched grids would interpolate the ship it *was* watching towards a ship
   * that merely shares its id on the grid it is watching now, and the hulls
   * would smear across the gap between two worlds. That is not a rendering
   * artefact to tune away: the two records describe different ships.
   *
   * It costs two bytes of the header and, as the static assert below records,
   * **no ships at all** -- 43 records still fit, because the cap had that much
   * slack. A field that changes no budget is the cheapest moment to add one.
   */
  AnchorId gridAnchor = INVALID_ID;

  std::uint16_t shipCount = 0;

  /// Order state, which drives ghost-to-underway promotion and per-leg ETAs.
  /// Always zero until S9 gives the client something to have ordered.
  std::uint16_t orderCount = 0;

  /*
   * Closes the order feedback loop even when an `OrderAck` is delayed or lost.
   *
   * **Per viewer as of U3d-a** (ADR-022 §7). The field did not move -- it is
   * still the game's header, still in this position, still the same width --
   * but what fills it did: it was world-global state folded into the world
   * hash, and it is now the session's own count of what *this* commander has
   * had accepted. The wire always read as per-viewer; now it is.
   */
  std::uint32_t lastOrderSeqProcessed = 0;
};

inline constexpr std::size_t SNAPSHOT_HEADER_BYTES = 18;

/*
 * How many ships fit in one datagram.
 *
 * The transport's cap is 1,152 bytes (ADR-003) and the framing costs a type
 * word, so this is what is left after the header, divided by the record. At MVP
 * scale -- 41 ships, about 840 bytes -- there is room to spare. At the corpus's
 * 1,024-entity cap a full snapshot is roughly 20 KB, which is precisely why
 * **delta encoding plus interest management is the designed growth path and a
 * bigger datagram is not** (ADR-004 §6).
 */
inline constexpr std::size_t SNAPSHOT_BUDGET_BYTES = Neuron::MAX_DATAGRAM_BYTES - sizeof(std::uint16_t);

/*
 * One order's state, for the ghost the client is drawing (ADR-004 §6).
 *
 * Twelve bytes. `serverOrderId` is what the client matched its ghost to when
 * the ack arrived, and `clientOrderSeq` is what it can match on if the ack was
 * lost -- carrying both is what makes the promotion path work over an
 * unreliable channel without a retransmit.
 *
 * `legIndex` and `legCount` are the ETA row's: "second of four" is what a
 * player reads, and it is not derivable from anything else in the snapshot.
 */
struct OrderStateRecord
{
  std::uint32_t serverOrderId = 0;
  std::uint32_t clientOrderSeq = 0;

  /*
   * Seconds until this group finishes the leg it is on, or `NO_ETA` (S12).
   *
   * The authority's own number, so the ETA under a ghost is a replicated fact
   * rather than the prediction the client's pre-check made before sending. It
   * is the slowest member's *remaining* journey from wherever it already is and
   * at whatever speed it already has -- a rest-to-rest estimate of a remaining
   * distance charges a ramp the ships already paid, and stalls in the last few
   * seconds, which is exactly when a player is watching it.
   *
   * Whole seconds in 16 bits. The sub-second part is below what a 20 Hz stream
   * can honestly claim, and the range covers five hours against a longest leg
   * of about six and a half minutes.
   */
  std::uint16_t etaSeconds = 0;

  std::uint8_t state = 0; // OrderState
  std::uint8_t legIndex = 0;
  std::uint8_t legCount = 0;
  std::uint8_t memberCount = 0;
};

/// `etaSeconds` when the game will not say -- a group that is Done, or one
/// whose members cannot move. Distinct from zero, which means *arriving now*.
inline constexpr std::uint16_t NO_ETA = 0xffffu;

/*
 * The bytes one record puts on the wire.
 *
 * **Not `sizeof`,** and the change is worth explaining rather than just making:
 * the record now has a `uint16_t` beside four `uint8_t`s, so the compiler
 * rounds the struct up to its own alignment and `sizeof` reports sixteen where
 * fourteen are written. `sizeof` was only ever standing in for the real
 * invariant; summing the fields asserts the thing the budget actually cares
 * about, and cannot drift when padding does.
 *
 * The comment it replaces was right: a field added here costs ships. This one
 * cost two -- the fleet cap fell from 47 to 45 (and to 43 when ADR-017 §5's
 * status byte widened the entity record), still above the 41 the MVP
 * fields and the static assert below still checks it.
 */
inline constexpr std::size_t ORDER_STATE_RECORD_BYTES = 14;
static_assert(sizeof(OrderStateRecord::serverOrderId) + sizeof(OrderStateRecord::clientOrderSeq) +
                      sizeof(OrderStateRecord::etaSeconds) + sizeof(OrderStateRecord::state) +
                      sizeof(OrderStateRecord::legIndex) + sizeof(OrderStateRecord::legCount) +
                      sizeof(OrderStateRecord::memberCount) ==
                  ORDER_STATE_RECORD_BYTES,
              "the record is written field by field; the sum of the fields is the budget, so a field added here costs ships");

/*
 * How many orders ride along with the ships.
 *
 * A fixed reservation rather than whatever is left, so the ship budget is a
 * constant and not a function of how busy the player has been. Sixteen groups
 * is four times the fleet an MVP session fields, and it costs 192 bytes of the
 * 1,150.
 *
 * Orders are the half that may be truncated. A missing ship record reads as a
 * despawn and is unrecoverable; a missing order record costs one frame of ETA,
 * and the header's `lastOrderSeqProcessed` still promotes the ghost.
 */
inline constexpr std::uint16_t MAX_ORDERS_PER_SNAPSHOT = 16;
inline constexpr std::size_t ORDER_AREA_BYTES = MAX_ORDERS_PER_SNAPSHOT * ORDER_STATE_RECORD_BYTES;

/*
 * What the game means by `EntityRecord::statusBits` (ADR-017 §5, ADR-014 §4).
 *
 * The engine carries the byte and reads none of it; the bits are defined here
 * because a bit's *meaning* is game semantics. Bit 0 is undock protection --
 * immunity to damage, drawn as a shimmer -- and the other seven are where
 * in-warp, combat-flagged and [ADR-022](../Design/ADR/ADR-022-interest-and-delta.md)'s
 * two relationship bits will live, which is why this is a byte and not a
 * widened `typeId`.
 */
inline constexpr std::uint8_t SHIP_STATUS_PROTECTED = 1u << 0;

inline constexpr std::uint16_t MAX_SHIPS_PER_SNAPSHOT = static_cast<std::uint16_t>(
    (SNAPSHOT_BUDGET_BYTES - SNAPSHOT_HEADER_BYTES - ORDER_AREA_BYTES) / Neuron::ENTITY_RECORD_BYTES);

/// Bytes a snapshot of this many ships and orders occupies, framing excluded.
[[nodiscard]] constexpr std::size_t SnapshotBytes(std::size_t _shipCount, std::size_t _orderCount = 0) noexcept
{
  return SNAPSHOT_HEADER_BYTES + _shipCount * Neuron::ENTITY_RECORD_BYTES + _orderCount * ORDER_STATE_RECORD_BYTES;
}

// The budget ADR-004 §6 states, asserted rather than believed. The order area
// is reserved whether or not any order is flying, which is what keeps the ship
// cap from moving under the client.
static_assert(SnapshotBytes(41, MAX_ORDERS_PER_SNAPSHOT) <= SNAPSHOT_BUDGET_BYTES, "the MVP fleet must fit one datagram");
// U3b's `gridAnchor` widened the header by two bytes and moved the cap by none:
// the arithmetic had that much slack. Asserted rather than remembered, because
// the next field to land here may not be free and this is where it finds out.
static_assert(MAX_SHIPS_PER_SNAPSHOT == 43, "the header grew without costing a ship; if this fires, the cap moved");
static_assert(MAX_SHIPS_PER_SNAPSHOT >= 41, "the MVP fleet must fit one datagram");
static_assert(SnapshotBytes(MAX_SHIPS_PER_SNAPSHOT, MAX_ORDERS_PER_SNAPSHOT) <= SNAPSHOT_BUDGET_BYTES,
              "the cap must be a cap");
static_assert(SnapshotBytes(MAX_SHIPS_PER_SNAPSHOT + 1u, MAX_ORDERS_PER_SNAPSHOT) > SNAPSHOT_BUDGET_BYTES,
              "the cap must be the largest that fits");

/// Quantises one ship into the neutral record the engine carries.
[[nodiscard]] Neuron::EntityRecord MakeShipRecord(const World& _world, std::uint32_t _slot) noexcept;

/*
 * Writes a full snapshot of the world.
 *
 * Returns false if the fleet does not fit one datagram, and writes nothing --
 * a truncated snapshot is worse than no snapshot, because the client would
 * treat the missing ships as despawned and then resurrect them next tick. The
 * caller's job is to say so loudly; the growth path is deltas, not silence.
 *
 * `_lastOrderSeqProcessed` is the **session's** number and travels in
 * (ADR-022 §7): the world stopped owning it with U3d-a, because a world has no
 * viewers and that field only ever meant something about one. It defaults to
 * zero for the callers that are asking about a world rather than serving a
 * commander -- the self test's determinism harness and most of this suite --
 * where "no orders have been acknowledged to anybody" is the honest answer
 * rather than a stub.
 */
[[nodiscard]] bool WriteSnapshot(const World& _world, Neuron::ByteWriter& _writer,
                                 std::uint32_t _lastOrderSeqProcessed = 0);

/// Reads one back. Returns false on a truncated or implausible payload, leaving
/// the outputs untouched -- a half-applied snapshot is a corrupt view.
[[nodiscard]] bool ReadSnapshot(Neuron::ByteReader& _reader, SnapshotHeader& _outHeader,
                                std::vector<Neuron::EntityRecord>& _outShips);

/// The same, with the order records. The two-argument form stays because most
/// callers -- and every test written before S9 -- want the fleet and nothing
/// else, and reading orders they will not look at is work with no reader.
[[nodiscard]] bool ReadSnapshot(Neuron::ByteReader& _reader, SnapshotHeader& _outHeader,
                                std::vector<Neuron::EntityRecord>& _outShips,
                                std::vector<OrderStateRecord>& _outOrders);

} // namespace Game
