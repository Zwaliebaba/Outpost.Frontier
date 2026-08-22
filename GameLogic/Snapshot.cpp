#include "pch.h"

#include "Snapshot.h"

#include "Eta.h"

#include <algorithm>
#include <cmath>

using DirectX::XMFLOAT2;

namespace Game
{

Neuron::EntityRecord MakeShipRecord(const World& _world, std::uint32_t _slot, Relationship _relationship) noexcept
{
  Neuron::EntityRecord record;
  record.id = _world.Ids()[_slot];
  record.typeId = _world.Classes()[_slot];
  record.groupId = _world.Wings()[_slot];

  const XMFLOAT2& position = _world.Positions()[_slot];
  record.posXCm = Neuron::MetresToCentimetres(position.x);
  record.posYCm = Neuron::MetresToCentimetres(position.y);

  const XMFLOAT2& velocity = _world.Velocities()[_slot];
  // Clamped rather than wrapped: a ship past the velocity range would otherwise
  // replicate as travelling backwards, and the client would interpolate one
  // tick and then correct -- a visible glitch from an invisible cause.
  const std::int32_t velocityX = Neuron::MetresToCentimetres(velocity.x);
  const std::int32_t velocityY = Neuron::MetresToCentimetres(velocity.y);
  constexpr std::int32_t VELOCITY_LIMIT = 32767;
  record.velXCmPerSec = static_cast<std::int16_t>(std::clamp(velocityX, -VELOCITY_LIMIT, VELOCITY_LIMIT));
  record.velYCmPerSec = static_cast<std::int16_t>(std::clamp(velocityY, -VELOCITY_LIMIT, VELOCITY_LIMIT));

  record.headingTurns16 = Neuron::RadiansToHeading(_world.Headings()[_slot]);
  record.gaugeA = _world.Hulls()[_slot];
  record.gaugeB = _world.Shields()[_slot];

  /*
   * The status byte (ADR-017 §5, ADR-022 §8b). Two things live in it now.
   *
   * Undock protection is read against the world's *own* tick rather than a tick
   * passed in: the world knows its own and they are the same number, and
   * writing the world's is the one that cannot drift.
   *
   * The relationship is the **viewer's** and cannot come from the world at all,
   * which is why it arrives as a parameter. Bits rather than a field because
   * the client asks "what colour is this hull" every frame and "whose is it"
   * once a session (§8b).
   */
  record.statusBits = _world.IsProtected(record.id, _world.Tick()) ? SHIP_STATUS_PROTECTED : 0;
  record.statusBits = WithRelationship(record.statusBits, _relationship);
  return record;
}

bool WriteTickTail(const World& _world, Neuron::ByteWriter& _writer, std::uint32_t _lastOrderSeqProcessed)
{
  const auto orderCount = static_cast<std::uint16_t>(std::min<std::size_t>(_world.Groups().size(), MAX_ORDERS_PER_SNAPSHOT));

  /*
   * Which sixteen: the live ones first, then the finished ones with whatever
   * room is left. When the cap bites, the record that gets dropped must be a
   * corpse serving out its linger, never the order the player just gave -- a
   * live group with no record is a ghost with no ETA and no promotion, while a
   * `Done` group missing a frame costs a retirement one tick of lateness.
   * Selection order, not table order, so nothing here touches the world or the
   * replay.
   */
  std::uint16_t chosen[MAX_ORDERS_PER_SNAPSHOT] = {};
  std::uint16_t chosenCount = 0;
  for (int pass = 0; pass < 2 && chosenCount < orderCount; ++pass)
  {
    for (std::size_t index = 0; index < _world.Groups().size() && chosenCount < orderCount; ++index)
    {
      const bool done = _world.Groups()[index].state == OrderState::Done;
      if (done == (pass == 1))
      {
        chosen[chosenCount] = static_cast<std::uint16_t>(index);
        ++chosenCount;
      }
    }
  }

  if (_writer.BytesRemaining() < TICK_TAIL_HEADER_BYTES + static_cast<std::size_t>(orderCount) * ORDER_STATE_RECORD_BYTES)
  {
    return false;
  }

  _writer.WriteUInt16(orderCount);
  _writer.WriteUInt32(_lastOrderSeqProcessed);

  for (std::uint16_t index = 0; index < orderCount; ++index)
  {
    const OrderGroup& group = _world.Groups()[chosen[index]];
    _writer.WriteUInt32(group.serverOrderId);
    _writer.WriteUInt32(group.clientOrderSeq);

    /*
     * The authority's ETA, rounded up and clamped (S12).
     *
     * Up rather than to nearest, for the reason the ghost's label rounds up
     * too: `0s` while the ships are visibly still moving reads as a broken
     * readout, and a second of pessimism at the end costs nothing. `NO_ETA`
     * when the game will not say, which is distinct from zero -- zero means
     * *arriving now*.
     */
    const float eta = _world.LegEtaSeconds(group);
    _writer.WriteUInt16(eta < 0.0f ? NO_ETA
                                   : static_cast<std::uint16_t>(std::min(std::ceil(eta), static_cast<float>(NO_ETA - 1))));

    _writer.WriteUInt8(static_cast<std::uint8_t>(group.state));
    _writer.WriteUInt8(group.legIndex);
    _writer.WriteUInt8(group.legCount);
    _writer.WriteUInt8(static_cast<std::uint8_t>(std::min<std::uint16_t>(group.memberCount, 255)));
  }
  return _writer.Ok();
}

bool ReadTickTail(Neuron::ByteReader& _reader, std::uint32_t& _outLastOrderSeqProcessed, std::vector<OrderStateRecord>& _outOrders)
{
  const std::uint16_t orderCount = _reader.ReadUInt16();
  const std::uint32_t lastOrderSeq = _reader.ReadUInt32();
  if (!_reader.Ok())
  {
    return false;
  }

  // Checked before allocating: a corrupt or hostile count is a refusal, not a
  // reservation. The cap is what this build ever writes, so a larger number did
  // not come from a tail this build produced.
  if (orderCount > MAX_ORDERS_PER_SNAPSHOT)
  {
    return false;
  }

  std::vector<OrderStateRecord> orders;
  orders.reserve(orderCount);
  for (std::uint16_t index = 0; index < orderCount; ++index)
  {
    OrderStateRecord record;
    record.serverOrderId = _reader.ReadUInt32();
    record.clientOrderSeq = _reader.ReadUInt32();
    record.etaSeconds = _reader.ReadUInt16();
    record.state = _reader.ReadUInt8();
    record.legIndex = _reader.ReadUInt8();
    record.legCount = _reader.ReadUInt8();
    record.memberCount = _reader.ReadUInt8();
    orders.push_back(record);
  }

  if (!_reader.Ok())
  {
    return false; // Truncated: leave the caller's view untouched.
  }

  _outLastOrderSeqProcessed = lastOrderSeq;
  _outOrders = std::move(orders);
  return true;
}

} // namespace Game
