#include "pch.h"

#include "WorldRegistry.h"

#include "Formation.h"
#include "ShipClass.h"
#include "UniverseGen.h"
#include "WorldHash.h"

#include "EntityRecord.h"
#include "Hash.h"

#include <algorithm>
#include <cmath>
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

/// The member a solved station belongs to. A linear scan over at most 64 ids,
/// which is the same shape `SolveFormation`'s own class lookup takes.
[[nodiscard]] const TransferMember* FindMember(const TransferRequest& _request, ShipId _shipId) noexcept
{
  for (std::uint16_t index = 0; index < _request.memberCount; ++index)
  {
    if (_request.members[index].shipId == _shipId)
    {
      return &_request.members[index];
    }
  }
  return nullptr;
}

} // namespace

void WorldRegistry::Reset(const UniverseDef* _universe, const RegistryConfig& _config)
{
  m_universe = _universe;
  m_config = _config;
  m_shardTick = 0;
  m_live.clear();
  m_locationByShip.clear();
  m_bus.clear();
  m_rosters.clear();
  m_events.Clear();
  m_nextTransferCounter = 1;

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

void WorldRegistry::HandOff() noexcept
{
  for (LiveWorld& entry : m_live)
  {
    entry.world->ReleaseOwner();
  }
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
  ShipId stationShip = INVALID_SHIP_ID;
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
        if (_anchor.kind == AnchorKind::Station && stationShip == INVALID_SHIP_ID)
        {
          stationShip = id; // The first occupant is the structure itself.
        }
        RecordLocation(id, _anchor.id);
      }
    }
  }

  /*
   * The grid's own identity (ADR-017 §2). A `Dock` names an anchor and the
   * validator has to answer "is that this grid's?", which it can only do if the
   * world knows which anchor it is. An id, not a coordinate -- the tick still
   * has no idea where in the galaxy it is.
   */
  entry.world->SetAnchor(_anchor.id, stationShip, ReachableFrom(_anchor.id));

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

void WorldRegistry::RecordLocation(ShipId _shipId, AnchorId _anchor)
{
  if (_shipId >= m_locationByShip.size())
  {
    m_locationByShip.resize(static_cast<std::size_t>(_shipId) + 1, INVALID_ID);
  }
  m_locationByShip[_shipId] = _anchor;
}

ShipId WorldRegistry::Spawn(AnchorId _anchor, const ShipSpawn& _spawn)
{
  World* world = Borrow(_anchor);
  if (world == nullptr)
  {
    return INVALID_SHIP_ID;
  }
  const ShipId id = world->Spawn(_spawn, AllocateShipId());
  if (id != INVALID_SHIP_ID)
  {
    RecordLocation(id, _anchor);
  }
  return id;
}

