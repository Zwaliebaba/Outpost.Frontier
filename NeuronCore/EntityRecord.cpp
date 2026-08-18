#include "pch.h"

#include "EntityRecord.h"

#include <cmath>
#include <numbers>

namespace Neuron
{
namespace
{

constexpr float TURNS_TO_RADIANS = 2.0f * std::numbers::pi_v<float> / 65536.0f;
constexpr float RADIANS_TO_TURNS = 65536.0f / (2.0f * std::numbers::pi_v<float>);

} // namespace

void WriteEntityRecord(ByteWriter& _writer, const EntityRecord& _record) noexcept
{
  _writer.WriteUInt16(_record.id);
  _writer.WriteUInt8(_record.typeId);
  _writer.WriteUInt8(_record.flags);
  _writer.WriteInt32(_record.posXCm);
  _writer.WriteInt32(_record.posYCm);
  _writer.WriteInt16(_record.velXCmPerSec);
  _writer.WriteInt16(_record.velYCmPerSec);
  _writer.WriteUInt16(_record.headingTurns16);
  _writer.WriteUInt8(_record.gaugeA);
  _writer.WriteUInt8(_record.gaugeB);
}

EntityRecord ReadEntityRecord(ByteReader& _reader) noexcept
{
  EntityRecord record{};
  record.id = _reader.ReadUInt16();
  record.typeId = _reader.ReadUInt8();
  record.flags = _reader.ReadUInt8();
  record.posXCm = _reader.ReadInt32();
  record.posYCm = _reader.ReadInt32();
  record.velXCmPerSec = _reader.ReadInt16();
  record.velYCmPerSec = _reader.ReadInt16();
  record.headingTurns16 = _reader.ReadUInt16();
  record.gaugeA = _reader.ReadUInt8();
  record.gaugeB = _reader.ReadUInt8();
  return record;
}

float HeadingToRadians(std::uint16_t _headingTurns16) noexcept
{
  return static_cast<float>(_headingTurns16) * TURNS_TO_RADIANS;
}

std::uint16_t RadiansToHeading(float _radians) noexcept
{
  const float turns = _radians * RADIANS_TO_TURNS;
  // fmod keeps the value in range before the cast; a heading of -pi and one of
  // +pi must land on the same quantised value.
  float wrapped = std::fmod(turns, 65536.0f);
  if (wrapped < 0.0f)
  {
    wrapped += 65536.0f;
  }
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(wrapped + 0.5f) & 0xffffu);
}

std::int32_t MetresToCentimetres(float _metres) noexcept
{
  const float centimetres = _metres * 100.0f;
  return static_cast<std::int32_t>(centimetres >= 0.0f ? centimetres + 0.5f : centimetres - 0.5f);
}

float CentimetresToMetres(std::int32_t _centimetres) noexcept
{
  return static_cast<float>(_centimetres) * 0.01f;
}

} // namespace Neuron
