#pragma once

#include "Orders.h"

#include "ByteReader.h"
#include "ByteWriter.h"

/*
 * An order, as bytes (ADR-004 §7).
 *
 * `OrderSubmit{ u32 orderSeq, u8 kind, u8 formationId, u8 queueMode,
 * u16 shipCount, u16 shipIds[], Leg{ i32 xCm, i32 yCm, u16 facing } }`, in
 * that order, little-endian, no padding.
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
 */

namespace Game
{

/// Bytes one order occupies. Fixed part plus two per ship.
[[nodiscard]] constexpr std::size_t OrderSubmitBytes(std::size_t _shipCount) noexcept
{
  return 4 + 1 + 1 + 1 + 2 + _shipCount * 2 + 4 + 4 + 2;
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
 */
[[nodiscard]] bool ReadOrderSubmit(Neuron::ByteReader& _reader, OrderSubmit& _outOrder) noexcept;

} // namespace Game
