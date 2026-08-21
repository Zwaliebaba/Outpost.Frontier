#include "pch.h"

#include "DurableState.h"

#include "WorldRegistry.h"

#include "Hash.h"

#include <algorithm>
#include <string>

namespace Game
{

namespace
{

/*
 * The one place a diagnostic is made, so every one of them reads the same.
 *
 * The offset is taken from the reader rather than passed in, because the
 * interesting number is always "where the read that failed started looking" and
 * a caller computing it separately is a caller that will eventually compute it
 * wrong.
 */
void Report(std::vector<PersistenceDiagnostic>& _outDiagnostics, const Neuron::ByteReader& _reader, std::string _what,
            std::string _message)
{
  PersistenceDiagnostic diagnostic;
  diagnostic.byteOffset = static_cast<std::uint64_t>(_reader.BytesRead());
  diagnostic.what = std::move(_what);
  diagnostic.message = std::move(_message);
  _outDiagnostics.push_back(std::move(diagnostic));
}

/// "ship[12]", built without a stringstream because this is an error path and
/// the message is read by a person, not parsed.
[[nodiscard]] std::string Indexed(std::string_view _what, std::size_t _index)
{
  return std::string(_what) + "[" + std::to_string(_index) + "]";
}

void WriteOre(Neuron::ByteWriter& _writer, const std::uint32_t (&_oreUnits)[ORE_COUNT])
{
  for (const std::uint32_t units : _oreUnits)
  {
    _writer.WriteUInt32(units);
  }
}

void ReadOre(Neuron::ByteReader& _reader, std::uint32_t (&_outOreUnits)[ORE_COUNT])
{
  for (std::uint32_t& units : _outOreUnits)
  {
    units = _reader.ReadUInt32();
  }
}

void HashOre(std::uint64_t& _hash, const std::uint32_t (&_oreUnits)[ORE_COUNT]) noexcept
{
  for (const std::uint32_t units : _oreUnits)
  {
    _hash = Neuron::HashValue(units, _hash);
  }
}

void WriteMember(Neuron::ByteWriter& _writer, const TransferMember& _member)
{
  _writer.WriteUInt16(_member.shipId);
  _writer.WriteUInt8(static_cast<std::uint8_t>(_member.hullClass));
  _writer.WriteUInt8(_member.wing);
  WriteOre(_writer, _member.oreUnits);
}

[[nodiscard]] bool ReadHull(std::uint8_t _raw, HullClass& _outHull) noexcept
{
  if (_raw >= HULL_CLASS_COUNT)
  {
    return false;
  }
  _outHull = static_cast<HullClass>(_raw);
  return true;
}

} // namespace

std::string PersistenceDiagnostic::Text(std::string_view _file) const
{
  std::string text(_file);
  text += "(+";
  text += std::to_string(byteOffset);
  text += ") ";
  if (!what.empty())
  {
    text += what;
    text += ": ";
  }
  text += message;
  return text;
}

void CaptureDurableState(const WorldRegistry& _registry, DurableState& _outState)
{
  _outState = DurableState{};
  _outState.shardTick = _registry.ShardTick();
  _outState.nextDynamicShipId = _registry.NextDynamicShipId();

  /*
   * Ships in space, grid by grid, in anchor order and then in the order the
   * world's own tables hold them -- which is *not* id order, because removal is
   * swap-and-pop. So they are sorted afterwards: the hash folds this list and a
   * hash that depended on which ship happened to despawn last would be a hash
   * of the session's history rather than of its state.
   */
  for (const AnchorId anchor : _registry.LiveAnchors())
  {
    const World* world = _registry.Peek(anchor);
    if (world == nullptr)
    {
      continue;
    }
    const std::span<const ShipId> ids = world->Ids();
    for (std::uint32_t slot = 0; slot < ids.size(); ++slot)
    {
      const ShipId shipId = ids[slot];
      if (_registry.IsAuthoredOccupant(anchor, shipId))
      {
        continue; // Spin-up rebuilds these from the bake (ADR-018 D6a).
      }
      DurableShip ship;
      ship.shipId = shipId;
      ship.anchor = anchor;
      ship.hullClass = static_cast<HullClass>(world->Classes()[slot]);
      ship.wing = world->Wings()[slot];
      ship.owner = Neuron::SOLE_PLAYER_ID;
      ship.xMetres = world->Positions()[slot].x;
      ship.yMetres = world->Positions()[slot].y;
      ship.headingRadians = world->Headings()[slot];
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        ship.oreUnits[ore] = world->Cargo()[slot].oreUnits[ore];
      }
      _outState.ships.push_back(ship);
    }
  }
  std::sort(_outState.ships.begin(), _outState.ships.end(),
            [](const DurableShip& _a, const DurableShip& _b) noexcept
            {
              return _a.anchor != _b.anchor ? _a.anchor < _b.anchor : _a.shipId < _b.shipId;
            });

