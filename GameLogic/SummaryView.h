#pragma once

#include "EconomyMessages.h"
#include "FleetSummary.h"
#include "Ids.h"
#include "Station.h"
#include "SummaryMessages.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * The summary family's receiving end (ADR-016 §6, build order E5a).
 *
 * The sender has grown to six kinds -- rosters and fleet summaries since T1 and
 * U3b, and the economy's four since E3 and E4b -- and every one of them is
 * written per viewer into one frame by `WriteSummaries`. This is the other half:
 * one place that reads a whole frame and holds what it said, so that a screen
 * asks a store rather than a decoder.
 *
 * **Here rather than in the composition root, which is where it started.** Two
 * kinds' worth of decode was a `switch` in `ReplicatedWorldView::ApplySummary`
 * and that was defensible while it was wiring. Six is not: it is bounds
 * checking, staging, cross-record merging and a refusal policy, which is logic,
 * and ADR-014 §6 says the composition root does not hold logic. Moving it also
 * buys the thing that mattered more -- **it becomes testable without a device**,
 * beside the codecs it consumes, on the same harness that already proves them.
 *
 * ## The defect this file exists to close
 *
 * Before it, the composition root's `switch` had cases for two kinds and no
 * `default`, so a `SiteStatus`, `CargoStatus`, `BayStatus` or `RefineryStatus`
 * record **fell through without its body being read**. The reader was then
 * sitting in the middle of a body, and the next `ReadSummaryRecord` took a
 * length or an anchor byte for a kind. That is worse than dropping the record:
 * it either refuses a frame that was perfectly well formed, or -- when the
 * stray byte happens to be 0 or 1 -- decodes a roster out of the middle of a
 * field's cluster percentages and shows it to the player.
 *
 * It was reachable the moment any commander's fleet stood on a field or held
 * any ore, which is to say from the first minute of the economy's own loop, and
 * nothing failed loudly when it happened. The structural answer is that this
 * file handles **every** kind and refuses anything else, and that the refusal
 * is a `default` rather than an omission -- a seventh kind added to the enum
 * tomorrow and not to this file stops a frame instead of corrupting one.
 *
 * ## A frame replaces; it does not merge
 *
 * Each frame is a complete statement about one viewer at one moment, so
 * applying one clears what the last one said rather than updating it in place.
 * That is not an optimisation, it is the only correct reading: the sender
 * **omits what is empty** -- a Bay with nothing in it is not written at all --
 * so a merging store would show a Bay the player emptied ten minutes ago, and
 * would keep showing it for the rest of the session.
 *
 * The cost is the one ADR-016 §6 already accepted for rosters: a record the
 * sender's byte budget dropped reads as absence for a frame. That is the honest
 * failure, and it is why `FleetSummary::shipCount` and not the roster's length
 * is what a hangar count is drawn from.
 *
 * ## Staged, then committed
 *
 * Records are read into scratch and published together at the end, so the
 * answer does not depend on the order the writer happened to emit them in --
 * and a frame that fails halfway leaves the previous one standing rather than a
 * half-updated mixture of two. Two builds disagreeing about record order would
 * otherwise disagree about a panel rather than fail a handshake, which is the
 * worst kind of version skew.
 */

namespace Game
{

/*
 * One station's hangar as this viewer sees it.
 *
 * The count and the rows come from **different records**, and that is worth
 * saying where it will be read: `shipCount` is the fleet summary's, which is
 * always written, while `docked` is the roster's, which is dropped first when
 * the frame runs out of room. So a station can legitimately report six ships
 * and list none, and a screen that derived the count from `docked.size()`
 * would tell the player their ships had gone.
 */
struct DockedStationView
{
  AnchorId anchor = INVALID_ID;
  std::uint16_t shipCount = 0;
  std::vector<RosterEntry> docked;
};

/// One commander's Bay at one station: what they have committed there.
/// Ore and alloys together because the wire carries them together, for the
/// reason `BAY_STATUS_BYTES` gives -- a Bay is one statement about one place.
struct BayView
{
  AnchorId station = INVALID_ID;
  std::uint32_t oreUnits[ORE_COUNT] = {};
  std::uint32_t alloyUnits[ALLOY_COUNT] = {};

