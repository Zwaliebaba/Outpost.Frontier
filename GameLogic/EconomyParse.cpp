#include "pch.h"

#include "EconomyParse.h"

#include "Json.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <string>

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
 *
 * Deliberately the same shape as `UniverseParse.cpp`'s reader rather than a
 * cleverer one. It is a *smaller* shape -- there are no universe coordinates
 * here, so nothing needs the full int64 range -- and the two are kept apart
 * because extracting a shared reader would edit a parser this slice has no
 * reason to touch. If a third content parser ever wants one, that is the slice
 * that should lift it out.
 */
class Reader
{
public:
  explicit Reader(std::vector<EconomyDiagnostic>& _diagnostics) noexcept : m_diagnostics(_diagnostics) {}

  void Fail(const JsonValue& _at, const std::string& _path, std::string _message)
  {
    m_diagnostics.push_back(EconomyDiagnostic{_at.Valid() ? _at.Line() : 0, _path, std::move(_message)});
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
      Fail(member, Join(_path, _name), std::string("'") + std::string(_name) + "' is the wrong kind here");
      return {};
    }
    return member;
  }

  /*
   * A whole number in range.
   *
   * `JsonKind::Number` is rejected rather than rounded, which is ADR-012 §C7
   * applied to balance: a hold written as 1.2e5 has already stopped being an
   * exact number of litres, and two clients that round it differently disagree
   * about when a hold is full.
   */
  [[nodiscard]] bool ReadUInt(const JsonValue& _parent, std::string_view _name, const std::string& _path, std::int64_t _min,
                              std::int64_t _max, std::int64_t& _outValue)
  {
    const JsonValue member = _parent.Member(_name);
    if (!member.Valid())
    {
      Fail(_parent, _path, "missing '" + std::string(_name) + "'");
      return false;
    }
    return ReadUIntValue(member, Join(_path, _name), _min, _max, _outValue);
  }

  /// Present-and-valid, or nothing. A missing optional key is not a problem;
  /// a present one that is malformed still is.
  [[nodiscard]] bool ReadOptionalUInt(const JsonValue& _parent, std::string_view _name, const std::string& _path,
                                      std::int64_t _min, std::int64_t _max, std::int64_t& _outValue)
  {
    if (!_parent.Member(_name).Valid())
    {
      return false;
    }
    return ReadUInt(_parent, _name, _path, _min, _max, _outValue);
  }

  /// The same, for a value already in hand -- an array element, which has no
  /// name to look up.
  [[nodiscard]] bool ReadUIntValue(const JsonValue& _value, const std::string& _path, std::int64_t _min, std::int64_t _max,
                                   std::int64_t& _outValue)
  {
    if (_value.Kind() == JsonKind::Number)
    {
      Fail(_value, _path, "must be a whole number -- economy values are exact integers, never rounded");
      return false;
    }
    if (_value.Kind() != JsonKind::Integer)
    {
      Fail(_value, _path, "must be a whole number");
      return false;
    }

    const std::int64_t value = _value.AsInt64();
    if (value < _min || value > _max)
    {
      Fail(_value, _path, "is out of range: expected " + std::to_string(_min) + " to " + std::to_string(_max));
      return false;
    }
    _outValue = value;
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
      Fail(member, Join(_path, _key), "is empty");
      return false;
    }
    return true;
  }

  /*
   * A fixed-length array of whole numbers.
   *
   * The length is checked before the contents, because "expected 3 entries,
   * found 2" is a more useful sentence than three complaints about the entry
   * that is not there.
   */
  [[nodiscard]] bool ReadUIntArray(const JsonValue& _parent, std::string_view _name, const std::string& _path,
                                   std::size_t _count, std::int64_t _min, std::int64_t _max, std::int64_t* _outValues)
  {
    const JsonValue member = Require(_parent, _name, JsonKind::Array, _path);
    if (!member.Valid())
    {
      return false;
    }
    const std::string path = Join(_path, _name);
    if (member.Count() != _count)
    {
      Fail(member, path, "expected " + std::to_string(_count) + " entries, found " + std::to_string(member.Count()));
      return false;
    }
    bool ok = true;
    for (std::size_t index = 0; index < _count; ++index)
    {
      if (!ReadUIntValue(member.At(index), path + "[" + std::to_string(index) + "]", _min, _max, _outValues[index]))
      {
        ok = false;
      }
    }
    return ok;
  }

  [[nodiscard]] static std::string Join(const std::string& _path, std::string_view _name)
  {
    return _path.empty() ? std::string(_name) : _path + "." + std::string(_name);
  }

  [[nodiscard]] static std::string Index(const std::string& _path, std::size_t _index)
  {
    return _path + "[" + std::to_string(_index) + "]";
  }

private:
  std::vector<EconomyDiagnostic>& m_diagnostics;
};

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

/*
 * A per-band object -- `{ "high": ..., "low": ..., "null": ... }`.
 *
 * Every band is required. A band left out would not be a default, it would be a
 * hole in the distribution that the bake would silently roll against.
 */
constexpr const char* BAND_KEYS[SECURITY_BAND_COUNT] = {"high", "low", "null"};

// ------------------------------------------------------------------- ores

