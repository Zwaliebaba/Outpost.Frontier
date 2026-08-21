#include "pch.h"

#include "WorldRegistry.h"

#include "DurableState.h"
#include "Formation.h"
#include "ShipClass.h"
#include "SiteEpoch.h"
#include "UniverseGen.h"
#include "WorldHash.h"

#include "EntityRecord.h"
#include "Hash.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

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
  // A gate is the same arrangement one class along (ADR-016 §10, U4): the
  // structure a fleet has to be standing at, spawned from the anchor's own
  // block so a torn-down and recreated gate grid is the same gate.
  if (_kind == AnchorKind::Gate)
  {
    _outHull = HullClass::Gate;
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

void WorldRegistry::Reset(const UniverseDef* _universe, const EconomyDef* _economy, const RegistryConfig& _config)
{
  m_universe = _universe;
  m_economy = _economy;
  m_config = _config;
  m_shardTick = 0;
  m_live.clear();
  m_locationByShip.clear();
  m_bus.clear();
  m_rosters.clear();
  m_siteLedgers.clear();
  m_bays.clear();
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
  ShipId gateShip = INVALID_SHIP_ID;
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
        if (_anchor.kind == AnchorKind::Gate && gateShip == INVALID_SHIP_ID)
        {
          gateShip = id; // And on a gate's grid, the gate.
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

  /*
   * The economy's numbers, on every grid (ADR-024 §7).
   *
   * Not only on sites: cargo is a property of a hull wherever it is standing,
   * and E3's Bay and E4's refinery are station-side. A world with no economy
   * simply has no field, no holds and nothing to refine, which is what every
   * grid was before this slice.
   */
  entry.world->SetEconomy(m_economy);

  /*
   * And the field, on a site (ADR-024 §3d).
   *
   * **This is the only place an epoch is allowed to apply.** A grid that is
   * already live keeps the field it was built with -- rocks do not teleport
   * under a wing that is working them, and a continuously-worked site keeps its
   * emptiness until presence leaves. Spin-up is the moment there is nobody to
   * do it to.
   */
  if (_anchor.kind == AnchorKind::Site)
  {
    std::uint32_t epoch = 0;
    const SiteField field = ResolveField(_anchor, epoch);
    entry.fieldEpoch = epoch;
    entry.world->SetSite(field, epoch);

    /*
     * A ledger from a passed epoch counted a pool that no longer exists, and
     * **dropping the row is the refill**: no ledger means the bake's pools
     * untouched, which is the same statement this file makes everywhere else.
     * Doing it here rather than on a timer is what keeps the durable set
     * proportional to what is being mined instead of to what has ever been.
     */
    if (const SiteLedger* ledger = Ledger(_anchor.id); ledger != nullptr && ledger->epochIndex != epoch)
    {
      m_siteLedgers.erase(m_siteLedgers.begin() + (ledger - m_siteLedgers.data()));
    }
  }

  /*
   * And where the gate leads, on a gate's grid (ADR-016 §5, U4).
   *
   * The pair comes from the universe rather than from anything the registry
   * keeps, for the reason `ReachableFrom` reads it too: the topology is
   * content, and a runtime that answered "which gate is on the far side" from
   * its own bookkeeping would be a second copy of the map to keep true.
   */
  if (_anchor.kind == AnchorKind::Gate && m_universe != nullptr)
  {
    entry.world->SetJump(m_universe->PairedGateAnchor(_anchor.id), gateShip);
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

OrderVerdict WorldRegistry::SubmitStationCommand(Neuron::PlayerId _owner, const StationCommand& _command)
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

      /*
       * And what this commander has stored here (ADR-024 §5b), which is what a
       * `TransferToShip` is judged against.
       *
       * `Bay` answers null for a Bay nobody has filled, and the view's span
       * stays empty -- which the validator reads as an empty Bay, because that
       * is exactly what it is. The alternative, materialising a Bay to look at
       * it, would put a durable record in the hash for a command that was about
       * to be refused.
       */
      if (const StationBay* bay = Bay(_owner, _command.station); bay != nullptr)
      {
        view.bayUnits = bay->oreUnits;
      }
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
   * The two transfer verbs, applied on the spot for `AssignWing`'s reason
   * exactly (ADR-024 §5c).
   *
   * Nothing crosses. A roster and a Bay are both universe-layer state on this
   * host, and the bus exists to keep one grid from reading another mid-tick --
   * this reads no grid at all. Filing a record for it would buy an apply-order
   * guarantee over a move that has no second party to be ordered against.
   *
   * Validation has already established the source covers the amount, so what
   * is left here is the arithmetic and the event. `moved` can still come back
   * short on the way *out* -- holds have capacity where a Bay does not -- and
   * the event carries what actually happened rather than what was asked for,
   * because a log that reported the request would be describing the command
   * instead of the world.
   */
  if (_command.verb == StationVerb::TransferToBay || _command.verb == StationVerb::TransferToShip)
  {
    const bool toBay = _command.verb == StationVerb::TransferToBay;
    StationBay& bay = BayFor(_owner, _command.station);
    const std::uint32_t moved = MoveOre(roster, bay, _command, toBay);
    m_events.Emit(m_shardTick, toBay ? EventKind::OreStored : EventKind::OreWithdrawn, _command.station,
                  static_cast<std::uint16_t>(std::min<std::uint32_t>(moved, 0xffffu)));
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
    TransferMember member;
    member.shipId = row->shipId;
    member.hullClass = row->hullClass;
    member.wing = row->wing;
    // Whatever was not committed to the Bay flies out with it (E3). The two
    // directions of the crossing are symmetric on purpose: a ship that docks
    // loaded and undocks without transferring is carrying the same ore.
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      member.oreUnits[ore] = row->oreUnits[ore];
    }
    if (!request.AddMember(member))
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

SiteLedger& WorldRegistry::LedgerFor(AnchorId _anchor)
{
  const auto at = std::lower_bound(m_siteLedgers.begin(), m_siteLedgers.end(), _anchor,
                                   [](const SiteLedger& _entry, AnchorId _id) { return _entry.anchor < _id; });
  if (at != m_siteLedgers.end() && at->anchor == _anchor)
  {
    return *at;
  }
  SiteLedger created;
  created.anchor = _anchor;
  return *m_siteLedgers.insert(at, created);
}

const SiteLedger* WorldRegistry::Ledger(AnchorId _anchor) const noexcept
{
  const auto at = std::lower_bound(m_siteLedgers.begin(), m_siteLedgers.end(), _anchor,
                                   [](const SiteLedger& _entry, AnchorId _id) { return _entry.anchor < _id; });
  return at != m_siteLedgers.end() && at->anchor == _anchor ? &*at : nullptr;
}

/*
 * The Bays, keyed by `(station, owner)` and kept sorted on that pair.
 *
 * The same lower-bound-and-insert shape the ledgers use, over two keys instead
 * of one: station first because a Bay is a fact about a place before it is a
 * fact about a person, which is also the order the hash folds them and the
 * order a station screen reads them.
 */
StationBay& WorldRegistry::BayFor(Neuron::PlayerId _owner, AnchorId _station)
{
  const auto less = [](const StationBay& _entry, const std::pair<AnchorId, Neuron::PlayerId>& _key)
  { return _entry.station != _key.first ? _entry.station < _key.first : _entry.owner < _key.second; };

  const std::pair<AnchorId, Neuron::PlayerId> key{_station, _owner};
  const auto at = std::lower_bound(m_bays.begin(), m_bays.end(), key, less);
  if (at != m_bays.end() && at->station == _station && at->owner == _owner)
  {
    return *at;
  }
  StationBay created;
  created.station = _station;
  created.owner = _owner;
  return *m_bays.insert(at, created);
}

const StationBay* WorldRegistry::Bay(Neuron::PlayerId _owner, AnchorId _station) const noexcept
{
  const auto less = [](const StationBay& _entry, const std::pair<AnchorId, Neuron::PlayerId>& _key)
  { return _entry.station != _key.first ? _entry.station < _key.first : _entry.owner < _key.second; };

  const std::pair<AnchorId, Neuron::PlayerId> key{_station, _owner};
  const auto at = std::lower_bound(m_bays.begin(), m_bays.end(), key, less);
  return at != m_bays.end() && at->station == _station && at->owner == _owner ? &*at : nullptr;
}

/*
 * The arithmetic behind both transfer verbs (ADR-024 §5c).
 *
 * One function for the two directions because they are one move with its sign
 * flipped, and two would be two places for the conservation rule to be got
 * wrong in. Nothing is created and nothing is destroyed here: every unit
 * subtracted from one side is added to the other in the same statement, which
 * is the property the suite asserts rather than trusting.
 *
 * **Roster order, and it is the contract.** Into the Bay the holds are drained
 * in the order the ships docked; out of it they are filled the same way. That
 * makes the outcome a function of the world rather than of the order the client
 * happened to list its ships in -- the same reason the parking scan fixes its
 * bearing order.
 *
 * Returns what actually moved, which can be less than asked on the way *out*:
 * validation checked the Bay covers it, not that the holds have room, and a
 * hold that fills stops taking. Nothing is lost -- the remainder stays in the
 * Bay -- and E4 is where a refinery makes partial fills worth reporting.
 */
std::uint32_t WorldRegistry::MoveOre(StationRoster& _roster, StationBay& _bay, const StationCommand& _command, bool _toBay)
{
  const auto oreIndex = static_cast<std::uint8_t>(_command.ore);
  const std::uint32_t litresPerUnit =
    m_economy != nullptr ? m_economy->ores[oreIndex].unitVolumeLitres : 0;

  std::uint32_t moved = 0;
  for (RosterEntry& row : _roster.docked)
  {
    if (moved >= _command.units)
    {
      break;
    }
    const bool named = std::any_of(_command.shipIds, _command.shipIds + _command.shipCount,
                                   [&row](ShipId _id) { return _id == row.shipId; });
    if (!named)
    {
      continue;
    }

    const std::uint32_t wanted = _command.units - moved;
    if (_toBay)
    {
      const std::uint32_t taken = std::min(wanted, row.oreUnits[oreIndex]);
      row.oreUnits[oreIndex] -= taken;
      _bay.oreUnits[oreIndex] += taken;
      moved += taken;
      continue;
    }

    /*
     * Outward, the hold's own capacity is the limit -- the same litre
     * arithmetic `World::OreHoldFreeLitres` does, done here because a docked
     * ship has no world to ask.
     *
     * A zero volume means the economy is absent, and then nothing moves rather
     * than everything: an unbounded hold would be the one place in this file
     * where missing content invented capacity.
     */
    const std::uint32_t capacity = m_economy != nullptr ? m_economy->Cargo(row.hullClass).oreHoldLitres : 0;
    if (litresPerUnit == 0 || capacity == 0)
    {
      continue;
    }
    std::uint32_t usedLitres = 0;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      usedLitres += row.oreUnits[ore] * m_economy->ores[ore].unitVolumeLitres;
    }
    const std::uint32_t freeLitres = capacity > usedLitres ? capacity - usedLitres : 0;
    const std::uint32_t room = freeLitres / litresPerUnit;

    const std::uint32_t given = std::min({wanted, room, _bay.oreUnits[oreIndex]});
    _bay.oreUnits[oreIndex] -= given;
    row.oreUnits[oreIndex] += given;
    moved += given;
  }
  return moved;
}

std::uint32_t WorldRegistry::EpochNow(AnchorId _anchor) const noexcept
{
  if (m_economy == nullptr)
  {
    return 0;
  }
  return SiteEpochIndex(m_shardTick, _anchor, m_economy->sites.regenSeconds * TICKS_PER_SECOND);
}

bool WorldRegistry::LedgerIsCurrent(const SiteLedger& _ledger) const noexcept
{
  const LiveWorld* live = Find(_ledger.anchor);
  const bool shipless = live != nullptr && live->world->ShipCount() <= live->authoredCount;
  if (live != nullptr && !(shipless && live->viewers > 0))
  {
    // A field with ships standing in it is not re-formed under them, so the
    // grid's own epoch is the one that counts (ADR-024 §3d).
    return live->fieldEpoch == _ledger.epochIndex;
  }
  // Nobody there, or only a camera. ADR-018 D8: what a viewer holds alive must
  // not change what the session hashes, so the calendar decides.
  return EpochNow(_ledger.anchor) == _ledger.epochIndex;
}

SiteField WorldRegistry::ResolveField(const Anchor& _anchor, std::uint32_t& _outEpoch) const
{
  _outEpoch = 0;
  SiteEpochState epoch;
  if (m_universe == nullptr || m_economy == nullptr ||
      !ResolveSiteEpoch(*m_universe, _anchor.id, m_economy->sites, m_shardTick, epoch))
  {
    return SiteField{}; // Not a site, and `Exists` says so.
  }
  _outEpoch = epoch.epochIndex;

  /*
   * The bake is the pristine truth and the ledger is the shard's (ADR-024 §3d).
   *
   * Laid out by *this* epoch's salt, so a field that has turned over comes back
   * reshuffled as well as refilled -- the rocks are somewhere else, the bearing
   * is somewhere else, and yesterday's scouting is stale. Then whatever the
   * ledger says has been taken is written over it, which is what makes a grid
   * spun up on a half-eaten field come back half eaten.
   */
  SiteField field = BuildSiteField(m_economy->sites, _anchor.site.archetype, _anchor.site.grade,
                                   _anchor.site.fieldRadiusCm, epoch.placement.layoutSalt, _anchor.site.poolUnits);
  const SiteLedger* ledger = Ledger(_anchor.id);
  if (ledger != nullptr && ledger->epochIndex == epoch.epochIndex)
  {
    ApplySiteLedger(*ledger, field);
  }
  return field;
}

SiteField WorldRegistry::FieldAt(AnchorId _anchor) const
{
  const Anchor* anchor = m_universe == nullptr ? nullptr : m_universe->FindAnchor(_anchor);
  if (anchor == nullptr || anchor->kind != AnchorKind::Site)
  {
    return SiteField{};
  }
  std::uint32_t epoch = 0;
  return ResolveField(*anchor, epoch);
}

/*
 * The field, and the field the epoch laid out, diffed into a wire row.
 *
 * A **live grid is the authority on its own field** and the ledger is the
 * authority on one that is not, which is the same split `SpinUp` and
 * `ResolveField` already make -- asked here so the sender never has to know
 * which case it is in. The pristine field comes from `BuildSiteField` on the
 * bake's own pools with the epoch's salt: the same call `ResolveField` makes
 * before it subtracts, which is what makes the denominator the numerator's
 * actual starting point rather than an estimate of it.
 */
SiteStatusRow WorldRegistry::SiteStatusFor(AnchorId _anchor) const
{
  const Anchor* anchor = m_universe == nullptr ? nullptr : m_universe->FindAnchor(_anchor);
  if (anchor == nullptr || anchor->kind != AnchorKind::Site || m_economy == nullptr)
  {
    return SiteStatusRow{};
  }

  std::uint32_t epoch = 0;
  SiteField field = ResolveField(*anchor, epoch);

  // A grid with ships in it has been mining out of its own copy since spin-up,
  // and that copy is ahead of the ledger by whatever has not yet crossed the
  // bus. Preferring it is what keeps a `SiteStatus` from reporting rocks a
  // wing has visibly already eaten.
  if (const LiveWorld* live = Find(_anchor); live != nullptr)
  {
    field = live->world->Site();
    epoch = live->fieldEpoch;
  }

  /*
   * The denominator: the same `BuildSiteField` call `ResolveField` makes before
   * it subtracts, with the salt that laid *this* epoch out.
   *
   * Resolved through `ResolveSiteEpochAt` for the epoch the field is actually
   * in -- a live grid keeps its own until it tears down (ADR-024 §3d), so
   * asking about "now" would be the wrong question. The salt is the whole
   * identity of a layout -- a status computed against a
   * different epoch's clusters would compare a field to rocks that were never
   * there, and the failure mode is silent: every cluster reads 100 % while the
   * totals visibly fall.
   */
  SiteEpochState state;
  if (!ResolveSiteEpochAt(*m_universe, _anchor, m_economy->sites, epoch, state))
  {
    return SiteStatusRow{};
  }
  const SiteField pristine = BuildSiteField(m_economy->sites, anchor->site.archetype, anchor->site.grade,
                                            anchor->site.fieldRadiusCm, state.placement.layoutSalt, anchor->site.poolUnits);
  return MakeSiteStatus(_anchor, field, pristine, epoch);
}

std::vector<CargoStatusRow> WorldRegistry::CargoFor(Neuron::PlayerId _owner) const
{
  /*
   * One player today, so every ship on a grid is theirs (ADR-018 D5).
   *
   * The parameter is not decoration: it is where the filter goes when there are
   * two, and taking it now is what makes that a one-line change rather than a
   * signature change through the sender. `INVALID_PLAYER_ID` gets nothing,
   * which is the honest answer to "what does nobody own".
   */
  std::vector<CargoStatusRow> rows;
  if (_owner == Neuron::INVALID_PLAYER_ID)
  {
    return rows;
  }

  for (const LiveWorld& entry : m_live)
  {
    const std::span<const ShipId> ids = entry.world->Ids();
    const std::span<const ShipCargo> holds = entry.world->Cargo();
    for (std::size_t slot = 0; slot < ids.size() && slot < holds.size(); ++slot)
    {
      const ShipCargo& cargo = holds[slot];
      const bool carrying = std::any_of(std::begin(cargo.oreUnits), std::end(cargo.oreUnits),
                                        [](std::uint32_t _units) { return _units > 0; });
      if (!carrying || rows.size() >= MAX_CARGO_STATUS_ROWS)
      {
        continue;
      }
      CargoStatusRow row;
      row.shipId = ids[slot];
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        row.oreUnits[ore] = cargo.oreUnits[ore];
      }
      rows.push_back(row);
    }
  }
  return rows;
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
        // And the hold with it (E3) -- ore is property, so docking parks it
        // rather than spending it.
        RosterEntry row;
        row.shipId = member.shipId;
        row.hullClass = member.hullClass;
        row.wing = member.wing;
        for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
        {
          row.oreUnits[ore] = member.oreUnits[ore];
        }
        roster.docked.push_back(row);

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
    else if (record.what.kind == TransferKind::MineYield)
    {
      ApplyMineYield(record.what);
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
  reachable.reserve(system->anchors.size() + 1);
  for (const Anchor& anchor : system->anchors)
  {
    if (anchor.id != _anchor)
    {
      reachable.push_back(anchor.id);
    }
  }

  /*
   * And the far side of the gate, if this grid is one (U4).
   *
   * **One id, appended to the same list**, so that `UnknownAnchor` keeps
   * meaning exactly "not from here" and the validator needs no second question
   * to decide whether a destination exists. Which of these is a *jump* is said
   * separately, by `SetJump`, because that changes how the order is judged and
   * not whether the place can be named.
   *
   * One hop and no further: a gate leads to the gate that leads back, and the
   * anchors around *that* system are reachable from there rather than from
   * here. Routing across several systems is the client feeding one order per
   * completed hop (ADR-016 §8) -- there is no server-side planner, and this
   * list is deliberately not the beginning of one.
   */
  if (here->kind == AnchorKind::Gate)
  {
    const AnchorId paired = m_universe->PairedGateAnchor(_anchor);
    if (paired != INVALID_ID)
    {
      reachable.push_back(paired);
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
   * A gate jump is flat (ADR-016 §5, U4), and "crosses systems" is what says it
   * is one -- the validator has already refused any crossing that is not
   * through this grid's own gate, so a destination in another system arrived
   * here through one.
   *
   * Asked before the distance is measured rather than after, because the
   * distance between two systems is the map's spacing and not a journey: the
   * two are light-years apart on the plane and the arithmetic below would
   * price a hop at hours.
   */
  if (from->system != to->system)
  {
    return std::max<std::uint32_t>(TRANSFER_FLOOR_TICKS, GATE_JUMP_TICKS);
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

void WorldRegistry::ApplyMineYield(const TransferRequest& _request)
{
  const Anchor* anchor = m_universe == nullptr ? nullptr : m_universe->FindAnchor(_request.anchor);
  if (anchor == nullptr || anchor->kind != AnchorKind::Site || m_economy == nullptr)
  {
    return;
  }

  SiteLedger& ledger = LedgerFor(_request.anchor);
  if (ledger.clusterCount == 0 || ledger.epochIndex != _request.epoch)
  {
    /*
     * The first cycle worked out of this epoch's pool, so the ledger starts
     * here -- seeded pristine and then debited, which is what makes "no ledger"
     * and "untouched" the same statement everywhere else.
     *
     * A ledger from a *later* epoch would mean this debit came out of a pool
     * that has since been replaced. Reseeding it would hand the new field's ore
     * to a cycle that never touched it, so the record is dropped instead. It
     * cannot happen while a grid stays live -- which is the point of the epoch
     * riding on the record rather than being looked up here.
     */
    if (ledger.clusterCount != 0 && ledger.epochIndex > _request.epoch)
    {
      return;
    }

    /*
     * Seeded from **the record's own epoch**, not from the calendar.
     *
     * One tick a day, a boundary falls between the tick that files a debit and
     * the tick that applies it -- and a grid stays on its own epoch while it is
     * live, so the record is right and "now" is not. Asking for the named epoch
     * costs a parameter and closes a window that would never have been
     * reproduced.
     */
    SiteEpochState state;
    if (!ResolveSiteEpochAt(*m_universe, _request.anchor, m_economy->sites, _request.epoch, state))
    {
      return;
    }
    const SiteField pristine =
      BuildSiteField(m_economy->sites, anchor->site.archetype, anchor->site.grade, anchor->site.fieldRadiusCm,
                     state.placement.layoutSalt, anchor->site.poolUnits);
    if (!pristine.Exists())
    {
      return;
    }
    ledger.epochIndex = _request.epoch;
    ledger.layoutSalt = state.placement.layoutSalt;
    StoreSiteField(pristine, ledger);
  }

  const auto ore = static_cast<std::uint8_t>(_request.ore);
  if (_request.cluster >= ledger.clusterCount || ore >= ORE_COUNT)
  {
    return;
  }
  std::uint32_t& remaining = ledger.remainingUnits[_request.cluster][ore];

  /*
   * Never below zero, and the clamp is not defensive dressing: the world debits
   * its own copy from `min(yield, remaining, room)` and this is the same
   * arithmetic arriving a tick later, so the two agree by construction -- and
   * "by construction" is exactly the kind of agreement that a future second
   * writer would break silently. A ledger that went negative would wrap.
   */
  remaining -= std::min(remaining, _request.units);

  if (_request.filledHold)
  {
    m_events.Emit(m_shardTick, EventKind::HoldFull, _request.anchor, 1);
  }
  if (ledger.TotalRemaining() == 0)
  {
    // The field, not the cluster: "this system is chewed out until tomorrow" is
    // the sentence a mining corp plans logistics around (ADR-024 §3d).
    m_events.Emit(m_shardTick, EventKind::SiteExhausted, _request.anchor, 0);
  }
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

    /*
     * And the hold (E3). A crossing is a crossing: a fleet that warps out
     * loaded arrives loaded, exactly as one that undocks does.
     *
     * This was the one of the three that got missed, and the G0 scenario is
     * what found it -- the unit suite had a dock and an undock and no warp, so
     * ore survived both boundaries anybody had thought to test and evaporated
     * on the one nobody had.
     */
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      spawn.cargo.oreUnits[ore] = member->oreUnits[ore];
    }
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
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      spawn.cargo.oreUnits[ore] = member->oreUnits[ore];
    }

    // Undocking spawns it **full**, and that is not a repair step: the roster
    // held no gauges to be damaged (ADR-017 §1). It also spawns it **loaded**,
    // which is not a gauge either: the hold crossed on the record (E3).
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

bool WorldRegistry::IsAuthoredOccupant(AnchorId _anchor, ShipId _shipId) const noexcept
{
  if (m_universe == nullptr)
  {
    return false;
  }
  const Anchor* anchor = m_universe->FindAnchor(_anchor);
  if (anchor == nullptr || anchor->occupantCount == 0)
  {
    return false;
  }
  // The bake's window, exactly as `SpinUp` issues it: base + index, for
  // `occupantCount` ids. Arithmetic in u32 because `occupantIdBase` is one --
  // a baked id is not a wire value (ADR-018 D6a).
  const std::uint32_t id = _shipId;
  return id >= anchor->occupantIdBase && id < anchor->occupantIdBase + anchor->occupantCount;
}

bool WorldRegistry::LoadDurable(const DurableState& _state, std::vector<PersistenceDiagnostic>& _outDiagnostics)
{
  const auto refuse = [&_outDiagnostics](std::string _what, std::string _message)
  {
    PersistenceDiagnostic diagnostic;
    diagnostic.what = std::move(_what);
    diagnostic.message = std::move(_message);
    _outDiagnostics.push_back(std::move(diagnostic));
    return false;
  };

  /*
   * A load is into a fresh registry, and everything else is refused.
   *
   * Not defensiveness: there is no answer to what it would mean to merge a save
   * file into a running shard that is better than declining to have one, and a
   * refusal at boot is visible where a silent merge would be visible weeks
   * later as a duplicated fleet.
   */
  if (!m_live.empty() || !m_rosters.empty() || !m_bays.empty() || !m_siteLedgers.empty() || !m_bus.empty())
  {
    return refuse("registry", "a durable set loads into a freshly reset registry, and this one is running");
  }
  if (m_universe == nullptr)
  {
    return refuse("registry", "no universe: a durable set names anchors, and nothing here can resolve one");
  }

  /*
   * The high-water mark first, and a mark that would go backwards is a refusal
   * rather than a clamp (ADR-025 §1a).
   *
   * Clamping would be the failure this rule exists to prevent, dressed as a
   * repair: the shard would carry on and re-issue ids that the rosters and
   * transfers being loaded in the next few lines still name.
   */
  if (_state.nextDynamicShipId < m_nextDynamicId)
  {
    return refuse("allocator", "the ship-id mark would go backwards, from " + std::to_string(m_nextDynamicId) + " to " +
                                 std::to_string(_state.nextDynamicShipId));
  }

  m_shardTick = _state.shardTick;
  m_nextDynamicId = _state.nextDynamicShipId;

  for (const DurableRoster& roster : _state.rosters)
  {
    StationRoster& row = RosterFor(roster.anchor);
    row.docked = roster.docked;
    for (const RosterEntry& docked : row.docked)
    {
      // Docked counts as presence (ADR-017 §7), so the index has to point at
      // the station for a reloaded ship exactly as it does for one that docked
      // a moment ago.
      RecordLocation(docked.shipId, roster.anchor);
    }
  }

  m_bays.assign(_state.bays.begin(), _state.bays.end());
  m_siteLedgers.assign(_state.ledgers.begin(), _state.ledgers.end());
  m_bus.assign(_state.transfers.begin(), _state.transfers.end());

  /*
   * The transfer counter resumes past the highest record on the bus.
   *
   * The same trap as the ship-id mark, one size down: a counter that restarted
   * at one would stamp a new record with an id a pending record already has,
   * and the bus's total order (ADR-018 D17) would have two records claiming one
   * place in it.
   */
  m_nextTransferCounter = 1;
  for (const TransferRecord& record : m_bus)
  {
    m_nextTransferCounter = std::max(m_nextTransferCounter, record.id.counter + 1);
  }
  std::sort(m_bus.begin(), m_bus.end());

  /*
   * And the ships, at rest (ADR-025 §1).
   *
   * Spinning the grid up is what having a ship there *means* -- a world is a
   * runtime, and one with a fleet standing on it is live by the same rule that
   * keeps it live after a warp arrival. What the ship does not come back with
   * is any of its intention: no order queue, no leg, no guidance target, and no
   * undock protection, because fifteen seconds do not survive a restart and a
   * shard that restarts owes nobody a protected launch.
   */
  for (const DurableShip& ship : _state.ships)
  {
    World* world = Borrow(ship.anchor);
    if (world == nullptr)
    {
      return refuse("ship " + std::to_string(ship.shipId), "anchor " + std::to_string(ship.anchor) + " is not a place in this universe");
    }
    if (IsAuthoredOccupant(ship.anchor, ship.shipId))
    {
      return refuse("ship " + std::to_string(ship.shipId), "id belongs to anchor " + std::to_string(ship.anchor) + "'s authored occupants");
    }
    ShipSpawn spawn;
    spawn.hullClass = ship.hullClass;
    spawn.wing = ship.wing;
    spawn.xMetres = ship.xMetres;
    spawn.yMetres = ship.yMetres;
    spawn.headingRadians = ship.headingRadians;
    spawn.protectedUntilTick = 0;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      spawn.cargo.oreUnits[ore] = ship.oreUnits[ore];
    }
    if (world->Spawn(spawn, ship.shipId) == INVALID_SHIP_ID)
    {
      return refuse("ship " + std::to_string(ship.shipId), "the grid at anchor " + std::to_string(ship.anchor) + " refused it");
    }
    RecordLocation(ship.shipId, ship.anchor);
  }

  return true;
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
      // And the hold (E3). Ore that survived a dock is state a reload has to
      // reproduce, so a replay that lost a manifest in the crossing says so
      // here rather than at the moment somebody notices their haul is gone.
      for (const std::uint32_t units : docked.oreUnits)
      {
        hash = Neuron::HashValue(units, hash);
      }
    }
  }

  /*
   * And the Bays (ADR-024 §5b, E3), on the roster's terms exactly.
   *
   * Durable, universe-layer, folded in key order -- and **unlike the ledgers,
   * with no currency rule**: a Bay has no epoch to go stale against. What is in
   * it was put there by a command and stays until another command moves it,
   * which is the whole difference between committed property and a pool the
   * shard refills on a calendar.
   *
   * A Bay exists only once something has been put in it, so this loop is
   * proportional to what commanders have actually committed rather than to how
   * many stations the bake authored -- the same discipline the ledgers keep.
   */
  for (const StationBay& bay : m_bays)
  {
    hash = Neuron::HashValue(bay.station, hash);
    hash = Neuron::HashValue(bay.owner, hash);
    for (const std::uint32_t units : bay.oreUnits)
    {
      hash = Neuron::HashValue(units, hash);
    }
  }
  /*
   * And the site ledgers, on the same terms and with one extra rule
   * (ADR-024 §3d, ADR-018 D8).
   *
   * A ledger the shard owes a refill is skipped -- `LedgerIsCurrent` carries
   * the argument -- because the pool it counts was replaced at the epoch
   * boundary and folding it would make the session's hash depend on whether
   * anybody happened to spin the grid up and notice. What is folded is what a
   * reload has to reproduce, which is exactly the ledgers that still describe
   * a pool.
   */
  for (const SiteLedger& ledger : m_siteLedgers)
  {
    if (!LedgerIsCurrent(ledger))
    {
      continue;
    }
    hash = Neuron::HashValue(ledger.anchor, hash);
    hash = Neuron::HashValue(ledger.epochIndex, hash);
    hash = Neuron::HashValue(ledger.clusterCount, hash);
    for (std::uint8_t index = 0; index < ledger.clusterCount; ++index)
    {
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        hash = Neuron::HashValue(ledger.remainingUnits[index][ore], hash);
      }
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
    // The mining payload, folded for the reason the rest of the record is: a
    // replay that lost a yield in flight would agree about two ledgers and
    // disagree about a pool one tick later.
    hash = Neuron::HashValue(record.what.cluster, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.ore), hash);
    hash = Neuron::HashValue(record.what.units, hash);
    hash = Neuron::HashValue(record.what.epoch, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.filledHold ? 1 : 0), hash);
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
