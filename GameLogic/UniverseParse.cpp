#include "pch.h"

#include "UniverseParse.h"

#include "Json.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <unordered_set>

namespace Game
{
namespace
{

using Neuron::JsonDocument;
using Neuron::JsonError;
using Neuron::JsonKind;
using Neuron::JsonValue;

/*
 * Collects problems instead of returning on the first one (ADR-012 §C8), and
 * carries the JSON path so a message names the thing a person has to edit.
 */
class Reader
{
public:
  explicit Reader(std::vector<UniverseDiagnostic>& _diagnostics) noexcept : m_diagnostics(_diagnostics) {}

  void Fail(const JsonValue& _at, const std::string& _path, std::string _message)
  {
    m_diagnostics.push_back(UniverseDiagnostic{_at.Valid() ? _at.Line() : 0, _path, std::move(_message)});
  }

  [[nodiscard]] bool Ok() const noexcept { return m_diagnostics.empty(); }

  /// A required object member of a given kind. Records what was missing or
  /// wrong and returns an invalid handle, so the caller reads on and collects
  /// the rest of the file's problems in the same pass.
  [[nodiscard]] JsonValue Require(const JsonValue& _parent, std::string_view _name, JsonKind _kind, const std::string& _path)
  {
    const JsonValue member = _parent.Member(_name);
    if (!member.Valid())
    {
      Fail(_parent, _path, "missing '" + std::string(_name) + "'");
      return {};
    }
    if (member.Kind() != _kind)
    {
      Fail(member, _path + "." + std::string(_name), std::string("'") + std::string(_name) + "' is the wrong kind here");
      return {};
    }
    return member;
  }

  /*
   * A whole number, read exactly.
   *
   * `JsonKind::Number` is rejected rather than rounded, and that is the point
   * of ADR-012 §C7: a universe coordinate written as 1.2e19 has already lost
   * digits by the time it is a double, and a parser that accepted it would
   * place a station somewhere nobody authored.
   */
  [[nodiscard]] bool ReadInt64(const JsonValue& _parent, std::string_view _name, const std::string& _path, std::int64_t _min,
                               std::int64_t _max, std::int64_t& _outValue)
  {
    const JsonValue member = _parent.Member(_name);
    const std::string path = _path + "." + std::string(_name);
    if (!member.Valid())
    {
      Fail(_parent, _path, "missing '" + std::string(_name) + "'");
      return false;
    }
    if (member.Kind() == JsonKind::Number)
    {
      Fail(member, path, "must be a whole number -- universe values are exact integers, never rounded");
      return false;
    }
    if (member.Kind() != JsonKind::Integer)
    {
      Fail(member, path, "must be a whole number");
      return false;
    }

    const std::int64_t value = member.AsInt64();
    if (value < _min || value > _max)
    {
      Fail(member, path, "is out of range");
      return false;
    }
    _outValue = value;
    return true;
  }

  [[nodiscard]] bool ReadId(const JsonValue& _parent, std::string_view _name, const std::string& _path, std::uint16_t& _outId)
  {
    std::int64_t value = 0;
    // From 1: id 0 is reserved so a zero-initialised field reads as empty
    // rather than as a valid reference to the first thing in the file.
    if (!ReadInt64(_parent, _name, _path, 1, std::numeric_limits<std::uint16_t>::max(), value))
    {
      return false;
    }
    _outId = static_cast<std::uint16_t>(value);
    return true;
  }

  [[nodiscard]] bool ReadName(const JsonValue& _parent, std::string_view _key, const std::string& _path, std::string& _outName)
  {
    const JsonValue member = Require(_parent, _key, JsonKind::String, _path);
    if (!member.Valid())
    {
      return false;
    }
    _outName.assign(member.AsString());
    if (_outName.empty())
    {
      Fail(member, _path + "." + std::string(_key), "is empty");
      return false;
    }
    return true;
  }