  for (const WorldRegistry::StationRoster& roster : _registry.Rosters())
  {
    DurableRoster row;
    row.anchor = roster.anchor;
    row.docked.assign(roster.docked.begin(), roster.docked.end());
    _outState.rosters.push_back(std::move(row));
  }

  const std::span<const StationBay> bays = _registry.Bays();
  _outState.bays.assign(bays.begin(), bays.end());

  const std::span<const SiteLedger> ledgers = _registry.Ledgers();
  _outState.ledgers.assign(ledgers.begin(), ledgers.end());

  const std::span<const TransferRecord> transfers = _registry.PendingTransfers();
  _outState.transfers.assign(transfers.begin(), transfers.end());
}

std::uint64_t DurableHash(const DurableState& _state) noexcept
{
  /*
   * §1's list, in §1a's order, and nothing else.
   *
   * Every count is folded before the things it counts, so a set that lost its
   * last entry cannot hash the same as one that never had it -- the failure
   * this hash exists to catch is *loss*, which is exactly the failure a
   * count-free fold is blind to.
   */
  std::uint64_t hash = Neuron::HashValue(_state.shardTick, Neuron::FNV_OFFSET_BASIS_64);
  hash = Neuron::HashValue(_state.nextDynamicShipId, hash);

  hash = Neuron::HashValue(static_cast<std::uint32_t>(_state.ships.size()), hash);
  for (const DurableShip& ship : _state.ships)
  {
    hash = Neuron::HashValue(ship.anchor, hash);
    hash = Neuron::HashValue(ship.shipId, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(ship.hullClass), hash);
    hash = Neuron::HashValue(ship.wing, hash);
    hash = Neuron::HashValue(ship.owner, hash);
    hash = Neuron::HashValue(ship.xMetres, hash);
    hash = Neuron::HashValue(ship.yMetres, hash);
    hash = Neuron::HashValue(ship.headingRadians, hash);
    HashOre(hash, ship.oreUnits);
  }

  hash = Neuron::HashValue(static_cast<std::uint32_t>(_state.rosters.size()), hash);
  for (const DurableRoster& roster : _state.rosters)
  {
    hash = Neuron::HashValue(roster.anchor, hash);
    hash = Neuron::HashValue(static_cast<std::uint32_t>(roster.docked.size()), hash);
    for (const RosterEntry& docked : roster.docked)
    {
      hash = Neuron::HashValue(docked.shipId, hash);
      hash = Neuron::HashValue(static_cast<std::uint8_t>(docked.hullClass), hash);
      hash = Neuron::HashValue(docked.wing, hash);
      HashOre(hash, docked.oreUnits);
    }
  }

  hash = Neuron::HashValue(static_cast<std::uint32_t>(_state.bays.size()), hash);
  for (const StationBay& bay : _state.bays)
  {
    hash = Neuron::HashValue(bay.station, hash);
    hash = Neuron::HashValue(bay.owner, hash);
    HashOre(hash, bay.oreUnits);
  }

  hash = Neuron::HashValue(static_cast<std::uint32_t>(_state.ledgers.size()), hash);
  for (const SiteLedger& ledger : _state.ledgers)
  {
    hash = Neuron::HashValue(ledger.anchor, hash);
    hash = Neuron::HashValue(ledger.epochIndex, hash);
    hash = Neuron::HashValue(ledger.layoutSalt, hash);
    hash = Neuron::HashValue(ledger.clusterCount, hash);
    for (std::uint8_t cluster = 0; cluster < ledger.clusterCount; ++cluster)
    {
      HashOre(hash, ledger.remainingUnits[cluster]);
    }
  }

  hash = Neuron::HashValue(static_cast<std::uint32_t>(_state.transfers.size()), hash);
  for (const TransferRecord& record : _state.transfers)
  {
    hash = Neuron::HashValue(record.id.host, hash);
    hash = Neuron::HashValue(record.id.counter, hash);
    hash = Neuron::HashValue(record.applyTick, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.kind), hash);
    hash = Neuron::HashValue(record.what.anchor, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.formation), hash);
    hash = Neuron::HashValue(record.what.cluster, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.ore), hash);
    hash = Neuron::HashValue(record.what.units, hash);
    hash = Neuron::HashValue(record.what.epoch, hash);
    hash = Neuron::HashValue(static_cast<std::uint8_t>(record.what.filledHold ? 1 : 0), hash);
    hash = Neuron::HashValue(record.what.memberCount, hash);
    for (std::uint16_t index = 0; index < record.what.memberCount; ++index)
    {
      const TransferMember& member = record.what.members[index];
      hash = Neuron::HashValue(member.shipId, hash);
      hash = Neuron::HashValue(static_cast<std::uint8_t>(member.hullClass), hash);
      hash = Neuron::HashValue(member.wing, hash);
      HashOre(hash, member.oreUnits);
    }
  }

  return hash;
}