void ReadOres(Reader& _reader, const JsonValue& _root, EconomyDef& _economy)
{
  const JsonValue array = _reader.Require(_root, "ores", JsonKind::Array, "");
  if (!array.Valid())
  {
    return;
  }

  bool seen[ORE_COUNT] = {};
  for (std::size_t index = 0; index < array.Count(); ++index)
  {
    const JsonValue entry = array.At(index);
    const std::string path = Reader::Index("ores", index);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "name", "unitVolumeLitres", "valueIndexPct"}, path);

    std::string id;
    if (!_reader.ReadName(entry, "id", path, id))
    {
      continue;
    }
    OreId ore = OreId::FerroChroma;
    if (!TryOreId(id, ore))
    {
      _reader.Fail(entry, path, "'" + id + "' is not a known ore");
      continue;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(ore);
    if (seen[slot])
    {
      _reader.Fail(entry, path, "ore '" + id + "' is listed twice");
      continue;
    }
    seen[slot] = true;

    OreInfo& info = _economy.ores[slot];
    (void)_reader.ReadName(entry, "name", path, info.name);

    std::int64_t value = 0;
    if (_reader.ReadUInt(entry, "unitVolumeLitres", path, 1, std::numeric_limits<std::uint32_t>::max(), value))
    {
      info.unitVolumeLitres = static_cast<std::uint32_t>(value);
    }
    if (_reader.ReadUInt(entry, "valueIndexPct", path, 1, 100000, value))
    {
      info.valueIndexPct = static_cast<std::uint32_t>(value);
    }
  }

  for (std::uint8_t slot = 0; slot < ORE_COUNT; ++slot)
  {
    if (!seen[slot])
    {
      _reader.Fail(array, "ores", std::string("missing ore '") + OreIdName(static_cast<OreId>(slot)) + "'");
    }
  }
}

// ----------------------------------------------------------------- alloys

void ReadAlloyInputs(Reader& _reader, const JsonValue& _entry, const std::string& _path, AlloyInfo& _info)
{
  const JsonValue inputs = _reader.Require(_entry, "inputs", JsonKind::Array, _path);
  if (!inputs.Valid())
  {
    return;
  }
  const std::string path = Reader::Join(_path, "inputs");
  if (inputs.Count() == 0)
  {
    _reader.Fail(inputs, path, "a recipe with no inputs makes something out of nothing");
    return;
  }

  for (std::size_t index = 0; index < inputs.Count(); ++index)
  {
    const JsonValue input = inputs.At(index);
    const std::string inputPath = Reader::Index(path, index);
    if (!input.IsObject())
    {
      _reader.Fail(input, inputPath, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, input, {"ore", "units"}, inputPath);

    std::string oreId;
    if (!_reader.ReadName(input, "ore", inputPath, oreId))
    {
      continue;
    }
    OreId ore = OreId::FerroChroma;
    if (!TryOreId(oreId, ore))
    {
      _reader.Fail(input, inputPath, "'" + oreId + "' is not a known ore");
      continue;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(ore);
    if (_info.inputUnits[slot] != 0)
    {
      _reader.Fail(input, inputPath, "ore '" + oreId + "' is listed twice in one recipe");
      continue;
    }

    std::int64_t units = 0;
    if (_reader.ReadUInt(input, "units", inputPath, 1, std::numeric_limits<std::uint16_t>::max(), units))
    {
      _info.inputUnits[slot] = static_cast<std::uint16_t>(units);
    }
  }
}

void ReadAlloys(Reader& _reader, const JsonValue& _root, EconomyDef& _economy)
{
  const JsonValue array = _reader.Require(_root, "alloys", JsonKind::Array, "");
  if (!array.Valid())
  {
    return;
  }

  bool seen[ALLOY_COUNT] = {};
  for (std::size_t index = 0; index < array.Count(); ++index)
  {
    const JsonValue entry = array.At(index);
    const std::string path = Reader::Index("alloys", index);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "name", "tier", "unitVolumeLitres", "refineSeconds", "fuelPerJob", "inputs"}, path);

    std::string id;
    if (!_reader.ReadName(entry, "id", path, id))
    {
      continue;
    }
    AlloyId alloy = AlloyId::FerrocitePlates;
    if (!TryAlloyId(id, alloy))
    {
      _reader.Fail(entry, path, "'" + id + "' is not a known alloy");
      continue;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(alloy);
    if (seen[slot])
    {
      _reader.Fail(entry, path, "alloy '" + id + "' is listed twice");
      continue;
    }
    seen[slot] = true;

    AlloyInfo& info = _economy.alloys[slot];
    (void)_reader.ReadName(entry, "name", path, info.name);

    std::int64_t value = 0;
    // Tiers run 1..3: the recipe sheet has three, and a refinery's
    // `recipeTierCap` is compared against this number.
    if (_reader.ReadUInt(entry, "tier", path, 1, 3, value))
    {
      info.tier = static_cast<std::uint8_t>(value);
    }
    if (_reader.ReadUInt(entry, "unitVolumeLitres", path, 1, std::numeric_limits<std::uint32_t>::max(), value))
    {
      info.unitVolumeLitres = static_cast<std::uint32_t>(value);
    }
    if (_reader.ReadUInt(entry, "refineSeconds", path, 1, 86400, value))
    {
      info.refineSeconds = static_cast<std::uint32_t>(value);
    }
    // Reserved at zero today (ADR-024 §6d), and read rather than assumed so the
    // market phase is a content edit.
    if (_reader.ReadUInt(entry, "fuelPerJob", path, 0, std::numeric_limits<std::uint32_t>::max(), value))
    {
      info.fuelPerJob = static_cast<std::uint32_t>(value);
    }

    ReadAlloyInputs(_reader, entry, path, info);
  }

  for (std::uint8_t slot = 0; slot < ALLOY_COUNT; ++slot)
  {
    if (!seen[slot])
    {
      _reader.Fail(array, "alloys", std::string("missing alloy '") + AlloyIdName(static_cast<AlloyId>(slot)) + "'");
    }
  }
}

// ------------------------------------------------------------------ cargo

void ReadCargo(Reader& _reader, const JsonValue& _root, EconomyDef& _economy)
{
  const JsonValue object = _reader.Require(_root, "cargo", JsonKind::Object, "");
  if (!object.Valid())
  {
    return;
  }

  for (std::size_t index = 0; index < object.Count(); ++index)
  {
    const JsonValue entry = object.At(index);
    const std::string_view hullName = entry.Name();
    const std::string path = Reader::Join("cargo", hullName);

    HullClass hull = HullClass::Interceptor;
    bool found = false;
    for (std::uint8_t raw = 0; raw < HULL_CLASS_COUNT && !found; ++raw)
    {
      const HullClass candidate = static_cast<HullClass>(raw);
      if (HullClassName(candidate) == hullName)
      {
        hull = candidate;
        found = true;
      }
    }
    if (!found)
    {
      _reader.Fail(entry, "cargo", "'" + std::string(hullName) + "' is not a hull class");
      continue;
    }
    // A class with no content can never spawn, so a hold for it is a hold
    // nothing will ever fill -- the same reasoning that keeps Fighter and
    // Cruiser out of every other table.
    if (!HullClassHasContent(hull))
    {
      _reader.Fail(entry, path, "'" + std::string(hullName) + "' is a reserved class and cannot carry cargo");
      continue;
    }
    // Stations and gates store in Bays, not hulls (ADR-024 §5a). They are hull
    // classes because a structure is a ship-table entry that never moves, which
    // does not make them freighters.
    if (hull == HullClass::Structure || hull == HullClass::Gate)
    {
      _reader.Fail(entry, path, "'" + std::string(hullName) + "' stores in a Station Bay, not in a hull");
      continue;
    }
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"oreHoldLitres", "generalHoldLitres"}, path);

    CargoInfo& info = _economy.cargo[static_cast<std::uint8_t>(hull)];
    std::int64_t value = 0;
    if (_reader.ReadOptionalUInt(entry, "oreHoldLitres", path, 1, std::numeric_limits<std::uint32_t>::max(), value))
    {
      info.oreHoldLitres = static_cast<std::uint32_t>(value);
    }
    if (_reader.ReadOptionalUInt(entry, "generalHoldLitres", path, 1, std::numeric_limits<std::uint32_t>::max(), value))
    {
      info.generalHoldLitres = static_cast<std::uint32_t>(value);
    }
    if (!info.CarriesAnything())
    {
      _reader.Fail(entry, path, "carries nothing -- omit the hull instead of authoring an empty hold");
    }
  }
}

