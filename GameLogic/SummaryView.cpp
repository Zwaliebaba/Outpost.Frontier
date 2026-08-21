#include "pch.h"

#include "SummaryView.h"

#include "ByteReader.h"
#include "StationMessages.h"

#include <algorithm>
#include <utility>

namespace Game
{

namespace
{

/// Finds a row by the anchor it is about. One helper rather than five copies of
/// the same `find_if`, and it returns null rather than an end iterator because
/// every caller here wants "or nothing".
template <typename RowT, typename KeyFn>
[[nodiscard]] const RowT* FindBy(const std::vector<RowT>& _rows, KeyFn _key, AnchorId _wanted) noexcept
{
  for (const RowT& row : _rows)
  {
    if (_key(row) == _wanted)
    {
      return &row;
    }
  }
  return nullptr;
}

} // namespace

bool BayView::Empty() const noexcept
{
  for (const std::uint32_t units : oreUnits)
  {
    if (units > 0)
    {
      return false;
    }
  }
  for (const std::uint32_t units : alloyUnits)
  {
    if (units > 0)
    {
      return false;
    }
  }
  return true;
}

bool SummaryView::Apply(std::span<const std::uint8_t> _payload)
{
  Neuron::ByteReader reader{_payload};
  std::uint8_t records = 0;
  if (!ReadSummaryFrame(reader, records))
  {
    ++m_rejectedFrames;
    return false;
  }

  /*
   * Staged locally and moved in at the end. Everything below may bail out, and
   * a caller that had been handed half of a frame would have no way to tell --
   * so nothing this reads is visible until all of it read.
   */
  std::vector<FleetSummary> fleets;
  std::vector<DockedStationView> rosters;
  std::vector<SiteStatusRow> sites;
  std::vector<CargoStatusRow> cargo;
  std::vector<BayView> bays;
  std::vector<RefineryStatusRow> refineries;

  for (std::uint8_t index = 0; index < records; ++index)
  {
    SummaryKind kind{};
    if (!ReadSummaryRecord(reader, kind))
    {
      // An unknown kind is a schema disagreement the handshake was supposed to
      // have refused, and there is no way to skip a body whose length only its
      // own reader knows -- so the frame stops here rather than guessing.
      ++m_rejectedFrames;
      return false;
    }

    switch (kind)
    {
    case SummaryKind::FleetSummaries:
      if (!ReadFleetSummaries(reader, fleets))
      {
        ++m_rejectedFrames;
        return false;
      }
      break;

    case SummaryKind::StationRoster:
    {
      DockedStationView row;
      if (!ReadStationRoster(reader, row.anchor, row.docked))
      {
        ++m_rejectedFrames;
        return false;
      }
      rosters.push_back(std::move(row));
      break;
    }

    case SummaryKind::SiteStatus:
    {
      SiteStatusRow row;
      if (!ReadSiteStatus(reader, row))
      {
        ++m_rejectedFrames;
        return false;
      }
      sites.push_back(row);
      break;
    }

    case SummaryKind::CargoStatus:
      // Appended rather than assigned: the sender writes one record today, and
      // a reader that assumed so would silently keep the last of two if paging
      // ever splits it (ADR-016 §6 owes that).
      {
        std::vector<CargoStatusRow> part;
        if (!ReadCargoStatus(reader, part))
        {
          ++m_rejectedFrames;
          return false;
        }
        cargo.insert(cargo.end(), part.begin(), part.end());
      }
      break;

    case SummaryKind::BayStatus:
    {
      BayView bay;
      if (!ReadBayStatus(reader, bay.station, bay.oreUnits, bay.alloyUnits))
      {
        ++m_rejectedFrames;
        return false;
      }
      bays.push_back(bay);
      break;
    }

    case SummaryKind::RefineryStatus:
    {
      RefineryStatusRow row;
      if (!ReadRefineryStatus(reader, row))
      {
        ++m_rejectedFrames;
        return false;
      }
      refineries.push_back(std::move(row));
      break;
    }

    default:
      /*
       * Unreachable through `ReadSummaryRecord`, which refuses a byte outside
       * the enum -- and written anyway, because the case this guards is not a
       * bad byte but a **seventh kind added to the enum and not to this
       * switch**. Without it that kind would fall out of the switch with its
       * body unread, leaving the reader mid-record and the next kind byte taken
       * from the middle of somebody's cargo. That is the defect this file was
       * written to close, and leaving the door open for its successor would
       * make the fix a one-off rather than a rule.
       */
      ++m_rejectedFrames;
      return false;
    }
  }

  /*
   * The count and the roster meet here, and nowhere else.
   *
   * Only the docked fleets get a block: a fleet on a grid is drawn as ships and
   * a fleet in transit is the strategic surface's business (U3b), so this list
   * is about the ones that are nowhere the scene can show them.
   */
  std::vector<DockedStationView> docked;
  for (const FleetSummary& row : fleets)
  {
    if (row.state != FleetState::Docked)
    {
      continue;
    }
    DockedStationView block;
    block.anchor = row.anchor;
    block.shipCount = row.shipCount;
    const auto match = std::find_if(rosters.begin(), rosters.end(),
                                    [&](const DockedStationView& _entry) { return _entry.anchor == row.anchor; });
    if (match != rosters.end())
    {
      block.docked = std::move(match->docked);
    }
    docked.push_back(std::move(block));
  }

  // By anchor, so the panel's order is the universe's rather than the order the
  // records happened to arrive in.
  const auto byAnchor = [](const auto& _left, const auto& _right) { return _left.anchor < _right.anchor; };
  std::sort(docked.begin(), docked.end(), byAnchor);
  std::sort(sites.begin(), sites.end(), byAnchor);
  std::sort(bays.begin(), bays.end(),
            [](const BayView& _left, const BayView& _right) { return _left.station < _right.station; });
  std::sort(refineries.begin(), refineries.end(),
            [](const RefineryStatusRow& _left, const RefineryStatusRow& _right) { return _left.station < _right.station; });
  std::sort(cargo.begin(), cargo.end(),
            [](const CargoStatusRow& _left, const CargoStatusRow& _right) { return _left.shipId < _right.shipId; });

  m_docked = std::move(docked);
  m_fleets = std::move(fleets);
  m_sites = std::move(sites);
  m_cargo = std::move(cargo);
  m_bays = std::move(bays);
  m_refineries = std::move(refineries);
  m_haveSummary = true;
  return true;
}

const DockedStationView* SummaryView::DockedAt(AnchorId _anchor) const noexcept
{
  return FindBy(m_docked, [](const DockedStationView& _row) { return _row.anchor; }, _anchor);
}

const SiteStatusRow* SummaryView::SiteAt(AnchorId _anchor) const noexcept
{
  return FindBy(m_sites, [](const SiteStatusRow& _row) { return _row.anchor; }, _anchor);
}

const BayView* SummaryView::BayAt(AnchorId _anchor) const noexcept
{
  return FindBy(m_bays, [](const BayView& _row) { return _row.station; }, _anchor);
}

const RefineryStatusRow* SummaryView::RefineryAt(AnchorId _station) const noexcept
{
  return FindBy(m_refineries, [](const RefineryStatusRow& _row) { return _row.station; }, _station);
}

const CargoStatusRow* SummaryView::CargoOf(ShipId _ship) const noexcept
{
  for (const CargoStatusRow& row : m_cargo)
  {
    if (row.shipId == _ship)
    {
      return &row;
    }
  }
  return nullptr;
}

std::uint16_t SummaryView::DockedCountAt(AnchorId _anchor) const noexcept
{
  const DockedStationView* const found = DockedAt(_anchor);
  return found == nullptr ? std::uint16_t{0} : found->shipCount;
}

} // namespace Game
