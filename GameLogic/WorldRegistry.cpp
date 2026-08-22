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
  m_ownerByShip.clear();
  m_bus.clear();
  m_rosters.clear();
  m_siteLedgers.clear();
  m_bays.clear();
  m_refineJobs.clear();
  m_stationTiers.clear();
  m_projects.clear();
  m_events.Clear();
  m_nextTransferCounter = 1;
  m_nextRefineSequence = 1;

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
        /*
         * Authored occupants belong to NOBODY (ADR-018 D5, U3c-a). A station,
         * a gate and the scenery around them are the universe's furniture, and
         * an owner here would hand whoever connects first a structure they did
         * not build -- and, through `HasPresence`, the right to watch every
         * grid that has one.
         */
        RecordShip(id, _anchor.id, Neuron::INVALID_PLAYER_ID);
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

void WorldRegistry::RecordShip(ShipId _shipId, AnchorId _anchor, Neuron::PlayerId _owner)
{
  if (_shipId >= m_locationByShip.size())
  {
    m_locationByShip.resize(static_cast<std::size_t>(_shipId) + 1, INVALID_ID);
  }
  if (_shipId >= m_ownerByShip.size())
  {
    m_ownerByShip.resize(static_cast<std::size_t>(_shipId) + 1, Neuron::INVALID_PLAYER_ID);
  }
  m_locationByShip[_shipId] = _anchor;
  m_ownerByShip[_shipId] = _owner;
}

void WorldRegistry::ForgetShip(ShipId _shipId)
{
  if (_shipId < m_locationByShip.size())
  {
    m_locationByShip[_shipId] = INVALID_ID;
  }
  if (_shipId < m_ownerByShip.size())
  {
    m_ownerByShip[_shipId] = Neuron::INVALID_PLAYER_ID;
  }
}

Neuron::PlayerId WorldRegistry::OwnerOf(ShipId _shipId) const noexcept
{
  if (_shipId >= m_ownerByShip.size())
  {
    return Neuron::INVALID_PLAYER_ID;
  }
  return m_ownerByShip[_shipId];
}

bool WorldRegistry::HasPresence(Neuron::PlayerId _owner, AnchorId _anchor) const noexcept
{
  /*
   * Nobody is not somebody. Without this, `INVALID_PLAYER_ID` would match every
   * authored occupant in the universe and an anonymous handshake would be the
   * one identity that can watch any grid with a statue on it.
   */
  if (_owner == Neuron::INVALID_PLAYER_ID || _anchor == INVALID_ID)
  {
    return false;
  }

  /*
   * One walk of the index answers both halves of ADR-017 §7's rule, because a
   * docked ship keeps its location pointing at the station it is docked at --
   * which is exactly why teardown sweeps around the roster rather than through
   * it.
   */
  const std::size_t count = std::min(m_locationByShip.size(), m_ownerByShip.size());
  for (std::size_t index = 0; index < count; ++index)
  {
    if (m_locationByShip[index] == _anchor && m_ownerByShip[index] == _owner)
    {
      return true;
    }
  }
  return false;
}

ShipId WorldRegistry::Spawn(AnchorId _anchor, const ShipSpawn& _spawn, Neuron::PlayerId _owner)
{
  World* world = Borrow(_anchor);
  if (world == nullptr)
  {
    return INVALID_SHIP_ID;
  }
  const ShipId id = world->Spawn(_spawn, AllocateShipId());
  if (id != INVALID_SHIP_ID)
  {
    RecordShip(id, _anchor, _owner);
  }
  return id;
}