// ----------------------------------------------------------------- mining

void ReadMining(Reader& _reader, const JsonValue& _root, EconomyDef& _economy)
{
  const JsonValue object = _reader.Require(_root, "mining", JsonKind::Object, "");
  if (!object.Valid())
  {
    return;
  }
  RejectUnknownKeys(_reader, object, {"cycleSeconds", "unitsPerCycle"}, "mining");

  std::int64_t value = 0;
  if (_reader.ReadUInt(object, "cycleSeconds", "mining", 1, 86400, value))
  {
    _economy.mining.cycleSeconds = static_cast<std::uint32_t>(value);
  }

  const JsonValue perCycle = _reader.Require(object, "unitsPerCycle", JsonKind::Object, "mining");
  if (!perCycle.Valid())
  {
    return;
  }
  RejectUnknownKeys(_reader, perCycle, {"ferroChroma", "astracite", "nebulite"}, "mining.unitsPerCycle");
  for (std::uint8_t slot = 0; slot < ORE_COUNT; ++slot)
  {
    const char* key = OreContentKey(static_cast<OreId>(slot));
    if (_reader.ReadUInt(perCycle, key, "mining.unitsPerCycle", 1, 100000, value))
    {
      _economy.mining.unitsPerCycle[slot] = static_cast<std::uint32_t>(value);
    }
  }
}

// ------------------------------------------------------------------ sites

/// A per-band object. Every band is required: a band left out would not be a
/// default, it would be a hole the bake rolls against.
[[nodiscard]] JsonValue RequireBands(Reader& _reader, const JsonValue& _parent, std::string_view _name, const std::string& _path)
{
  const JsonValue object = _reader.Require(_parent, _name, JsonKind::Object, _path);
  if (object.Valid())
  {
    RejectUnknownKeys(_reader, object, {"high", "low", "null"}, Reader::Join(_path, _name));
  }
  return object;
}