OrderVerdict WorldRegistry::SubmitStationCommand(const StationCommand& _command)
{
  RosterView view;
  view.station = INVALID_ID;
  if (m_universe != nullptr)
  {
    const Anchor* anchor = m_universe->FindAnchor(_command.station);
    if (anchor != nullptr && anchor->kind == AnchorKind::Station)
    {
      // A station exists whether or not anything is docked at it. The roster is
      // a fact about ships, not a property the station has to be given, so an
      // empty one is still *this* station's -- and the command is refused
      // `NotDocked` rather than `UnknownStation`, which is the honest answer.
      view.station = _command.station;
      view.docked = Roster(_command.station);
    }
  }

  const OrderVerdict verdict = ValidateStationCommand(view, _command);
  if (!verdict.accepted)
  {
    return verdict;
  }

  StationRoster& roster = RosterFor(_command.station);

  if (_command.verb == StationVerb::AssignWing)
  {
    /*
     * Nothing crosses, so nothing is filed. A wing is a number a ship carries
     * (ADR-017 §6) -- there is no wing table to create, no entity to name, and
     * disbanding a wing is reassigning its last member. Applied on the spot for
     * the same reason a dock is not: the bus exists to keep one grid from
     * reading another mid-tick, and this reads no grid at all.
     */
    for (std::uint16_t index = 0; index < _command.shipCount; ++index)
    {
      const ShipId shipId = _command.shipIds[index];
      for (RosterEntry& row : roster.docked)
      {
        if (row.shipId == shipId)
        {
          row.wing = _command.wing;
          break;
        }
      }
    }
    m_events.Emit(m_shardTick, EventKind::WingAssigned, _command.station, _command.shipCount);
    return verdict;
  }

  /*
   * Undock. The ships leave the roster **now** and arrive on the grid when the
   * record applies, which is exactly the shape a dock has in reverse: leave the
   * source at filing, arrive at the destination at the apply point. It is also
   * what makes a second undock naming the same ship in the same tick impossible
   * rather than merely unlikely -- the row is already gone.
   */
  TransferRequest request;
  request.kind = TransferKind::Undock;
  request.anchor = _command.station;
  request.formation = _command.formation;

  for (std::uint16_t index = 0; index < _command.shipCount; ++index)
  {
    const ShipId shipId = _command.shipIds[index];
    const auto row = std::find_if(roster.docked.begin(), roster.docked.end(),
                                  [shipId](const RosterEntry& _row) { return _row.shipId == shipId; });
    if (row == roster.docked.end())
    {
      continue; // Validation already refused this case; belt and braces.
    }
    if (!request.AddMember(TransferMember{row->shipId, row->hullClass, row->wing}))
    {
      break;
    }
    roster.docked.erase(row);
  }

  if (request.memberCount > 0)
  {
    TransferRecord record;
    record.id = TransferId{m_config.hostId, m_nextTransferCounter++};
    record.applyTick = m_shardTick + 1;
    record.what = request;
    m_bus.push_back(record);
  }
  return verdict;
}

bool WorldRegistry::Despawn(AnchorId _anchor, ShipId _shipId)
{
  LiveWorld* entry = Find(_anchor);
  if (entry == nullptr || !entry->world->Despawn(_shipId))
  {
    return false;
  }
  if (_shipId < m_locationByShip.size())
  {
    m_locationByShip[_shipId] = INVALID_ID;
  }
  return true;
}

WorldRegistry::StationRoster& WorldRegistry::RosterFor(AnchorId _anchor)
{
  const auto at = std::lower_bound(m_rosters.begin(), m_rosters.end(), _anchor,
                                   [](const StationRoster& _entry, AnchorId _id) { return _entry.anchor < _id; });
  if (at != m_rosters.end() && at->anchor == _anchor)
  {
    return *at;
  }
  StationRoster created;
  created.anchor = _anchor;
  return *m_rosters.insert(at, std::move(created));
}

std::span<const RosterEntry> WorldRegistry::Roster(AnchorId _anchor) const noexcept
{
  const auto at = std::lower_bound(m_rosters.begin(), m_rosters.end(), _anchor,
                                   [](const StationRoster& _entry, AnchorId _id) { return _entry.anchor < _id; });
  if (at == m_rosters.end() || at->anchor != _anchor)
  {
    return {};
  }
  return at->docked;
}

