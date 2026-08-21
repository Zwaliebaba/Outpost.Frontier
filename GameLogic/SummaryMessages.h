#pragma once

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstddef>
#include <cstdint>

/*
 * The envelope ADR-016 §6's summary family travels in.
 *
 * The family already has two residents in the tree -- `StationRoster` (ADR-017,
 * T1/T2) and `FleetSummaries` (ADR-016 §6, U3b's sim half) -- and they share
 * everything that matters about how they are sent: **per viewer**, at about
 * 1 Hz, describing places rather than entities, and answering questions a
 * snapshot deliberately does not. What they lacked was a way onto the wire.
 *
 * **One engine wire type carries all of them** (`Neuron::WireType::Summary`),
 * and this is the byte that says which is which. The alternative -- an
 * enumerator per message kind -- would spend a slot of the engine's framing
 * enum on every game concept that ever wants a 1 Hz feed, which is precisely
 * the coupling ADR-014 §5 keeps out of NeuronCore: the engine frames and
 * carries, and what is inside is the game's schema under the game's own hash.
 *
 * **A frame is a count and then that many records**, and a record is a kind
 * byte and a body. There is no length prefix, because every body in the family
 * is already self-delimiting -- each begins with its own row count and its rows
 * are fixed width -- so a reader that understands a kind knows where it ends.
 * The cost of that choice is that a reader cannot *skip* a kind it does not
 * know, and the answer is that it should not try: an unrecognised kind means
 * the two builds disagree about the schema, which `GAME_SCHEMA_TEXT` already
 * refuses at the door. A decoder inventing a recovery for a state the handshake
 * has ruled out is a decoder that hides a version mismatch.
 *
 * Everything here is bounded before it loops: the count is checked against
 * `MAX_SUMMARY_RECORDS` before a single record is read, so a hostile payload
 * cannot ask for work by naming a large number.
 */

namespace Game
{

/*
 * Which member of the family a record carries.
 *
 * Appended to, never renumbered: the values are on the wire, and this enum is
 * written into `GAME_SCHEMA_TEXT` so a build that reorders it refuses a build
 * that did not.
 */
enum class SummaryKind : std::uint8_t
{
  StationRoster = 0,
  FleetSummaries = 1,

  /*
   * The economy's three (ADR-024 §8, E3).
   *
   * `SiteStatus` is public -- how eaten a field is, is what anybody standing at
   * it can see. The other two are **owner-only**, and that is enforced by what
   * the sender is handed rather than by a filter it is trusted to apply: both
   * are written from state keyed by the viewer, so there is no code path on
   * which one commander's holds could be written into another's frame.
   */
  SiteStatus = 2,
  CargoStatus = 3,
  BayStatus = 4,

  /*
   * One commander's refinery at one station (ADR-024 §6, E4b): the tier, their
   * jobs in queue order, and the station's open project.
   *
   * On `BayStatus`'s cadence and framing, and per viewer for the same reason:
   * slots are the player's, so a refinery status addressed to anyone else would
   * be answering a question nobody asked.
   */
  RefineryStatus = 5
};

/*
 * How many records one frame may carry.
 *
 * A commander with more than this many summary records in one tick has an
 * interface problem before a bandwidth one -- the same argument
 * `MAX_FLEET_SUMMARIES` makes. It is here so the decode bound does not depend
 * on a number the payload chose.
 */
inline constexpr std::uint8_t MAX_SUMMARY_RECORDS = 32;

/// The frame's own overhead, before any record: the count byte.
inline constexpr std::size_t SUMMARY_FRAME_HEADER_BYTES = 1;

/// One record's overhead, before its body: the kind byte.
inline constexpr std::size_t SUMMARY_RECORD_HEADER_BYTES = 1;

/*
 * Opens a frame for `_recordCount` records.
 *
 * Two-part rather than one call taking everything, because the bodies are
 * written by the existing family writers (`WriteStationRoster`,
 * `WriteFleetSummaries`) and this must not learn what any of them contain --
 * the whole point of the envelope is that it does not.
 *
 * False if the count is past the cap or the writer has no room, and nothing is
 * written: a frame that promised records it could not fit is a frame a reader
 * would report as truncated.
 */
[[nodiscard]] bool BeginSummaryFrame(std::uint8_t _recordCount, Neuron::ByteWriter& _writer) noexcept;

/// Opens one record. The caller writes the body immediately after.
[[nodiscard]] bool BeginSummaryRecord(SummaryKind _kind, Neuron::ByteWriter& _writer) noexcept;

/*
 * Reads the frame's record count, or refuses.
 *
 * Refuses a count past `MAX_SUMMARY_RECORDS` rather than clamping it, because
 * a clamp would silently drop a station's roster and leave a hangar screen
 * short of the ships it is meant to undock.
 */
[[nodiscard]] bool ReadSummaryFrame(Neuron::ByteReader& _reader, std::uint8_t& _outRecordCount) noexcept;

/*
 * Reads one record's kind.
 *
 * Refuses a kind this build does not define -- see the header comment: that is
 * a schema disagreement the handshake should already have caught, and inventing
 * a skip here would hide it.
 */
[[nodiscard]] bool ReadSummaryRecord(Neuron::ByteReader& _reader, SummaryKind& _outKind) noexcept;

} // namespace Game
