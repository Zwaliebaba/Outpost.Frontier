#include "pch.h"

#include "Wire.h"

#include "Hash.h"

#include <algorithm>

namespace Neuron
{

void WriteWireType(ByteWriter& _writer, WireType _type) noexcept
{
  _writer.WriteUInt16(static_cast<std::uint16_t>(_type));
}

WireType ReadWireType(ByteReader& _reader) noexcept
{
  const std::uint16_t value = _reader.ReadUInt16();
  return _reader.Ok() ? static_cast<WireType>(value) : WireType::None;
}

void Write(ByteWriter& _writer, const Hello& _message) noexcept
{
  _writer.WriteUInt16(_message.protocolVersion);
  _writer.WriteUInt64(_message.schemaHash);
  _writer.WriteUInt64(_message.contentHash);
  _writer.WriteText(_message.playerName);

  // Identity last, after the name, so the fields the handshake fails closed on
  // stay at fixed offsets from the front of the message (ADR-018 D5).
  _writer.WriteUInt32(_message.playerId);
  _writer.WriteUInt64(_message.resumeToken);
}

bool Read(ByteReader& _reader, Hello& _outMessage) noexcept
{
  _outMessage.protocolVersion = _reader.ReadUInt16();
  _outMessage.schemaHash = _reader.ReadUInt64();
  _outMessage.contentHash = _reader.ReadUInt64();
  _outMessage.playerName = std::string(_reader.ReadText());
  _outMessage.playerId = _reader.ReadUInt32();
  _outMessage.resumeToken = _reader.ReadUInt64();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const Welcome& _message) noexcept
{
  _writer.WriteUInt32(_message.clientId);
  _writer.WriteUInt32(_message.tick);
  _writer.WriteUInt16(_message.tickRate);
  _writer.WriteUInt64(_message.schemaHash);
  _writer.WriteUInt64(_message.contentHash);
  _writer.WriteUInt16(_message.worldId);
  // Which grid, beside where it is: a client that cannot name its anchor cannot
  // compose a Dock or a station command (ADR-017 §2, §3).
  _writer.WriteUInt16(_message.gridAnchor);
  // Signed and full-width: the universe plane runs to +/-9.2e18 metres
  // (ADR-009 §1), so anything narrower would silently fold it.
  _writer.WriteInt64(_message.anchorX);
  _writer.WriteInt64(_message.anchorY);
  _writer.WriteText(_message.worldName);
  _writer.WriteText(_message.worldDetail);
  _writer.WriteText(_message.worldBadge);

  // Identity last, after the variable-length strings, so the fields the
  // handshake fails closed on stay at fixed offsets from the front.
  _writer.WriteUInt32(_message.playerId);
  _writer.WriteUInt64(_message.resumeToken);
}

bool Read(ByteReader& _reader, Welcome& _outMessage) noexcept
{
  _outMessage.clientId = _reader.ReadUInt32();
  _outMessage.tick = _reader.ReadUInt32();
  _outMessage.tickRate = _reader.ReadUInt16();
  _outMessage.schemaHash = _reader.ReadUInt64();
  _outMessage.contentHash = _reader.ReadUInt64();
  _outMessage.worldId = _reader.ReadUInt16();
  _outMessage.gridAnchor = _reader.ReadUInt16();
  _outMessage.anchorX = _reader.ReadInt64();
  _outMessage.anchorY = _reader.ReadInt64();
  _outMessage.worldName = std::string(_reader.ReadText());
  _outMessage.worldDetail = std::string(_reader.ReadText());
  _outMessage.worldBadge = std::string(_reader.ReadText());
  _outMessage.playerId = _reader.ReadUInt32();
  _outMessage.resumeToken = _reader.ReadUInt64();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const ViewRequest& _message) noexcept
{
  _writer.WriteUInt16(_message.gridAnchor);
}

bool Read(ByteReader& _reader, ViewRequest& _outMessage) noexcept
{
  _outMessage.gridAnchor = _reader.ReadUInt16();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const ViewChanged& _message) noexcept
{
  _writer.WriteUInt16(_message.gridAnchor);
  _writer.WriteUInt16(_message.reasonCode);
  _writer.WriteBool(_message.accepted);
}

bool Read(ByteReader& _reader, ViewChanged& _outMessage) noexcept
{
  _outMessage.gridAnchor = _reader.ReadUInt16();
  _outMessage.reasonCode = _reader.ReadUInt16();
  _outMessage.accepted = _reader.ReadBool();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const UpdateRequired& _message) noexcept
{
  _writer.WriteUInt64(_message.serverSchemaHash);
  _writer.WriteUInt64(_message.serverContentHash);
}

bool Read(ByteReader& _reader, UpdateRequired& _outMessage) noexcept
{
  _outMessage.serverSchemaHash = _reader.ReadUInt64();
  _outMessage.serverContentHash = _reader.ReadUInt64();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const Refuse& _message) noexcept
{
  _writer.WriteUInt16(static_cast<std::uint16_t>(_message.reason));
}

bool Read(ByteReader& _reader, Refuse& _outMessage) noexcept
{
  _outMessage.reason = static_cast<RefuseReason>(_reader.ReadUInt16());
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const Ping& _message) noexcept
{
  _writer.WriteUInt64(_message.clientSendMicroseconds);
}

bool Read(ByteReader& _reader, Ping& _outMessage) noexcept
{
  _outMessage.clientSendMicroseconds = _reader.ReadUInt64();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const Pong& _message) noexcept
{
  _writer.WriteUInt64(_message.clientSendMicroseconds);
  _writer.WriteUInt32(_message.serverTick);
}

bool Read(ByteReader& _reader, Pong& _outMessage) noexcept
{
  _outMessage.clientSendMicroseconds = _reader.ReadUInt64();
  _outMessage.serverTick = _reader.ReadUInt32();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const OrderAck& _message) noexcept
{
  _writer.WriteUInt32(_message.orderSeq);
  _writer.WriteUInt32(_message.serverOrderId);
  _writer.WriteUInt16(_message.reasonCode);
  _writer.WriteUInt8(_message.accepted);
}

bool Read(ByteReader& _reader, OrderAck& _outMessage) noexcept
{
  _outMessage.orderSeq = _reader.ReadUInt32();
  _outMessage.serverOrderId = _reader.ReadUInt32();
  _outMessage.reasonCode = _reader.ReadUInt16();
  _outMessage.accepted = _reader.ReadUInt8();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const Goodbye& _message) noexcept
{
  _writer.WriteUInt16(_message.reason);
}

bool Read(ByteReader& _reader, Goodbye& _outMessage) noexcept
{
  _outMessage.reason = _reader.ReadUInt16();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const SnapshotAck& _message) noexcept
{
  _writer.WriteUInt16(_message.gridId);
  _writer.WriteUInt32(_message.tick);
}

bool Read(ByteReader& _reader, SnapshotAck& _outMessage) noexcept
{
  _outMessage.gridId = _reader.ReadUInt16();
  _outMessage.tick = _reader.ReadUInt32();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const DeltaHeader& _message) noexcept
{
  _writer.WriteUInt32(_message.tick);
  _writer.WriteUInt32(_message.baselineTick);
  _writer.WriteUInt16(_message.gridId);
  _writer.WriteUInt16(_message.culledCount);
  _writer.WriteUInt8(_message.partIndex);
  _writer.WriteUInt8(_message.partCount);
  _writer.WriteUInt16(_message.recordCount);
}

bool Read(ByteReader& _reader, DeltaHeader& _outMessage) noexcept
{
  _outMessage.tick = _reader.ReadUInt32();
  _outMessage.baselineTick = _reader.ReadUInt32();
  _outMessage.gridId = _reader.ReadUInt16();
  _outMessage.culledCount = _reader.ReadUInt16();
  _outMessage.partIndex = _reader.ReadUInt8();
  _outMessage.partCount = _reader.ReadUInt8();
  _outMessage.recordCount = _reader.ReadUInt16();
  // A header that indexes past its own part count describes a message that
  // cannot exist. Refused here rather than treated as a partial tick, because
  // ADR-022 §2d's whole-tick rule is checked against `partCount` and a nonsense
  // one would make the ack fire on a tick the client does not hold.
  return _reader.Ok() && _outMessage.partCount > 0 && _outMessage.partIndex < _outMessage.partCount;
}

void Write(ByteWriter& _writer, const KeyframeHeader& _message) noexcept
{
  _writer.WriteUInt32(_message.tick);
  _writer.WriteUInt16(_message.gridId);
  _writer.WriteUInt16(_message.culledCount);
  _writer.WriteUInt32(_message.recordCount);
}

bool Read(ByteReader& _reader, KeyframeHeader& _outMessage) noexcept
{
  _outMessage.tick = _reader.ReadUInt32();
  _outMessage.gridId = _reader.ReadUInt16();
  _outMessage.culledCount = _reader.ReadUInt16();
  _outMessage.recordCount = _reader.ReadUInt32();
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const ViewFocus& _message) noexcept
{
  _writer.WriteUInt16(_message.gridId);
  _writer.WriteInt32(_message.focusXCm);
  _writer.WriteInt32(_message.focusYCm);
  _writer.WriteUInt32(_message.halfExtentCm);
  const auto count = static_cast<std::uint16_t>(std::min<std::size_t>(_message.selection.size(), MAX_VIEW_SELECTION));
  _writer.WriteUInt16(count);
  for (std::uint16_t index = 0; index < count; ++index)
  {
    _writer.WriteUInt32(_message.selection[index]);
  }
}

bool Read(ByteReader& _reader, ViewFocus& _outMessage)
{
  _outMessage.gridId = _reader.ReadUInt16();
  _outMessage.focusXCm = _reader.ReadInt32();
  _outMessage.focusYCm = _reader.ReadInt32();
  _outMessage.halfExtentCm = _reader.ReadUInt32();
  const std::uint16_t count = _reader.ReadUInt16();
  if (!_reader.Ok() || count > MAX_VIEW_SELECTION)
  {
    // Refused, not clamped. A truncated selection is a *different* selection,
    // and ADR-022 §5a's guarantee would then be kept for the wrong set of
    // ships -- which is worse than not keeping it, because it would look kept.
    return false;
  }
  _outMessage.selection.clear();
  _outMessage.selection.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index)
  {
    _outMessage.selection.push_back(_reader.ReadUInt32());
  }
  return _reader.Ok();
}

std::uint64_t CoreSchemaHash() noexcept
{
  // Folded in with the protocol version: a framing change and a layout change
  // are both reasons for two builds to refuse each other.
  return HashText(CORE_SCHEMA_TEXT, HashValue(PROTOCOL_VERSION));
}

} // namespace Neuron