void WorldRegistry::ApplyDueTransfers()
{
  /*
   * Between ticks, in `(applyTick, transferId)` order (ADR-018 D17).
   *
   * Sorted rather than assumed sorted: records are filed by several worlds in
   * one tick and the order they were *collected* in is the registry's
   * iteration order, which is an implementation detail nothing may depend on.
   * The total order is a contract, so it is imposed here, once.
   */
  if (m_bus.empty())
  {
    return;
  }
  std::sort(m_bus.begin(), m_bus.end());

  std::size_t applied = 0;
  for (const TransferRecord& record : m_bus)
  {
    if (record.applyTick > m_shardTick)
    {
      break; // Sorted, so everything after this is also in the future.
    }
    ++applied;

    if (record.what.kind == TransferKind::Dock)
    {
      StationRoster& roster = RosterFor(record.what.anchor);
      for (std::uint16_t index = 0; index < record.what.memberCount; ++index)
      {
        const TransferMember& member = record.what.members[index];

        // The roster keeps the id (ADR-017 §1): the ship that undocks is the
        // ship that docked, and every log and order that named it still does.
        roster.docked.push_back(RosterEntry{member.shipId, member.hullClass, member.wing});

        // Docked counts as presence (ADR-017 §7), so the index keeps pointing
        // at the station rather than forgetting where the ship went.
        RecordLocation(member.shipId, record.what.anchor);
      }
      m_events.Emit(m_shardTick, EventKind::Docked, record.what.anchor, record.what.memberCount);
    }
    else if (record.what.kind == TransferKind::Undock)
    {
      ApplyUndock(record.what);
    }
    else if (record.what.kind == TransferKind::Transit)
    {
      ApplyTransit(record.what);
    }
  }
  m_bus.erase(m_bus.begin(), m_bus.begin() + static_cast<std::ptrdiff_t>(applied));
}

std::vector<AnchorId> WorldRegistry::ReachableFrom(AnchorId _anchor) const
{
  /*
   * Every other anchor in the same system (ADR-016 §5).
   *
   * In-system only, and that is U3a's whole scope: leaving a system is what
   * gates are for, and routing through them is U4's. Itself excluded, because
   * "warp to where you already are" is not a refusal worth a reason -- it is a
   * destination that should not be offered.
   */
  std::vector<AnchorId> reachable;
  if (m_universe == nullptr)
  {
    return reachable;
  }
  const Anchor* here = m_universe->FindAnchor(_anchor);
  if (here == nullptr)
  {
    return reachable;
  }
  const SolarSystem* system = m_universe->FindSystem(here->system);
  if (system == nullptr)
  {
    return reachable;
  }
  reachable.reserve(system->anchors.size());
  for (const Anchor& anchor : system->anchors)
  {
    if (anchor.id != _anchor)
    {
      reachable.push_back(anchor.id);
    }
  }
  return reachable;
}

std::uint32_t WorldRegistry::TransitTicks(AnchorId _from, const TransferRequest& _request) const
{
  const Anchor* from = m_universe == nullptr ? nullptr : m_universe->FindAnchor(_from);
  const Anchor* to = m_universe == nullptr ? nullptr : m_universe->FindAnchor(_request.anchor);
  if (from == nullptr || to == nullptr)
  {
    return 1;
  }

  /*
   * The distance, in `double` rather than shifted integers.
   *
   * Two anchors in one system are at most a few astronomical units apart --
   * about 1e12 metres -- and squaring that is 1e24, which overflows `int64`.
   * `double` counts integers exactly to 9e15, so the *inputs* are exact, and
   * the product and the root are IEEE operations the build already pins
   * (`/fp:precise`, no `/arch`, ADR-010 §6): same binary, same answer. What
   * this must never become is a `float`, which stops counting metres exactly at
   * about 16 million of them.
   */
  const auto dx = static_cast<double>(to->origin.x - from->origin.x);
  const auto dy = static_cast<double>(to->origin.y - from->origin.y);
  const double metres = std::sqrt(dx * dx + dy * dy);

  // The slowest member sets the pace, because a fleet arrives together.
  float slowest = 0.0f;
  for (std::uint16_t index = 0; index < _request.memberCount; ++index)
  {
    const float speed = ShipClass(_request.members[index].hullClass).warpSpeedMetresPerSec;
    slowest = slowest == 0.0f ? speed : std::min(slowest, speed);
  }
  if (slowest <= 0.0f)
  {
    // Nothing here can warp -- a fleet of structures, which validation should
    // never have let through. The floor rather than a division by zero.
    return TRANSFER_FLOOR_TICKS;
  }

  const double seconds = static_cast<double>(WARP_BASE_SECONDS) + metres / static_cast<double>(slowest);
  const double ticks = seconds / static_cast<double>(World::TICK_SECONDS);

  // The floor is ADR-019 §4b's, and it is a constraint on this table rather
  // than a clamp on a mistake: a transit shorter than it leaves a second host
  // no slack to receive the record in.
  return std::max<std::uint32_t>(TRANSFER_FLOOR_TICKS, static_cast<std::uint32_t>(ticks));
}

