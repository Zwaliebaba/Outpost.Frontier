#include "pch.h"

#include "WorldRegistry.h"

#include "ShipClass.h"
#include "UniverseGen.h"
#include "WorldHash.h"

#include "Hash.h"

#include <algorithm>
#include <cstdint>

namespace Game
{
namespace
{

/*
 * A world's seed, from (session seed, anchor id) -- ADR-016 §4.
 *
 * Mixed rather than added so that adjacent anchors do not get adjacent
 * streams: PCG32 decorrelates its output well, but two worlds seeded one apart
 * would still be a thing somebody eventually noticed and could not explain.
 */
[[nodiscard]] std::uint64_t SeedFor(std::uint64_t _sessionSeed, AnchorId _anchor) noexcept
{
  std::uint64_t seed = _sessionSeed ^ (static_cast<std::uint64_t>(_anchor) * 0x9e3779b97f4a7c15ull);
  seed ^= seed >> 30;
  seed *= 0xbf58476d1ce4e5b9ull;
  seed ^= seed >> 27;
  seed *= 0x94d049bb133111ebull;
  seed ^= seed >> 31;
  return seed;
}

/// The hull an anchor's authored occupant wears. Only stations author anything
/// today; gates get theirs when U4 gives them an entity.
[[nodiscard]] bool AuthoredHull(AnchorKind _kind, HullClass& _outHull) noexcept
{
  if (_kind == AnchorKind::Station)
  {
    _outHull = HullClass::Structure;
    return true;
  }
  return false;
}

} // namespace

void WorldRegistry::Reset(const UniverseDef* _universe, const RegistryConfig& _config)
{
  m_universe = _universe;
  m_config = _config;
  m_shardTick = 0;
  m_live.clear();
  m_locationByShip.clear();

  /*
   * The dynamic id space is this host's to issue from (ADR-019 §5c): two hosts
   * must never mint the same id, and the mechanism is that each issues out of a
   * disjoint block. There is one host, so its block is the whole space.
   *
   * No `hostId * blockSize` here, deliberately. The whole dynamic space is
   * ~32k ids -- the u16 wire window D6 keeps until the delta cluster widens
   * `EntityRecord.id` -- so partitioning it now would spend a scarce budget on
   * hosts that do not exist, against a block size nobody has measured. T2 is
   * where a second host makes this arithmetic, and where the record is already
   * wide enough to afford it.
   */
  m_nextDynamicId = DYNAMIC_SHIP_ID_BASE;
}

HostId WorldRegistry::HostForAnchor(AnchorId _anchor) noexcept
{
  (void)_anchor;
  return 0; // ADR-019 §3: placement is per-anchor, and there is one host.
}

WorldRegistry::LiveWorld* WorldRegistry::Find(AnchorId _anchor) noexcept
{
  const auto found = std::lower_bound(m_live.begin(), m_live.end(), _anchor,
                                      [](const LiveWorld& _entry, AnchorId _id) { return _entry.anchor < _id; });
  return found != m_live.end() && found->anchor == _anchor ? &*found : nullptr;
}

const WorldRegistry::LiveWorld* WorldRegistry::Find(AnchorId _anchor) const noexcept
{
  const auto found = std::lower_bound(m_live.begin(), m_live.end(), _anchor,
                                      [](const LiveWorld& _entry, AnchorId _id) { return _entry.anchor < _id; });
  return found != m_live.end() && found->anchor == _anchor ? &*found : nullptr;
}

const World* WorldRegistry::Peek(AnchorId _anchor) const noexcept
{
  const LiveWorld* entry = Find(_anchor);
  return entry == nullptr ? nullptr : entry->world.get();
}

WorldRegistry::LiveWorld& WorldRegistry::SpinUp(const Anchor& _anchor)
{
  LiveWorld entry;
  entry.anchor = _anchor.id;
  entry.world = std::make_unique<World>();
  entry.spunUpAtTick = m_shardTick;
  entry.world->Reset(SeedFor(m_config.sessionSeed, _anchor.id));

  /*
   * The authored occupants, with the ids the bake derived from this anchor
   * (ADR-018 D6a). This is what makes teardown and recreate reproduce a world
   * exactly: the structure that comes back is the same *ship* as the one that
   * went away, not a new one that looks like it.
   *
   * At the grid's centre, because the anchor's origin *is* the structure's
   * universe position -- the grid is anchored on it.
   */
  HullClass hull = HullClass::Structure;
  if (AuthoredHull(_anchor.kind, hull))
  {
    for (std::uint16_t index = 0; index < _anchor.occupantCount; ++index)
    {
      ShipSpawn spawn;
      spawn.hullClass = hull;
      spawn.wing = INVALID_WING_ID;
      spawn.xMetres = 0.0f;
      spawn.yMetres = 0.0f;
      const auto id = static_cast<ShipId>(_anchor.occupantIdBase + index);
      if (entry.world->Spawn(spawn, id) != INVALID_SHIP_ID)
      {
        ++entry.authoredCount;
        if (id >= m_locationByShip.size())
        {
          m_locationByShip.resize(static_cast<std::size_t>(id) + 1, INVALID_ID);
        }
        m_locationByShip[id] = _anchor.id;
      }
    }
  }

  const auto at = std::lower_bound(m_live.begin(), m_live.end(), _anchor.id,
                                   [](const LiveWorld& _entry, AnchorId _id) { return _entry.anchor < _id; });
  return *m_live.insert(at, std::move(entry));
}

World* WorldRegistry::Borrow(AnchorId _anchor)
{
  if (LiveWorld* entry = Find(_anchor); entry != nullptr)
  {
    return entry->world.get();
  }
  if (m_universe == nullptr)
  {
    return nullptr;
  }
  const Anchor* anchor = m_universe->FindAnchor(_anchor);
  if (anchor == nullptr)
  {
    return nullptr; // Warping to somewhere nobody authored is a refusal, not a world.
  }
  return SpinUp(*anchor).world.get();
}

void WorldRegistry::AddViewer(AnchorId _anchor)
{
  if (Borrow(_anchor) != nullptr)
  {
    if (LiveWorld* entry = Find(_anchor); entry != nullptr)
    {
      ++entry->viewers;
    }
  }
}

void WorldRegistry::RemoveViewer(AnchorId _anchor)
{
  if (LiveWorld* entry = Find(_anchor); entry != nullptr && entry->viewers > 0)
  {
    --entry->viewers;
  }
}

void WorldRegistry::Tick(std::uint32_t _shardTick)
{
  m_shardTick = _shardTick;

  /*
   * Every live world, with the same number, sharing nothing.
   *
   * There is no communication between these calls and there must not be: the
   * transfer bus (T1) applies *between* ticks precisely so that a world's tick
   * can never read another's mid-flight. That independence is what makes
   * world-level fan-out the pre-approved first parallel consumer (ADR-018 D1a),
   * and it is why the suite ticks the same worlds in a permuted order and
   * demands the same hash.
   */
  for (LiveWorld& entry : m_live)
  {
    entry.world->Tick(m_shardTick);
  }

  TearDownIdle();
}

void WorldRegistry::TearDownIdle()
{
  /*
   * A world goes when the last ship leaves and nobody is watching (ADR-016 §7,
   * ADR-018 D8). Authored occupants do not count as ships for this: a station
   * grid with nothing but its station on it is an empty grid, and keeping it
   * live would mean 3,356 worlds ticking forever.
   *
   * **Worlds forget.** Nothing is saved on the way out. Durable state lives at
   * the universe layer -- rosters are the precedent -- so a world that comes
   * back is rebuilt from content, not restored from a memory of itself.
   */
  m_live.erase(std::remove_if(m_live.begin(), m_live.end(),
                              [this](LiveWorld& _entry)
                              {
                                const bool empty = _entry.world->ShipCount() <= _entry.authoredCount;
                                if (!empty || _entry.viewers > 0)
                                {
                                  return false;
                                }
                                for (const ShipId id : _entry.world->Ids())
                                {
                                  if (id < m_locationByShip.size())
                                  {
                                    m_locationByShip[id] = INVALID_ID;
                                  }
                                }
                                return true;
                              }),
               m_live.end());
}

ShipId WorldRegistry::AllocateShipId()
{
  // The u16 window D6 keeps until the delta cluster widens the record. An
  // assert rather than a cast, as D6 says: a wrapped id would put two ships on
  // one number and the wire would never know.
  if (m_nextDynamicId >= INVALID_SHIP_ID)
  {
    return INVALID_SHIP_ID;
  }
  return static_cast<ShipId>(m_nextDynamicId++);
}

bool WorldRegistry::LocationOf(ShipId _shipId, AnchorId& _outAnchor) const noexcept
{
  if (_shipId >= m_locationByShip.size() || m_locationByShip[_shipId] == INVALID_ID)
  {
    return false;
  }
  _outAnchor = m_locationByShip[_shipId];
  return true;
}

std::vector<AnchorId> WorldRegistry::LiveAnchors() const
{
  std::vector<AnchorId> anchors;
  anchors.reserve(m_live.size());
  for (const LiveWorld& entry : m_live)
  {
    anchors.push_back(entry.anchor);
  }
  return anchors;
}

std::uint64_t WorldRegistry::Hash() const
{
  std::uint64_t hash = Neuron::HashValue(m_shardTick, Neuron::FNV_OFFSET_BASIS_64);
  for (const LiveWorld& entry : m_live)
  {
    // ADR-018 D8: a world alive only because someone is watching it, holding
    // nothing but its authored occupants, is outside the replay domain. Fold it
    // and the session's hash would depend on where a camera was pointed.
    const bool shipless = entry.world->ShipCount() <= entry.authoredCount;
    if (shipless && entry.viewers > 0)
    {
      continue;
    }
    hash = Neuron::HashValue(entry.anchor, hash);
    hash = Neuron::HashValue(ComputeWorldHash(*entry.world), hash);
  }
  return hash;
}

} // namespace Game
