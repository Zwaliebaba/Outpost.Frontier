#include "pch.h"

#include "OrderMessages.h"

namespace Game
{

bool WriteOrderSubmit(const OrderSubmit& _order, Neuron::ByteWriter& _writer) noexcept
{
  if (_order.shipCount > MAX_SHIPS_PER_ORDER)
  {
    return false; // Refused rather than truncated: half a selection is not the order.
  }
  if (_writer.BytesRemaining() < OrderSubmitBytes(_order.shipCount))
  {
    return false;
  }

  _writer.WriteUInt32(_order.orderSeq);
  _writer.WriteUInt8(static_cast<std::uint8_t>(_order.kind));
  _writer.WriteUInt8(static_cast<std::uint8_t>(_order.formation));
  _writer.WriteUInt8(static_cast<std::uint8_t>(_order.queueMode));
  _writer.WriteUInt16(_order.shipCount);
  for (std::uint16_t index = 0; index < _order.shipCount; ++index)
  {
    _writer.WriteUInt16(_order.shipIds[index]);
  }
  _writer.WriteInt32(_order.target.xCm);
  _writer.WriteInt32(_order.target.yCm);
  _writer.WriteUInt16(_order.target.facingTurns16);
  return _writer.Ok();
}

bool ReadOrderSubmit(Neuron::ByteReader& _reader, OrderSubmit& _outOrder) noexcept
{
  _outOrder = OrderSubmit{};

  _outOrder.orderSeq = _reader.ReadUInt32();

  // The enums are read as their underlying width and cast without checking.
  // Validation is what refuses an unknown kind or formation, and it has to be
  // the thing that refuses them: a decoder that rejected a kind it did not
  // recognise would return "malformed" where the client is owed `UnknownKind`.
  _outOrder.kind = static_cast<OrderKind>(_reader.ReadUInt8());
  _outOrder.formation = static_cast<FormationId>(_reader.ReadUInt8());
  _outOrder.queueMode = static_cast<QueueMode>(_reader.ReadUInt8());

  const std::uint16_t shipCount = _reader.ReadUInt16();
  if (!_reader.Ok() || shipCount > MAX_SHIPS_PER_ORDER)
  {
    // Before the loop, not after. A count of 65,535 would otherwise run the
    // read sixty-five thousand times against an underflowing reader before
    // anything noticed.
    return false;
  }

  _outOrder.shipCount = shipCount;
  for (std::uint16_t index = 0; index < shipCount; ++index)
  {
    _outOrder.shipIds[index] = _reader.ReadUInt16();
  }

  _outOrder.target.xCm = _reader.ReadInt32();
  _outOrder.target.yCm = _reader.ReadInt32();
  _outOrder.target.facingTurns16 = _reader.ReadUInt16();
  return _reader.Ok();
}

} // namespace Game