OrderVerdict WorldRegistry::SubmitStationCommand(Neuron::PlayerId _owner, const StationCommand& _command)
{
  RosterView view;
  view.station = INVALID_ID;

  /// What the open project still wants, per alloy. A local the view borrows,
  /// because the remaining counts are derived and there is nowhere durable they
  /// belong -- the project stores what was *given*, and what is left is
  /// arithmetic against the content.
  std::uint32_t remainingScratch[ALLOY_COUNT] = {};

  /// This commander's half of the station's roster, for the same reason: the
  /// view borrows a span and something has to own it.
  std::vector<RosterEntry> dockedScratch;

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

      /*
       * **This commander's docked ships, not the station's** (U3c-a).
       *
       * The view is what the validator judges a command against, so an
       * unfiltered roster here is not a display bug, it is an authority one: a
       * commander could name somebody else's hull in an `Undock` and the
       * validator would find it on the roster and agree. Reading the filtered
       * set means that command is refused `NotDocked` -- which is the truth
       * from where the asker stands, and says nothing about whose it is.
       *
       * A local the view borrows, like `remainingScratch` above, because a
       * filtered roster is a new vector and the span has to point at something
       * that outlives the call.
       */
      dockedScratch = DockedFor(_owner, _command.station);
      view.docked = dockedScratch;

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
        view.bayAlloyUnits = bay->alloyUnits;
      }

      /*
       * And the refinery's half of the view (ADR-024 §6, E4b).
       *
       * Answers rather than tables, which is the shape `bayUnits` already set:
       * the shared validator is a pure function over a small struct on both
       * machines, so what crosses is "may this recipe be cooked here" and never
       * the tier table it was derived from.
       */
      view.refineryTier = TierAt(_command.station);
      view.refineJobCount = RefineJobCountFor(_owner, _command.station);
      if (m_economy != nullptr && view.refineryTier != 0)
      {
        view.refineSlotCount = m_economy->Tier(view.refineryTier).slotsPerPlayer;
        view.refineQueueDepth = m_economy->refining.queueDepthPerPlayer;
        view.recipeUnlocked = TierCooks(*m_economy, view.refineryTier, _command.alloy);
        view.batchKnown = FindRefineBatch(m_economy->refining, _command.units) != nullptr;
      }
      if (const UpgradeProject* project = Project(_command.station); project != nullptr && m_economy != nullptr)
      {
        const RefineryUpgradeProject& cost = m_economy->refining.upgradeProjects[project->toTier - 1];
        for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
        {
          remainingScratch[alloy] = cost.costUnits[alloy] - std::min(cost.costUnits[alloy], project->contributedUnits[alloy]);
        }
        view.projectOpen = true;
        view.projectRemainingUnits = remainingScratch;
      }
      else if (m_economy != nullptr && ProjectFor(_command.station) != nullptr)
      {
        // No contributions yet, so no row -- but the project is open, and the
        // validator must say so or the first contribution to every station in
        // the shard would be refused for want of a record it is about to make.
        const UpgradeProject* opened = Project(_command.station);
        const RefineryUpgradeProject& cost = m_economy->refining.upgradeProjects[opened->toTier - 1];
        for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
        {
          remainingScratch[alloy] = cost.costUnits[alloy];
        }
        view.projectOpen = true;
        view.projectRemainingUnits = remainingScratch;
      }
    }
  }

  const OrderVerdict verdict = ValidateStationCommand(view, _command);
  if (!verdict.accepted)
  {
    return verdict;
  }

  // The refining verbs apply on the spot beside `AssignWing` and for the same
  // reason: both ends are universe-layer state on one host, and the bus exists
  // to stop one grid reading another mid-tick, which this does not do.
  if (!NamesShips(_command.verb))
  {
    return ApplyRefineCommand(_owner, _command);
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
    // And whose it is, from the row rather than from the command: a commander
    // undocking somebody else's ship is a thing validation refuses, and reading
    // the owner off the roster means this path cannot be the way it happens.
    member.owner = row->owner;
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
  ForgetShip(_shipId);
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
   * This commander's ships, and only theirs (ADR-018 D5, U3c-a).
   *
   * The parameter was here before the filter was, with a note saying it was
   * where the filter would go and that taking it now made that a one-line
   * change rather than a signature change through the sender. It was a one-line
   * change. `INVALID_PLAYER_ID` still gets nothing, which is the honest answer
   * to "what does nobody own" and now also excludes the authored occupants,
   * which own no cargo but would have been walked anyway.
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
      if (OwnerOf(ids[slot]) != _owner)
      {
        continue; // Somebody else's hold is not this commander's business.
      }
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

/*
 * --- what docking does to a wing (ADR-017 §3, §6) --------------------------
 *
 * Three functions, and the shard-wide scan in the first two is the price of the
 * one question that matters: *is this the whole wing, or part of it?* A count
 * kept per wing would be faster and would be a second copy of a fact that is
 * already written on every ship, so it would drift the first time a hull died
 * somewhere nobody remembered to decrement. The population is small -- tens of
 * ships -- and this runs once per dock rather than once per tick.
 *
 * All three walk the same three places, and the third one is the one that is
 * easy to forget: a ship mid-crossing is on no grid and no roster, and a scan
 * that missed the bus would call a wing empty while its members were in the air.
 */

std::uint32_t WorldRegistry::WingPopulation(Neuron::PlayerId _owner, WingId _wing) const noexcept
{
  if (_wing == INVALID_WING_ID)
  {
    return 0; // Wing zero is "no wing", and counting it would count strays.
  }

  std::uint32_t found = 0;

  for (const LiveWorld& live : m_live)
  {
    if (live.world == nullptr)
    {
      continue;
    }
    const std::span<const ShipId> ids = live.world->Ids();
    const std::span<const WingId> wings = live.world->Wings();
    for (std::size_t slot = 0; slot < ids.size() && slot < wings.size(); ++slot)
    {
      if (wings[slot] == _wing && OwnerOf(ids[slot]) == _owner)
      {
        ++found;
      }
    }
  }

  for (const StationRoster& roster : m_rosters)
  {
    for (const RosterEntry& row : roster.docked)
    {
      if (row.wing == _wing && row.owner == _owner)
      {
        ++found;
      }
    }
  }

  for (const TransferRecord& record : m_bus)
  {
    for (std::uint16_t index = 0; index < record.what.memberCount; ++index)
    {
      const TransferMember& member = record.what.members[index];
      if (member.wing == _wing && member.owner == _owner)
      {
        ++found;
      }
    }
  }

  return found;
}

WingId WorldRegistry::UnusedWingFor(Neuron::PlayerId _owner) const noexcept
{
  /*
   * The lowest free number, which is the one a player would guess at and the
   * only choice that is stable across a replay: "lowest unused" depends on the
   * *set* of wings in play and not on the order anything was walked in, so two
   * runs of the same script pick the same number (ADR-005's determinism rule).
   */
  for (std::uint32_t candidate = 1; candidate <= std::numeric_limits<WingId>::max(); ++candidate)
  {
    if (WingPopulation(_owner, static_cast<WingId>(candidate)) == 0)
    {
      return static_cast<WingId>(candidate);
    }
  }
  return INVALID_WING_ID;
}

WingId WorldRegistry::WingForDockedGroup(const TransferRequest& _what) const noexcept
{
  if (_what.memberCount == 0)
  {
    return INVALID_WING_ID;
  }

  /*
   * One wing among the members, or more than one.
   *
   * More than one is decided here and needs no count: ships from two wings have
   * visibly stopped flying with somebody, so they are a group whatever else is
   * true. Wing zero among them counts as a difference rather than as a wildcard
   * -- a stray joining a wing's dock is still a change of composition.
   */
  const WingId first = _what.members[0].wing;
  bool uniform = true;
  for (std::uint16_t index = 1; index < _what.memberCount; ++index)
  {
    uniform = uniform && _what.members[index].wing == first;
  }

  if (uniform && first != INVALID_WING_ID)
  {
    /*
     * All of one wing. Now the only question left: **all of it?**
     *
     * The members are still on the bus at this point -- a dock despawns at
     * filing and lands here -- so they are inside `WingPopulation`'s count, and
     * equality is exactly "nobody was left behind". That is the case the owner
     * chose to protect: docking a whole wing and undocking it must give the
     * fleet back with the name it had, or every routine round trip would spend
     * a call sign.
     */
    if (WingPopulation(_what.members[0].owner, first) == _what.memberCount)
    {
      return INVALID_WING_ID; // No change.
    }
  }

  /*
   * Otherwise they are a new group, and it gets the lowest free number.
   *
   * `INVALID_WING_ID` back from here means all 255 are in use, and it flows
   * through as "leave them alone" -- which is the caller's sentinel and the
   * right answer: not the regrouping the player asked for, but far better than
   * emptying the ships into wing zero, where the roster draws no row and they
   * would vanish off the HUD.
   */
  return UnusedWingFor(_what.members[0].owner);
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
      /*
       * What wing these ships are in once they are inside (ADR-017 §3).
       *
       * Asked **once for the record and before the first row is filed**, and
       * both halves of that matter. Once, because the ships that dock together
       * are one group and asking per ship would give the first one a number and
       * then measure the rest against a wing that had just changed size. Before
       * filing, because the answer counts the wing's members and the members of
       * this dock are still on the bus -- start writing rows and the count
       * shifts under the question.
       */
      const WingId formed = WingForDockedGroup(record.what);

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
        // The group's wing, or the one it arrived with when the dock changed
        // nothing -- `INVALID_WING_ID` is the "leave it" sentinel rather than a
        // wing to put anybody in. See `WingForDockedGroup`.
        row.wing = formed == INVALID_WING_ID ? member.wing : formed;
        for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
        {
          row.oreUnits[ore] = member.oreUnits[ore];
        }
        // Whose it is, carried across by the crossing rather than looked up
        // here: mid-dock the ship is on no grid, so there is nothing to ask.
        row.owner = member.owner;
        roster.docked.push_back(row);

        // Docked counts as presence (ADR-017 §7), so the index keeps pointing
        // at the station rather than forgetting where the ship went.
        RecordShip(member.shipId, record.what.anchor, member.owner);
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
      RecordShip(member->shipId, _request.anchor, member->owner);
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
      RecordShip(member->shipId, _request.anchor, member->owner);
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
      /*
       * And whose ships these are (ADR-018 D5, U3c-a).
       *
       * Stamped HERE rather than filed by the world, because a world knows
       * only its own ships and must never learn who owns one -- D2's boundary,
       * and the reason `TransferMember::owner` is documented as the registry's
       * to fill. At file time the fleet is still standing on the grid, so the
       * index still has the answer; one tick later, mid-crossing, it would not.
       */
      record.what = request;
      for (std::uint16_t index = 0; index < record.what.memberCount; ++index)
      {
        TransferMember& member = record.what.members[index];
        member.owner = OwnerOf(member.shipId);
      }
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
   * And the refinery, beside the bus and for the same reason (ADR-024 §6b).
   *
   * Here rather than in a world's tick because a job **runs while its commander
   * is offline and while its station's grid is torn down** -- it is universe-
   * layer work on the shard-global tick, which is precisely what the transfer
   * bus's apply point already is. A refinery that needed a live grid would be a
   * refinery nobody could walk away from, and walking away is the feature.
   */
  AdvanceRefining();

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
                                    ForgetShip(id);
                                  }
                                }
                                return true;
                              }),
               m_live.end());
}

