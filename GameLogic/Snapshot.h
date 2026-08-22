#pragma once

#include "Relevance.h"
#include "World.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "EntityRecord.h"
// For the datagram cap. GameLogic does not touch the transport, but the tick
// tail has to leave room for records inside one datagram, so the budget below
// is derived from the one definition of that number rather than restating it
// (ADR-004 §6). The header is a pure interface -- no Windows, no sockets.
#include "Transport.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * The game's half of state replication (ADR-004 §6, ADR-022).
 *
 * **This file used to own the whole snapshot payload and no longer does.**
 * Until U3d-b a snapshot was `[game header][entity records][order records]`,
 * written here and opaque to the engine, one datagram, full every tick, refused
 * outright when the fleet outgrew the cap. ADR-022 replaced all of that: the
 * session host ranks (through `RankRelevance`), truncates to a byte budget,
 * delta-encodes against the view it last *sent*, and packs the result into as
 * many datagrams as it takes. Every one of those is a decision about a *viewer*
 * and a *link*, and the sim tier has neither (§1), so the engine owns the
 * envelope now.
 *
 * What is left here is the part that is genuinely the game's:
 *
 * - **the entity record's meaning** -- `typeId` is a `HullClass`, `gaugeA` is
 *   hull, `gaugeB` is shield, and two bits of `statusBits` are the viewer's
 *   relationship to the ship (§8b);
 * - **the tick tail** -- the order-state records that promote a client's ghost,
 *   and the session's order high-water mark beside them. Opaque to the engine,
 *   under the game's own schema hash, exactly as `Summary` already is.
 *
 * The ship record is still `Neuron::EntityRecord` and still not a type of ours,
 * for ADR-014 §4's reason: the engine has to buffer, interpolate, diff and draw
 * without knowing what a ship is. Declaring a `ShipRecord` here that happened to
 * match would be two layouts to keep in step, and one of them would eventually
 * lose.
 *
 * Quantisation is the wire contract, and it is what both sides validate against
 * so a client pre-check and the authority cannot disagree on a rounding
 * boundary: position in centimetres, velocity in centimetres per second,
 * heading in 1/65,536 of a turn.
 */

