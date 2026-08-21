#pragma once

#include "Orders.h"

#include "ByteReader.h"
#include "ByteWriter.h"

/*
 * An order, as bytes (ADR-004 §7).
 *
 * `OrderSubmit{ u32 orderSeq, u8 kind, u8 formationId, u8 queueMode,
 * u16 shipCount, u16 shipIds[], Leg{ i32 xCm, i32 yCm, u16 facing },
 * u16 anchor, u8 oreFilter }`, in that order, little-endian, no padding.
 *
 * **The game owns this layout, not the engine.** NeuronCore frames the payload
 * behind a `WireType::OrderSubmit` word and copies the bytes without reading
 * them (ADR-004 ruling 4, ADR-014 §5) -- the same arrangement `Snapshot` has,
 * and for the same reason: an order is game semantics, and an engine that could
 * parse one would have learned what a move is.
 *
 * **The leg crosses as it was validated.** No conversion happens here, because
 * `OrderLeg` is already the wire's units -- which is the point of it being
 * quantised in the first place (ADR-005 §4). The client validates the same
 * integers it sends, and the server validates the same integers it received.
 *
 * **Two message kinds share this stream**, and the byte that tells them apart
 * is below. ADR-017 §8 put `StationCommand` on the *acked order stream* rather
 * than on a stream of its own -- one sequence counter, one `OrderAck`, one
 * reason enum, so "I told the authority to do something" has one feedback loop
 * and not two. That decision is what makes a discriminator necessary: both
 * messages open with `u32 orderSeq` and then a byte (`kind` for one, `verb` for
 * the other) whose value spaces overlap, so nothing in either payload could
 * tell a reader which it was holding.
 */

namespace Game
{

/*
 * Which message the acked stream is carrying.
 *
 * The same shape `SummaryKind` gives the server-to-client family: **one engine
 * wire type, a game-owned byte inside it.** `WireType::OrderSubmit` frames both
 * and NeuronCore reads neither (ADR-004 ruling 4, ADR-014 §5); spending a
 * second enumerator of the engine's framing enum on a station command would
 * teach the engine that stations exist.
 *
 * It lives here rather than in a file of its own because this *is* the stream's
 * wire file -- ADR-017 §8 named it "the acked order stream", and a family's
 * discriminator belongs with the family's envelope.
 *
 * Appended to, never renumbered: the values are on the wire and this enum is in
 * `GAME_SCHEMA_TEXT`, so a build that reordered it refuses one that did not.
 */
enum class CommandKind : std::uint8_t
{
  Order = 0,  // `OrderSubmit`
  Station = 1 // `StationCommand`
};

/// The discriminator's own cost: one byte in front of either message.
inline constexpr std::size_t COMMAND_KIND_BYTES = 1;

/// Opens a payload. The caller writes the message immediately after.
[[nodiscard]] bool WriteCommandKind(CommandKind _kind, Neuron::ByteWriter& _writer) noexcept;

/*
 * Reads it back, or refuses.
 *
 * Refuses a kind this build does not define, for `ReadSummaryRecord`'s reason:
 * an unrecognised kind is a schema disagreement the handshake should already
 * have caught, and a decoder that guessed would put a protocol error where the
 * player is owed a bounce.
 */
[[nodiscard]] bool ReadCommandKind(Neuron::ByteReader& _reader, CommandKind& _outKind) noexcept;

/// Bytes one order occupies. Fixed part plus two per ship. The trailing three
/// are the anchor a Dock names (ADR-017 §2) and the ore a Mine filters by
/// (ADR-024 §4a) -- written for every kind rather than only for the ones that
/// use them, because a variable-shape record would make the decode bound depend
/// on a byte the payload chose.
[[nodiscard]] constexpr std::size_t OrderSubmitBytes(std::size_t _shipCount) noexcept
{
  return 4 + 1 + 1 + 1 + 2 + _shipCount * 2 + 4 + 4 + 2 + 2 + 1;
}

/// The largest an order can be: the per-order ship cap, which is what makes the
/// decode bound checkable without trusting the count in the payload.
inline constexpr std::size_t MAX_ORDER_SUBMIT_BYTES = OrderSubmitBytes(MAX_SHIPS_PER_ORDER);

/// Writes the order. Returns false and writes nothing useful if it does not
/// fit; the caller must treat that as "not sent" rather than "sent short".
[[nodiscard]] bool WriteOrderSubmit(const OrderSubmit& _order, Neuron::ByteWriter& _writer) noexcept;

/*
 * Reads one back, or refuses.
 *
 * This runs on bytes a client sent, so it is the boundary where a malformed or
 * hostile payload has to stop. A ship count past the cap is refused before a
 * single id is read, rather than after the read has already walked off the end
 * of the buffer -- `ByteReader` would flag the underflow, but by then the loop
 * has run sixty-five thousand times.
 *
 * **The ore filter is the one field this refuses on**, where `kind` and
 * `formation` are cast through unchecked. The difference is that those two have
 * reason codes -- `UnknownKind` and `InvalidFormation` are sentences a player
 * reads -- and the filter has none, because every value the enum defines is
 * legal on every grid. So a byte outside it is not a refusal owed to anyone; it
 * is a schema disagreement the handshake should already have caught, which is
 * exactly what `ReadCommandKind` says about its own.
 */
[[nodiscard]] bool ReadOrderSubmit(Neuron::ByteReader& _reader, OrderSubmit& _outOrder) noexcept;

} // namespace Game
