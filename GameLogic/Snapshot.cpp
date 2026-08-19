#include "pch.h"

#include "Snapshot.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Game
{

Neuron::EntityRecord MakeShipRecord(const World& _world, std::uint32_t _slot) noexcept
{
  Neuron::EntityRecord record;
  if (_slot >= _world.ShipCount())
  {
    return record; // Leaves INVALID_ENTITY_ID, which is what an empty slot is.
  }

  const XMFLOAT2& position = _world.Positions()[_slot];
  const XMFLOAT2& velocity = _world.Velocities()[_slot];

  record.id = _world.Ids()[_slot];

  // `typeId` is the hull class and `gaugeA`/`gaugeB` are hull and shield. The
  // engine moves all four and reads none of them (ADR-014 §4).
  record.typeId = _world.Classes()[_slot];
  // The ship's wing, which is what the HUD's roster is a roster *of*. It rides
  // in the neutral `groupId` byte: the engine carries the number and only this
  // library knows the word for it (ADR-014 §4).
  record.groupId = _world.Wings()[_slot];

  record.posXCm = Neuron::MetresToCentimetres(position.x);
  record.posYCm = Neuron::MetresToCentimetres(position.y);

  // Velocity is 16-bit centimetres per second: +/-327 m/s, which is above every
  // hull's top speed with room to spare. Clamped rather than wrapped, because a
  // velocity that wrapped would send a ship backwards on the client for one
  // tick and then correct -- a visible glitch from an invisible cause.
  const std::int32_t velocityX = Neuron::MetresToCentimetres(velocity.x);
  const std::int32_t velocityY = Neuron::MetresToCentimetres(velocity.y);
  constexpr std::int32_t VELOCITY_LIMIT = 32767;
  record.velXCmPerSec = static_cast<std::int16_t>(std::clamp(velocityX, -VELOCITY_LIMIT, VELOCITY_LIMIT));
  record.velYCmPerSec = static_cast<std::int16_t>(std::clamp(velocityY, -VELOCITY_LIMIT, VELOCITY_LIMIT));

  record.headingTurns16 = Neuron::RadiansToHeading(_world.Headings()[_slot]);
  record.gaugeA = _world.Hulls()[_slot];
  record.gaugeB = _world.Shields()[_slot];
  return record;
}

bool WriteSnapshot(const World& _world, Neuron::ByteWriter& _writer)
{
  const std::uint32_t shipCount = _world.ShipCount();
  if (shipCount > MAX_SHIPS_PER_SNAPSHOT)
  {
    // Nothing is written. A truncated snapshot is worse than none: the client
    // would read the missing ships as despawned and resurrect them next tick.
    return false;
  }
  // Orders are truncated where ships are not, and the asymmetry is deliberate:
  // a missing ship record reads as a despawn and is unrecoverable, while a
  // missing order record costs one frame of ETA and the header's
  // `lastOrderSeqProcessed` still promotes the client's ghost.
  const auto orderCount =
      static_cast<std::uint16_t>(std::min<std::size_t>(_world.Groups().size(), MAX_ORDERS_PER_SNAPSHOT));

  /*
   * Which sixteen: the live ones first, then the finished ones with whatever
   * room is left. When the cap bites, the record that gets dropped must be a
   * corpse serving out its linger, never the order the player just gave --
   * a live group with no record is a ghost with no ETA and no promotion,
   * while a `Done` group missing a frame costs a retirement one snapshot of
   * lateness. Selection order, not table order, so nothing here touches the
   * world or the replay.
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

  if (_writer.BytesRemaining() < SnapshotBytes(shipCount, orderCount))
  {
    return false;
  }

  _writer.WriteUInt32(_world.Tick());
  _writer.WriteUInt32(0); // baselineTick: full snapshots only (ADR-004 §6).
  _writer.WriteUInt16(static_cast<std::uint16_t>(shipCount));
  _writer.WriteUInt16(orderCount);
  _writer.WriteUInt32(_world.LastOrderSeqProcessed());

  // Dense-array order, which is the world's only iteration order and therefore
  // the one a replay reproduces.
  for (std::uint32_t slot = 0; slot < shipCount; ++slot)
  {
    Neuron::WriteEntityRecord(_writer, MakeShipRecord(_world, slot));
  }

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

bool ReadSnapshot(Neuron::ByteReader& _reader, SnapshotHeader& _outHeader, std::vector<Neuron::EntityRecord>& _outShips)
{
  std::vector<OrderStateRecord> ignored;
  return ReadSnapshot(_reader, _outHeader, _outShips, ignored);
}

bool ReadSnapshot(Neuron::ByteReader& _reader, SnapshotHeader& _outHeader, std::vector<Neuron::EntityRecord>& _outShips,
                  std::vector<OrderStateRecord>& _outOrders)
{
  SnapshotHeader header;
  header.tick = _reader.ReadUInt32();
  header.baselineTick = _reader.ReadUInt32();
  header.shipCount = _reader.ReadUInt16();
  header.orderCount = _reader.ReadUInt16();
  header.lastOrderSeqProcessed = _reader.ReadUInt32();

  if (!_reader.Ok())
  {
    return false;
  }

  // Checked before allocating: a corrupt or hostile count is a refusal, not a
  // reservation. The cap is what one datagram can carry, so a larger number did
  // not come from a snapshot this build wrote.
  if (header.shipCount > MAX_SHIPS_PER_SNAPSHOT || header.orderCount > MAX_ORDERS_PER_SNAPSHOT)
  {
    return false;
  }

  std::vector<Neuron::EntityRecord> ships;
  ships.reserve(header.shipCount);
  for (std::uint16_t index = 0; index < header.shipCount; ++index)
  {
    ships.push_back(Neuron::ReadEntityRecord(_reader));
  }

  std::vector<OrderStateRecord> orders;
  orders.reserve(header.orderCount);
  for (std::uint16_t index = 0; index < header.orderCount; ++index)
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

  _outHeader = header;
  _outShips = std::move(ships);
  _outOrders = std::move(orders);
  return true;
}

} // namespace Game