std::uint64_t DurableHash(const WorldRegistry& _registry)
{
  DurableState state;
  CaptureDurableState(_registry, state);
  return DurableHash(state);
}

bool WriteDurableState(const DurableState& _state, Neuron::ByteWriter& _writer)
{
  _writer.WriteUInt32(DURABLE_MAGIC);
  _writer.WriteUInt32(DURABLE_FORMAT_VERSION);
  _writer.WriteUInt32(_state.shardTick);
  _writer.WriteUInt32(_state.nextDynamicShipId);

  _writer.WriteUInt32(static_cast<std::uint32_t>(_state.ships.size()));
  for (const DurableShip& ship : _state.ships)
  {
    _writer.WriteUInt16(ship.shipId);
    _writer.WriteUInt16(ship.anchor);
    _writer.WriteUInt8(static_cast<std::uint8_t>(ship.hullClass));
    _writer.WriteUInt8(ship.wing);
    _writer.WriteUInt32(ship.owner);
    _writer.WriteFloat(ship.xMetres);
    _writer.WriteFloat(ship.yMetres);
    _writer.WriteFloat(ship.headingRadians);
    WriteOre(_writer, ship.oreUnits);
  }

  _writer.WriteUInt32(static_cast<std::uint32_t>(_state.rosters.size()));
  for (const DurableRoster& roster : _state.rosters)
  {
    _writer.WriteUInt16(roster.anchor);
    _writer.WriteUInt16(static_cast<std::uint16_t>(roster.docked.size()));
    for (const RosterEntry& docked : roster.docked)
    {
      _writer.WriteUInt16(docked.shipId);
      _writer.WriteUInt8(static_cast<std::uint8_t>(docked.hullClass));
      _writer.WriteUInt8(docked.wing);
      WriteOre(_writer, docked.oreUnits);
    }
  }

  _writer.WriteUInt32(static_cast<std::uint32_t>(_state.bays.size()));
  for (const StationBay& bay : _state.bays)
  {
    _writer.WriteUInt32(bay.owner);
    _writer.WriteUInt16(bay.station);
    WriteOre(_writer, bay.oreUnits);
  }

  _writer.WriteUInt32(static_cast<std::uint32_t>(_state.ledgers.size()));
  for (const SiteLedger& ledger : _state.ledgers)
  {
    _writer.WriteUInt16(ledger.anchor);
    _writer.WriteUInt32(ledger.epochIndex);
    _writer.WriteUInt32(ledger.layoutSalt);
    _writer.WriteUInt8(ledger.clusterCount);
    for (std::uint8_t cluster = 0; cluster < ledger.clusterCount; ++cluster)
    {
      WriteOre(_writer, ledger.remainingUnits[cluster]);
    }
  }

  _writer.WriteUInt32(static_cast<std::uint32_t>(_state.transfers.size()));
  for (const TransferRecord& record : _state.transfers)
  {
    _writer.WriteUInt16(record.id.host);
    _writer.WriteUInt32(record.id.counter);
    _writer.WriteUInt32(record.applyTick);
    _writer.WriteUInt8(static_cast<std::uint8_t>(record.what.kind));
    _writer.WriteUInt16(record.what.anchor);
    _writer.WriteUInt8(static_cast<std::uint8_t>(record.what.formation));
    _writer.WriteUInt8(record.what.cluster);
    _writer.WriteUInt8(static_cast<std::uint8_t>(record.what.ore));
    _writer.WriteUInt32(record.what.units);
    _writer.WriteUInt32(record.what.epoch);
    _writer.WriteBool(record.what.filledHold);
    _writer.WriteUInt16(record.what.memberCount);
    for (std::uint16_t index = 0; index < record.what.memberCount; ++index)
    {
      WriteMember(_writer, record.what.members[index]);
    }
  }

  return _writer.Ok();
}