void WorldRegistry::ApplyTransit(const TransferRequest& _request)
{
  const Anchor* anchor = m_universe == nullptr ? nullptr : m_universe->FindAnchor(_request.anchor);
  if (anchor == nullptr)
  {
    return;
  }

  // Arriving spins the destination up, which is the same door an undock opens
  // from the inside (ADR-016 §4).
  World* world = Borrow(_request.anchor);
  if (world == nullptr)
  {
    return;
  }

  const DirectX::XMFLOAT2 arrival{Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->warpInPoint.x)),
                                  Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->warpInPoint.y))};
  const float facing = Neuron::HeadingToRadians(anchor->warpInFacingTurns16);

  struct MemberLookup
  {
    const TransferRequest* request = nullptr;

    [[nodiscard]] static HullClass Of(ShipId _shipId, void* _context) noexcept
    {
      const auto* lookup = static_cast<const MemberLookup*>(_context);
      for (std::uint16_t index = 0; index < lookup->request->memberCount; ++index)
      {
        if (lookup->request->members[index].shipId == _shipId)
        {
          return lookup->request->members[index].hullClass;
        }
      }
      return HullClass::Interceptor;
    }
  };

  ShipId ids[MAX_SHIPS_PER_ORDER];
  for (std::uint16_t index = 0; index < _request.memberCount; ++index)
  {
    ids[index] = _request.members[index].shipId;
  }

  FormationStation stations[MAX_SHIPS_PER_ORDER];
  MemberLookup lookup{&_request};
  const std::uint32_t placed =
    SolveFormation(_request.formation, std::span<const ShipId>{ids, _request.memberCount}, &MemberLookup::Of, &lookup,
                   arrival, facing, std::span<FormationStation>{stations});

  // By id rather than by index, for the reason spelled out in `ApplyUndock`:
  // the solve orders its stations by ascending ship id and the record does not.
  for (std::uint32_t index = 0; index < placed; ++index)
  {
    const TransferMember* member = FindMember(_request, stations[index].shipId);
    if (member == nullptr)
    {
      continue;
    }

    ShipSpawn spawn;
    spawn.hullClass = member->hullClass;
    spawn.wing = member->wing;
    spawn.xMetres = stations[index].positionMetres.x;
    spawn.yMetres = stations[index].positionMetres.y;
    spawn.headingRadians = facing;
    if (world->Spawn(spawn, member->shipId) != INVALID_SHIP_ID)
    {
      RecordLocation(member->shipId, _request.anchor);
    }
  }

  m_events.Emit(m_shardTick, EventKind::Arrived, _request.anchor, static_cast<std::uint16_t>(placed));
}

