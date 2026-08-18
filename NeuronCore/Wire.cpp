#include "pch.h"

#include "Wire.h"

#include "Hash.h"

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
}

bool Read(ByteReader& _reader, Hello& _outMessage) noexcept
{
  _outMessage.protocolVersion = _reader.ReadUInt16();
  _outMessage.schemaHash = _reader.ReadUInt64();
  _outMessage.contentHash = _reader.ReadUInt64();
  _outMessage.playerName = std::string(_reader.ReadText());
  return _reader.Ok();
}

void Write(ByteWriter& _writer, const Welcome& _message) noexcept
{
  _writer.WriteUInt32(_message.clientId);
  _writer.WriteUInt32(_message.tick);
  _writer.WriteUInt16(_message.tickRate);
  _writer.WriteUInt64(_message.schemaHash);
  _writer.WriteUInt64(_message.contentHash);
}

bool Read(ByteReader& _reader, Welcome& _outMessage) noexcept
{
  _outMessage.clientId = _reader.ReadUInt32();
  _outMessage.tick = _reader.ReadUInt32();
  _outMessage.tickRate = _reader.ReadUInt16();
  _outMessage.schemaHash = _reader.ReadUInt64();
  _outMessage.contentHash = _reader.ReadUInt64();
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

void Write(ByteWriter& _writer, const Goodbye& _message) noexcept
{
  _writer.WriteUInt16(_message.reason);
}

bool Read(ByteReader& _reader, Goodbye& _outMessage) noexcept
{
  _outMessage.reason = _reader.ReadUInt16();
  return _reader.Ok();
}

std::uint64_t CoreSchemaHash() noexcept
{
  // Folded in with the protocol version: a framing change and a layout change
  // are both reasons for two builds to refuse each other.
  return HashText(CoreSchemaText, HashValue(ProtocolVersion));
}

} // namespace Neuron