bool WriteDurableState(const WorldRegistry& _registry, Neuron::ByteWriter& _writer)
{
  DurableState state;
  CaptureDurableState(_registry, state);
  return WriteDurableState(state, _writer);
}

bool ReadDurableState(Neuron::ByteReader& _reader, DurableState& _outState, std::vector<PersistenceDiagnostic>& _outDiagnostics)
{
  DurableState state;

  const std::uint32_t magic = _reader.ReadUInt32();
  if (!_reader.Ok() || magic != DURABLE_MAGIC)
  {
    Report(_outDiagnostics, _reader, "header", "not durable state: magic is " + std::to_string(magic));
    return false;
  }

  const std::uint32_t version = _reader.ReadUInt32();
  if (!_reader.Ok() || version != DURABLE_FORMAT_VERSION)
  {
    /*
     * ADR-025 §6.1: no silent upgrade path. Naming both numbers is the whole
     * of the message, because the operator's next question is which build
     * wrote it.
     */
    Report(_outDiagnostics, _reader, "header",
           "format version " + std::to_string(version) + ", this build knows " + std::to_string(DURABLE_FORMAT_VERSION));
    return false;
  }

  state.shardTick = _reader.ReadUInt32();
  state.nextDynamicShipId = _reader.ReadUInt32();

  const std::uint32_t shipCount = _reader.ReadUInt32();
  if (!_reader.Ok())
  {
    Report(_outDiagnostics, _reader, "header", "ended before the ship count");
    return false;
  }
  state.ships.reserve(shipCount);
  for (std::uint32_t index = 0; index < shipCount; ++index)
  {
    DurableShip ship;
    ship.shipId = _reader.ReadUInt16();
    ship.anchor = _reader.ReadUInt16();
    const std::uint8_t hull = _reader.ReadUInt8();
    ship.wing = _reader.ReadUInt8();
    ship.owner = _reader.ReadUInt32();
    ship.xMetres = _reader.ReadFloat();
    ship.yMetres = _reader.ReadFloat();
    ship.headingRadians = _reader.ReadFloat();
    ReadOre(_reader, ship.oreUnits);
    if (!_reader.Ok())
    {
      Report(_outDiagnostics, _reader, Indexed("ship", index), "ended mid-record");
      return false;
    }
    if (!ReadHull(hull, ship.hullClass))
    {
      Report(_outDiagnostics, _reader, Indexed("ship", index), "hull class " + std::to_string(hull) + " does not exist");
      return false;
    }
    if (ship.anchor == INVALID_ID || ship.shipId == INVALID_SHIP_ID)
    {
      Report(_outDiagnostics, _reader, Indexed("ship", index), "names no ship or no place");
      return false;
    }
    state.ships.push_back(ship);
  }

  const std::uint32_t rosterCount = _reader.ReadUInt32();
  if (!_reader.Ok())
  {
    Report(_outDiagnostics, _reader, "rosters", "ended before the roster count");
    return false;
  }
  state.rosters.reserve(rosterCount);
  for (std::uint32_t index = 0; index < rosterCount; ++index)
  {
    DurableRoster roster;
    roster.anchor = _reader.ReadUInt16();
    const std::uint16_t dockedCount = _reader.ReadUInt16();
    if (!_reader.Ok())
    {
      Report(_outDiagnostics, _reader, Indexed("roster", index), "ended mid-record");
      return false;
    }
    roster.docked.reserve(dockedCount);
    for (std::uint16_t row = 0; row < dockedCount; ++row)
    {
      RosterEntry entry;
      entry.shipId = _reader.ReadUInt16();
      const std::uint8_t hull = _reader.ReadUInt8();
      entry.wing = _reader.ReadUInt8();
      ReadOre(_reader, entry.oreUnits);
      if (!_reader.Ok())
      {
        Report(_outDiagnostics, _reader, Indexed("roster", index), "ended inside its docked set");
        return false;
      }
      if (!ReadHull(hull, entry.hullClass))
      {
        Report(_outDiagnostics, _reader, Indexed("roster", index), "hull class " + std::to_string(hull) + " does not exist");
        return false;
      }
      roster.docked.push_back(entry);
    }
    state.rosters.push_back(std::move(roster));
  }

  const std::uint32_t bayCount = _reader.ReadUInt32();
  if (!_reader.Ok())
  {
    Report(_outDiagnostics, _reader, "bays", "ended before the Bay count");
    return false;
  }
  state.bays.reserve(bayCount);
  for (std::uint32_t index = 0; index < bayCount; ++index)
  {
    StationBay bay;
    bay.owner = _reader.ReadUInt32();
    bay.station = _reader.ReadUInt16();
    ReadOre(_reader, bay.oreUnits);
    if (!_reader.Ok())
    {
      Report(_outDiagnostics, _reader, Indexed("bay", index), "ended mid-record");
      return false;
    }
    state.bays.push_back(bay);
  }

  const std::uint32_t ledgerCount = _reader.ReadUInt32();
  if (!_reader.Ok())
  {
    Report(_outDiagnostics, _reader, "ledgers", "ended before the ledger count");
    return false;
  }
  state.ledgers.reserve(ledgerCount);
  for (std::uint32_t index = 0; index < ledgerCount; ++index)
  {
    SiteLedger ledger;
    ledger.anchor = _reader.ReadUInt16();
    ledger.epochIndex = _reader.ReadUInt32();
    ledger.layoutSalt = _reader.ReadUInt32();
    ledger.clusterCount = _reader.ReadUInt8();
    if (!_reader.Ok())
    {
      Report(_outDiagnostics, _reader, Indexed("ledger", index), "ended mid-record");
      return false;
    }
    if (ledger.clusterCount > MAX_SITE_CLUSTERS)
    {
      // Bounded before it is used as an index, which is the one thing a
      // corrupt count must never be allowed to be.
      Report(_outDiagnostics, _reader, Indexed("ledger", index),
             std::to_string(ledger.clusterCount) + " clusters, and a field holds at most " + std::to_string(MAX_SITE_CLUSTERS));
      return false;
    }
    for (std::uint8_t cluster = 0; cluster < ledger.clusterCount; ++cluster)
    {
      ReadOre(_reader, ledger.remainingUnits[cluster]);
    }
    if (!_reader.Ok())
    {
      Report(_outDiagnostics, _reader, Indexed("ledger", index), "ended inside its clusters");
      return false;
    }
    state.ledgers.push_back(ledger);
  }

  const std::uint32_t transferCount = _reader.ReadUInt32();
  if (!_reader.Ok())
  {
    Report(_outDiagnostics, _reader, "transfers", "ended before the transfer count");
    return false;
  }
  state.transfers.reserve(transferCount);
  for (std::uint32_t index = 0; index < transferCount; ++index)
  {
    TransferRecord record;
    record.id.host = _reader.ReadUInt16();
    record.id.counter = _reader.ReadUInt32();
    record.applyTick = _reader.ReadUInt32();
    const std::uint8_t kind = _reader.ReadUInt8();
    record.what.anchor = _reader.ReadUInt16();
    const std::uint8_t formation = _reader.ReadUInt8();
    record.what.cluster = _reader.ReadUInt8();
    const std::uint8_t ore = _reader.ReadUInt8();
    record.what.units = _reader.ReadUInt32();
    record.what.epoch = _reader.ReadUInt32();
    record.what.filledHold = _reader.ReadBool();
    record.what.memberCount = _reader.ReadUInt16();
    if (!_reader.Ok())
    {
      Report(_outDiagnostics, _reader, Indexed("transfer", index), "ended mid-record");
      return false;
    }
    if (kind > static_cast<std::uint8_t>(TransferKind::MineYield))
    {
      Report(_outDiagnostics, _reader, Indexed("transfer", index), "crossing kind " + std::to_string(kind) + " does not exist");
      return false;
    }
    if (formation > static_cast<std::uint8_t>(FormationId::Claw) || ore >= ORE_COUNT)
    {
      Report(_outDiagnostics, _reader, Indexed("transfer", index), "names a formation or an ore that does not exist");
      return false;
    }
    if (record.what.memberCount > MAX_SHIPS_PER_ORDER)
    {
      Report(_outDiagnostics, _reader, Indexed("transfer", index),
             std::to_string(record.what.memberCount) + " members, and a crossing carries at most " + std::to_string(MAX_SHIPS_PER_ORDER));
      return false;
    }
    record.what.kind = static_cast<TransferKind>(kind);
    record.what.formation = static_cast<FormationId>(formation);
    record.what.ore = static_cast<OreId>(ore);
    for (std::uint16_t member = 0; member < record.what.memberCount; ++member)
    {
      TransferMember& row = record.what.members[member];
      row.shipId = _reader.ReadUInt16();
      const std::uint8_t hull = _reader.ReadUInt8();
      row.wing = _reader.ReadUInt8();
      ReadOre(_reader, row.oreUnits);
      if (!_reader.Ok())
      {
        Report(_outDiagnostics, _reader, Indexed("transfer", index), "ended inside its members");
        return false;
      }
      if (!ReadHull(hull, row.hullClass))
      {
        Report(_outDiagnostics, _reader, Indexed("transfer", index), "hull class " + std::to_string(hull) + " does not exist");
        return false;
      }
    }
    state.transfers.push_back(record);
  }

  _outState = std::move(state);
  return true;
}

bool ReadDurableState(Neuron::ByteReader& _reader, WorldRegistry& _registry, std::vector<PersistenceDiagnostic>& _outDiagnostics)
{
  /*
   * Read into a local first, and only then hand it to the registry.
   *
   * That is what makes "the registry is left as it was found" true for a
   * truncated file: the bytes are all judged before anything is put anywhere,
   * so a file that ends halfway through its Bays cannot leave a shard holding
   * half of them.
   */
  DurableState state;
  if (!ReadDurableState(_reader, state, _outDiagnostics))
  {
    return false;
  }
  return _registry.LoadDurable(state, _outDiagnostics);
}

} // namespace Game