  /// `_key` because an anchor's universe position is called `origin` -- it is
  /// where a grid sits, not where a body is -- and the two read the same way.
  [[nodiscard]] bool ReadPosition(const JsonValue& _parent, const std::string& _path, UniversePos& _outPosition,
                                  std::string_view _key = "position")
  {
    const JsonValue position = Require(_parent, _key, JsonKind::Object, _path);
    if (!position.Valid())
    {
      return false;
    }

    // A universe coordinate has no range to check beyond its own type: the whole
    // int64 plane is legal ground (ADR-009 §1).
    constexpr std::int64_t LOWEST = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t HIGHEST = std::numeric_limits<std::int64_t>::max();

    const std::string path = _path + "." + std::string(_key);
    std::int64_t x = 0;
    std::int64_t y = 0;
    const bool readX = ReadInt64(position, "x", path, LOWEST, HIGHEST, x);
    const bool readY = ReadInt64(position, "y", path, LOWEST, HIGHEST, y);
    if (!readX || !readY)
    {
      return false;
    }
    _outPosition = UniversePos{x, y};
    return true;
  }

private:
  std::vector<UniverseDiagnostic>& m_diagnostics;
};

[[nodiscard]] std::string IndexedPath(const std::string& _path, std::string_view _member, std::size_t _index)
{
  return _path + (_path.empty() ? "" : ".") + std::string(_member) + "[" + std::to_string(_index) + "]";
}

/// Warns about members the schema does not know, so a typo'd key is visible
/// rather than silently ignored. Content is stricter than config here: an
/// unknown key in a universe file means one half may be reading something the
/// other is not, and the hash would not catch it because it hashes what parsed.
void RejectUnknownKeys(Reader& _reader, const JsonValue& _object, std::initializer_list<const char*> _known, const std::string& _path)
{
  if (!_object.IsObject())
  {
    return;
  }
  for (std::size_t i = 0; i < _object.Count(); ++i)
  {
    const JsonValue member = _object.At(i);
    const std::string_view name = member.Name();
    const bool known = std::any_of(_known.begin(), _known.end(), [name](const char* _k) { return name == _k; });
    if (!known)
    {
      _reader.Fail(member, _path, "'" + std::string(name) + "' is not a known key");
    }
  }
}

void ReadCelestials(Reader& _reader, const JsonValue& _system, const std::string& _systemPath, SolarSystem& _outSystem)
{
  const JsonValue celestials = _system.Member("celestials");
  if (!celestials.Valid())
  {
    return; // 0..N is legal (ADR-009 §4).
  }
  if (!celestials.IsArray())
  {
    _reader.Fail(celestials, _systemPath + ".celestials", "must be a list");
    return;
  }

  std::unordered_set<CelestialId> seen;
  for (std::size_t i = 0; i < celestials.Count(); ++i)
  {
    const JsonValue entry = celestials.At(i);
    const std::string path = IndexedPath(_systemPath, "celestials", i);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "kind", "name", "position", "radiusMetres"}, path);

    Celestial celestial;
    const bool haveId = _reader.ReadId(entry, "id", path, celestial.id);
    std::string kindText;
    const bool haveKind = _reader.ReadName(entry, "kind", path, kindText);
    const bool haveName = _reader.ReadName(entry, "name", path, celestial.name);
    const bool havePosition = _reader.ReadPosition(entry, path, celestial.position);
    const bool haveRadius =
        _reader.ReadInt64(entry, "radiusMetres", path, 1, std::numeric_limits<std::int64_t>::max(), celestial.radiusMetres);

    if (haveKind && !ParseCelestialKindName(kindText, celestial.kind))
    {
      _reader.Fail(entry, path + ".kind", "'" + kindText + "' is not star, planet or moon");
      continue;
    }
    if (!haveId || !haveKind || !haveName || !havePosition || !haveRadius)
    {
      continue;
    }
    if (!seen.insert(celestial.id).second)
    {
      _reader.Fail(entry, path, "duplicate celestial id " + std::to_string(celestial.id));
      continue;
    }
    _outSystem.celestials.push_back(std::move(celestial));
  }
}

