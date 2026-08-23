#pragma once

#include "Ids.h"

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/*
 * Where a commander's ships are, without looking at any of them
 * (ADR-016 §6, build order U3b).
 *
 * A **fleet is emergent** -- your ships sharing a location -- so a summary is
 * not a record of an entity, it is a count grouped by place. That is why there
 * is no fleet id here and nothing to keep in step: the roster screen's blocks,
 * the strategic map's markers and the "where is everything" question are all the
 * same query answered once.
 *
 * **A summary is not a snapshot, and the difference is the point.** A snapshot
 * is one grid at 20 Hz; this is every place at about 1 Hz, and it exists so the
 * client can show a fleet it is *not watching*. Without it the answer to "what
 * is my other fleet doing" is "subscribe to its grid", which is exactly the
 * clutter and the bandwidth ADR-016 §6 refused.
 *
 * **Keyed on `PlayerId`** when there is more than one commander (ADR-018 D5).
 * There is one, so the query is over everything; the shape does not change when
 * a second arrives, only the filter does.
 */

namespace Game
{

/*
 * What a group of ships is doing, at the resolution a summary has.
 *
 * Three, and they are places rather than activities: on a grid, docked at a
 * station, or crossing between anchors. What a fleet is *doing* on its grid is
 * the snapshot's business, and a summary that tried to say would be a second
 * source of truth about it.
 */
enum class FleetState : std::uint8_t
{
  OnGrid = 0,
  Docked = 1,
  InTransit = 2
};

/// What to call one. Never null, for the same reason `OrderReasonText` is not.
[[nodiscard]] const char* FleetStateName(FleetState _state) noexcept;

/// No estimate. The same sentinel `OrderStateRecord::etaSeconds` uses, because
/// a client that already knows what 65,535 means should not have to learn a
/// second convention.
inline constexpr std::uint16_t FLEET_ETA_NONE = 0xffffu;

/*
 * One row.
 *
 * `anchor` is where the ships *are* for the first two states and where they are
 * **going** for the third -- a fleet in transit is nowhere, and the useful
 * answer to "where is it" is its destination, not the nothing it is crossing.
 */
struct FleetSummary
{
  AnchorId anchor = INVALID_ID;
  FleetState state = FleetState::OnGrid;
  std::uint16_t shipCount = 0;

  /// Seconds until arrival, for `InTransit`, and `FLEET_ETA_NONE` otherwise.
  /// This is the number U3a owed and could not give: a fleet mid-crossing is in
  /// no world, so no grid's order records can carry its ETA.
  std::uint16_t etaSeconds = FLEET_ETA_NONE;

  /*
   * Which wing a **crossing** belongs to (U6b, 2026-08-23), and
   * `INVALID_WING_ID` on every other row.
   *
   * **Because a crossing is the one row whose `anchor` is not where the ships
   * are.** The other two states name a place the player can point at and the
   * count under it is the answer; a crossing names a destination, so a block
   * drawn from `anchor` alone reads *"VESTA-3, in warp"* -- the station wearing
   * the fleet's status, at exactly the moment the fleet's own roster row has
   * gone (`BuildRoster` draws wings with members on the watched grid, and a
   * wing mid-warp has none). The call sign is the only thing that identifies
   * it, and it was the one thing the summary could not say.
   *
   * **Crossings split on it and nothing else does.** Two wings warping to one
   * anchor become two rows, because one row would have to be labelled with one
   * of two names; a station's docked ships stay one row per state, because
   * *there* the place is the answer and per-wing rows would turn one hangar
   * into four entries in a list of places. So the field is on every row and the
   * grouping changes for one state -- which is the smallest change that makes
   * the label honest.
   *
   * `INVALID_WING_ID` is a real value here rather than an absence: ships with
   * no wing cross too, and they group together.
   */
  WingId wing = INVALID_WING_ID;
};

/// Bytes one message occupies. Fixed part plus eight per row -- seven until U6b
/// gave a crossing its wing.
[[nodiscard]] constexpr std::size_t FleetSummariesBytes(std::size_t _rowCount) noexcept
{
  return 2 + _rowCount * 8;
}

/// How many rows one message may carry. A commander with more places than this
/// has an interface problem before it has a bandwidth problem; the cap is here
/// so the decode bound does not depend on a number the payload chose.
inline constexpr std::uint16_t MAX_FLEET_SUMMARIES = 256;

[[nodiscard]] bool WriteFleetSummaries(std::span<const FleetSummary> _summaries, Neuron::ByteWriter& _writer) noexcept;

/// Not `noexcept`: it fills a caller-owned vector, so it allocates, and a
/// `noexcept` over an allocation is `std::terminate` on `bad_alloc` rather than
/// a promise.
[[nodiscard]] bool ReadFleetSummaries(Neuron::ByteReader& _reader, std::vector<FleetSummary>& _outSummaries);

} // namespace Game