void WorldRegistry::ApplyUndock(const TransferRequest& _request)
{
  const Anchor* anchor = m_universe == nullptr ? nullptr : m_universe->FindAnchor(_request.anchor);
  if (anchor == nullptr)
  {
    return;
  }

  /*
   * Spawning into a world with no live grid spins one up (ADR-016 §4). An
   * undock *is* ships arriving -- the same door warp will use, opened from the
   * inside.
   */
  World* world = Borrow(_request.anchor);
  if (world == nullptr)
  {
    return;
  }

  // The authored undock point and facing (ADR-017 §3): ~800 m off the
  // structure, facing outward, clear of its contact radius. Authored rather
  // than computed, so two fleets undocking the same tick differ only by
  // ADR-015's separation.
  const DirectX::XMFLOAT2 undockPoint{Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->undockPoint.x)),
                                      Neuron::CentimetresToMetres(static_cast<std::int32_t>(anchor->undockPoint.y))};
  const float facing = Neuron::HeadingToRadians(anchor->undockFacingTurns16);

  /*
   * Solved together, because they left together. The lookup reads the record
   * rather than the world -- the ships are not in the world yet, which is the
   * whole reason the crossing carries their classes.
   */
  struct MemberLookup
  {
    const TransferRequest* request = nullptr;

    [[nodiscard]] static HullClass Of(ShipId _shipId, void* _context) noexcept
    {
      const auto* lookup = static_cast<const MemberLookup*>(_context);
      for (std::uint16_t index = 0; index < lookup->request->memberCount; ++index)
      {
        if (lookup->request->members[index].shipId == _shipId)
        {
          return lookup->request->members[index].hullClass;
        }
      }
      return HullClass::Interceptor;
    }
  };

  ShipId ids[MAX_SHIPS_PER_ORDER];
  for (std::uint16_t index = 0; index < _request.memberCount; ++index)
  {
    ids[index] = _request.members[index].shipId;
  }

  FormationStation stations[MAX_SHIPS_PER_ORDER];
  MemberLookup lookup{&_request};
  const std::uint32_t placed =
    SolveFormation(_request.formation, std::span<const ShipId>{ids, _request.memberCount}, &MemberLookup::Of, &lookup,
                   undockPoint, facing, std::span<FormationStation>{stations});

  // Fifteen seconds, stamped at arrival (ADR-017 §5). Computed from the tick
  // length rather than written as a tick count, so the window stays fifteen
  // seconds if the tick rate ever moves (ADR-002 §1).
  const auto protectionTicks =
    static_cast<std::uint32_t>(static_cast<float>(UNDOCK_PROTECTION_SECONDS) / World::TICK_SECONDS);

  ShipId parked[MAX_SHIPS_PER_ORDER];
  std::uint16_t parkedCount = 0;

  /*
   * Paired by **id**, not by index.
   *
   * `SolveFormation` assigns stations in ascending `ShipId` order (Formation.h
   * says so, and the client's preview relies on it), which is not the order the
   * record happens to list its members in. Walking both by index would put each
   * ship on somebody else's station -- a fleet that arrives in the right shape
   * with the wrong ships in it, which no "did it land near the anchor" test
   * would ever notice.
   */
  for (std::uint32_t index = 0; index < placed; ++index)
  {
    const TransferMember* member = FindMember(_request, stations[index].shipId);
    if (member == nullptr)
    {
      continue;
    }

    ShipSpawn spawn;
    spawn.hullClass = member->hullClass;
    spawn.wing = member->wing;
    spawn.xMetres = stations[index].positionMetres.x;
    spawn.yMetres = stations[index].positionMetres.y;
    spawn.headingRadians = facing;
    spawn.protectedUntilTick = m_shardTick + protectionTicks;

    // Undocking spawns it **full**, and that is not a repair step: the roster
    // held no gauges to be damaged (ADR-017 §1).
    if (world->Spawn(spawn, member->shipId) != INVALID_SHIP_ID)
    {
      RecordLocation(member->shipId, _request.anchor);
      parked[parkedCount++] = member->shipId;
    }
  }

  /*
   * And the parking order (ADR-017 §4), issued the moment the ships exist.
   *
   * A **real order group**, so the ETA, the drawn lane, the straggler deadline
   * and player override all come free rather than being reimplemented for one
   * case. `SubmitSystemOrder` rather than `SubmitOrder` for the one difference
   * that matters: it must not end the protection it was issued alongside.
   *
   * All 24 candidates taken means the fleet holds where it is -- not a refusal,
   * and not an error. Undocking is never refused for clutter; the player can
   * replace the parking order at any time, because it is just an order.
   */
  m_events.Emit(m_shardTick, EventKind::Undocked, _request.anchor, static_cast<std::uint16_t>(parkedCount));

  if (parkedCount == 0)
  {
    return;
  }

  // The station is the ring's centre, and it is where the anchor is: the grid
  // is anchored on the structure, so the local origin is the structure.
  const DirectX::XMFLOAT2 centre{0.0f, 0.0f};
  DirectX::XMFLOAT2 berth{};
  float berthFacing = 0.0f;
  if (!world->FindBerth(std::span<const ShipId>{parked, parkedCount}, _request.formation, centre, undockPoint, berth,
                        berthFacing))
  {
    // Worth telling the player precisely because nothing was refused: the fleet
    // is fine, it is simply still standing in the doorway (ADR-017 §4).
    m_events.Emit(m_shardTick, EventKind::BerthHeld, _request.anchor, static_cast<std::uint16_t>(parkedCount));
    return;
  }

  OrderSubmit parking;
  parking.kind = OrderKind::Move;
  parking.formation = _request.formation;
  parking.queueMode = QueueMode::Replace;
  for (std::uint16_t index = 0; index < parkedCount; ++index)
  {
    (void)parking.AddShip(parked[index]);
  }
  parking.target.xCm = Neuron::MetresToCentimetres(berth.x);
  parking.target.yCm = Neuron::MetresToCentimetres(berth.y);
  parking.target.facingTurns16 = Neuron::RadiansToHeading(berthFacing);
  (void)world->SubmitSystemOrder(parking);
}

