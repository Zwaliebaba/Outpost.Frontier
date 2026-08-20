#include "pch.h"

#include "Universe.h"

#include "Hash.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace Game
{
namespace
{

using Neuron::HashText;
using Neuron::HashValue;

/// Folds a string in length-prefixed, so "ab" + "c" and "a" + "bc" cannot
/// produce the same running hash.
[[nodiscard]] std::uint64_t HashName(const std::string& _name, std::uint64_t _seed) noexcept
{
  const std::uint64_t withLength = HashValue(static_cast<std::uint32_t>(_name.size()), _seed);
  return HashText(_name, withLength);
}

[[nodiscard]] std::uint64_t HashPosition(const UniversePos& _position, std::uint64_t _seed) noexcept
{
  return HashValue(_position.y, HashValue(_position.x, _seed));
}

/// Indices into _items, ordered by id. Hashing walks this rather than file
/// order, which is what makes reordering an array a non-change (ADR-012 §D).
template <typename T> [[nodiscard]] std::vector<std::size_t> OrderById(const std::vector<T>& _items)
{
  std::vector<std::size_t> order(_items.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&_items](std::size_t _a, std::size_t _b) { return _items[_a].id < _items[_b].id; });
  return order;
}

/// One-sided bounds that cannot overflow. Forming `_anchor + _limit` directly
/// would be undefined for an anchor near the end of the plane -- which is
/// exactly the far-apart case this function exists to reject.
[[nodiscard]] bool WithinLimit(std::int64_t _anchor, std::int64_t _value, std::int64_t _limit) noexcept
{
  const std::int64_t low = _anchor < std::numeric_limits<std::int64_t>::min() + _limit ? std::numeric_limits<std::int64_t>::min()
                                                                                      : _anchor - _limit;
  const std::int64_t high = _anchor > std::numeric_limits<std::int64_t>::max() - _limit ? std::numeric_limits<std::int64_t>::max()
                                                                                       : _anchor + _limit;
  return _value >= low && _value <= high;
}

} // namespace

const SolarSystem* UniverseDef::FindSystem(SystemId _id) const noexcept
{
  for (const SolarSystem& system : systems)
  {
    if (system.id == _id)
    {
      return &system;
    }
  }
  return nullptr;
}

const Station* UniverseDef::FindStation(SystemId _system, StationId _station) const noexcept
{
  const SolarSystem* system = FindSystem(_system);
  if (system == nullptr)
  {
    return nullptr;
  }
  for (const Station& station : system->stations)
  {
    if (station.id == _station)
    {
      return &station;
    }
  }
  return nullptr;
}

const Anchor* UniverseDef::FindAnchor(AnchorId _id) const noexcept
{
  // Linear across systems, then across their anchors. The universe is read at
  // boot and warp orders are rare; an index would be a second thing to keep
  // true for a lookup nobody measures.
  for (const SolarSystem& system : systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.id == _id)
      {
        return &anchor;
      }
    }
  }
  return nullptr;
}

AnchorId UniverseDef::PairedGateAnchor(AnchorId _gateAnchor) const noexcept
{
  const Anchor* here = FindAnchor(_gateAnchor);
  if (here == nullptr || here->kind != AnchorKind::Gate)
  {
    return INVALID_ID;
  }

  // The gate this anchor belongs to, and the system it leads to. `owner` is a
  // `GateId` because the anchor's kind says so -- the id spaces are per-system
  // and independent, which is the whole reason `owner` is scoped by kind.
  const SolarSystem* system = FindSystem(here->system);
  if (system == nullptr)
  {
    return INVALID_ID;
  }
  const Gate* gate = nullptr;
  for (const Gate& candidate : system->gates)
  {
    if (candidate.id == here->owner)
    {
      gate = &candidate;
      break;
    }
  }
  if (gate == nullptr)
  {
    return INVALID_ID;
  }

  /*
   * The far side is the gate that leads back, and it is unique: the bake emits
   * one record per system per edge and never links a pair twice, so "the gate
   * in `toSystem` whose `toSystem` is this system" names exactly one. Matching
   * on the return edge rather than on a stored pair id is what keeps a gate
   * from having to carry a number that could disagree with the topology it is
   * part of.
   */
  const SolarSystem* far = FindSystem(gate->toSystem);
  if (far == nullptr)
  {
    return INVALID_ID;
  }
  for (const Gate& candidate : far->gates)
  {
    if (candidate.toSystem != system->id)
    {
      continue;
    }
    for (const Anchor& anchor : far->anchors)
    {
      if (anchor.kind == AnchorKind::Gate && anchor.owner == candidate.id)
      {
        return anchor.id;
      }
    }
  }
  return INVALID_ID;
}