void ReadStations(Reader& _reader, const JsonValue& _system, const std::string& _systemPath, SolarSystem& _outSystem)
{
  const JsonValue stations = _reader.Require(_system, "stations", JsonKind::Array, _systemPath);
  if (!stations.Valid())
  {
    return;
  }

  // 1-2 per system is normative (ADR-009 §4), not a soft expectation: the
  // station is where a grid anchors, so a system with none has nowhere to play
  // and one with five is content that outgrew a decision nobody revisited.
  if (stations.Count() < 1 || stations.Count() > 2)
  {
    _reader.Fail(stations, _systemPath + ".stations", "a system carries 1 or 2 stations, not " + std::to_string(stations.Count()));
  }

  std::unordered_set<StationId> seen;
  for (std::size_t i = 0; i < stations.Count(); ++i)
  {
    const JsonValue entry = stations.At(i);
    const std::string path = IndexedPath(_systemPath, "stations", i);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "name", "position"}, path);

    Station station;
    const bool haveId = _reader.ReadId(entry, "id", path, station.id);
    const bool haveName = _reader.ReadName(entry, "name", path, station.name);
    const bool havePosition = _reader.ReadPosition(entry, path, station.position);
    if (!haveId || !haveName || !havePosition)
    {
      continue;
    }
    if (!seen.insert(station.id).second)
    {
      _reader.Fail(entry, path, "duplicate station id " + std::to_string(station.id));
      continue;
    }
    _outSystem.stations.push_back(std::move(station));
  }
}

void ReadGates(Reader& _reader, const JsonValue& _system, const std::string& _systemPath, SolarSystem& _outSystem)
{
  const JsonValue gates = _system.Member("gates");
  if (!gates.Valid())
  {
    return; // 0..N is legal; the MVP system has none.
  }
  if (!gates.IsArray())
  {
    _reader.Fail(gates, _systemPath + ".gates", "must be a list");
    return;
  }

  std::unordered_set<GateId> seen;
  for (std::size_t i = 0; i < gates.Count(); ++i)
  {
    const JsonValue entry = gates.At(i);
    const std::string path = IndexedPath(_systemPath, "gates", i);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "name", "to", "position"}, path);

    Gate gate;
    const bool haveId = _reader.ReadId(entry, "id", path, gate.id);
    const bool haveName = _reader.ReadName(entry, "name", path, gate.name);
    const bool haveTo = _reader.ReadId(entry, "to", path, gate.toSystem);
    const bool havePosition = _reader.ReadPosition(entry, path, gate.position);
    if (!haveId || !haveName || !haveTo || !havePosition)
    {
      continue;
    }
    if (!seen.insert(gate.id).second)
    {
      _reader.Fail(entry, path, "duplicate gate id " + std::to_string(gate.id));
      continue;
    }
    _outSystem.gates.push_back(std::move(gate));
  }
}

void ReadRegions(Reader& _reader, const JsonValue& _root, UniverseDef& _outUniverse)
{
  const JsonValue regions = _reader.Require(_root, "regions", JsonKind::Array, "");
  if (!regions.Valid())
  {
    return;
  }

  std::unordered_set<RegionId> seen;
  for (std::size_t i = 0; i < regions.Count(); ++i)
  {
    const JsonValue entry = regions.At(i);
    const std::string path = IndexedPath("", "regions", i);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "name", "securityFloor", "securityCeiling"}, path);

    Region region;
    const bool haveId = _reader.ReadId(entry, "id", path, region.id);
    const bool haveName = _reader.ReadName(entry, "name", path, region.name);

    // The band is optional: hand-authored content that predates the bake has
    // none, and a region with no stated band is simply unbanded rather than a
    // parse failure.
    std::int64_t floorValue = 0;
    std::int64_t ceilingValue = 0;
    if (_reader.ReadInt64(entry, "securityFloor", path, 0, 100, floorValue))
    {
      region.securityFloor = static_cast<std::uint8_t>(floorValue);
    }
    if (_reader.ReadInt64(entry, "securityCeiling", path, 0, 100, ceilingValue))
    {
      region.securityCeiling = static_cast<std::uint8_t>(ceilingValue);
    }
    if (region.securityCeiling < region.securityFloor)
    {
      _reader.Fail(entry, path, "securityCeiling is below securityFloor");
      continue;
    }

    if (!haveId || !haveName)
    {
      continue;
    }
    if (!seen.insert(region.id).second)
    {
      _reader.Fail(entry, path, "duplicate region id " + std::to_string(region.id));
      continue;
    }
    _outUniverse.regions.push_back(std::move(region));
  }
}

