#include "pch.h"

#include "CppUnitTest.h"

#include "EconomyMessages.h"
#include "FleetSummary.h"
#include "Station.h"
#include "StationMessages.h"
#include "SummaryMessages.h"
#include "SummaryView.h"

#include "ByteWriter.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * The summary family's receiving end (build order E5a).
 *
 * The codecs were proved one body at a time when each landed -- `CargoTests`
 * and `RefiningTests` both round-trip their own. What was never proved is the
 * thing above them: that a **whole frame** of mixed kinds decodes, that the
 * reader is where the next record starts when one finishes, and that a frame
 * which fails halfway does not leave a screen holding half of it.
 *
 * That gap was not theoretical. Until this file the client's decoder handled
 * two kinds of six and had no `default`, so four kinds fell through with their
 * bodies unread and the next kind byte came out of the middle of somebody's
 * cargo -- and nothing anywhere failed when it happened, because no test ever
 * built a frame with more than one kind in it. `AFullFrameOfEverySixKindsReads`
 * and `ASiteRecordDoesNotSwallowWhatFollowsIt` are that test, written after the
 * fact and kept as the regression.
 */

namespace GameLogicTests
{

namespace
{

/// Builds one frame the way `WriteSummaries` does: the header, then the records
/// in the order the sender emits them. A helper rather than six copies, so a
/// test reads as the *shape* of a frame instead of as byte plumbing.
class FrameBuilder
{
public:
  explicit FrameBuilder(std::uint8_t _records) : m_writer{m_buffer}
  {
    Assert::IsTrue(BeginSummaryFrame(_records, m_writer), L"the frame header did not write");
  }

  void Fleets(const std::vector<FleetSummary>& _rows)
  {
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::FleetSummaries, m_writer), L"record header");
    Assert::IsTrue(WriteFleetSummaries(_rows, m_writer), L"fleet summaries did not write");
  }

  void Roster(AnchorId _station, const std::vector<RosterEntry>& _rows)
  {
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::StationRoster, m_writer), L"record header");
    Assert::IsTrue(WriteStationRoster(_station, _rows, m_writer), L"roster did not write");
  }

  void Site(const SiteStatusRow& _row)
  {
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::SiteStatus, m_writer), L"record header");
    Assert::IsTrue(WriteSiteStatus(_row, m_writer), L"site status did not write");
  }

  void Cargo(const std::vector<CargoStatusRow>& _rows)
  {
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::CargoStatus, m_writer), L"record header");
    Assert::IsTrue(WriteCargoStatus(_rows, m_writer), L"cargo status did not write");
  }

  void Bay(AnchorId _station, const std::uint32_t (&_ore)[ORE_COUNT], const std::uint32_t (&_alloys)[ALLOY_COUNT])
  {
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::BayStatus, m_writer), L"record header");
    Assert::IsTrue(WriteBayStatus(_station, _ore, _alloys, m_writer), L"bay status did not write");
  }

  void Refinery(const RefineryStatusRow& _row)
  {
    Assert::IsTrue(BeginSummaryRecord(SummaryKind::RefineryStatus, m_writer), L"record header");
    Assert::IsTrue(WriteRefineryStatus(_row, m_writer), L"refinery status did not write");
  }

  /// A kind byte this build does not define, for the schema-skew case. Written
  /// raw because no writer will produce one.
  void RawKind(std::uint8_t _raw) { m_writer.WriteUInt8(_raw); }

  [[nodiscard]] std::span<const std::uint8_t> Bytes() const { return m_writer.Written(); }

  /// The frame with its last `_count` bytes cut off, for the truncation cases.
  [[nodiscard]] std::span<const std::uint8_t> Truncated(std::size_t _count) const
  {
    const std::span<const std::uint8_t> whole = m_writer.Written();
    return whole.subspan(0, whole.size() - _count);
  }

private:
  std::vector<std::uint8_t> m_buffer = std::vector<std::uint8_t>(4096);
  Neuron::ByteWriter m_writer;
};

[[nodiscard]] FleetSummary Fleet(AnchorId _anchor, FleetState _state, std::uint16_t _ships)
{
  FleetSummary row;
  row.anchor = _anchor;
  row.state = _state;
  row.shipCount = _ships;
  return row;
}