namespace Game
{

/*
 * One order's state, for the ghost the client is drawing (ADR-004 §6).
 *
 * Twelve bytes. `serverOrderId` is what the client matched its ghost to when
 * the ack arrived, and `clientOrderSeq` is what it can match on if the ack was
 * lost -- carrying both is what makes the promotion path work over an
 * unreliable channel without a retransmit.
 *
 * `legIndex` and `legCount` are the ETA row's: "second of four" is what a
 * player reads, and it is not derivable from anything else in the tail.
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
 * the record has a `uint16_t` beside four `uint8_t`s, so the compiler rounds the
 * struct up to its own alignment and `sizeof` reports sixteen where fourteen are
 * written. Summing the fields asserts the thing the budget actually cares about,
 * and cannot drift when padding does.
 */
inline constexpr std::size_t ORDER_STATE_RECORD_BYTES = 14;
static_assert(sizeof(OrderStateRecord::serverOrderId) + sizeof(OrderStateRecord::clientOrderSeq) +
                      sizeof(OrderStateRecord::etaSeconds) + sizeof(OrderStateRecord::state) +
                      sizeof(OrderStateRecord::legIndex) + sizeof(OrderStateRecord::legCount) +
                      sizeof(OrderStateRecord::memberCount) ==
                  ORDER_STATE_RECORD_BYTES,
              "the record is written field by field; the sum of the fields is the budget");

/*
 * How many orders ride in one tick's tail.
 *
 * A fixed reservation rather than whatever is left, so the record budget is a
 * constant and not a function of how busy the player has been. Sixteen groups is
 * four times the fleet an MVP session fields.
 *
 * Orders are the half that may be truncated. An entity record absent from a
 * delta means *unchanged* and one that has left is named explicitly (ADR-022
 * §5e), so neither reads as a despawn; a missing order record costs one frame of
 * ETA, and `lastOrderSeqProcessed` still promotes the ghost.
 */
inline constexpr std::uint16_t MAX_ORDERS_PER_SNAPSHOT = 16;
inline constexpr std::size_t ORDER_AREA_BYTES = MAX_ORDERS_PER_SNAPSHOT * ORDER_STATE_RECORD_BYTES;

/// `[u16 orderCount][u32 lastOrderSeqProcessed]`, then that many records.
inline constexpr std::size_t TICK_TAIL_HEADER_BYTES = 6;

/// The most one tick tail can occupy. The engine reserves this out of part
/// zero's datagram, so a tail can never be the reason a record does not fit.
inline constexpr std::size_t MAX_TICK_TAIL_BYTES = TICK_TAIL_HEADER_BYTES + ORDER_AREA_BYTES;

// The tail has to leave a datagram usefully full of records, or reserving it
// would starve the thing it rides with. Asserted rather than assumed, because
// this is the number a future order field would silently spend.
static_assert(MAX_TICK_TAIL_BYTES + Neuron::ENTITY_RECORD_BYTES * 16 < Neuron::MAX_DATAGRAM_BYTES,
              "the tick tail must leave room for a useful number of records in part zero");

/*
 * What the game means by `EntityRecord::statusBits` (ADR-017 §5, ADR-022 §8b).
 *
 * The engine carries the byte and reads none of it; the bits are defined here
 * because a bit's *meaning* is game semantics.
 *
 * Bit 0 is undock protection -- immunity to damage, drawn as a shimmer. Bits 1
 * and 2 are the viewer's **relationship** to the ship, which is ADR-022 §8b's
 * whole trick: OWN / ALLIED / NEUTRAL / HOSTILE is the icon sheet's colour
 * channel (`tactical-icon-system.png` §3) and it costs **zero extra bytes**,
 * where an owner id per record would have cost four on every entity every tick
 * to answer a question a player asks once a session.
 *
 * The value stored is `Relationship`, which is why that enum's numbering is
 * fixed at four values: it is a wire encoding, not a convenience.
 */
inline constexpr std::uint8_t SHIP_STATUS_PROTECTED = 1u << 0;
inline constexpr std::uint8_t SHIP_STATUS_RELATIONSHIP_SHIFT = 1;
inline constexpr std::uint8_t SHIP_STATUS_RELATIONSHIP_MASK = 0x3u << SHIP_STATUS_RELATIONSHIP_SHIFT;

/// Packs a relationship into a status byte, leaving every other bit alone.
[[nodiscard]] constexpr std::uint8_t WithRelationship(std::uint8_t _statusBits, Relationship _relationship) noexcept
{
  return static_cast<std::uint8_t>((_statusBits & ~SHIP_STATUS_RELATIONSHIP_MASK) |
                                   ((static_cast<std::uint8_t>(_relationship) << SHIP_STATUS_RELATIONSHIP_SHIFT) &
                                    SHIP_STATUS_RELATIONSHIP_MASK));
}

/// And back out. Every two-bit value is a legal `Relationship`, so this cannot
/// fail -- which is the point of spending exactly two bits on a four-valued
/// channel rather than a byte with a hole in it.
[[nodiscard]] constexpr Relationship RelationshipFrom(std::uint8_t _statusBits) noexcept
{
  return static_cast<Relationship>((_statusBits & SHIP_STATUS_RELATIONSHIP_MASK) >> SHIP_STATUS_RELATIONSHIP_SHIFT);
}

/*
 * Quantises one ship into the neutral record the engine carries.
 *
 * `_relationship` is the **viewer's**, which is why it is a parameter rather
 * than something the world could answer: the same hull is `Own` to one
 * commander and `Neutral` to the next, and a column on `World` could only hold
 * one of those answers (ADR-022 §8b).
 */
[[nodiscard]] Neuron::EntityRecord MakeShipRecord(const World& _world, std::uint32_t _slot,
                                                  Relationship _relationship = Relationship::Neutral) noexcept;

/*
 * Writes the game's tick tail for one viewer.
 *
 * `_lastOrderSeqProcessed` is the **session's** number and travels in
 * (ADR-022 §7): the world stopped owning it with U3d-a, because a world has no
 * viewers and that field only ever meant something about one.
 *
 * Returns false only when the writer could not hold `MAX_TICK_TAIL_BYTES`, which
 * the engine reserves in advance -- so in practice this does not fail, and a
 * false is a caller that did not reserve.
 */
[[nodiscard]] bool WriteTickTail(const World& _world, Neuron::ByteWriter& _writer, std::uint32_t _lastOrderSeqProcessed);

/// Reads one back. Returns false on a truncated or implausible payload, leaving
/// the outputs untouched -- a half-applied tail is a corrupt view.
[[nodiscard]] bool ReadTickTail(Neuron::ByteReader& _reader, std::uint32_t& _outLastOrderSeqProcessed,
                                std::vector<OrderStateRecord>& _outOrders);

} // namespace Game