void ReadSiteGrades(Reader& _reader, const JsonValue& _sites, EconomyDef& _economy)
{
  const JsonValue array = _reader.Require(_sites, "grades", JsonKind::Array, "sites");
  if (!array.Valid())
  {
    return;
  }

  bool seen[SITE_GRADE_COUNT] = {};
  for (std::size_t index = 0; index < array.Count(); ++index)
  {
    const JsonValue entry = array.At(index);
    const std::string path = Reader::Index("sites.grades", index);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"grade", "poolUnits", "hazardScalePct"}, path);

    std::int64_t grade = 0;
    if (!_reader.ReadUInt(entry, "grade", path, 1, SITE_GRADE_COUNT, grade))
    {
      continue;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(grade - 1);
    if (seen[slot])
    {
      _reader.Fail(entry, path, "grade " + std::to_string(grade) + " is listed twice");
      continue;
    }
    seen[slot] = true;

    SiteGradeInfo& info = _economy.sites.grades[slot];
    std::int64_t value = 0;
    if (_reader.ReadUInt(entry, "poolUnits", path, 1, std::numeric_limits<std::uint32_t>::max(), value))
    {
      info.poolUnits = static_cast<std::uint32_t>(value);
    }
    if (_reader.ReadUInt(entry, "hazardScalePct", path, 0, 10000, value))
    {
      info.hazardScalePct = static_cast<std::uint32_t>(value);
    }
  }

  for (std::uint8_t slot = 0; slot < SITE_GRADE_COUNT; ++slot)
  {
    if (!seen[slot])
    {
      _reader.Fail(array, "sites.grades", "missing grade " + std::to_string(slot + 1));
    }
  }
}

/*
 * A hazard reads by kind: the keys that mean anything depend on it, so each
 * kind states its own known set. That is what makes a radiation field carrying
 * a heat threshold a diagnostic rather than a number nothing reads.
 */
void ReadHazard(Reader& _reader, const JsonValue& _entry, const std::string& _entryPath, SiteHazard& _hazard)
{
  const JsonValue object = _reader.Require(_entry, "hazard", JsonKind::Object, _entryPath);
  if (!object.Valid())
  {
    return;
  }
  const std::string path = Reader::Join(_entryPath, "hazard");

  std::string kind;
  if (!_reader.ReadName(object, "kind", path, kind))
  {
    return;
  }

  std::int64_t value = 0;
  if (kind == "none")
  {
    _hazard.kind = HazardKind::None;
    RejectUnknownKeys(_reader, object, {"kind"}, path);
    return;
  }
  if (kind == "radiation")
  {
    _hazard.kind = HazardKind::Radiation;
    RejectUnknownKeys(_reader, object, {"kind", "cycleSlowPerStackPct", "stackDecaySeconds", "burnStackThreshold"}, path);
    if (_reader.ReadUInt(object, "cycleSlowPerStackPct", path, 1, 100, value))
    {
      _hazard.cycleSlowPerStackPct = static_cast<std::uint16_t>(value);
    }
    if (_reader.ReadUInt(object, "stackDecaySeconds", path, 1, 3600, value))
    {
      _hazard.stackDecaySeconds = static_cast<std::uint16_t>(value);
    }
    if (_reader.ReadUInt(object, "burnStackThreshold", path, 1, 1000, value))
    {
      _hazard.burnStackThreshold = static_cast<std::uint16_t>(value);
    }
    return;
  }
  if (kind == "nebula")
  {
    _hazard.kind = HazardKind::Nebula;
    RejectUnknownKeys(_reader, object,
                      {"kind", "sensorDampingPctByGrade", "heatPerCycle", "heatThreshold", "heatDecayPerSecond", "lockoutSeconds",
                       "pacedCycleSlowPct", "detonationThreshold"},
                      path);

    std::int64_t damping[SITE_GRADE_COUNT] = {};
    if (_reader.ReadUIntArray(object, "sensorDampingPctByGrade", path, SITE_GRADE_COUNT, 1, 100, damping))
    {
      for (std::uint8_t slot = 0; slot < SITE_GRADE_COUNT; ++slot)
      {
        _hazard.sensorDampingPctByGrade[slot] = static_cast<std::uint8_t>(damping[slot]);
      }
    }
    if (_reader.ReadUInt(object, "heatPerCycle", path, 1, 10000, value))
    {
      _hazard.heatPerCycle = static_cast<std::uint16_t>(value);
    }
    if (_reader.ReadUInt(object, "heatThreshold", path, 1, 10000, value))
    {
      _hazard.heatThreshold = static_cast<std::uint16_t>(value);
    }
    if (_reader.ReadUInt(object, "heatDecayPerSecond", path, 0, 10000, value))
    {
      _hazard.heatDecayPerSecond = static_cast<std::uint16_t>(value);
    }
    if (_reader.ReadUInt(object, "lockoutSeconds", path, 1, 3600, value))
    {
      _hazard.lockoutSeconds = static_cast<std::uint16_t>(value);
    }
    if (_reader.ReadUInt(object, "pacedCycleSlowPct", path, 1, 500, value))
    {
      _hazard.pacedCycleSlowPct = static_cast<std::uint16_t>(value);
    }
    if (_reader.ReadUInt(object, "detonationThreshold", path, 1, 10000, value))
    {
      _hazard.detonationThreshold = static_cast<std::uint16_t>(value);
    }
    return;
  }

  _reader.Fail(object, Reader::Join(path, "kind"), "'" + kind + "' is not a known hazard kind");
}

void ReadComposition(Reader& _reader, const JsonValue& _entry, const std::string& _entryPath, SiteArchetypeInfo& _info)
{
  const JsonValue object = RequireBands(_reader, _entry, "compositionPct", _entryPath);
  if (!object.Valid())
  {
    return;
  }
  const std::string path = Reader::Join(_entryPath, "compositionPct");

  for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
  {
    std::int64_t shares[ORE_COUNT] = {};
    if (!_reader.ReadUIntArray(object, BAND_KEYS[band], path, ORE_COUNT, 0, 100, shares))
    {
      continue;
    }
    std::int64_t total = 0;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      _info.compositionPct[band][ore] = static_cast<std::uint8_t>(shares[ore]);
      total += shares[ore];
    }
    // A composition that does not total 100 is not a rounding question -- it is
    // a share of a pool, and the missing percent would be ore that exists in
    // the file and in no field.
    if (total != 100)
    {
      _reader.Fail(object, Reader::Join(path, BAND_KEYS[band]), "must total 100, not " + std::to_string(total));
    }
  }
}