[[nodiscard]] RosterEntry Docked(ShipId _ship, std::uint32_t _ore0)
{
  RosterEntry entry;
  entry.shipId = _ship;
  entry.oreUnits[0] = _ore0;
  return entry;
}

} // namespace

TEST_CLASS(SummaryViewTests)
{
public:
  TEST_METHOD(AFullFrameOfEverySixKindsReads)
  {
    /*
     * The regression case, and the reason this file exists.
     *
     * Six kinds in the order `WriteSummaries` emits them. On the decoder this
     * replaced, everything from the `SiteStatus` onward was lost -- its body
     * was never consumed, so the next kind byte was read out of the middle of
     * it. Every assertion below the site row is therefore load-bearing rather
     * than thorough.
     */
    FrameBuilder frame{6};
    frame.Fleets({Fleet(4, FleetState::Docked, 3), Fleet(9, FleetState::OnGrid, 2)});
    frame.Roster(4, {Docked(11, 7), Docked(12, 0)});

    SiteStatusRow site;
    site.anchor = 9;
    site.epoch = 2;
    site.remainingUnits[1] = 4321;
    site.clusterCount = 3;
    site.clusterFullPct[0] = 100;
    site.clusterFullPct[1] = 40;
    site.clusterFullPct[2] = 0;
    frame.Site(site);

    CargoStatusRow hold;
    hold.shipId = 21;
    hold.oreUnits[2] = 64;
    frame.Cargo({hold});

    const std::uint32_t ore[ORE_COUNT] = {5, 0, 900};
    const std::uint32_t alloys[ALLOY_COUNT] = {1, 2, 3, 4, 5};
    frame.Bay(4, ore, alloys);

    RefineryStatusRow refinery;
    refinery.station = 4;
    refinery.tier = 2;
    refinery.projectToTier = 3;
    refinery.projectContributedUnits[0] = 12;
    refinery.jobs.push_back(RefineryJobRow{7, AlloyId::AstraGlass, 10, 5000});
    frame.Refinery(refinery);

    SummaryView view;
    Assert::IsFalse(view.HaveSummary(), L"a view reported a summary before it had one");
    Assert::IsTrue(view.Apply(frame.Bytes()), L"a well-formed six-kind frame was refused");
    Assert::IsTrue(view.HaveSummary());
    Assert::AreEqual<std::uint32_t>(0, view.RejectedFrames(), L"a frame that read was counted as refused");

    Assert::AreEqual<std::size_t>(2, view.Fleets().size(), L"fleets");
    Assert::AreEqual<std::size_t>(1, view.DockedStations().size(), L"only the docked fleet becomes a block");

    const SiteStatusRow* const backSite = view.SiteAt(9);
    Assert::IsNotNull(backSite, L"the site record was lost");
    Assert::AreEqual<std::uint32_t>(4321, backSite->remainingUnits[1], L"site units");
    Assert::AreEqual<std::uint8_t>(40, backSite->clusterFullPct[1], L"cluster fraction");

    const CargoStatusRow* const backHold = view.CargoOf(21);
    Assert::IsNotNull(backHold, L"the cargo record was lost");
    Assert::AreEqual<std::uint32_t>(64, backHold->oreUnits[2], L"hold units");

    const BayView* const backBay = view.BayAt(4);
    Assert::IsNotNull(backBay, L"the bay record was lost");
    Assert::AreEqual<std::uint32_t>(900, backBay->oreUnits[2], L"bay ore");
    Assert::AreEqual<std::uint32_t>(5, backBay->alloyUnits[4], L"bay alloys");

    const RefineryStatusRow* const backRefinery = view.RefineryAt(4);
    Assert::IsNotNull(backRefinery, L"the refinery record was lost");
    Assert::AreEqual<std::uint8_t>(2, backRefinery->tier, L"tier");
    Assert::AreEqual<std::uint8_t>(3, backRefinery->projectToTier, L"project");
    Assert::AreEqual<std::size_t>(1, backRefinery->jobs.size(), L"jobs");
    Assert::AreEqual<std::uint32_t>(5000, backRefinery->jobs[0].completeTick, L"job completion");
  }

  TEST_METHOD(ASiteRecordDoesNotSwallowWhatFollowsIt)
  {
    /*
     * The defect stated as narrowly as it can be: two records, and the second
     * only arrives if the first left the reader where the first ended.
     *
     * A site body is variable width -- a byte per cluster -- so a decoder that
     * skipped a fixed amount would pass this for one cluster count and fail for
     * another. Twelve clusters is the cap, chosen so a wrong stride is a wrong
     * answer rather than a lucky one.
     */
    SiteStatusRow site;
    site.anchor = 3;
    site.clusterCount = MAX_SITE_CLUSTERS;
    for (std::uint8_t index = 0; index < MAX_SITE_CLUSTERS; ++index)
    {
      site.clusterFullPct[index] = static_cast<std::uint8_t>(100 - index);
    }

    FrameBuilder frame{3};
    frame.Fleets({Fleet(3, FleetState::OnGrid, 1)});
    frame.Site(site);
    const std::uint32_t ore[ORE_COUNT] = {0, 0, 42};
    const std::uint32_t alloys[ALLOY_COUNT] = {};
    frame.Bay(8, ore, alloys);

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()), L"the frame after a site record was refused");
    const BayView* const bay = view.BayAt(8);
    Assert::IsNotNull(bay, L"the record after a site record was lost");
    Assert::AreEqual<std::uint32_t>(42, bay->oreUnits[2], L"the record after a site record was misread");
  }

  TEST_METHOD(AnUnknownKindStopsTheFrame)
  {
    // A schema disagreement the handshake was supposed to refuse. There is no
    // way to skip a body whose length only its own reader knows, so the frame
    // stops -- and stops *without* publishing the records before it.
    FrameBuilder frame{2};
    frame.Fleets({Fleet(4, FleetState::Docked, 3)});
    frame.RawKind(200);

    SummaryView view;
    Assert::IsFalse(view.Apply(frame.Bytes()), L"an unknown kind was accepted");
    Assert::IsFalse(view.HaveSummary(), L"a refused frame was published");
    Assert::AreEqual<std::uint32_t>(1, view.RejectedFrames(), L"the refusal was not counted");
  }

  TEST_METHOD(AFrameThatFailsHalfwayLeavesTheLastOneStanding)
  {
    /*
     * The staging rule, asserted rather than described. A caller handed half a
     * frame has no way to tell which half it got, so the previous frame stays
     * whole -- a panel one second stale beats a panel showing one station's
     * ships beside another station's Bay.
     */
    FrameBuilder good{2};
    good.Fleets({Fleet(4, FleetState::Docked, 3)});
    const std::uint32_t ore[ORE_COUNT] = {11, 0, 0};
    const std::uint32_t alloys[ALLOY_COUNT] = {};
    good.Bay(4, ore, alloys);

    SummaryView view;
    Assert::IsTrue(view.Apply(good.Bytes()));

    FrameBuilder torn{2};
    torn.Fleets({Fleet(7, FleetState::Docked, 9)});
    const std::uint32_t otherOre[ORE_COUNT] = {0, 0, 99};
    torn.Bay(7, otherOre, alloys);

    Assert::IsFalse(view.Apply(torn.Truncated(4)), L"a truncated frame was accepted");
    Assert::AreEqual<std::uint32_t>(1, view.RejectedFrames());

    const BayView* const bay = view.BayAt(4);
    Assert::IsNotNull(bay, L"a refused frame destroyed the frame before it");
    Assert::AreEqual<std::uint32_t>(11, bay->oreUnits[0], L"a refused frame partly overwrote the frame before it");
    Assert::IsNull(view.BayAt(7), L"half of a refused frame was published");
  }

  TEST_METHOD(AnEmptiedBayStopsBeingShown)
  {
    /*
     * Replace, not merge, and this is why. The sender omits a Bay with nothing
     * in it, so a store that merged would show the player ore they moved out
     * ten minutes ago and would go on showing it for the session.
     */
    FrameBuilder withBay{2};
    withBay.Fleets({Fleet(4, FleetState::Docked, 1)});
    const std::uint32_t ore[ORE_COUNT] = {50, 0, 0};
    const std::uint32_t alloys[ALLOY_COUNT] = {};
    withBay.Bay(4, ore, alloys);

    SummaryView view;
    Assert::IsTrue(view.Apply(withBay.Bytes()));
    Assert::IsNotNull(view.BayAt(4));

    FrameBuilder emptied{1};
    emptied.Fleets({Fleet(4, FleetState::Docked, 1)});
    Assert::IsTrue(view.Apply(emptied.Bytes()));
    Assert::IsNull(view.BayAt(4), L"an emptied Bay was still being shown");
    Assert::AreEqual<std::size_t>(0, view.Bays().size(), L"the emptied Bay was still in the list");
  }

  TEST_METHOD(TheHangarCountComesFromTheFleetSummaryAndNotTheRoster)
  {
    /*
     * The two numbers come from two records, and only one of them is always
     * sent. A roster the sender's byte budget dropped must read as "the rows
     * did not fit", never as "the ships are gone".
     */
    FrameBuilder frame{1};
    frame.Fleets({Fleet(4, FleetState::Docked, 6)});

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()));
    Assert::AreEqual<std::uint16_t>(6, view.DockedCountAt(4), L"the count did not survive a dropped roster");

    const DockedStationView* const block = view.DockedAt(4);
    Assert::IsNotNull(block);
    Assert::AreEqual<std::size_t>(0, block->docked.size(), L"rows appeared from nowhere");
  }

  TEST_METHOD(EachRostersRowsGoToItsOwnStation)
  {
    // Written station-9-first so a decoder that paired by arrival order rather
    // than by anchor gets it wrong.
    FrameBuilder frame{4};
    frame.Fleets({Fleet(4, FleetState::Docked, 1), Fleet(9, FleetState::Docked, 2)});
    frame.Roster(9, {Docked(91, 900), Docked(92, 0)});
    frame.Roster(4, {Docked(41, 400)});
    const std::uint32_t ore[ORE_COUNT] = {1, 1, 1};
    const std::uint32_t alloys[ALLOY_COUNT] = {};
    frame.Bay(4, ore, alloys);

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()));
    Assert::AreEqual<std::size_t>(2, view.DockedStations().size());

    // And sorted by anchor rather than by arrival: a list that reshuffles
    // between frames is a list the player cannot point at.
    Assert::AreEqual<std::uint16_t>(4, view.DockedStations()[0].anchor, L"blocks were not ordered by anchor");
    Assert::AreEqual<std::uint16_t>(9, view.DockedStations()[1].anchor);

    Assert::AreEqual<std::size_t>(1, view.DockedAt(4)->docked.size(), L"station 4's rows");
    Assert::AreEqual<std::uint16_t>(41, view.DockedAt(4)->docked[0].shipId, L"station 4 got another station's ships");
    Assert::AreEqual<std::size_t>(2, view.DockedAt(9)->docked.size(), L"station 9's rows");
    Assert::AreEqual<std::uint32_t>(900, view.DockedAt(9)->docked[0].oreUnits[0], L"station 9's manifest");
  }

  TEST_METHOD(OnlyDockedFleetsBecomeBlocks)
  {
    // A fleet on a grid is drawn as ships and a fleet in transit belongs to the
    // strategic surface; this list is the ones the scene cannot show.
    FrameBuilder frame{1};
    frame.Fleets({Fleet(1, FleetState::OnGrid, 4), Fleet(2, FleetState::InTransit, 5),
                  Fleet(3, FleetState::Docked, 6)});

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()));
    Assert::AreEqual<std::size_t>(3, view.Fleets().size(), L"a fleet row was dropped");
    Assert::AreEqual<std::size_t>(1, view.DockedStations().size(), L"a fleet that is not docked became a block");
    Assert::AreEqual<std::uint16_t>(3, view.DockedStations()[0].anchor);
  }

  TEST_METHOD(AShipCarryingNothingIsAbsentRatherThanZero)
  {
    // The sender writes holds that have something in them, so a lookup that
    // misses is the answer "READY" and not a failure.
    FrameBuilder frame{2};
    frame.Fleets({Fleet(4, FleetState::OnGrid, 2)});
    CargoStatusRow hold;
    hold.shipId = 21;
    hold.oreUnits[0] = 3;
    frame.Cargo({hold});

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()));
    Assert::IsNotNull(view.CargoOf(21));
    Assert::IsNull(view.CargoOf(22), L"a ship nobody reported on came back with a hold");
  }

  TEST_METHOD(ARefineryKeepsItsQueueInTheOrderItWasSent)
  {
    /*
     * Refineries are sorted by station so the tab's stations do not reshuffle;
     * the **jobs inside one** are not, because their order is the queue and
     * sorting it would reorder the player's own decisions. Asserted because the
     * two sorts sit three lines apart and doing both is the easy mistake.
     */
    RefineryStatusRow row;
    row.station = 4;
    row.tier = 1;
    row.jobs.push_back(RefineryJobRow{9, AlloyId::NovaSteel, 50, 0});
    row.jobs.push_back(RefineryJobRow{3, AlloyId::FerrocitePlates, 1, 700});
    row.jobs.push_back(RefineryJobRow{5, AlloyId::AstraGlass, 10, 0});

    FrameBuilder frame{2};
    frame.Fleets({Fleet(4, FleetState::Docked, 1)});
    frame.Refinery(row);

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()));
    const RefineryStatusRow* const back = view.RefineryAt(4);
    Assert::IsNotNull(back);
    Assert::AreEqual<std::size_t>(3, back->jobs.size());
    Assert::AreEqual<std::uint32_t>(9, back->jobs[0].sequence, L"the queue was reordered");
    Assert::AreEqual<std::uint32_t>(3, back->jobs[1].sequence, L"the queue was reordered");
    Assert::AreEqual<std::uint32_t>(5, back->jobs[2].sequence, L"the queue was reordered");
  }

  TEST_METHOD(TwoRefineriesComeBackByStation)
  {
    RefineryStatusRow high;
    high.station = 12;
    high.tier = 3;
    RefineryStatusRow low;
    low.station = 2;
    low.tier = 1;

    FrameBuilder frame{3};
    frame.Fleets({Fleet(2, FleetState::Docked, 1)});
    frame.Refinery(high);
    frame.Refinery(low);

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()));
    Assert::AreEqual<std::size_t>(2, view.Refineries().size());
    Assert::AreEqual<std::uint16_t>(2, view.Refineries()[0].station, L"refineries were not ordered by station");
    Assert::AreEqual<std::uint8_t>(3, view.RefineryAt(12)->tier, L"the wrong refinery was found");
  }

  TEST_METHOD(AFrameWithNoRecordsIsUnderstoodAndSaysNothing)
  {
    /*
     * The sender never writes one -- it returns false rather than send an empty
     * frame -- but a decoder that treated zero records as malformed would be
     * refusing something well formed, and the refusal counter is a diagnostic
     * somebody will one day read.
     */
    FrameBuilder frame{0};

    SummaryView view;
    Assert::IsTrue(view.Apply(frame.Bytes()), L"an empty frame was refused");
    Assert::IsTrue(view.HaveSummary());
    Assert::AreEqual<std::size_t>(0, view.Fleets().size());
    Assert::AreEqual<std::uint32_t>(0, view.RejectedFrames());
  }

  TEST_METHOD(AFrameShorterThanItsHeaderIsRefused)
  {
    SummaryView view;
    Assert::IsFalse(view.Apply({}), L"an empty payload was accepted");
    Assert::AreEqual<std::uint32_t>(1, view.RejectedFrames());
  }

  TEST_METHOD(AFrameClaimingMoreRecordsThanItHasIsRefused)
  {
    // The count is a promise; a frame that does not keep it is truncated, and
    // the records it *did* carry are not published on their own.
    FrameBuilder frame{3};
    frame.Fleets({Fleet(4, FleetState::Docked, 1)});

    SummaryView view;
    Assert::IsFalse(view.Apply(frame.Bytes()), L"a frame short of its own count was accepted");
    Assert::IsFalse(view.HaveSummary());
  }
};

} // namespace GameLogicTests