  [[nodiscard]] bool Empty() const noexcept;
};

class SummaryView
{
public:
  /*
   * Reads one frame and, if all of it was understood, replaces what this holds.
   *
   * False leaves the previous frame standing untouched. The caller uses it for
   * a diagnostic and nothing else: a refused summary is a stale panel, not a
   * broken world, and there is no recovery to attempt -- a malformed frame
   * means the two builds disagree about a schema the handshake was supposed to
   * have refused at the door.
   */
  [[nodiscard]] bool Apply(std::span<const std::uint8_t> _payload);

  /// True once a frame has been understood. **The first one is not news**: a
  /// client joining a session in progress is being told a state, and a caller
  /// that treated the difference from nothing as a set of events would announce
  /// everything that happened before the player was watching.
  [[nodiscard]] bool HaveSummary() const noexcept { return m_haveSummary; }

  /// How many frames were refused. A counter rather than a log line because it
  /// is a rate that matters, and a per-frame log at 1 Hz per viewer is a way to
  /// fill a disk with one number.
  [[nodiscard]] std::uint32_t RejectedFrames() const noexcept { return m_rejectedFrames; }

  /// Docked stations, ordered by anchor. **Sorted rather than left in arrival
  /// order** so a panel's rows are the universe's order: a list that reshuffles
  /// between frames is a list the player cannot point at.
  [[nodiscard]] std::span<const DockedStationView> DockedStations() const noexcept { return m_docked; }

  /// Every fleet the viewer owns, in whatever order the sender listed them --
  /// which is the registry's, and is stable for the same reason its hash is.
  [[nodiscard]] std::span<const FleetSummary> Fleets() const noexcept { return m_fleets; }

  /// Fields the viewer has a fleet standing at. Public information: how eaten a
  /// field is, is what anybody standing at it can see.
  [[nodiscard]] std::span<const SiteStatusRow> Sites() const noexcept { return m_sites; }

  /// The viewer's ships that are carrying something, by ship. Ships with empty
  /// holds are absent rather than listed as zeroes, so a caller asking about
  /// one gets null and draws READY.
  [[nodiscard]] std::span<const CargoStatusRow> Cargo() const noexcept { return m_cargo; }

  [[nodiscard]] std::span<const BayView> Bays() const noexcept { return m_bays; }
  [[nodiscard]] std::span<const RefineryStatusRow> Refineries() const noexcept { return m_refineries; }

  /*
   * The one-place lookups a screen actually makes.
   *
   * Null for "nothing was said about that", which is a real answer and the
   * common one -- most stations have no Bay of yours and most ships are empty.
   * A returned pointer is valid until the next `Apply`, which is the same
   * lifetime every other borrowed row in this codebase has.
   */
  [[nodiscard]] const DockedStationView* DockedAt(AnchorId _anchor) const noexcept;
  [[nodiscard]] const SiteStatusRow* SiteAt(AnchorId _anchor) const noexcept;
  [[nodiscard]] const BayView* BayAt(AnchorId _anchor) const noexcept;
  [[nodiscard]] const RefineryStatusRow* RefineryAt(AnchorId _station) const noexcept;
  [[nodiscard]] const CargoStatusRow* CargoOf(ShipId _ship) const noexcept;

  /// How many of the viewer's ships are docked at this anchor, or zero. Read
  /// off the fleet summary rather than the roster, for `DockedStationView`'s
  /// stated reason.
  [[nodiscard]] std::uint16_t DockedCountAt(AnchorId _anchor) const noexcept;

private:
  std::vector<DockedStationView> m_docked;
  std::vector<FleetSummary> m_fleets;
  std::vector<SiteStatusRow> m_sites;
  std::vector<CargoStatusRow> m_cargo;
  std::vector<BayView> m_bays;
  std::vector<RefineryStatusRow> m_refineries;

  bool m_haveSummary = false;
  std::uint32_t m_rejectedFrames = 0;
};

} // namespace Game