namespace
{
/// `Station.cpp`'s helper, repeated rather than exported: a two-line
/// constructor on a public header would be a shared function whose only job is
/// to save typing a brace.
[[nodiscard]] OrderVerdict Refuse(OrderReason _reason) noexcept
{
  return OrderVerdict{false, _reason};
}
} // namespace

SecurityBand WorldRegistry::BandAt(AnchorId _station) const noexcept
{
  if (m_universe == nullptr || m_economy == nullptr)
  {
    return SecurityBand::High;
  }
  const Anchor* anchor = m_universe->FindAnchor(_station);
  if (anchor == nullptr)
  {
    return SecurityBand::High;
  }
  const SolarSystem* system = m_universe->FindSystem(anchor->system);
  // A system the anchor names and the universe does not have is content that
  // could not have baked; High-Sec is the cautious answer, being the band that
  // permits least.
  return system == nullptr ? SecurityBand::High : m_economy->BandFor(system->security);
}

std::uint8_t WorldRegistry::TierAt(AnchorId _station) const noexcept
{
  if (m_universe == nullptr || m_economy == nullptr)
  {
    return 0;
  }
  const Anchor* anchor = m_universe->FindAnchor(_station);
  if (anchor == nullptr || anchor->kind != AnchorKind::Station)
  {
    return 0;
  }

  const bool starter = anchor->id == m_universe->StartAnchorId();
  std::uint8_t tier = AuthoredStationTier(starter);
  for (const StationTier& row : m_stationTiers)
  {
    if (row.station == _station)
    {
      tier = std::max(tier, row.tier);
      break;
    }
  }

  /*
   * And never above the band's cap (ADR-024 §6c).
   *
   * Clamped on the way *out* rather than trusted on the way in, because this is
   * the industrial map and it has to hold against every path that could ever
   * raise a tier -- a project, a reload, a content retune that lowered a cap
   * under a station somebody already upgraded.
   */
  const std::uint8_t cap = BandTierCap(*m_economy, BandAt(_station));
  return std::min(tier, cap == 0 ? tier : cap);
}

