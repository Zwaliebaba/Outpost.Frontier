#include "pch.h"

#include "FleetSummary.h"

namespace Game
{

const char* FleetStateName(FleetState _state) noexcept
{
  switch (_state)
  {
  case FleetState::OnGrid:
    return "On grid";
  case FleetState::Docked:
    return "Docked";
  case FleetState::InTransit:
    return "In warp";
  }
  // Not a `default` label: a fourth state should fail the switch's
  // exhaustiveness warning here rather than reach a roster block unnamed.
  return "unnamed state";
}

bool WriteFleetSummaries(std::span<const FleetSummary> _summaries, Neuron::ByteWriter& _writer) noexcept
{
  if (_summaries.size() > MAX_FLEET_SUMMARIES)
  {
    return false; // Refused rather than truncated: a partial answer to "where is everything" is a wrong one.
  }
  if (_writer.BytesRemaining() < FleetSummariesBytes(_summaries.size()))
  {
    return false;
  }

  _writer.WriteUInt16(static_cast<std::uint16_t>(_summaries.size()));
  for (const FleetSummary& row : _summaries)
  {
    _writer.WriteUInt16(row.anchor);
    _writer.WriteUInt8(static_cast<std::uint8_t>(row.state));
    _writer.WriteUInt16(row.shipCount);
    _writer.WriteUInt16(row.etaSeconds);
    _writer.WriteUInt8(row.wing);
  }
  return _writer.Ok();
}

bool ReadFleetSummaries(Neuron::ByteReader& _reader, std::vector<FleetSummary>& _outSummaries)
{
  _outSummaries.clear();

  const std::uint16_t count = _reader.ReadUInt16();
  if (!_reader.Ok() || count > MAX_FLEET_SUMMARIES)
  {
    // Before the loop, so a count of 65,535 does not run the read sixty-five
    // thousand times against an underflowing reader.
    return false;
  }

  _outSummaries.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index)
  {
    FleetSummary row;
    row.anchor = _reader.ReadUInt16();

    // Cast without checking, like every other enum off the wire: a state this
    // build does not know is a row it draws as unnamed rather than a message it
    // calls malformed. There is no verdict to carry a reason back on, but there
    // is also nothing dangerous in a number nobody switches on exhaustively.
    row.state = static_cast<FleetState>(_reader.ReadUInt8());
    row.shipCount = _reader.ReadUInt16();
    row.etaSeconds = _reader.ReadUInt16();
    row.wing = _reader.ReadUInt8();
    _outSummaries.push_back(row);
  }
  return _reader.Ok();
}

} // namespace Game