void ReadConstellations(Reader& _reader, const JsonValue& _root, UniverseDef& _outUniverse)
{
  const JsonValue constellations = _root.Member("constellations");
  if (!constellations.Valid())
  {
    return; // Optional: the hand-authored one-system universe has none.
  }
  if (!constellations.IsArray())
  {
    _reader.Fail(constellations, "constellations", "must be a list");
    return;
  }

  std::unordered_set<ConstellationId> seen;
  for (std::size_t i = 0; i < constellations.Count(); ++i)
  {
    const JsonValue entry = constellations.At(i);
    const std::string path = IndexedPath("", "constellations", i);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "region", "name"}, path);

    Constellation constellation;
    const bool haveId = _reader.ReadId(entry, "id", path, constellation.id);
    const bool haveRegion = _reader.ReadId(entry, "region", path, constellation.region);
    const bool haveName = _reader.ReadName(entry, "name", path, constellation.name);
    if (!haveId || !haveRegion || !haveName)
    {
      continue;
    }
    if (!seen.insert(constellation.id).second)
    {
      _reader.Fail(entry, path, "duplicate constellation id " + std::to_string(constellation.id));
      continue;
    }
    _outUniverse.constellations.push_back(std::move(constellation));
  }
}

/// A local offset in centimetres. Anchors carry three of them, and the grid
/// bound is checked here rather than in `ResolveReferences` because an offset
/// outside the grid is malformed content, not a dangling reference.
[[nodiscard]] bool ReadOffsetCm(Reader& _reader, const JsonValue& _parent, std::string_view _key, const std::string& _path,
                                LocalOffsetCm& _outOffset)
{
  const JsonValue offset = _reader.Require(_parent, _key, JsonKind::Object, _path);
  if (!offset.Valid())
  {
    return false;
  }
  const std::string path = _path + "." + std::string(_key);
  RejectUnknownKeys(_reader, offset, {"xCm", "yCm"}, path);

  constexpr std::int64_t BOUND = GRID_HALF_EXTENT_METRES * 100;
  std::int64_t x = 0;
  std::int64_t y = 0;
  const bool readX = _reader.ReadInt64(offset, "xCm", path, -BOUND, BOUND, x);
  const bool readY = _reader.ReadInt64(offset, "yCm", path, -BOUND, BOUND, y);
  _outOffset = LocalOffsetCm{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
  return readX && readY;
}

[[nodiscard]] bool ParseAnchorKindName(std::string_view _text, AnchorKind& _outKind) noexcept
{
  if (_text == "station")
  {
    _outKind = AnchorKind::Station;
    return true;
  }
  if (_text == "planet")
  {
    _outKind = AnchorKind::Planet;
    return true;
  }
  if (_text == "gate")
  {
    _outKind = AnchorKind::Gate;
    return true;
  }
  return false; // `site` is reserved and never baked (ADR-016 §3).
}

void ReadAnchors(Reader& _reader, const JsonValue& _system, const std::string& _systemPath, SolarSystem& _outSystem)
{
  const JsonValue anchors = _system.Member("anchors");
  if (!anchors.Valid())
  {
    return; // Optional, for the same reason constellations are.
  }
  if (!anchors.IsArray())
  {
    _reader.Fail(anchors, _systemPath + ".anchors", "must be a list");
    return;
  }

  for (std::size_t i = 0; i < anchors.Count(); ++i)
  {
    const JsonValue entry = anchors.At(i);
    const std::string path = IndexedPath(_systemPath, "anchors", i);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry,
                      {"id", "kind", "owner", "origin", "warpIn", "warpInFacingTurns16", "arrivalSpreadRadiusCm", "undock",
                       "undockFacingTurns16", "occupantIdBase", "occupantCount"},
                      path);

    Anchor anchor;
    anchor.system = _outSystem.id;
    const bool haveId = _reader.ReadId(entry, "id", path, anchor.id);
    std::string kindText;
    const bool haveKind = _reader.ReadName(entry, "kind", path, kindText);
    const bool haveOwner = _reader.ReadId(entry, "owner", path, anchor.owner);
    const bool haveOrigin = _reader.ReadPosition(entry, path, anchor.origin, "origin");
    const bool haveWarpIn = ReadOffsetCm(_reader, entry, "warpIn", path, anchor.warpInPoint);

    if (haveKind && !ParseAnchorKindName(kindText, anchor.kind))
    {
      _reader.Fail(entry, path + ".kind", "'" + kindText + "' is not station, planet or gate");
      continue;
    }

    std::int64_t facing = 0;
    const bool haveFacing = _reader.ReadInt64(entry, "warpInFacingTurns16", path, 0, 65535, facing);
    anchor.warpInFacingTurns16 = static_cast<std::uint16_t>(facing);

    std::int64_t spread = 0;
    if (_reader.ReadInt64(entry, "arrivalSpreadRadiusCm", path, 0, GRID_HALF_EXTENT_METRES * 100, spread))
    {
      anchor.arrivalSpreadRadiusCm = static_cast<std::int32_t>(spread);
    }

    bool haveUndock = true;
    if (anchor.kind == AnchorKind::Station)
    {
      haveUndock = ReadOffsetCm(_reader, entry, "undock", path, anchor.undockPoint);
      std::int64_t undockFacing = 0;
      haveUndock = _reader.ReadInt64(entry, "undockFacingTurns16", path, 0, 65535, undockFacing) && haveUndock;
      anchor.undockFacingTurns16 = static_cast<std::uint16_t>(undockFacing);
    }

    std::int64_t idBase = 0;
    std::int64_t occupants = 0;
    const bool haveIdBase = _reader.ReadInt64(entry, "occupantIdBase", path, 0, 0xffffffffll, idBase);
    const bool haveOccupants = _reader.ReadInt64(entry, "occupantCount", path, 0, 65535, occupants);
    anchor.occupantIdBase = static_cast<std::uint32_t>(idBase);
    anchor.occupantCount = static_cast<std::uint16_t>(occupants);

    if (!haveId || !haveKind || !haveOwner || !haveOrigin || !haveWarpIn || !haveFacing || !haveUndock || !haveIdBase ||
        !haveOccupants)
    {
      continue;
    }
    _outSystem.anchors.push_back(anchor);
  }
}