void ReadArchetypes(Reader& _reader, const JsonValue& _sites, EconomyDef& _economy)
{
  const JsonValue array = _reader.Require(_sites, "archetypes", JsonKind::Array, "sites");
  if (!array.Valid())
  {
    return;
  }

  bool seen[SITE_ARCHETYPE_COUNT] = {};
  for (std::size_t index = 0; index < array.Count(); ++index)
  {
    const JsonValue entry = array.At(index);
    const std::string path = Reader::Index("sites.archetypes", index);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"id", "name", "hazard", "compositionPct", "gradeCapByBand"}, path);

    std::string id;
    if (!_reader.ReadName(entry, "id", path, id))
    {
      continue;
    }
    SiteArchetype archetype = SiteArchetype::FerrousField;
    if (!TrySiteArchetype(id, archetype))
    {
      _reader.Fail(entry, path, "'" + id + "' is not a known site archetype");
      continue;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(archetype);
    if (seen[slot])
    {
      _reader.Fail(entry, path, "archetype '" + id + "' is listed twice");
      continue;
    }
    seen[slot] = true;

    SiteArchetypeInfo& info = _economy.sites.archetypes[slot];
    (void)_reader.ReadName(entry, "name", path, info.name);
    ReadHazard(_reader, entry, path, info.hazard);
    ReadComposition(_reader, entry, path, info);

    // Optional, and partial on purpose: only High-Sec caps a pocket today
    // (ruling 1b), so a band absent here is "no cap" rather than a gap.
    const JsonValue caps = entry.Member("gradeCapByBand");
    if (caps.Valid())
    {
      const std::string capsPath = Reader::Join(path, "gradeCapByBand");
      if (!caps.IsObject())
      {
        _reader.Fail(caps, capsPath, "must be an object");
        continue;
      }
      RejectUnknownKeys(_reader, caps, {"high", "low", "null"}, capsPath);
      for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
      {
        std::int64_t cap = 0;
        if (_reader.ReadOptionalUInt(caps, BAND_KEYS[band], capsPath, 1, SITE_GRADE_COUNT, cap))
        {
          info.gradeCapByBand[band] = static_cast<std::uint8_t>(cap);
        }
      }
    }
  }

  for (std::uint8_t slot = 0; slot < SITE_ARCHETYPE_COUNT; ++slot)
  {
    if (!seen[slot])
    {
      _reader.Fail(array, "sites.archetypes",
                   std::string("missing archetype '") + SiteArchetypeName(static_cast<SiteArchetype>(slot)) + "'");
    }
  }
}