const UpgradeProject* WorldRegistry::Project(AnchorId _station) const noexcept
{
  const auto at = std::lower_bound(m_projects.begin(), m_projects.end(), _station,
                                   [](const UpgradeProject& _entry, AnchorId _id) { return _entry.station < _id; });
  return at != m_projects.end() && at->station == _station ? &*at : nullptr;
}

StationTier& WorldRegistry::TierFor(AnchorId _station)
{
  const auto at = std::lower_bound(m_stationTiers.begin(), m_stationTiers.end(), _station,
                                   [](const StationTier& _entry, AnchorId _id) { return _entry.station < _id; });
  if (at != m_stationTiers.end() && at->station == _station)
  {
    return *at;
  }
  StationTier row;
  row.station = _station;
  row.tier = TierAt(_station);
  return *m_stationTiers.insert(at, row);
}

UpgradeProject* WorldRegistry::ProjectFor(AnchorId _station)
{
  if (m_economy == nullptr)
  {
    return nullptr;
  }

  /*
   * The project a station is *currently* building, which is the next tier and
   * nothing else.
   *
   * A station already at its band's cap has none, and that is the amber row
   * the print draws pointing out of the station entirely: geography rather than
   * progress. A row whose `toTier` no longer matches -- because the project it
   * was for completed -- is retargeted rather than kept, since a station builds
   * one thing at a time and the contributions to a finished project were
   * consumed by finishing it.
   */
  const std::uint8_t tier = TierAt(_station);
  const std::uint8_t cap = BandTierCap(*m_economy, BandAt(_station));
  if (tier == 0 || tier >= REFINERY_TIER_COUNT || (cap != 0 && tier >= cap))
  {
    return nullptr;
  }
  const std::uint8_t next = static_cast<std::uint8_t>(tier + 1);
  if (!m_economy->refining.upgradeProjects[next - 1].Exists())
  {
    return nullptr;
  }

  const auto at = std::lower_bound(m_projects.begin(), m_projects.end(), _station,
                                   [](const UpgradeProject& _entry, AnchorId _id) { return _entry.station < _id; });
  if (at != m_projects.end() && at->station == _station)
  {
    if (at->toTier != next)
    {
      *at = UpgradeProject{};
      at->station = _station;
      at->toTier = next;
    }
    return &*at;
  }
  UpgradeProject row;
  row.station = _station;
  row.toTier = next;
  return &*m_projects.insert(at, row);
}

RefineryStatusRow WorldRegistry::RefineryStatusFor(Neuron::PlayerId _owner, AnchorId _station) const
{
  RefineryStatusRow row;
  row.station = _station;
  row.tier = TierAt(_station);

  // In the vector's own order, which is `(station, owner, sequence)` -- so a
  // commander's jobs arrive in the order the queue will drain them, without the
  // sender having to know that is what queue order means.
  for (const RefineJob& job : m_refineJobs)
  {
    if (job.owner != _owner || job.station != _station)
    {
      continue;
    }
    RefineryJobRow entry;
    entry.sequence = job.sequence;
    entry.alloy = job.alloy;
    entry.batchUnits = job.batchUnits;
    entry.completeTick = job.completeTick;
    row.jobs.push_back(entry);
  }

  if (const UpgradeProject* project = Project(_station); project != nullptr)
  {
    row.projectToTier = project->toTier;
    for (std::uint8_t alloy = 0; alloy < ALLOY_COUNT; ++alloy)
    {
      row.projectContributedUnits[alloy] = project->contributedUnits[alloy];
    }
  }
  return row;
}