GridAnchor UniverseDef::StartAnchor() const noexcept
{
  const Station* station = FindStation(start.system, start.station);
  if (station == nullptr)
  {
    return GridAnchor{};
  }
  return GridAnchor{start.system, station->position};
}

AnchorId UniverseDef::StartAnchorId() const noexcept
{
  const SolarSystem* system = FindSystem(start.system);
  if (system == nullptr)
  {
    return INVALID_ID;
  }
  for (const Anchor& anchor : system->anchors)
  {
    if (anchor.kind == AnchorKind::Station && anchor.owner == start.station)
    {
      return anchor.id;
    }
  }
  return INVALID_ID;
}

std::uint64_t ComputeUniverseHash(const UniverseDef& _universe)
{
  std::uint64_t hash = HashName(_universe.name, Neuron::FNV_OFFSET_BASIS_64);

  hash = HashValue(static_cast<std::uint32_t>(_universe.regions.size()), hash);
  for (const std::size_t index : OrderById(_universe.regions))
  {
    const Region& region = _universe.regions[index];
    hash = HashValue(region.id, hash);
    hash = HashName(region.name, hash);
    hash = HashValue(region.securityFloor, hash);
    hash = HashValue(region.securityCeiling, hash);
  }

  hash = HashValue(static_cast<std::uint32_t>(_universe.constellations.size()), hash);
  for (const std::size_t index : OrderById(_universe.constellations))
  {
    const Constellation& constellation = _universe.constellations[index];
    hash = HashValue(constellation.id, hash);
    hash = HashValue(constellation.region, hash);
    hash = HashName(constellation.name, hash);
    hash = HashPosition(constellation.centre, hash);
  }

  hash = HashValue(static_cast<std::uint32_t>(_universe.systems.size()), hash);
  for (const std::size_t systemIndex : OrderById(_universe.systems))
  {
    const SolarSystem& system = _universe.systems[systemIndex];
    hash = HashValue(system.id, hash);
    hash = HashValue(system.region, hash);
    hash = HashValue(system.constellation, hash);
    hash = HashName(system.name, hash);
    hash = HashValue(system.security, hash);
    hash = HashValue(static_cast<std::uint8_t>(system.starter ? 1 : 0), hash);
    hash = HashPosition(system.centre, hash);

    hash = HashValue(static_cast<std::uint32_t>(system.celestials.size()), hash);
    for (const std::size_t index : OrderById(system.celestials))
    {
      const Celestial& celestial = system.celestials[index];
      hash = HashValue(celestial.id, hash);
      hash = HashValue(static_cast<std::uint8_t>(celestial.kind), hash);
      hash = HashName(celestial.name, hash);
      hash = HashPosition(celestial.position, hash);
      hash = HashValue(celestial.radiusMetres, hash);
    }

    hash = HashValue(static_cast<std::uint32_t>(system.stations.size()), hash);
    for (const std::size_t index : OrderById(system.stations))
    {
      const Station& station = system.stations[index];
      hash = HashValue(station.id, hash);
      hash = HashName(station.name, hash);
      hash = HashPosition(station.position, hash);
    }

    hash = HashValue(static_cast<std::uint32_t>(system.gates.size()), hash);
    for (const std::size_t index : OrderById(system.gates))
    {
      const Gate& gate = system.gates[index];
      hash = HashValue(gate.id, hash);
      hash = HashValue(gate.toSystem, hash);
      hash = HashName(gate.name, hash);
      hash = HashPosition(gate.position, hash);
    }

    /*
     * Anchors are content, and the most load-bearing content there is: a warp
     * order names one, so two halves disagreeing about where an anchor's grid
     * sits is two halves flying to different places. They fold here rather than
     * being left out as "derived", because they are not derived -- the bake
     * chose them.
     */
    hash = HashValue(static_cast<std::uint32_t>(system.anchors.size()), hash);
    for (const std::size_t index : OrderById(system.anchors))
    {
      const Anchor& anchor = system.anchors[index];
      hash = HashValue(anchor.id, hash);
      hash = HashValue(static_cast<std::uint8_t>(anchor.kind), hash);
      hash = HashValue(anchor.owner, hash);
      hash = HashPosition(anchor.origin, hash);
      hash = HashValue(anchor.warpInPoint.x, hash);
      hash = HashValue(anchor.warpInPoint.y, hash);
      hash = HashValue(anchor.warpInFacingTurns16, hash);
      hash = HashValue(anchor.arrivalSpreadRadiusCm, hash);
      hash = HashValue(anchor.undockPoint.x, hash);
      hash = HashValue(anchor.undockPoint.y, hash);
      hash = HashValue(anchor.undockFacingTurns16, hash);
      hash = HashValue(anchor.occupantIdBase, hash);
      hash = HashValue(anchor.occupantCount, hash);

      // The site block, folded unconditionally rather than only for sites: it
      // is zeroed on every other kind, so hashing it costs nothing and cannot
      // be forgotten the day a fourth kind carries one. A pool that changed
      // without moving `universeHash` would be two halves quietly disagreeing
      // about how much ore a field holds.
      hash = HashValue(static_cast<std::uint8_t>(anchor.site.archetype), hash);
      hash = HashValue(anchor.site.grade, hash);
      hash = HashValue(anchor.site.orbitRingRadiusMetres, hash);
      hash = HashValue(anchor.site.fieldRadiusCm, hash);
      hash = HashValue(anchor.site.layoutSeed, hash);
      for (const std::uint32_t pool : anchor.site.poolUnits)
      {
        hash = HashValue(pool, hash);
      }
    }
  }

  hash = HashValue(_universe.start.system, hash);
  hash = HashValue(_universe.start.station, hash);
  return hash;
}