void WorldRegistry::CollectFiledTransfers()
{
  for (LiveWorld& entry : m_live)
  {
    for (const TransferRequest& request : entry.world->FiledTransfers())
    {
      TransferRecord record;
      record.id = TransferId{m_config.hostId, m_nextTransferCounter++};

      /*
       * Filed during this tick, applied before the next one -- which is what
       * makes "no world ever reads another mid-tick" true rather than hoped.
       *
       * A transit is the exception that proves it: it applies at its *arrival*
       * tick, which is the same rule with a longer number. The journey is the
       * delay, and the fleet is nowhere in the meantime -- which is why the
       * in-flight bus is in the hash (ADR-018 D17): a replay that lost a fleet
       * mid-crossing would agree about two empty grids.
       */
      record.applyTick = m_shardTick + 1;
      if (request.kind == TransferKind::Transit)
      {
        record.applyTick = m_shardTick + TransitTicks(entry.anchor, request);
      }
      record.what = request;
      m_bus.push_back(record);
    }
    entry.world->ClearFiledTransfers();
  }
}

void WorldRegistry::Tick(std::uint32_t _shardTick)
{
  m_shardTick = _shardTick;

  // Between ticks, before any world runs: a transfer filed last tick lands now,
  // and no world's tick can observe another's mid-flight (ADR-017 §9).
  ApplyDueTransfers();

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

  // What this tick filed, stamped and queued. After the worlds have run, so a
  // record filed during a tick cannot apply within it.
  CollectFiledTransfers();

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
                                /*
                                 * The index forgets this grid entirely, not
                                 * just the ships still standing on it.
                                 *
                                 * A ship despawned through the borrowed world
                                 * rather than through `Despawn` leaves an entry
                                 * behind, and a stale "it is over there" is
                                 * worse than no answer: the roster, the order
                                 * routing and the summaries all believe it. So
                                 * teardown sweeps by anchor -- **except the
                                 * ships docked here**, which are not on the
                                 * grid and do not leave with it (ADR-017 §1).
                                 */
                                const std::span<const RosterEntry> docked = Roster(_entry.anchor);
                                for (std::size_t index = 0; index < m_locationByShip.size(); ++index)
                                {
                                  if (m_locationByShip[index] != _entry.anchor)
                                  {
                                    continue;
                                  }
                                  const auto id = static_cast<ShipId>(index);
                                  const bool onTheRoster =
                                    std::any_of(docked.begin(), docked.end(),
                                                [id](const RosterEntry& _row) { return _row.shipId == id; });
                                  if (!onTheRoster)
                                  {
                                    m_locationByShip[index] = INVALID_ID;
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

std::vector<FleetSummary> WorldRegistry::Summaries() const
{
  std::vector<FleetSummary> rows;

  /*
   * Grids first, then rosters, then the bus -- the three places a ship can be
   * (ADR-016 §4, ADR-017 §1), asked in that order so the result is sorted by
   * anchor within each state and the merge below only has to sort the states.
   */
  for (const LiveWorld& entry : m_live)
  {
    /*
     * The authored occupants are *not* the commander's ships. A station stands
     * on its own grid and would otherwise show up as a one-ship fleet parked at
     * every station in the universe -- which is the same reason `TearDownIdle`
     * does not count them as ships either.
     */
    const std::uint32_t count = entry.world->ShipCount() - entry.authoredCount;
    if (count > 0)
    {
      rows.push_back(FleetSummary{entry.anchor, FleetState::OnGrid, static_cast<std::uint16_t>(count),
                                  FLEET_ETA_NONE});
    }
  }

  for (const StationRoster& roster : m_rosters)
  {
    if (!roster.docked.empty())
    {
      rows.push_back(FleetSummary{roster.anchor, FleetState::Docked,
                                  static_cast<std::uint16_t>(roster.docked.size()), FLEET_ETA_NONE});
    }
  }

  for (const TransferRecord& record : m_bus)
  {
    if (record.what.kind != TransferKind::Transit)
    {
      // A dock or an undock applies on the next tick. Reporting it as a fleet
      // "in transit for 0 seconds" would put a row on screen that is gone
      // before it is read.
      continue;
    }

    // Rounded up, so a fleet one tick out reads as "1s" rather than "arrived".
    const std::uint32_t ticks = record.applyTick > m_shardTick ? record.applyTick - m_shardTick : 0;
    const auto seconds = static_cast<std::uint32_t>(
      (static_cast<double>(ticks) * static_cast<double>(World::TICK_SECONDS)) + 0.999);
    rows.push_back(FleetSummary{record.what.anchor, FleetState::InTransit, record.what.memberCount,
                                static_cast<std::uint16_t>(std::min<std::uint32_t>(seconds, FLEET_ETA_NONE - 1))});
  }

  std::sort(rows.begin(), rows.end(),
            [](const FleetSummary& _a, const FleetSummary& _b)
            {
              if (_a.anchor != _b.anchor)
              {
                return _a.anchor < _b.anchor;
              }
              return static_cast<std::uint8_t>(_a.state) < static_cast<std::uint8_t>(_b.state);
            });
  return rows;
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

  /*
   * The rosters and the bus (ADR-017 §9, ADR-018 D17).
   *
   * They are in the replay domain because they are durable state that outlives
   * the worlds that produced it: a station grid can tear down with a full
   * roster, and a replay that reproduced the grids but not the roster would
   * agree about an empty universe. In anchor order and then filing order,
   * because both are the orders everything else iterates them in.
   */
  for (const StationRoster& roster : m_rosters)
  {
    hash = Neuron::HashValue(roster.anchor, hash);
    for (const RosterEntry& docked : roster.docked)
    {
      hash = Neuron::HashValue(docked.shipId, hash);
      hash = Neuron::HashValue(static_cast<std::uint8_t>(docked.hullClass), hash);
      hash = Neuron::HashValue(docked.wing, hash);
    }
  }
  for (const TransferRecord& record : m_bus)
  {
    hash = Neuron::HashValue(record.applyTick, hash);
    hash = Neuron::HashValue(record.id.host, hash);
    hash = Neuron::HashValue(record.id.counter, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.kind), hash);
    hash = Neuron::HashValue(record.what.anchor, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.formation), hash);
    for (std::uint16_t index = 0; index < record.what.memberCount; ++index)
    {
      hash = Neuron::HashValue(record.what.members[index].shipId, hash);
      hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.members[index].hullClass), hash);
      hash = Neuron::HashValue(record.what.members[index].wing, hash);
    }
  }
  return hash;
}

} // namespace Game
