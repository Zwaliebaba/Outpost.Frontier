#include "pch.h"

#include "EconomyMessages.h"

#include <algorithm>

namespace Game
{

SiteStatusRow MakeSiteStatus(AnchorId _anchor, const SiteField& _field, const SiteField& _pristine,
                             std::uint32_t _epoch) noexcept
{
  SiteStatusRow row;
  row.anchor = _anchor;
  row.epoch = _epoch;
  row.clusterCount = _field.clusterCount;

  for (std::uint8_t index = 0; index < _field.clusterCount && index < MAX_SITE_CLUSTERS; ++index)
  {
    std::uint64_t remaining = 0;
    std::uint64_t started = 0;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      remaining += _field.clusters[index].remainingUnits[ore];
      started += index < _pristine.clusterCount ? _pristine.clusters[index].remainingUnits[ore] : 0;
    }

    /*
     * **100 means untouched and 0 means empty, and nothing else may claim
     * either.** The middle is floored into 1..99.
     *
     * The obvious roundings both lie at one end. Rounding up hides the first
     * dent -- a 4,000-unit cluster with forty units taken reads 100 %, so a
     * field being visibly worked looks pristine, which is exactly the signal
     * ADR-024 §3d wants a player to be able to see. Rounding down does the
     * mirror image at the other end: the last forty units read 0 %, and a
     * commander skips a rock that still has a cycle in it.
     *
     * Reserving the two endpoints for the two facts they name costs one
     * comparison and makes the bar honest at both ends -- which matters more
     * here than a percentage point of precision in the middle, because the
     * questions a player asks of this number are "has someone been here" and
     * "is there anything left".
     *
     * A cluster the epoch laid out empty reads as full: it is the honest answer
     * to "how much of this is left" when the answer is "all of the nothing it
     * had", and drawing an exhausted rock where the field never put one would
     * teach the wrong thing about the system.
     */
    std::uint64_t pct = 100;
    if (started != 0)
    {
      pct = remaining == 0            ? 0
            : remaining >= started    ? 100
                                      : std::max<std::uint64_t>(1, std::min<std::uint64_t>(99, remaining * 100 / started));
    }
    row.clusterFullPct[index] = static_cast<std::uint8_t>(pct);
  }

  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    std::uint64_t total = 0;
    for (std::uint8_t index = 0; index < _field.clusterCount && index < MAX_SITE_CLUSTERS; ++index)
    {
      total += _field.clusters[index].remainingUnits[ore];
    }
    row.remainingUnits[ore] = static_cast<std::uint32_t>(std::min<std::uint64_t>(total, 0xffffffffull));
  }
  return row;
}

bool WriteSiteStatus(const SiteStatusRow& _row, Neuron::ByteWriter& _writer) noexcept
{
  if (_row.clusterCount > MAX_SITE_CLUSTERS)
  {
    return false; // A count past the cap is a caller bug, not a payload to send.
  }
  if (_writer.BytesRemaining() < SiteStatusBytes(_row.clusterCount))
  {
    return false;
  }

  _writer.WriteUInt16(_row.anchor);
  _writer.WriteUInt32(_row.epoch);
  for (const std::uint32_t units : _row.remainingUnits)
  {
    _writer.WriteUInt32(units);
  }
  _writer.WriteUInt8(_row.clusterCount);
  for (std::uint8_t index = 0; index < _row.clusterCount; ++index)
  {
    _writer.WriteUInt8(_row.clusterFullPct[index]);
  }
  return _writer.Ok();
}

bool ReadSiteStatus(Neuron::ByteReader& _reader, SiteStatusRow& _outRow) noexcept
{
  _outRow = SiteStatusRow{};

  _outRow.anchor = _reader.ReadUInt16();
  _outRow.epoch = _reader.ReadUInt32();
  for (std::uint32_t& units : _outRow.remainingUnits)
  {
    units = _reader.ReadUInt32();
  }

  const std::uint8_t clusterCount = _reader.ReadUInt8();
  if (!_reader.Ok() || clusterCount > MAX_SITE_CLUSTERS)
  {
    // Before the loop, the family's rule: a count is checked against the cap
    // this build knows rather than against the payload's own promise.
    return false;
  }
  _outRow.clusterCount = clusterCount;
  for (std::uint8_t index = 0; index < clusterCount; ++index)
  {
    // Clamped rather than refused. Unlike a count, a percentage past 100 costs
    // nothing to make sense of, and refusing a whole field's status over one
    // wide bar would lose the other fifteen numbers in the message.
    _outRow.clusterFullPct[index] = std::min<std::uint8_t>(_reader.ReadUInt8(), 100);
  }
  return _reader.Ok();
}

bool WriteCargoStatus(std::span<const CargoStatusRow> _rows, Neuron::ByteWriter& _writer) noexcept
{
  if (_rows.size() > MAX_CARGO_STATUS_ROWS)
  {
    return false;
  }
  if (_writer.BytesRemaining() < CargoStatusBytes(_rows.size()))
  {
    return false;
  }

  _writer.WriteUInt16(static_cast<std::uint16_t>(_rows.size()));
  for (const CargoStatusRow& row : _rows)
  {
    // u32 for the ship id, like the roster and the command: these are the
    // durable-ish artifacts ADR-018 A11 widened in advance, and a summary that
    // still said u16 would be the one place the widening had to happen twice.
    _writer.WriteUInt32(row.shipId);
    for (const std::uint32_t units : row.oreUnits)
    {
      _writer.WriteUInt32(units);
    }
  }
  return _writer.Ok();
}

