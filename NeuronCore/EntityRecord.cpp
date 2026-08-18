#include "pch.h"

#include "EntityRecord.h"

#include <cmath>
#include <limits>
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
  const float rounded = centimetres >= 0.0f ? centimetres + 0.5f : centimetres - 0.5f;

  /*
   * Saturated, not cast straight through.
   *
   * Converting a float outside `int32`'s range is undefined behaviour, and on
   * the machines that do not trap it the value that comes back is wrapped: a
   * target 10,000,000 km east arriving as one somewhere west, which the bounds
   * check would then wave through because the number it sees is small. That is
   * reachable from any client that sends a large coordinate, which makes it a
   * validation hole rather than a rounding curiosity.
   *
   * The bound is the largest float strictly below 2^31; the values between it
   * and INT32_MAX are not representable as floats at all.
   */
  constexpr float LOWEST = -2147483648.0f;
  constexpr float HIGHEST = 2147483520.0f;
  if (!(rounded > LOWEST))
  {
    // NaN lands here too, every comparison against it being false. INT32_MIN is
    // far outside any play area, so a NaN position is refused rather than
    // quietly becoming the origin.
    return std::numeric_limits<std::int32_t>::min();
  }
  if (rounded >= HIGHEST)
  {
    return std::numeric_limits<std::int32_t>::max();
  }
  return static_cast<std::int32_t>(rounded);
}

float CentimetresToMetres(std::int32_t _centimetres) noexcept
{
  return static_cast<float>(_centimetres) * 0.01f;
}

} // namespace Neuron
