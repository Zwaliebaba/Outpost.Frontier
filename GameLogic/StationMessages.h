#pragma once

#include "Station.h"

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/*
 * The station family on the wire (ADR-017 §8, ADR-018 A11).
 *
 * `StationCommand` rides the **reliable acked order stream**, sharing its
 * sequence counter, its `OrderAck` and its reason enum -- one feedback loop for
 * "I told the authority to do something", not two. `StationRoster` is the
 * server's answer to "what is docked here", sent per viewer at about 1 Hz: it
 * is the first resident of ADR-016 §6's summary family, and U3b builds the rest
 * beside it rather than instead of it.
 *
 * **Ship ids are u32 here** (ADR-018 A11/D6). The sim's `ShipId` is still u16
 * and stays that way until the delta cluster widens `EntityRecord`, but these
 * are *durable-ish* artifacts -- a roster is what the reconnect screen reads
 * and what a save file would hold -- and a field that has to widen later is a
 * schema break later. Four bytes now costs 128 bytes on a full 64-ship command
 * and buys never having to do it.
 *
 * **One function here is not `noexcept`, and it is the one that allocates.**
 * `ReadStationRoster` fills a caller-owned `std::vector`, so it can throw
 * `bad_alloc`; declaring it `noexcept` would turn that into `std::terminate`
 * on the path that reads the wire. The CI clang-tidy step found it -- the
 * second time it has caught a `noexcept` an allocation could not keep, and the
 * second time only the Windows toolchain saw it.
 *
 * Decoding is where a hostile payload stops, and the rule is the order family's:
 * a count past the cap is refused **before** the loop, and a value the game
 * merely does not recognise is passed through for *validation* to refuse with a
 * reason -- a decoder that answered "malformed" would put a protocol error
 * where the player is owed a bounce.
 */

namespace Game
{

/// Bytes one command occupies. Fixed part plus four per ship. The fixed part
/// grew by five with E3's transfer verbs: an ore byte and a unit count.
[[nodiscard]] constexpr std::size_t StationCommandBytes(std::size_t _shipCount) noexcept
{
  return 4 + 1 + 2 + 1 + 1 + 1 + 4 + 2 + _shipCount * 4;
}

/// The largest a command can be: the per-order ship cap, which is what makes
/// the decode bound checkable without trusting the count in the payload.
inline constexpr std::size_t MAX_STATION_COMMAND_BYTES = StationCommandBytes(MAX_SHIPS_PER_ORDER);

/*
 * Bytes one roster occupies. Fixed part plus eighteen per docked ship: six for
 * the identity and twelve for the hold (E3).
 *
 * Tripling the row is the cost of ADR-024 §5's decision that a docked ship
 * keeps its cargo, and it is worth stating what it buys and what it spends. A
 * full 64-ship roster goes from 388 bytes to 1,156 -- comfortably inside a
 * datagram either way, because a roster is a ~1 Hz summary rather than a
 * per-tick record, which is precisely why the manifest rides here and not on
 * `EntityRecord` (ADR-024 §4d).
 */
[[nodiscard]] constexpr std::size_t StationRosterBytes(std::size_t _shipCount) noexcept
{
  return 2 + 2 + _shipCount * (6 + ORE_COUNT * 4);
}

/// Writes the command. False and nothing useful written if it does not fit; the
/// caller must treat that as "not sent" rather than "sent short".
[[nodiscard]] bool WriteStationCommand(const StationCommand& _command, Neuron::ByteWriter& _writer) noexcept;

/// Reads one back, or refuses. Runs on bytes a client sent.
[[nodiscard]] bool ReadStationCommand(Neuron::ByteReader& _reader, StationCommand& _outCommand) noexcept;

/*
 * Writes one station's roster.
 *
 * The roster is written **per viewer** rather than broadcast, and that is a
 * wire fact rather than a policy: ADR-017 §1 says other commanders cannot see
 * what is docked at a station, and on a broadcast-shaped sender that promise is
 * a silent leak nothing catches -- because nothing before U3c runs two clients.
 */
[[nodiscard]] bool WriteStationRoster(AnchorId _station, std::span<const RosterEntry> _docked,
                                      Neuron::ByteWriter& _writer) noexcept;

/*
 * Reads one back. **Not `noexcept`, unlike every other function here**, and the
 * difference is the `std::vector` it fills: it allocates, so it can throw, and
 * a `noexcept` over an allocation is `std::terminate` on `bad_alloc` rather
 * than a promise.
 *
 * The vector is the caller's because a roster is unbounded by design (ADR-017
 * has no hangar capacity) even though one *message* is capped, so there is no
 * fixed array to read into the way the command has one.
 */
[[nodiscard]] bool ReadStationRoster(Neuron::ByteReader& _reader, AnchorId& _outStation,
                                     std::vector<RosterEntry>& _outDocked);

} // namespace Game