bool LocalFromUniverse(UniversePos _anchor, UniversePos _position, LocalOffsetCm& _outLocal) noexcept
{
  if (!WithinLimit(_anchor.x, _position.x, GRID_HALF_EXTENT_METRES) ||
      !WithinLimit(_anchor.y, _position.y, GRID_HALF_EXTENT_METRES))
  {
    return false;
  }

  // Both differences are now known to be within +/-20,000, so neither the
  // subtraction nor the centimetre scaling can overflow.
  _outLocal.x = static_cast<std::int32_t>((_position.x - _anchor.x) * 100);
  _outLocal.y = static_cast<std::int32_t>((_position.y - _anchor.y) * 100);
  return true;
}

UniversePos UniverseFromLocal(UniversePos _anchor, LocalOffsetCm _local) noexcept
{
  return UniversePos{_anchor.x + _local.x / 100, _anchor.y + _local.y / 100};
}

DirectX::XMFLOAT2 LocalMetresFromUniverse(UniversePos _anchor, UniversePos _position) noexcept
{
  LocalOffsetCm local;
  if (!LocalFromUniverse(_anchor, _position, local))
  {
    return DirectX::XMFLOAT2{0.0f, 0.0f};
  }
  return DirectX::XMFLOAT2{static_cast<float>(local.x) * 0.01f, static_cast<float>(local.y) * 0.01f};
}

const char* CelestialKindName(CelestialKind _kind) noexcept
{
  switch (_kind)
  {
  case CelestialKind::Star:
    return "star";
  case CelestialKind::Planet:
    return "planet";
  case CelestialKind::Moon:
    return "moon";
  }
  return "?";
}

bool ParseCelestialKindName(std::string_view _name, CelestialKind& _outKind) noexcept
{
  if (_name == "star")
  {
    _outKind = CelestialKind::Star;
    return true;
  }
  if (_name == "planet")
  {
    _outKind = CelestialKind::Planet;
    return true;
  }
  if (_name == "moon")
  {
    _outKind = CelestialKind::Moon;
    return true;
  }
  return false;
}

} // namespace Game