std::vector<AnchorId> WorldRegistry::RefineriesFor(Neuron::PlayerId _owner) const
{
  std::vector<AnchorId> stations;
  for (const RefineJob& job : m_refineJobs)
  {
    if (job.owner == _owner && (stations.empty() || stations.back() != job.station))
    {
      // The jobs are sorted by station, so "not the last one added" is the
      // whole of the de-duplication.
      stations.push_back(job.station);
    }
  }
  return stations;
}

std::uint32_t WorldRegistry::RefineJobCountFor(Neuron::PlayerId _owner, AnchorId _station) const noexcept
{
  std::uint32_t count = 0;
  for (const RefineJob& job : m_refineJobs)
  {
    if (job.owner == _owner && job.station == _station)
    {
      ++count;
    }
  }
  return count;
}

void WorldRegistry::AdvanceRefining()
{
  if (m_economy == nullptr)
  {
    return;
  }

  /*
   * Completion first (ADR-024 §6b): outputs and the refund credit the Bay, and
   * the event record learns what finished so the away-log can say so.
   *
   * The plan was priced at submission and is carried on the record, so nothing
   * here reads a tier table -- a station that was upgraded mid-job does not
   * retroactively improve a job the player already agreed the numbers for.
   */
  for (std::size_t index = 0; index < m_refineJobs.size();)
  {
    const RefineJob& job = m_refineJobs[index];
    if (!job.Running() || job.completeTick > m_shardTick)
    {
      ++index;
      continue;
    }

    StationBay& bay = BayFor(job.owner, job.station);
    bay.alloyUnits[static_cast<std::uint8_t>(job.alloy)] += job.outputUnits;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      bay.oreUnits[ore] += job.refundUnits[ore];
    }
    m_events.Emit(m_shardTick, EventKind::RefineComplete, job.station, job.outputUnits);
    m_refineJobs.erase(m_refineJobs.begin() + static_cast<std::ptrdiff_t>(index));
  }

  /*
   * Then the starts, so a slot freed above is filled now rather than next tick.
   *
   * In `sequence` order within each `(owner, station)`, which is what makes
   * "the queue drains in order" a fact about the record: the vector is sorted
   * on that key, so the first queued job found for a pair is that pair's next.
   */
  for (RefineJob& job : m_refineJobs)
  {
    if (job.Running())
    {
      continue;
    }
    const std::uint8_t tier = TierAt(job.station);
    if (tier == 0)
    {
      continue;
    }
    std::uint32_t running = 0;
    for (const RefineJob& other : m_refineJobs)
    {
      if (other.owner == job.owner && other.station == job.station && other.Running())
      {
        ++running;
      }
    }
    if (running >= m_economy->Tier(tier).slotsPerPlayer)
    {
      continue;
    }
    job.completeTick = m_shardTick + job.durationTicks;
  }
}