void ReadDistribution(Reader& _reader, const JsonValue& _sites, EconomyDef& _economy)
{
  const JsonValue object = _reader.Require(_sites, "distribution", JsonKind::Object, "sites");
  if (!object.Valid())
  {
    return;
  }
  const std::string path = "sites.distribution";
  RejectUnknownKeys(_reader, object,
                    {"bandSecurityFloors", "siteCountWeights", "archetypeWeights", "gradeRange", "highSecFirstSiteArchetype",
                     "maxSameArchetypePerSystem", "fadedPocketSystemsPerHighRegion", "minRegionOreGrade"},
                    path);

  SiteDistributionInfo& info = _economy.sites.distribution;

  const JsonValue floors = _reader.Require(object, "bandSecurityFloors", JsonKind::Object, path);
  if (floors.Valid())
  {
    const std::string floorsPath = Reader::Join(path, "bandSecurityFloors");
    RejectUnknownKeys(_reader, floors, {"high", "low"}, floorsPath);
    std::int64_t high = 0;
    std::int64_t low = 0;
    const bool readHigh = _reader.ReadUInt(floors, "high", floorsPath, 1, 100, high);
    const bool readLow = _reader.ReadUInt(floors, "low", floorsPath, 0, 100, low);
    if (readHigh && readLow)
    {
      if (high <= low)
      {
        _reader.Fail(floors, floorsPath, "the High floor must sit above the Low floor");
      }
      info.highSecurityFloor = static_cast<std::uint8_t>(high);
      info.lowSecurityFloor = static_cast<std::uint8_t>(low);
    }
  }

  const JsonValue counts = RequireBands(_reader, object, "siteCountWeights", path);
  const JsonValue archetypes = RequireBands(_reader, object, "archetypeWeights", path);
  const JsonValue grades = RequireBands(_reader, object, "gradeRange", path);
  const JsonValue sameCaps = RequireBands(_reader, object, "maxSameArchetypePerSystem", path);

  for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
  {
    const char* bandKey = BAND_KEYS[band];

    if (counts.Valid())
    {
      const std::string countsPath = Reader::Join(path, "siteCountWeights");
      std::int64_t weights[2] = {};
      if (_reader.ReadUIntArray(counts, bandKey, countsPath, 2, 0, 100, weights))
      {
        if (weights[0] + weights[1] != 100)
        {
          _reader.Fail(counts, Reader::Join(countsPath, bandKey), "must total 100");
        }
        info.siteCountWeights[band][0] = static_cast<std::uint8_t>(weights[0]);
        info.siteCountWeights[band][1] = static_cast<std::uint8_t>(weights[1]);
      }
    }

    if (archetypes.Valid())
    {
      const std::string archetypesPath = Reader::Join(path, "archetypeWeights");
      std::int64_t weights[SITE_ARCHETYPE_COUNT] = {};
      if (_reader.ReadUIntArray(archetypes, bandKey, archetypesPath, SITE_ARCHETYPE_COUNT, 0, 100, weights))
      {
        std::int64_t total = 0;
        for (std::uint8_t slot = 0; slot < SITE_ARCHETYPE_COUNT; ++slot)
        {
          info.archetypeWeights[band][slot] = static_cast<std::uint8_t>(weights[slot]);
          total += weights[slot];
        }
        if (total != 100)
        {
          _reader.Fail(archetypes, Reader::Join(archetypesPath, bandKey), "must total 100, not " + std::to_string(total));
        }
      }
    }

    if (grades.Valid())
    {
      const std::string gradesPath = Reader::Join(path, "gradeRange");
      std::int64_t range[2] = {};
      if (_reader.ReadUIntArray(grades, bandKey, gradesPath, 2, 1, SITE_GRADE_COUNT, range))
      {
        if (range[0] > range[1])
        {
          _reader.Fail(grades, Reader::Join(gradesPath, bandKey), "the low grade must not sit above the high one");
        }
        info.gradeMin[band] = static_cast<std::uint8_t>(range[0]);
        info.gradeMax[band] = static_cast<std::uint8_t>(range[1]);
      }
    }

    if (sameCaps.Valid())
    {
      const std::string capsPath = Reader::Join(path, "maxSameArchetypePerSystem");
      std::int64_t cap = 0;
      if (_reader.ReadUInt(sameCaps, bandKey, capsPath, 1, SITE_ARCHETYPE_COUNT, cap))
      {
        info.maxSameArchetypePerSystem[band] = static_cast<std::uint8_t>(cap);
      }
    }
  }

  std::string firstArchetype;
  if (_reader.ReadName(object, "highSecFirstSiteArchetype", path, firstArchetype))
  {
    SiteArchetype archetype = SiteArchetype::FerrousField;
    if (!TrySiteArchetype(firstArchetype, archetype))
    {
      _reader.Fail(object, Reader::Join(path, "highSecFirstSiteArchetype"), "'" + firstArchetype + "' is not a known archetype");
    }
    else
    {
      info.highSecFirstSiteArchetype = archetype;
    }
  }

  std::int64_t value = 0;
  if (_reader.ReadUInt(object, "fadedPocketSystemsPerHighRegion", path, 0, 255, value))
  {
    info.fadedPocketSystemsPerHighRegion = static_cast<std::uint8_t>(value);
  }
  if (_reader.ReadUInt(object, "minRegionOreGrade", path, 1, SITE_GRADE_COUNT, value))
  {
    info.minRegionOreGrade = static_cast<std::uint8_t>(value);
  }
}

/// A `{ "min": ..., "max": ... }` pair, checked for order in one place so the
/// two call sites cannot disagree about which way round it reads.
void ReadRange(Reader& _reader, const JsonValue& _parent, std::string_view _name, const std::string& _path, std::int64_t _floor,
               std::int64_t _ceiling, std::uint32_t& _outMin, std::uint32_t& _outMax)
{
  const JsonValue object = _reader.Require(_parent, _name, JsonKind::Object, _path);
  if (!object.Valid())
  {
    return;
  }
  const std::string path = Reader::Join(_path, _name);
  RejectUnknownKeys(_reader, object, {"min", "max"}, path);

  std::int64_t min = 0;
  std::int64_t max = 0;
  if (!_reader.ReadUInt(object, "min", path, _floor, _ceiling, min) || !_reader.ReadUInt(object, "max", path, _floor, _ceiling, max))
  {
    return;
  }
  if (min > max)
  {
    _reader.Fail(object, path, "min is above max");
    return;
  }
  _outMin = static_cast<std::uint32_t>(min);
  _outMax = static_cast<std::uint32_t>(max);
}

void ReadSites(Reader& _reader, const JsonValue& _root, EconomyDef& _economy)
{
  const JsonValue object = _reader.Require(_root, "sites", JsonKind::Object, "");
  if (!object.Valid())
  {
    return;
  }
  RejectUnknownKeys(_reader, object,
                    {"regenSeconds", "warpInStandoffMetres", "fieldRadiusMetres", "arrivalSpreadPctOfField", "clusterCount",
                     "grades", "archetypes", "distribution"},
                    "sites");

  SitesInfo& sites = _economy.sites;
  std::int64_t value = 0;
  if (_reader.ReadUInt(object, "regenSeconds", "sites", 1, 31536000, value))
  {
    sites.regenSeconds = static_cast<std::uint32_t>(value);
  }
  if (_reader.ReadUInt(object, "warpInStandoffMetres", "sites", 1, 40000, value))
  {
    sites.warpInStandoffMetres = static_cast<std::uint32_t>(value);
  }
  if (_reader.ReadUInt(object, "arrivalSpreadPctOfField", "sites", 0, 100, value))
  {
    sites.arrivalSpreadPctOfField = static_cast<std::uint32_t>(value);
  }

  // 40 km is ADR-001's grid bound: a field wider than the grid it sits on is
  // content the bake could never place, so it is refused where it is authored
  // rather than where it is baked.
  ReadRange(_reader, object, "fieldRadiusMetres", "sites", 1, 40000, sites.fieldRadiusMinMetres, sites.fieldRadiusMaxMetres);
  ReadRange(_reader, object, "clusterCount", "sites", 1, 255, sites.clusterCountMin, sites.clusterCountMax);

  ReadSiteGrades(_reader, object, _economy);
  ReadArchetypes(_reader, object, _economy);
  ReadDistribution(_reader, object, _economy);
}

