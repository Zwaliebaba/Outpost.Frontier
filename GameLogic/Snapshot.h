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

/// `{tick, baselineTick, shipCount, orderCount, lastOrderSeqProcessed}`.
struct SnapshotHeader
{
  std::uint32_t tick = 0;

  /// Zero means "full". Reserved for the delta path (ADR-004 §6); nothing reads
  /// it yet, and it is on the wire from the first snapshot so that adding
  /// deltas is a behaviour change rather than a schema change.
  std::uint32_t baselineTick = 0;

  std::uint16_t shipCount = 0;

  /// Order state, which drives ghost-to-underway promotion and per-leg ETAs.
  /// Always zero until S9 gives the client something to have ordered.
  std::uint16_t orderCount = 0;

  /// Closes the order feedback loop even when an `OrderAck` is delayed or lost.
  /// Zero until S9.
  std::uint32_t lastOrderSeqProcessed = 0;
};

inline constexpr std::size_t SNAPSHOT_HEADER_BYTES = 16;

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
inline constexpr std::uint16_t MAX_SHIPS_PER_SNAPSHOT =
    static_cast<std::uint16_t>((SNAPSHOT_BUDGET_BYTES - SNAPSHOT_HEADER_BYTES) / Neuron::ENTITY_RECORD_BYTES);

/// Bytes a snapshot of this many ships occupies, framing excluded.
[[nodiscard]] constexpr std::size_t SnapshotBytes(std::size_t _shipCount) noexcept
{
  return SNAPSHOT_HEADER_BYTES + _shipCount * Neuron::ENTITY_RECORD_BYTES;
}

// The budget ADR-004 §6 states, asserted rather than believed.
static_assert(SnapshotBytes(41) <= SNAPSHOT_BUDGET_BYTES, "the MVP fleet must fit one datagram");
static_assert(MAX_SHIPS_PER_SNAPSHOT >= 41, "the MVP fleet must fit one datagram");
static_assert(SnapshotBytes(MAX_SHIPS_PER_SNAPSHOT) <= SNAPSHOT_BUDGET_BYTES, "the cap must be a cap");
static_assert(SnapshotBytes(MAX_SHIPS_PER_SNAPSHOT + 1u) > SNAPSHOT_BUDGET_BYTES, "the cap must be the largest that fits");

/// Quantises one ship into the neutral record the engine carries.
[[nodiscard]] Neuron::EntityRecord MakeShipRecord(const World& _world, std::uint32_t _slot) noexcept;

/*
 * Writes a full snapshot of the world.
 *
 * Returns false if the fleet does not fit one datagram, and writes nothing --
 * a truncated snapshot is worse than no snapshot, because the client would
 * treat the missing ships as despawned and then resurrect them next tick. The
 * caller's job is to say so loudly; the growth path is deltas, not silence.
 */
[[nodiscard]] bool WriteSnapshot(const World& _world, Neuron::ByteWriter& _writer);

/// Reads one back. Returns false on a truncated or implausible payload, leaving
/// the outputs untouched -- a half-applied snapshot is a corrupt view.
[[nodiscard]] bool ReadSnapshot(Neuron::ByteReader& _reader, SnapshotHeader& _outHeader,
                                std::vector<Neuron::EntityRecord>& _outShips);

} // namespace Game
