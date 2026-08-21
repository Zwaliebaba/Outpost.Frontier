#include "pch.h"

#include "StationMessages.h"

#include "ShipClass.h"

namespace Game
{
namespace
{

/*
 * A u32 ship id from the wire, narrowed to what this build can hold.
 *
 * Out of range becomes `INVALID_SHIP_ID` rather than a truncating cast: the
 * cast would alias a real ship -- id 65,537 would arrive as ship 1 -- which is
 * a validated command against the wrong hull. Invalid is a value validation
 * already refuses with a reason the player can read.
 */
[[nodiscard]] ShipId NarrowShipId(std::uint32_t _wireId) noexcept
{
  return _wireId > 0xfffeu ? INVALID_SHIP_ID : static_cast<ShipId>(_wireId);
}

} // namespace

bool WriteStationCommand(const StationCommand& _command, Neuron::ByteWriter& _writer) noexcept
{
  if (_command.shipCount > MAX_SHIPS_PER_ORDER)
  {
    return false; // Refused rather than truncated: half a selection is not the fleet.
  }
  if (_writer.BytesRemaining() < StationCommandBytes(_command.shipCount))
  {
    return false;
  }

  _writer.WriteUInt32(_command.orderSeq);
  _writer.WriteUInt8(static_cast<std::uint8_t>(_command.verb));
  _writer.WriteUInt16(_command.station);
  _writer.WriteUInt8(static_cast<std::uint8_t>(_command.formation));
  _writer.WriteUInt8(_command.wing);
  _writer.WriteUInt8(static_cast<std::uint8_t>(_command.ore));
  _writer.WriteUInt32(_command.units);
  _writer.WriteUInt8(static_cast<std::uint8_t>(_command.alloy));
  _writer.WriteUInt32(_command.sequence);
  _writer.WriteUInt16(_command.shipCount);
  for (std::uint16_t index = 0; index < _command.shipCount; ++index)
  {
    _writer.WriteUInt32(_command.shipIds[index]);
  }
  return _writer.Ok();
}

bool ReadStationCommand(Neuron::ByteReader& _reader, StationCommand& _outCommand) noexcept
{
  _outCommand = StationCommand{};

  _outCommand.orderSeq = _reader.ReadUInt32();

  // Cast without checking, like the order family's enums and for the same
  // reason: an unknown verb or formation is `ValidateStationCommand`'s to
  // refuse with a reason, not the decoder's to call malformed.
  _outCommand.verb = static_cast<StationVerb>(_reader.ReadUInt8());
  _outCommand.station = _reader.ReadUInt16();
  _outCommand.formation = static_cast<FormationId>(_reader.ReadUInt8());
  _outCommand.wing = _reader.ReadUInt8();

  /*
   * The ore byte is **refused** rather than cast, unlike the verb and the
   * formation beside it -- the same split `OrderSubmit`'s ore filter makes, and
   * for the same reason (ADR-024 §4a).
   *
   * A verb or a formation this build does not know is a value validation can
   * bounce with a reason the player reads. An ore outside the enum is not: it
   * would index the manifest arrays out of bounds before anything got the
   * chance to have an opinion, and there is no sentence to show somebody whose
   * client believes in a fourth ore.
   */
  if (!TryOreId(_reader.ReadUInt8(), _outCommand.ore))
  {
    return false;
  }
  _outCommand.units = _reader.ReadUInt32();

  // The alloy byte on the ore byte's terms exactly (E4b): refused rather than
  // cast, because it indexes a recipe and a Bay before validation could have an
  // opinion about it.
  if (!TryAlloyId(_reader.ReadUInt8(), _outCommand.alloy))
  {
    return false;
  }
  _outCommand.sequence = _reader.ReadUInt32();

  const std::uint16_t shipCount = _reader.ReadUInt16();
  if (!_reader.Ok() || shipCount > MAX_SHIPS_PER_ORDER)
  {
    // Before the loop, not after: a count of 65,535 would otherwise run the
    // read sixty-five thousand times against an underflowing reader.
    return false;
  }

  _outCommand.shipCount = shipCount;
  for (std::uint16_t index = 0; index < shipCount; ++index)
  {
    _outCommand.shipIds[index] = NarrowShipId(_reader.ReadUInt32());
  }
  return _reader.Ok();
}

bool WriteStationRoster(AnchorId _station, std::span<const RosterEntry> _docked, Neuron::ByteWriter& _writer) noexcept
{
  if (_docked.size() > MAX_SHIPS_PER_ORDER)
  {
    /*
     * A hangar is uncapped by design (ADR-017's "what this deliberately does
     * not do"), so a roster *can* outgrow one message. Refusing is the honest
     * answer here rather than truncating -- a roster short of its last ships is
     * a hangar screen that cannot undock them -- and paging is the summary
     * family's problem to solve when a station first holds more than 64.
     */
    return false;
  }
  if (_writer.BytesRemaining() < StationRosterBytes(_docked.size()))
  {
    return false;
  }

  _writer.WriteUInt16(_station);
  _writer.WriteUInt16(static_cast<std::uint16_t>(_docked.size()));
  for (const RosterEntry& row : _docked)
  {
    _writer.WriteUInt32(row.shipId);
    _writer.WriteUInt8(static_cast<std::uint8_t>(row.hullClass));
    _writer.WriteUInt8(row.wing);
    for (const std::uint32_t units : row.oreUnits)
    {
      _writer.WriteUInt32(units);
    }
  }
  return _writer.Ok();
}

bool ReadStationRoster(Neuron::ByteReader& _reader, AnchorId& _outStation, std::vector<RosterEntry>& _outDocked)
{
  _outDocked.clear();

  _outStation = _reader.ReadUInt16();
  const std::uint16_t count = _reader.ReadUInt16();
  if (!_reader.Ok() || count > MAX_SHIPS_PER_ORDER)
  {
    return false;
  }

  // Sized once from the count the header already bounded, rather than grown a
  // row at a time.
  _outDocked.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index)
  {
    RosterEntry row;
    row.shipId = NarrowShipId(_reader.ReadUInt32());

    // An unknown class is refused *here*, unlike an unknown verb, because there
    // is no verdict to carry a reason back on: a roster is an answer, not a
    // request, and a row nobody can draw is a row that would be drawn wrong.
    HullClass hullClass = HullClass::Interceptor;
    if (!TryShipClass(_reader.ReadUInt8(), hullClass))
    {
      return false;
    }
    row.hullClass = hullClass;
    row.wing = _reader.ReadUInt8();
    for (std::uint32_t& units : row.oreUnits)
    {
      units = _reader.ReadUInt32();
    }
    _outDocked.push_back(row);
  }
  return _reader.Ok();
}

} // namespace Game
