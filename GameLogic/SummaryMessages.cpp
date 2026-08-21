#include "pch.h"

#include "SummaryMessages.h"

namespace Game
{

bool BeginSummaryFrame(std::uint8_t _recordCount, Neuron::ByteWriter& _writer) noexcept
{
  if (_recordCount > MAX_SUMMARY_RECORDS)
  {
    return false;
  }
  if (_writer.BytesRemaining() < SUMMARY_FRAME_HEADER_BYTES)
  {
    return false;
  }
  _writer.WriteUInt8(_recordCount);
  return _writer.Ok();
}

bool BeginSummaryRecord(SummaryKind _kind, Neuron::ByteWriter& _writer) noexcept
{
  if (_writer.BytesRemaining() < SUMMARY_RECORD_HEADER_BYTES)
  {
    return false;
  }
  _writer.WriteUInt8(static_cast<std::uint8_t>(_kind));
  return _writer.Ok();
}

bool ReadSummaryFrame(Neuron::ByteReader& _reader, std::uint8_t& _outRecordCount) noexcept
{
  const std::uint8_t count = _reader.ReadUInt8();
  if (!_reader.Ok() || count > MAX_SUMMARY_RECORDS)
  {
    return false;
  }
  _outRecordCount = count;
  return true;
}

bool ReadSummaryRecord(Neuron::ByteReader& _reader, SummaryKind& _outKind) noexcept
{
  const std::uint8_t raw = _reader.ReadUInt8();
  if (!_reader.Ok())
  {
    return false;
  }
  // Refused rather than skipped: an unknown kind is a schema disagreement the
  // handshake is supposed to have caught, and a decoder that recovered from one
  // would hide it. Spelled as a switch so a kind added to the enum and not to
  // this list is a compiler warning rather than a runtime refusal.
  switch (static_cast<SummaryKind>(raw))
  {
  case SummaryKind::StationRoster:
  case SummaryKind::FleetSummaries:
  case SummaryKind::SiteStatus:
  case SummaryKind::CargoStatus:
  case SummaryKind::BayStatus:
  case SummaryKind::RefineryStatus:
    _outKind = static_cast<SummaryKind>(raw);
    return true;
  }
  return false;
}

} // namespace Game
