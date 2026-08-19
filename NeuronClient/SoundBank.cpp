#include "pch.h"

#include "SoundBank.h"

#include "Json.h"

#include <algorithm>
#include <array>

namespace Neuron
{
namespace
{

/// Indexed by `SoundCategory`, and the order is the enum's order. The authored
/// spelling is what a bank file says and what a diagnostic quotes back.
constexpr std::array<std::string_view, SOUND_CATEGORY_COUNT> CATEGORY_NAMES = {"world3d", "ambience", "ui", "music",
                                                                              "alerts"};

/// A number the bank may omit, clamped to something the mixer can use. A
/// negative gain is not a quieter sound, it is an inverted waveform.
[[nodiscard]] float ReadGain(const JsonValue& _object, std::string_view _name, float _fallback) noexcept
{
  const JsonValue member = _object.Member(_name);
  if (!member.Valid() || !member.IsNumeric())
  {
    return _fallback;
  }
  return std::max(0.0f, static_cast<float>(member.AsDouble(_fallback)));
}

[[nodiscard]] std::string Diagnostic(const JsonValue& _value, std::string_view _message)
{
  return "SoundBank.json:" + std::to_string(_value.Line()) + ": " + std::string(_message);
}

} // namespace

std::string_view SoundCategoryName(SoundCategory _category) noexcept
{
  const auto index = static_cast<std::uint32_t>(_category);
  return index < SOUND_CATEGORY_COUNT ? CATEGORY_NAMES[index] : std::string_view{};
}

bool TryParseSoundCategory(std::string_view _name, SoundCategory& _outCategory) noexcept
{
  for (std::uint32_t index = 0; index < SOUND_CATEGORY_COUNT; ++index)
  {
    if (CATEGORY_NAMES[index] == _name)
    {
      _outCategory = static_cast<SoundCategory>(index);
      return true;
    }
  }
  return false;
}

bool SoundBank::Parse(std::string_view _text, SoundBank& _outBank, std::vector<std::string>& _outErrors)
{
  _outBank.m_sounds.clear();

  JsonDocument document;
  std::vector<JsonError> jsonErrors;
  if (!JsonDocument::Parse(_text, document, jsonErrors))
  {
    for (const JsonError& error : jsonErrors)
    {
      _outErrors.push_back("SoundBank.json:" + std::to_string(error.line) + ":" + std::to_string(error.column) + ": " +
                           error.message);
    }
    return false;
  }

  const JsonValue root = document.Root();
  if (!root.IsObject())
  {
    _outErrors.emplace_back("SoundBank.json: the bank must be an object");
    return false;
  }

  const JsonValue sounds = root.Member("sounds");
  if (!sounds.Valid() || !sounds.IsObject())
  {
    _outErrors.emplace_back("SoundBank.json: no 'sounds' object");
    return false;
  }

  // An object keyed by sound id rather than an array of records with an `id`
  // field: the id is the thing a caller names, and a map cannot express the
  // same id twice by accident the way an array can.
  const std::size_t count = sounds.Count();
  _outBank.m_sounds.reserve(count);

  for (std::size_t index = 0; index < count; ++index)
  {
    const JsonValue entry = sounds.At(index);
    const std::string_view id = entry.Name();
    if (id.empty())
    {
      _outErrors.push_back(Diagnostic(entry, "a sound with no id was skipped"));
      continue;
    }
    if (!entry.IsObject())
    {
      _outErrors.push_back(Diagnostic(entry, "sound '" + std::string(id) + "' is not an object and was skipped"));
      continue;
    }

    const JsonValue file = entry.Member("file");
    const std::string_view fileName = file.AsString();
    if (fileName.empty())
    {
      // No file is not a tunable default: it is a cue that can never sound, and
      // one that would fail later at a point that no longer knows its name.
      _outErrors.push_back(Diagnostic(entry, "sound '" + std::string(id) + "' has no 'file' and was skipped"));
      continue;
    }

    SoundCategory category = SoundCategory::Ui;
    const JsonValue categoryValue = entry.Member("category");
    const std::string_view categoryName = categoryValue.AsString();
    if (!TryParseSoundCategory(categoryName, category))
    {
      _outErrors.push_back(Diagnostic(entry, "sound '" + std::string(id) + "' has an unknown category '" +
                                                 std::string(categoryName) + "' and was skipped"));
      continue;
    }

    SoundDef def;
    def.id = std::string(id);
    def.file = std::string(fileName);
    def.category = category;
    def.gain = ReadGain(entry, "gain", 1.0f);
    def.pitchVariance = std::clamp(ReadGain(entry, "pitchVariance", 0.0f), 0.0f, 0.5f);
    def.maxInstances =
        static_cast<std::uint32_t>(std::max<std::int64_t>(1, entry.Member("maxInstances").AsInt64(8)));
    def.cooldownMs = std::max(0.0, entry.Member("cooldownMs").AsDouble(0.0));
    def.minDistanceMetres = ReadGain(entry, "minDistance", 50.0f);
    def.maxDistanceMetres = ReadGain(entry, "maxDistance", 5000.0f);

    // An inverted falloff is silent everywhere it should be loud, which reads
    // as a broken mixer rather than as a broken bank. Named, then repaired,
    // because the cue is otherwise fine.
    if (def.maxDistanceMetres <= def.minDistanceMetres)
    {
      _outErrors.push_back(Diagnostic(entry, "sound '" + std::string(id) +
                                                 "' has maxDistance <= minDistance; it was widened to minDistance + 1 m"));
      def.maxDistanceMetres = def.minDistanceMetres + 1.0f;
    }

    if (_outBank.Find(def.id) != nullptr)
    {
      _outErrors.push_back(Diagnostic(entry, "sound '" + def.id + "' is declared twice; the later one was skipped"));
      continue;
    }

    _outBank.m_sounds.push_back(std::move(def));
  }

  return true;
}

const SoundDef* SoundBank::Find(std::string_view _id) const noexcept
{
  const auto found =
      std::find_if(m_sounds.begin(), m_sounds.end(), [_id](const SoundDef& _def) { return _def.id == _id; });
  return found == m_sounds.end() ? nullptr : &*found;
}

} // namespace Neuron