// -------------------------------------------------------------- refining

void ReadBatches(Reader& _reader, const JsonValue& _refining, EconomyDef& _economy)
{
  const JsonValue array = _reader.Require(_refining, "batches", JsonKind::Array, "refining");
  if (!array.Valid())
  {
    return;
  }
  if (array.Count() == 0 || array.Count() > MAX_REFINE_BATCHES)
  {
    _reader.Fail(array, "refining.batches", "expected 1 to " + std::to_string(MAX_REFINE_BATCHES) + " batch sizes");
    return;
  }

  RefiningInfo& refining = _economy.refining;
  for (std::size_t index = 0; index < array.Count(); ++index)
  {
    const JsonValue entry = array.At(index);
    const std::string path = Reader::Index("refining.batches", index);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"units", "timePct"}, path);

    RefineBatchInfo batch;
    std::int64_t value = 0;
    if (_reader.ReadUInt(entry, "units", path, 1, 1000000, value))
    {
      batch.units = static_cast<std::uint32_t>(value);
    }
    if (_reader.ReadUInt(entry, "timePct", path, 1, 100, value))
    {
      batch.timePct = static_cast<std::uint32_t>(value);
    }
    if (batch.units == 0 || batch.timePct == 0)
    {
      continue;
    }
    refining.batches[refining.batchCount] = batch;
    ++refining.batchCount;
  }

  // Sorted here rather than required in order, so that reordering the array in
  // the file is a non-change to the hash -- the property the whole indexed
  // storage scheme exists to give.
  std::sort(refining.batches, refining.batches + refining.batchCount,
            [](const RefineBatchInfo& _a, const RefineBatchInfo& _b) { return _a.units < _b.units; });
  for (std::uint8_t index = 1; index < refining.batchCount; ++index)
  {
    if (refining.batches[index].units == refining.batches[index - 1].units)
    {
      _reader.Fail(array, "refining.batches", "batch size " + std::to_string(refining.batches[index].units) + " is listed twice");
    }
  }
}

void ReadTiers(Reader& _reader, const JsonValue& _refining, EconomyDef& _economy)
{
  const JsonValue array = _reader.Require(_refining, "tiers", JsonKind::Array, "refining");
  if (!array.Valid())
  {
    return;
  }

  bool seen[REFINERY_TIER_COUNT] = {};
  for (std::size_t index = 0; index < array.Count(); ++index)
  {
    const JsonValue entry = array.At(index);
    const std::string path = Reader::Index("refining.tiers", index);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"tier", "slotsPerPlayer", "recipeTierCap", "refundPct", "timePct"}, path);

    std::int64_t tier = 0;
    if (!_reader.ReadUInt(entry, "tier", path, 1, REFINERY_TIER_COUNT, tier))
    {
      continue;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(tier - 1);
    if (seen[slot])
    {
      _reader.Fail(entry, path, "tier " + std::to_string(tier) + " is listed twice");
      continue;
    }
    seen[slot] = true;

    RefineryTierInfo& info = _economy.refining.tiers[slot];
    std::int64_t value = 0;
    if (_reader.ReadUInt(entry, "slotsPerPlayer", path, 1, 255, value))
    {
      info.slotsPerPlayer = static_cast<std::uint8_t>(value);
    }
    if (_reader.ReadUInt(entry, "recipeTierCap", path, 1, 3, value))
    {
      info.recipeTierCap = static_cast<std::uint8_t>(value);
    }
    // Below 100 rather than at or below it: a refund that returned the whole
    // input would make refining free, and one that returned more would mint ore.
    if (_reader.ReadUInt(entry, "refundPct", path, 0, 99, value))
    {
      info.refundPct = static_cast<std::uint8_t>(value);
    }
    if (_reader.ReadUInt(entry, "timePct", path, 1, 100, value))
    {
      info.timePct = static_cast<std::uint8_t>(value);
    }
  }

  for (std::uint8_t slot = 0; slot < REFINERY_TIER_COUNT; ++slot)
  {
    if (!seen[slot])
    {
      _reader.Fail(array, "refining.tiers", "missing tier " + std::to_string(slot + 1));
    }
  }
}

