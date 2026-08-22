#pragma once

#include "ByteReader.h"
#include "ByteWriter.h"

#include <cstdint>

/*
 * The replicated unit of state (ADR-014 §4).
 *
 * Deliberately game-free: an id, a type, a group, a position, a velocity, a
 * heading and two gauges. It names no ship, no order and no formation, so
 * NeuronCore keeps its zero-game-semantics charter while the client can still
 * buffer, interpolate and draw without knowing what any of it means. GameLogic
 * decides what typeId, the group and the gauges stand for; the engine only
 * moves them.
 *
 * Quantisation is the wire contract from ADR-004, and it is what both sides
 * validate against, so a pre-check and the authoritative check cannot disagree
 * on a rounding boundary:
 *   position  centimetres, +-21474 km
 *   velocity  centimetres per second, +-327 m/s
 *   heading   1/65536 of a turn
 *
 * **23 bytes per entity as of U3d-b**, the id having widened to u32
 * (ADR-022 §8a, ADR-018 D6). It was 21, and 41 ships plus a header fit one
 * datagram -- which was exactly the constraint that held the id at u16: a
 * 23-byte record fits 39 per datagram, under ADR-018's 41-ship floor, so the
 * widening was unsafe while one datagram had to hold everything. ADR-022 §5b is
 * what removed that: the per-tick budget is a *bandwidth* figure now, packed
 * into as many datagrams as it takes, and the datagram cap itself never moved.
 */

namespace Neuron
{

/*
 * A replicated entity's identity (ADR-022 §8a, U3d-b).
 *
 * A named alias rather than a bare `std::uint32_t`, and it earns its keep for
 * exactly the reason `Game::Ids.h` gives for *not* using one on its own ids:
 * "a strong id type earns its keep when ids of different kinds get passed to
 * the same function and can be swapped by mistake". That is this case. The
 * client's seams pass entity ids and grid ids side by side, both were
 * `std::uint16_t`, and when the entity id widened the compiler had nothing to
 * say about the dozens of sites that had to widen with it -- which is how a
 * ship id 65,537 would have arrived as ship 1 and been drawn, picked and
 * ordered as somebody else's hull.
 *
 * A plain alias and not a strong type, for the other half of that reasoning:
 * these are array-ish handles read off a wire, and a wrapper would be ceremony
 * at every call site. What is wanted is a name that moves when the width does.
 */
using EntityId = std::uint32_t;

inline constexpr EntityId INVALID_ENTITY_ID = 0xffffffffu;

struct EntityRecord
{
  /*
   * u32 as of U3d-b (ADR-022 §8a, ADR-018 D6).
   *
   * The staging was arithmetic rather than taste, and it is worth keeping the
   * arithmetic here where the next person widening a field will find it: at 23
   * bytes a record fits 39 per datagram, below the 41-ship floor ADR-018 set,
   * so this could not widen while a snapshot had to fit one datagram. It does
   * not any more (§5b), which is why the widening rides in this cluster and not
   * an earlier one.
   */
  EntityId id = INVALID_ENTITY_ID;
  std::uint8_t typeId = 0;

  /*
   * Which group this entity belongs to, or zero for none.
   *
   * Neutral like `typeId`: the engine carries the number and never learns what
   * a group is. This game means a *wing* by it, and the HUD's roster is the
   * reason it replicates -- a roster of groups cannot be built from a stream
   * that does not say which group anything is in.
   *
   * It was `flags`, an always-zero reserved byte, and the rename is the point
   * rather than tidiness: a field called `flags` carrying an id would be read
   * as a bitfield by the next person to touch it, and the first thing they
   * would do is `|=` something into it.
   */
  std::uint8_t groupId = 0;
  std::int32_t posXCm = 0;
  std::int32_t posYCm = 0;
  std::int16_t velXCmPerSec = 0;
  std::int16_t velYCmPerSec = 0;
  std::uint16_t headingTurns16 = 0;
  std::uint8_t gaugeA = 0;
  std::uint8_t gaugeB = 0;

  /*
   * A bitfield, on purpose (ADR-017 §5).
   *
   * Neutral like `typeId` and `groupId`: the engine carries the byte and the
   * game decides what each bit means. Unlike the old always-zero `flags` byte
   * this one *is* meant to be `|=`d into, which is exactly why it is named for
   * what it is rather than reserved under a vaguer word.
   *
   * A byte rather than a widened `typeId` because the other seven bits are
   * already spoken for: in-warp, combat-flagged, and -- per
   * [ADR-022](../Design/ADR/ADR-022-interest-and-delta.md) §8b, **landed with
   * U3d-b** -- two bits of viewer-relative relationship, which is how ownership
   * reaches the client costing no byte at all. An owner id per record would
   * have cost four bytes on every entity every tick to answer a question the
   * player asks once a session.
   *
   * **It cost ships.** `ENTITY_RECORD_BYTES` went 20 to 21 and the snapshot cap
   * fell from 45 to 43. That is recorded here rather than only in the ADR for
   * the same reason `ORDER_STATE_RECORD_BYTES` records its own: the next person
   * who wants a status bit should find the price already on the page.
   */
  std::uint8_t statusBits = 0;
};

inline constexpr std::size_t ENTITY_RECORD_BYTES = 23;
static_assert(ENTITY_RECORD_BYTES == sizeof(EntityRecord::id) + sizeof(EntityRecord::typeId) + sizeof(EntityRecord::groupId) +
                                       sizeof(EntityRecord::posXCm) + sizeof(EntityRecord::posYCm) +
                                       sizeof(EntityRecord::velXCmPerSec) + sizeof(EntityRecord::velYCmPerSec) +
                                       sizeof(EntityRecord::headingTurns16) + sizeof(EntityRecord::gaugeA) +
                                       sizeof(EntityRecord::gaugeB) + sizeof(EntityRecord::statusBits),
              "the record is written field by field; the sum of the fields is the budget, and padding must not enter it");

/*
 * Do two records describe the same state? (ADR-022 §2b.)
 *
 * The whole of delta encoding, in one comparison: a record equal to the one the
 * client was last *sent* is a record that does not need sending again. It is
 * here rather than beside the session host because the fields are this file's,
 * and a comparison written next to the fields cannot fall out of step with them
 * the way a hand-written field list one library away would.
 *
 * Not `memcmp` and not defaulted `operator==` on the struct: the compiler pads
 * this type, and padding is uninitialised memory that would make two identical
 * records compare different at random. Field by field, like the writer.
 */
[[nodiscard]] constexpr bool SameEntityState(const EntityRecord& _lhs, const EntityRecord& _rhs) noexcept
{
  return _lhs.id == _rhs.id && _lhs.typeId == _rhs.typeId && _lhs.groupId == _rhs.groupId && _lhs.posXCm == _rhs.posXCm &&
         _lhs.posYCm == _rhs.posYCm && _lhs.velXCmPerSec == _rhs.velXCmPerSec && _lhs.velYCmPerSec == _rhs.velYCmPerSec &&
         _lhs.headingTurns16 == _rhs.headingTurns16 && _lhs.gaugeA == _rhs.gaugeA && _lhs.gaugeB == _rhs.gaugeB &&
         _lhs.statusBits == _rhs.statusBits;
}

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