bool ReadCargoStatus(Neuron::ByteReader& _reader, std::vector<CargoStatusRow>& _outRows)
{
  _outRows.clear();

  const std::uint16_t count = _reader.ReadUInt16();
  if (!_reader.Ok() || count > MAX_CARGO_STATUS_ROWS)
  {
    return false;
  }

  _outRows.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index)
  {
    CargoStatusRow row;
    const std::uint32_t wide = _reader.ReadUInt32();
    row.shipId = wide > INVALID_SHIP_ID ? INVALID_SHIP_ID : static_cast<ShipId>(wide);
    for (std::uint32_t& units : row.oreUnits)
    {
      units = _reader.ReadUInt32();
    }
    _outRows.push_back(row);
  }
  return _reader.Ok();
}

bool WriteBayStatus(AnchorId _station, std::span<const std::uint32_t> _oreUnits, std::span<const std::uint32_t> _alloyUnits,
                    Neuron::ByteWriter& _writer) noexcept
{
  if (_alloyUnits.size() != ALLOY_COUNT)
  {
    return false; // Exactly five, for the ore counts' reason below.
  }
  if (_oreUnits.size() != ORE_COUNT)
  {
    // Exactly three, not at least three: a Bay short of an ore would decode as
    // a Bay holding none of it, and silently reporting zero of something is the
    // failure this whole family is meant not to have.
    return false;
  }
  if (_writer.BytesRemaining() < BAY_STATUS_BYTES)
  {
    return false;
  }

  _writer.WriteUInt16(_station);
  for (const std::uint32_t units : _oreUnits)
  {
    _writer.WriteUInt32(units);
  }
  for (const std::uint32_t units : _alloyUnits)
  {
    _writer.WriteUInt32(units);
  }
  return _writer.Ok();
}

bool ReadBayStatus(Neuron::ByteReader& _reader, AnchorId& _outStation, std::uint32_t (&_outOreUnits)[ORE_COUNT],
                   std::uint32_t (&_outAlloyUnits)[ALLOY_COUNT]) noexcept
{
  _outStation = _reader.ReadUInt16();
  for (std::uint32_t& units : _outAlloyUnits)
  {
    units = 0;
  }
  for (std::uint32_t& units : _outOreUnits)
  {
    units = _reader.ReadUInt32();
  }
  for (std::uint32_t& units : _outAlloyUnits)
  {
    units = _reader.ReadUInt32();
  }
  return _reader.Ok();
}

bool WriteRefineryStatus(const RefineryStatusRow& _row, Neuron::ByteWriter& _writer) noexcept
{
  if (_row.jobs.size() > MAX_REFINERY_JOB_ROWS)
  {
    // Refused rather than truncated, for the roster's reason: half a queue is
    // not the queue, and a tab drawing nine of ten jobs would be a tab lying
    // about what `RefineryBusy` is counting.
    return false;
  }
  if (_writer.BytesRemaining() < RefineryStatusBytes(_row.jobs.size()))
  {
    return false;
  }

  _writer.WriteUInt16(_row.station);
  _writer.WriteUInt8(_row.tier);
  _writer.WriteUInt16(static_cast<std::uint16_t>(_row.jobs.size()));
  for (const RefineryJobRow& job : _row.jobs)
  {
    _writer.WriteUInt32(job.sequence);
    _writer.WriteUInt8(static_cast<std::uint8_t>(job.alloy));
    _writer.WriteUInt32(job.batchUnits);
    _writer.WriteUInt32(job.completeTick);
  }
  _writer.WriteUInt8(_row.projectToTier);
  for (const std::uint32_t units : _row.projectContributedUnits)
  {
    _writer.WriteUInt32(units);
  }
  return _writer.Ok();
}

bool ReadRefineryStatus(Neuron::ByteReader& _reader, RefineryStatusRow& _outRow)
{
  _outRow = RefineryStatusRow{};
  _outRow.station = _reader.ReadUInt16();
  _outRow.tier = _reader.ReadUInt8();
  const std::uint16_t jobCount = _reader.ReadUInt16();
  if (!_reader.Ok() || jobCount > MAX_REFINERY_JOB_ROWS)
  {
    // The count is bounded before it is used to size anything, which is the
    // rule every count on this wire obeys.
    return false;
  }

  _outRow.jobs.reserve(jobCount);
  for (std::uint16_t index = 0; index < jobCount; ++index)
  {
    RefineryJobRow job;
    job.sequence = _reader.ReadUInt32();
    const std::uint8_t alloy = _reader.ReadUInt8();
    job.batchUnits = _reader.ReadUInt32();
    job.completeTick = _reader.ReadUInt32();
    if (!_reader.Ok() || !TryAlloyId(alloy, job.alloy))
    {
      // The alloy byte is refused rather than cast, exactly as the command's
      // is: it indexes a recipe on the way to a screen.
      return false;
    }
    _outRow.jobs.push_back(job);
  }

  _outRow.projectToTier = _reader.ReadUInt8();
  for (std::uint32_t& units : _outRow.projectContributedUnits)
  {
    units = _reader.ReadUInt32();
  }
  return _reader.Ok();
}

} // namespace Game