OrderVerdict WorldRegistry::ApplyRefineCommand(Neuron::PlayerId _owner, const StationCommand& _command)
{
  const OrderVerdict accepted{true, OrderReason::Accepted};

  if (_command.verb == StationVerb::RefineCancel)
  {
    /*
     * A **queued** job cancels whole and its inputs return to the Bay
     * untouched; a running one cannot be cancelled at all, its inputs being
     * spent (owner ruling, 2026-08-21). Nothing is created or destroyed here:
     * what goes back is exactly what came out, because it is the same array.
     */
    for (std::size_t index = 0; index < m_refineJobs.size(); ++index)
    {
      const RefineJob& job = m_refineJobs[index];
      if (job.owner != _owner || job.station != _command.station || job.sequence != _command.sequence)
      {
        continue;
      }
      if (job.Running())
      {
        return Refuse(OrderReason::RefineryBusy);
      }
      StationBay& bay = BayFor(_owner, _command.station);
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        bay.oreUnits[ore] += job.inputUnits[ore];
      }
      m_refineJobs.erase(m_refineJobs.begin() + static_cast<std::ptrdiff_t>(index));
      return accepted;
    }
    // No such queued job of yours here: the queue is not in the state the
    // command assumed, which is what `RefineryBusy` says.
    return Refuse(OrderReason::RefineryBusy);
  }

  if (_command.verb == StationVerb::ProjectContribute)
  {
    UpgradeProject* project = ProjectFor(_command.station);
    if (project == nullptr)
    {
      return Refuse(OrderReason::RecipeLocked);
    }
    StationBay& bay = BayFor(_owner, _command.station);
    const auto alloy = static_cast<std::uint8_t>(_command.alloy);
    const RefineryUpgradeProject& cost = m_economy->refining.upgradeProjects[project->toTier - 1];
    const std::uint32_t remaining = cost.costUnits[alloy] - std::min(cost.costUnits[alloy], project->contributedUnits[alloy]);
    const std::uint32_t moved = std::min({_command.units, bay.alloyUnits[alloy], remaining});
    if (moved == 0)
    {
      return Refuse(OrderReason::InsufficientMaterials);
    }
    bay.alloyUnits[alloy] -= moved;
    project->contributedUnits[alloy] += moved;

    /*
     * And the tier rises **exactly once**, on the tick the last unit lands.
     *
     * The check is right here rather than on a sweep, which is what makes
     * "exactly once" true when two commanders complete it in the same tick:
     * the second contribution finds `remaining` already zero and is refused
     * `InsufficientMaterials` before it can move a unit, so there is no second
     * completion to suppress.
     */
    if (project->IsComplete(cost))
    {
      TierFor(_command.station).tier = project->toTier;
      m_events.Emit(m_shardTick, EventKind::ProjectComplete, _command.station, project->toTier);
      m_projects.erase(m_projects.begin() + (project - m_projects.data()));
    }
    return accepted;
  }

  // `RefineStart`. The plan is priced against the tier *now* and carried, and
  // the inputs debit at submission (ADR-024 §6b) -- which is what makes the
  // print's "the Bay's after-state" the number the player is deciding with.
  const std::uint8_t tier = TierAt(_command.station);
  const RefinePlan plan = PlanRefine(*m_economy, _command.alloy, _command.units, tier);
  if (!plan.valid)
  {
    return Refuse(OrderReason::RecipeLocked);
  }
  StationBay& bay = BayFor(_owner, _command.station);
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    if (bay.oreUnits[ore] < plan.inputUnits[ore])
    {
      return Refuse(OrderReason::InsufficientMaterials);
    }
  }

  RefineJob job;
  job.owner = _owner;
  job.station = _command.station;
  job.alloy = _command.alloy;
  job.sequence = m_nextRefineSequence++;
  job.batchUnits = _command.units;
  job.outputUnits = plan.outputUnits;
  job.durationTicks = plan.durationTicks;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    bay.oreUnits[ore] -= plan.inputUnits[ore];
    job.inputUnits[ore] = plan.inputUnits[ore];
    job.refundUnits[ore] = plan.refundUnits[ore];
  }

  const auto at = std::lower_bound(m_refineJobs.begin(), m_refineJobs.end(), job,
                                   [](const RefineJob& _a, const RefineJob& _b)
                                   {
                                     if (_a.station != _b.station)
                                     {
                                       return _a.station < _b.station;
                                     }
                                     if (_a.owner != _b.owner)
                                     {
                                       return _a.owner < _b.owner;
                                     }
                                     return _a.sequence < _b.sequence;
                                   });
  m_refineJobs.insert(at, job);

  // Started now if a slot is free, rather than on the next tick: a job
  // submitted into an empty refinery should not sit idle for fifty
  // milliseconds because the loop had already run.
  AdvanceRefining();
  return accepted;
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
  if (!m_live.empty() || !m_rosters.empty() || !m_bays.empty() || !m_siteLedgers.empty() || !m_bus.empty()
      || !m_refineJobs.empty() || !m_stationTiers.empty() || !m_projects.empty())
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

  /*
   * Everything is judged before anything is written.
   *
   * The alternative is a load that refuses on its last ship having already put
   * the rosters, the Bays and the ledgers in -- which is the half-built
   * registry this function's contract promises not to leave, and the reason
   * that promise is worth making: a refusal is visible at boot, and a shard
   * holding two thirds of itself is visible when somebody notices a fleet is
   * missing. So the ships are checked here, against the *content*, without
   * spinning a single grid up.
   */
  for (const DurableShip& ship : _state.ships)
  {
    const Anchor* anchor = m_universe->FindAnchor(ship.anchor);
    if (anchor == nullptr)
    {
      return refuse("ship " + std::to_string(ship.shipId),
                    "anchor " + std::to_string(ship.anchor) + " is not a place in this universe");
    }
    if (IsAuthoredOccupant(ship.anchor, ship.shipId))
    {
      return refuse("ship " + std::to_string(ship.shipId),
                    "id belongs to anchor " + std::to_string(ship.anchor) + "'s authored occupants");
    }
  }

  /*
   * And no key twice, in any of the four sets (ADR-025 §1).
   *
   * A duplicate is not a merge problem, it is a *statement* problem: two rows
   * claiming one Bay are two answers to "what has this commander committed
   * here", and there is no rule that picks one which is not an invention. A
   * file that says it twice is a file that was not written by this code.
   *
   * `ShipId` bounds the ship check to one pass over a bitset-shaped vector,
   * which matters because a shard's ships are the largest of the four sets by
   * an order of magnitude and the others are tens of rows.
   */
  {
    /*
     * **This was a bitset indexed by `ShipId` and could not stay one** (U3d-b).
     * With a u16 id the table was 64k bits and free; with a u32 id it would be
     * half a gigabyte, allocated on the load path, sized by a constant rather
     * than by anything in the file.
     *
     * A sort of the ids present is O(n log n) over the shard's ships and
     * allocates once. It is the largest of the four duplicate checks here by an
     * order of magnitude, which is why it is the one that gets a container of
     * its own rather than a quadratic scan.
     */
    std::vector<ShipId> seenShip;
    seenShip.reserve(_state.ships.size());
    for (const DurableShip& ship : _state.ships)
    {
      seenShip.push_back(ship.shipId);
    }
    std::sort(seenShip.begin(), seenShip.end());
    const auto duplicate = std::adjacent_find(seenShip.begin(), seenShip.end());
    if (duplicate != seenShip.end())
    {
      return refuse("ship " + std::to_string(*duplicate), "is in the durable set twice");
    }
  }
  for (std::size_t index = 0; index + 1 < _state.rosters.size(); ++index)
  {
    for (std::size_t other = index + 1; other < _state.rosters.size(); ++other)
    {
      if (_state.rosters[index].anchor == _state.rosters[other].anchor)
      {
        return refuse("roster " + std::to_string(_state.rosters[index].anchor), "is in the durable set twice");
      }
    }
  }
  for (std::size_t index = 0; index + 1 < _state.bays.size(); ++index)
  {
    for (std::size_t other = index + 1; other < _state.bays.size(); ++other)
    {
      if (_state.bays[index].station == _state.bays[other].station && _state.bays[index].owner == _state.bays[other].owner)
      {
        return refuse("bay " + std::to_string(_state.bays[index].station), "is in the durable set twice for one owner");
      }
    }
  }
  for (std::size_t index = 0; index + 1 < _state.ledgers.size(); ++index)
  {
    for (std::size_t other = index + 1; other < _state.ledgers.size(); ++other)
    {
      if (_state.ledgers[index].anchor == _state.ledgers[other].anchor)
      {
        return refuse("ledger " + std::to_string(_state.ledgers[index].anchor), "is in the durable set twice");
      }
    }
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
      RecordShip(docked.shipId, roster.anchor, docked.owner);
    }
  }

  m_bays.assign(_state.bays.begin(), _state.bays.end());
  m_siteLedgers.assign(_state.ledgers.begin(), _state.ledgers.end());
  m_bus.assign(_state.transfers.begin(), _state.transfers.end());
  m_refineJobs.assign(_state.refineJobs.begin(), _state.refineJobs.end());
  m_stationTiers.assign(_state.stationTiers.begin(), _state.stationTiers.end());
  m_projects.assign(_state.projects.begin(), _state.projects.end());

  /*
   * The job sequence resumes past everything on the books, for the transfer
   * counter's reason exactly: a sequence is how a cancel names a job, and a
   * counter that restarted would let one command cancel a job it did not mean.
   */
  m_nextRefineSequence = 1;
  for (const RefineJob& job : m_refineJobs)
  {
    m_nextRefineSequence = std::max(m_nextRefineSequence, job.sequence + 1);
  }

  /*
   * Sorted on load, because sorted is not a property of the file -- it is a
   * property `Bay` and `Ledger` *depend on*.
   *
   * Both look their key up with `lower_bound`, so a set that arrived in any
   * other order would answer "no such Bay" for a Bay that is right there, and
   * it would do it silently. A capture writes them in order and a round trip
   * therefore never needs this; a file somebody edited, or a future writer that
   * did not know the rule, is exactly what it is for.
   */
  std::sort(m_bays.begin(), m_bays.end(), [](const StationBay& _a, const StationBay& _b) noexcept
            { return _a.station != _b.station ? _a.station < _b.station : _a.owner < _b.owner; });
  std::sort(m_siteLedgers.begin(), m_siteLedgers.end(),
            [](const SiteLedger& _a, const SiteLedger& _b) noexcept { return _a.anchor < _b.anchor; });
  std::sort(m_refineJobs.begin(), m_refineJobs.end(), [](const RefineJob& _a, const RefineJob& _b) noexcept
            {
              if (_a.station != _b.station)
              {
                return _a.station < _b.station;
              }
              return _a.owner != _b.owner ? _a.owner < _b.owner : _a.sequence < _b.sequence;
            });
  std::sort(m_stationTiers.begin(), m_stationTiers.end(),
            [](const StationTier& _a, const StationTier& _b) noexcept { return _a.station < _b.station; });
  std::sort(m_projects.begin(), m_projects.end(),
            [](const UpgradeProject& _a, const UpgradeProject& _b) noexcept { return _a.station < _b.station; });

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
      // The anchor resolved in the pass above, so this is the grid refusing to
      // exist rather than the record naming nowhere -- worth its own sentence
      // because the two failures want different investigations.
      return refuse("ship " + std::to_string(ship.shipId), "anchor " + std::to_string(ship.anchor) + " would not spin up");
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
    RecordShip(ship.shipId, ship.anchor, ship.owner);
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

