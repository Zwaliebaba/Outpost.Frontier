#include "pch.h"

#include "ReplicatedView.h"

#include "ByteReader.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace DirectX;

namespace Game
{
namespace
{

/// Interpolates an angle the short way round. Headings are on a circle, so the
/// difference has to be wrapped before it is scaled -- lerping the raw numbers
/// spins a ship the long way whenever it crosses pi, which reads as a ship
/// suddenly whipping through a full turn for no reason.
[[nodiscard]] float LerpAngle(float _from, float _to, float _blend) noexcept
{
  return XMScalarModAngle(_from + XMScalarModAngle(_to - _from) * _blend);
}

[[nodiscard]] float Lerp(float _from, float _to, float _blend) noexcept
{
  return _from + (_to - _from) * _blend;
}

/// Dequantises one wire record into the units the client draws in.
[[nodiscard]] ReplicatedShip Dequantise(const Neuron::EntityRecord& _record) noexcept
{
  ReplicatedShip ship;
  ship.id = _record.id;
  ship.classId = _record.typeId;
  ship.positionMetres = XMFLOAT2{Neuron::CentimetresToMetres(_record.posXCm), Neuron::CentimetresToMetres(_record.posYCm)};
  ship.velocityMetresPerSec =
      XMFLOAT2{Neuron::CentimetresToMetres(_record.velXCmPerSec), Neuron::CentimetresToMetres(_record.velYCmPerSec)};
  ship.headingRadians = Neuron::HeadingToRadians(_record.headingTurns16);
  ship.hullGauge = _record.gaugeA;
  ship.shieldGauge = _record.gaugeB;
  ship.wing = _record.groupId;
  ship.statusBits = _record.statusBits;
  return ship;
}

/// Finds a ship by id in a snapshot. Linear, and deliberately so: snapshots are
/// written in dense-array order and the two being matched are one tick apart, so
/// the ids line up and this exits on the first comparison almost every time. A
/// map would cost an allocation per frame to avoid a scan that rarely happens.
[[nodiscard]] const Neuron::EntityRecord* FindShip(const std::vector<Neuron::EntityRecord>& _ships, ShipId _id,
                                                   std::size_t _hint) noexcept
{
  if (_hint < _ships.size() && _ships[_hint].id == _id)
  {
    return &_ships[_hint];
  }
  for (const Neuron::EntityRecord& ship : _ships)
  {
    if (ship.id == _id)
    {
      return &ship;
    }
  }
  return nullptr;
}

} // namespace

bool ReplicatedView::ApplySnapshot(std::span<const std::uint8_t> _payload)
{
  Neuron::ByteReader reader{_payload};

  SnapshotHeader header;
  std::vector<Neuron::EntityRecord> ships;
  std::vector<OrderStateRecord> orders;
  if (!ReadSnapshot(reader, header, ships, orders))
  {
    return false;
  }

  /*
   * **The smear guard** (U3b). A snapshot from a different grid is not a later
   * frame of this one -- it is a different world, and the two share an id space
   * without sharing any ships.
   *
   * So the history is dropped rather than extended. Interpolating across the
   * switch would walk every hull from wherever it stood on the old grid to
   * wherever a ship of the same id happens to stand on the new one, which reads
   * as the fleet sliding across the gap between two worlds. The alternative --
   * matching ids across the boundary and hoping -- is worse, because it is the
   * same wrong answer without the visible symptom.
   *
   * Checked **before** the staleness test on purpose: the new grid's tick can
   * be anything relative to the old one's (ADR-019 §2 makes the tick
   * shard-global, so it is in fact the same clock, but a client must not depend
   * on that to avoid rendering nonsense). A frame from elsewhere is never stale;
   * it is a fresh start.
   */
  if (m_count > 0 && header.gridAnchor != m_frames[0].header.gridAnchor)
  {
    m_count = 0;
    m_latestOrders.clear();
  }

  // Out-of-order arrival is normal on an unordered channel (ADR-003), and a
  // stale snapshot is not an error -- it is simply already superseded. Dropping
  // it beats letting it overwrite newer state.
  if (m_count > 0 && header.tick <= m_frames[0].header.tick)
  {
    return true;
  }

  for (std::size_t i = std::min(m_count, HISTORY - 1); i > 0; --i)
  {
    m_frames[i] = std::move(m_frames[i - 1]);
  }
  m_frames[0].header = header;
  m_frames[0].ships = std::move(ships);
  m_count = std::min(m_count + 1, HISTORY);

  // Replaced wholesale, and only past the staleness check above: an order area
  // that arrived out of order describes a tick the view has already left.
  m_latestOrders = std::move(orders);
  return true;
}

void ReplicatedView::SampleAt(double _renderTick, std::vector<ReplicatedShip>& _outShips) const
{
  _outShips.clear();
  if (m_count == 0)
  {
    return;
  }

  // Find the newest frame at or before the render tick, and the one after it.
  // Frames are newest-first, so this walks forward in time.
  const Frame* older = nullptr;
  const Frame* newer = nullptr;
  for (std::size_t i = 0; i < m_count; ++i)
  {
    if (static_cast<double>(m_frames[i].header.tick) <= _renderTick)
    {
      older = &m_frames[i];
      newer = i > 0 ? &m_frames[i - 1] : nullptr;
      break;
    }
  }

  if (older == nullptr)
  {
    // The render tick is behind everything held -- the client is still filling
    // its buffer after a join. Show the oldest thing known rather than nothing:
    // a fleet that appears late is better than a frame that is empty.
    older = &m_frames[m_count - 1];
    newer = nullptr;
  }

  if (newer != nullptr)
  {
    const double span = static_cast<double>(newer->header.tick) - static_cast<double>(older->header.tick);
    const float blend = span > 0.0 ? static_cast<float>(std::clamp((_renderTick - older->header.tick) / span, 0.0, 1.0)) : 0.0f;
    const auto spanSeconds = static_cast<float>(span * World::TICK_SECONDS);

    _outShips.reserve(older->ships.size());
    for (std::size_t index = 0; index < older->ships.size(); ++index)
    {
      const Neuron::EntityRecord& from = older->ships[index];
      const Neuron::EntityRecord* to = FindShip(newer->ships, from.id, index);
      if (to == nullptr)
      {
        // Gone in the newer snapshot: the ship despawned somewhere in this
        // interval. Dropping it now rather than fading it out is the honest
        // reading -- the server has already stopped simulating it.
        continue;
      }

      ReplicatedShip ship = Dequantise(from);
      const ReplicatedShip target = Dequantise(*to);
      ship.positionMetres = XMFLOAT2{Lerp(ship.positionMetres.x, target.positionMetres.x, blend),
                                     Lerp(ship.positionMetres.y, target.positionMetres.y, blend)};
      ship.velocityMetresPerSec = XMFLOAT2{Lerp(ship.velocityMetresPerSec.x, target.velocityMetresPerSec.x, blend),
                                           Lerp(ship.velocityMetresPerSec.y, target.velocityMetresPerSec.y, blend)};
      const float fromHeading = ship.headingRadians;
      ship.headingRadians = LerpAngle(fromHeading, target.headingRadians, blend);
      // The observed turn, for the cosmetic bank: shortest-arc, like the lerp
      // above, or a ship crossing pi would report a full turn's worth of rate.
      if (spanSeconds > 0.0f)
      {
        ship.headingRateRadiansPerSec = XMScalarModAngle(target.headingRadians - fromHeading) / spanSeconds;
      }
      ship.hullGauge = blend < 0.5f ? ship.hullGauge : target.hullGauge;
      ship.shieldGauge = blend < 0.5f ? ship.shieldGauge : target.shieldGauge;
      // Same rule as the gauges, and for a stronger reason: a bit has no
      // halfway. The shimmer turns on at the tick the authority says it did.
      ship.statusBits = blend < 0.5f ? ship.statusBits : target.statusBits;
      _outShips.push_back(ship);
    }
    return;
  }

  /*
   * Nothing newer to blend toward: the stream has a gap, or the render tick has
   * run past the newest snapshot. Carry each ship forward on its last known
   * velocity, and stop doing so after the cap.
   *
   * The freeze matters more than the extrapolation. A ship still gliding on
   * five-second-old velocity looks entirely correct and is entirely wrong -- it
   * ends up somewhere the server never put it, and the correction when the
   * stream returns is a jump. Frozen and flagged is a visible, honest failure.
   */
  const double aheadTicks = std::max(0.0, _renderTick - static_cast<double>(older->header.tick));
  const bool stale = aheadTicks > MAX_EXTRAPOLATION_TICKS;
  const auto carried = static_cast<float>(std::min(aheadTicks, MAX_EXTRAPOLATION_TICKS) * World::TICK_SECONDS);

  _outShips.reserve(older->ships.size());
  for (const Neuron::EntityRecord& record : older->ships)
  {
    ReplicatedShip ship = Dequantise(record);
    ship.positionMetres = XMFLOAT2{ship.positionMetres.x + ship.velocityMetresPerSec.x * carried,
                                   ship.positionMetres.y + ship.velocityMetresPerSec.y * carried};
    ship.stale = stale;
    _outShips.push_back(ship);
  }
}

std::uint32_t ReplicatedView::LatestTick() const noexcept
{
  return m_count > 0 ? m_frames[0].header.tick : 0;
}

std::uint32_t ReplicatedView::OldestTick() const noexcept
{
  return m_count > 0 ? m_frames[m_count - 1].header.tick : 0;
}

std::uint16_t ReplicatedView::LatestShipCount() const noexcept
{
  return m_count > 0 ? m_frames[0].header.shipCount : 0;
}

std::span<const OrderStateRecord> ReplicatedView::LatestOrders() const noexcept
{
  return m_latestOrders;
}

std::uint32_t ReplicatedView::LastOrderSeqProcessed() const noexcept
{
  return m_count > 0 ? m_frames[0].header.lastOrderSeqProcessed : 0;
}

void ReplicatedView::Clear() noexcept
{
  for (Frame& frame : m_frames)
  {
    frame.header = SnapshotHeader{};
    frame.ships.clear();
  }
  m_count = 0;
  m_latestOrders.clear();
}

} // namespace Game