void ReadSystems(Reader& _reader, const JsonValue& _root, UniverseDef& _outUniverse)
{
  const JsonValue systems = _reader.Require(_root, "systems", JsonKind::Array, "");
  if (!systems.Valid())
  {
    return;
  }
  if (systems.Count() == 0)
  {
    _reader.Fail(systems, "systems", "a universe needs at least one system");
    return;
  }

  std::unordered_set<SystemId> seen;
  for (std::size_t i = 0; i < systems.Count(); ++i)
  {
    const JsonValue entry = systems.At(i);
    const std::string path = IndexedPath("", "systems", i);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry,
                      {"id", "region", "constellation", "name", "security", "starter", "centre", "celestials", "stations",
                       "gates", "anchors"},
                      path);

    SolarSystem system;
    const bool haveId = _reader.ReadId(entry, "id", path, system.id);
    const bool haveRegion = _reader.ReadId(entry, "region", path, system.region);
    const bool haveName = _reader.ReadName(entry, "name", path, system.name);

    (void)_reader.ReadId(entry, "constellation", path, system.constellation); // Optional.
    std::int64_t security = 0;
    if (_reader.ReadInt64(entry, "security", path, 0, 100, security))
    {
      system.security = static_cast<std::uint8_t>(security);
    }
    const JsonValue starter = entry.Member("starter");
    if (starter.Valid())
    {
      if (starter.Kind() != JsonKind::Bool)
      {
        _reader.Fail(starter, path + ".starter", "must be true or false");
      }
      else
      {
        system.starter = starter.AsBool();
      }
    }

    const JsonValue centre = _reader.Require(entry, "centre", JsonKind::Object, path);
    bool haveCentre = false;
    if (centre.Valid())
    {
      const std::string centrePath = path + ".centre";
      std::int64_t x = 0;
      std::int64_t y = 0;
      const bool readX =
          _reader.ReadInt64(centre, "x", centrePath, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), x);
      const bool readY =
          _reader.ReadInt64(centre, "y", centrePath, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), y);
      haveCentre = readX && readY;
      system.centre = UniversePos{x, y};
    }

    ReadCelestials(_reader, entry, path, system);
    ReadStations(_reader, entry, path, system);
    ReadGates(_reader, entry, path, system);
    ReadAnchors(_reader, entry, path, system);

    if (!haveId || !haveRegion || !haveName || !haveCentre)
    {
      continue;
    }
    if (!seen.insert(system.id).second)
    {
      _reader.Fail(entry, path, "duplicate system id " + std::to_string(system.id));
      continue;
    }
    _outUniverse.systems.push_back(std::move(system));
  }
}