std::vector<RosterEntry> WorldRegistry::DockedFor(Neuron::PlayerId _owner, AnchorId _anchor) const
{
  std::vector<RosterEntry> mine;
  if (_owner == Neuron::INVALID_PLAYER_ID)
  {
    return mine;
  }
  for (const RosterEntry& docked : Roster(_anchor))
  {
    if (docked.owner == _owner)
    {
      mine.push_back(docked);
    }
  }
  return mine;
}

std::vector<FleetSummary> WorldRegistry::Summaries(Neuron::PlayerId _viewer) const
{
  std::vector<FleetSummary> rows;
  if (_viewer == Neuron::INVALID_PLAYER_ID)
  {
    return rows;
  }

  /*
   * Grids first, then rosters, then the bus -- the three places a ship can be
   * (ADR-016 §4, ADR-017 §1), asked in that order so the result is sorted by
   * anchor within each state and the merge below only has to sort the states.
   */
  for (const LiveWorld& entry : m_live)
  {
    /*
     * This commander's ships, and the special case that used to be here went
     * away rather than being kept (U3c-a).
     *
     * It read `ShipCount() - authoredCount`, with a paragraph explaining that
     * authored occupants are not the commander's ships -- a station would
     * otherwise show up as a one-ship fleet parked at every station in the
     * universe. That is still true and is now simply *implied*: authored
     * occupants belong to `INVALID_PLAYER_ID`, so counting ships this viewer
     * owns cannot count them. A rule that survives as a consequence of a more
     * general one is worth more than the same rule spelled twice.
     */
    std::uint32_t count = 0;
    for (const ShipId shipId : entry.world->Ids())
    {
      count += OwnerOf(shipId) == _viewer ? 1u : 0u;
    }
    if (count > 0)
    {
      rows.push_back(FleetSummary{entry.anchor, FleetState::OnGrid, static_cast<std::uint16_t>(count),
                                  FLEET_ETA_NONE});
    }
  }

  for (const StationRoster& roster : m_rosters)
  {
    std::uint32_t mine = 0;
    for (const RosterEntry& docked : roster.docked)
    {
      mine += docked.owner == _viewer ? 1u : 0u;
    }
    if (mine > 0)
    {
      rows.push_back(FleetSummary{roster.anchor, FleetState::Docked,
                                  static_cast<std::uint16_t>(mine), FLEET_ETA_NONE});
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

    // This commander's ships in it, counted rather than assumed: a crossing is
    // one commander's order today, but the member carries the owner precisely
    // so that "today" is not load-bearing.
    std::uint16_t mine = 0;
    for (std::uint16_t index = 0; index < record.what.memberCount; ++index)
    {
      mine += record.what.members[index].owner == _viewer ? 1u : 0u;
    }
    if (mine == 0)
    {
      continue; // Somebody else's fleet is somebody else's business.
    }

    // Rounded up, so a fleet one tick out reads as "1s" rather than "arrived".
    const std::uint32_t ticks = record.applyTick > m_shardTick ? record.applyTick - m_shardTick : 0;
    const auto seconds = static_cast<std::uint32_t>(
      (static_cast<double>(ticks) * static_cast<double>(World::TICK_SECONDS)) + 0.999);
    rows.push_back(FleetSummary{record.what.anchor, FleetState::InTransit, mine,
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

    /*
     * And whose the ships on it are (ADR-018 D5, U3c-a).
     *
     * Separately from `ComputeWorldHash` because it is separate STATE: the
     * world does not know an owner and must not (D2), so the world's own hash
     * cannot carry one. Folded in the world's slot order, which is the order
     * `ComputeWorldHash` already depends on, so this adds no new ordering rule.
     *
     * It is in the replay domain for the rosters' reason exactly (below): a
     * replay that reproduced every ship and forgot who owned them would agree
     * about a universe where nothing belonged to anybody -- and would do it
     * silently, because every position, hold and gauge would match.
     */
    for (const ShipId shipId : entry.world->Ids())
    {
      hash = Neuron::HashValue(OwnerOf(shipId), hash);
    }
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
      // And whose (U3c-a). A roster that came back from a reload belonging to
      // the wrong commander is the failure this catches, and it is one a
      // player would find before any test did.
      hash = Neuron::HashValue(docked.owner, hash);
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
    // And the alloys refining turned it into (E4b), in the same fold: it is the
    // same statement about the same place, and a second loop would be a second
    // place for the order to drift.
    for (const std::uint32_t units : bay.alloyUnits)
    {
      hash = Neuron::HashValue(units, hash);
    }
  }

  /*
   * And the refinery (ADR-024 §6, E4b), on the Bays' terms exactly: durable,
   * universe-layer, folded in key order.
   *
   * Jobs are in the fold because a refine job **is** state a replay has to
   * reproduce -- a shard that lost one would have eaten somebody's ore and
   * produced nothing. Tiers and projects are in it for the stronger reason that
   * they are permanent and communal: a project that completed on one machine
   * and not the other is two different universes.
   */
  for (const RefineJob& job : m_refineJobs)
  {
    hash = Neuron::HashValue(job.station, hash);
    hash = Neuron::HashValue(job.owner, hash);
    hash = Neuron::HashValue(job.sequence, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(job.alloy), hash);
    hash = Neuron::HashValue(job.batchUnits, hash);
    hash = Neuron::HashValue(job.outputUnits, hash);
    hash = Neuron::HashValue(job.durationTicks, hash);
    hash = Neuron::HashValue(job.completeTick, hash);
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      hash = Neuron::HashValue(job.inputUnits[ore], hash);
      hash = Neuron::HashValue(job.refundUnits[ore], hash);
    }
  }
  for (const StationTier& row : m_stationTiers)
  {
    hash = Neuron::HashValue(row.station, hash);
    hash = Neuron::HashValue(row.tier, hash);
  }
  for (const UpgradeProject& project : m_projects)
  {
    hash = Neuron::HashValue(project.station, hash);
    hash = Neuron::HashValue(project.toTier, hash);
    for (const std::uint32_t units : project.contributedUnits)
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
