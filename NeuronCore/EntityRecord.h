#pragma once

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstdint>

/*
 * The replicated unit of state (ADR-014 §4).
 *
 * Deliberately game-free: an id, a type, a position, a velocity, a heading and
 * two gauges. It names no ship, no order and no formation, so NeuronCore keeps
 * its zero-game-semantics charter while the client can still buffer,
 * interpolate and draw without knowing what any of it means. GameLogic decides
 * what typeId and the gauges stand for; the engine only moves them.
 *
 * Quantisation is the wire contract from ADR-004, and it is what both sides
 * validate against, so a pre-check and the authoritative check cannot disagree
 * on a rounding boundary:
 *   position  centimetres, +-21474 km
 *   velocity  centimetres per second, +-327 m/s
 *   heading   1/65536 of a turn
 * 20 bytes per entity: 41 ships and a header fit one datagram, and the 1024-cap
 * case is what forces delta encoding later rather than a bigger packet.
 */

namespace Neuron
{

inline constexpr std::uint16_t InvalidEntityId = 0xffffu;

struct EntityRecord
{
  std::uint16_t id = InvalidEntityId;
  std::uint8_t typeId = 0;
  std::uint8_t flags = 0;
  std::int32_t posXCm = 0;
  std::int32_t posYCm = 0;
  std::int16_t velXCmPerSec = 0;
  std::int16_t velYCmPerSec = 0;
  std::uint16_t headingTurns16 = 0;
  std::uint8_t gaugeA = 0;
  std::uint8_t gaugeB = 0;
};

inline constexpr std::size_t EntityRecordBytes = 20;

void WriteEntityRecord(ByteWriter& _writer, const EntityRecord& _record) noexcept;
[[nodiscard]] EntityRecord ReadEntityRecord(ByteReader& _reader) noexcept;

/// Turns16 to radians, for presentation. The sim keeps its own float heading.
[[nodiscard]] float HeadingToRadians(std::uint16_t _headingTurns16) noexcept;
[[nodiscard]] std::uint16_t RadiansToHeading(float _radians) noexcept;

/// Metres to the wire's centimetres, and back. Rounds half away from zero so the
/// mapping is symmetric about the origin -- truncation would bias every position
/// toward it, and a formation solved on both sides would drift apart.
[[nodiscard]] std::int32_t MetresToCentimetres(float _metres) noexcept;
[[nodiscard]] float CentimetresToMetres(std::int32_t _centimetres) noexcept;

} // namespace Neuron