/// Cross-references, checked once every table is populated. Doing this in a
/// second pass is what lets a gate name a system declared later in the file.
void ResolveReferences(Reader& _reader, const JsonValue& _root, UniverseDef& _outUniverse)
{
  for (const SolarSystem& system : _outUniverse.systems)
  {
    const bool regionExists = std::any_of(_outUniverse.regions.begin(), _outUniverse.regions.end(),
                                          [&system](const Region& _region) { return _region.id == system.region; });
    if (!regionExists)
    {
      _reader.Fail(_root, "systems", "system " + std::to_string(system.id) + " names region " + std::to_string(system.region) +
                                         ", which is not declared");
    }

    for (const Gate& gate : system.gates)
    {
      if (_outUniverse.FindSystem(gate.toSystem) == nullptr)
      {
        _reader.Fail(_root, "systems", "gate " + std::to_string(gate.id) + " in system " + std::to_string(system.id) +
                                           " leads to system " + std::to_string(gate.toSystem) + ", which is not declared");
      }
    }
  }

  if (_outUniverse.FindStation(_outUniverse.start.system, _outUniverse.start.station) == nullptr)
  {
    _reader.Fail(_root, "start", "start names system " + std::to_string(_outUniverse.start.system) + " station " +
                                     std::to_string(_outUniverse.start.station) + ", which is not declared");
  }
}

} // namespace

std::string UniverseDiagnostic::Text(std::string_view _file) const
{
  std::string text(_file);
  if (line != 0)
  {
    text += "(" + std::to_string(line) + ")";
  }
  if (!text.empty())
  {
    text += " ";
  }
  if (!path.empty())
  {
    text += path + ": ";
  }
  text += message;
  return text;
}

bool ParseUniverse(std::string_view _json, UniverseDef& _outUniverse, std::vector<UniverseDiagnostic>& _outDiagnostics)
{
  _outUniverse = UniverseDef{};

  JsonDocument document;
  std::vector<JsonError> jsonErrors;
  if (!JsonDocument::Parse(_json, document, jsonErrors))
  {
    for (const JsonError& error : jsonErrors)
    {
      _outDiagnostics.push_back(UniverseDiagnostic{error.line, {}, error.message});
    }
    if (_outDiagnostics.empty())
    {
      _outDiagnostics.push_back(UniverseDiagnostic{0, {}, "the universe definition is not valid JSON"});
    }
    return false;
  }

  Reader reader(_outDiagnostics);
  const JsonValue root = document.Root();
  if (!root.IsObject())
  {
    reader.Fail(root, "", "the universe definition must be an object");
    return false;
  }
  RejectUnknownKeys(reader, root, {"name", "regions", "constellations", "systems", "start"}, "");

  (void)reader.ReadName(root, "name", "", _outUniverse.name);

  const JsonValue start = reader.Require(root, "start", JsonKind::Object, "");
  if (start.Valid())
  {
    RejectUnknownKeys(reader, start, {"system", "station"}, "start");
    (void)reader.ReadId(start, "system", "start", _outUniverse.start.system);
    (void)reader.ReadId(start, "station", "start", _outUniverse.start.station);
  }

  ReadRegions(reader, root, _outUniverse);
  ReadConstellations(reader, root, _outUniverse);
  ReadSystems(reader, root, _outUniverse);
  ResolveReferences(reader, root, _outUniverse);

  if (!reader.Ok())
  {
    _outUniverse = UniverseDef{}; // Never hand back a half-read universe.
    return false;
  }
  return true;
}

} // namespace Game