void ReadUpgradeProjects(Reader& _reader, const JsonValue& _refining, EconomyDef& _economy)
{
  const JsonValue array = _reader.Require(_refining, "upgradeProjects", JsonKind::Array, "refining");
  if (!array.Valid())
  {
    return;
  }

  bool seen[REFINERY_TIER_COUNT] = {};
  for (std::size_t index = 0; index < array.Count(); ++index)
  {
    const JsonValue entry = array.At(index);
    const std::string path = Reader::Index("refining.upgradeProjects", index);
    if (!entry.IsObject())
    {
      _reader.Fail(entry, path, "must be an object");
      continue;
    }
    RejectUnknownKeys(_reader, entry, {"toTier", "costs"}, path);

    // From 2: tier 1 is the bake's authored floor and is not a project anyone
    // builds.
    std::int64_t toTier = 0;
    if (!_reader.ReadUInt(entry, "toTier", path, 2, REFINERY_TIER_COUNT, toTier))
    {
      continue;
    }
    const std::uint8_t slot = static_cast<std::uint8_t>(toTier - 1);
    if (seen[slot])
    {
      _reader.Fail(entry, path, "tier " + std::to_string(toTier) + " has two projects");
      continue;
    }
    seen[slot] = true;

    const JsonValue costs = _reader.Require(entry, "costs", JsonKind::Array, path);
    if (!costs.Valid())
    {
      continue;
    }
    const std::string costsPath = Reader::Join(path, "costs");
    if (costs.Count() == 0)
    {
      _reader.Fail(costs, costsPath, "a project that costs nothing is not a project");
      continue;
    }

    RefineryUpgradeProject& project = _economy.refining.upgradeProjects[slot];
    for (std::size_t costIndex = 0; costIndex < costs.Count(); ++costIndex)
    {
      const JsonValue cost = costs.At(costIndex);
      const std::string costPath = Reader::Index(costsPath, costIndex);
      if (!cost.IsObject())
      {
        _reader.Fail(cost, costPath, "must be an object");
        continue;
      }
      RejectUnknownKeys(_reader, cost, {"alloy", "units"}, costPath);

      std::string alloyId;
      if (!_reader.ReadName(cost, "alloy", costPath, alloyId))
      {
        continue;
      }
      AlloyId alloy = AlloyId::FerrocitePlates;
      if (!TryAlloyId(alloyId, alloy))
      {
        _reader.Fail(cost, costPath, "'" + alloyId + "' is not a known alloy");
        continue;
      }
      const std::uint8_t alloySlot = static_cast<std::uint8_t>(alloy);
      if (project.costUnits[alloySlot] != 0)
      {
        _reader.Fail(cost, costPath, "alloy '" + alloyId + "' is listed twice in one project");
        continue;
      }

      std::int64_t units = 0;
      if (_reader.ReadUInt(cost, "units", costPath, 1, std::numeric_limits<std::uint32_t>::max(), units))
      {
        project.costUnits[alloySlot] = static_cast<std::uint32_t>(units);
      }
    }
  }

  // Every tier above the floor needs a way to be reached, or it is a tier the
  // shard can read about and never build.
  for (std::uint8_t slot = 1; slot < REFINERY_TIER_COUNT; ++slot)
  {
    if (!seen[slot])
    {
      _reader.Fail(array, "refining.upgradeProjects", "tier " + std::to_string(slot + 1) + " has no upgrade project");
    }
  }
}

void ReadRefining(Reader& _reader, const JsonValue& _root, EconomyDef& _economy)
{
  const JsonValue object = _reader.Require(_root, "refining", JsonKind::Object, "");
  if (!object.Valid())
  {
    return;
  }
  RejectUnknownKeys(_reader, object, {"batches", "queueDepthPerPlayer", "tiers", "bandTierCaps", "upgradeProjects"}, "refining");

  std::int64_t value = 0;
  if (_reader.ReadUInt(object, "queueDepthPerPlayer", "refining", 1, 255, value))
  {
    _economy.refining.queueDepthPerPlayer = static_cast<std::uint8_t>(value);
  }

  ReadBatches(_reader, object, _economy);
  ReadTiers(_reader, object, _economy);

  const JsonValue caps = RequireBands(_reader, object, "bandTierCaps", "refining");
  if (caps.Valid())
  {
    for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
    {
      std::int64_t cap = 0;
      if (_reader.ReadUInt(caps, BAND_KEYS[band], "refining.bandTierCaps", 1, REFINERY_TIER_COUNT, cap))
      {
        _economy.refining.bandTierCaps[band] = static_cast<std::uint8_t>(cap);
      }
    }
  }

  ReadUpgradeProjects(_reader, object, _economy);
}

} // namespace

std::string EconomyDiagnostic::Text(std::string_view _file) const
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

bool ParseEconomy(std::string_view _json, EconomyDef& _outEconomy, std::vector<EconomyDiagnostic>& _outDiagnostics)
{
  _outEconomy = EconomyDef{};

  JsonDocument document;
  std::vector<JsonError> jsonErrors;
  if (!JsonDocument::Parse(_json, document, jsonErrors))
  {
    for (const JsonError& error : jsonErrors)
    {
      _outDiagnostics.push_back(EconomyDiagnostic{error.line, {}, error.message});
    }
    if (_outDiagnostics.empty())
    {
      _outDiagnostics.push_back(EconomyDiagnostic{0, {}, "the economy definition is not valid JSON"});
    }
    return false;
  }

  Reader reader(_outDiagnostics);
  const JsonValue root = document.Root();
  if (!root.IsObject())
  {
    reader.Fail(root, "", "the economy definition must be an object");
    return false;
  }
  RejectUnknownKeys(reader, root, {"ores", "alloys", "cargo", "mining", "sites", "refining"}, "");

  ReadOres(reader, root, _outEconomy);
  ReadAlloys(reader, root, _outEconomy);
  ReadCargo(reader, root, _outEconomy);
  ReadMining(reader, root, _outEconomy);
  ReadSites(reader, root, _outEconomy);
  ReadRefining(reader, root, _outEconomy);

  if (!reader.Ok())
  {
    _outEconomy = EconomyDef{}; // Never hand back a half-read economy.
    return false;
  }
  return true;
}

} // namespace Game
