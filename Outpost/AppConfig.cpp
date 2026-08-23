#include "pch.h"

#include "AppConfig.h"

#include "JsonWriter.h"

#include <algorithm>
#include <initializer_list>

namespace Outpost
{
namespace
{

using Neuron::JsonKind;
using Neuron::JsonValue;

/// Warns about members the schema does not know. A newer config running against
/// an older build should start, so this is a warning; a wrong type is not.
void WarnUnknownKeys(const JsonValue& _object, std::initializer_list<const char*> _known, std::string_view _path,
                     ConfigDiagnostics& _diagnostics)
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
      const std::string prefix = _path.empty() ? std::string{} : std::string(_path) + ".";
      _diagnostics.warnings.push_back(prefix + std::string(name) + " is not a known setting (ignored)");
    }
  }
}

void ReadBool(const JsonValue& _parent, const char* _name, bool& _value, std::string_view _path, ConfigDiagnostics& _diagnostics)
{
  const JsonValue member = _parent.Member(_name);
  if (!member.Valid())
  {
    return;
  }
  if (member.Kind() != JsonKind::Bool)
  {
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " must be true or false");
    return;
  }
  _value = member.AsBool(_value);
}

void ReadText(const JsonValue& _parent, const char* _name, std::string& _value, std::string_view _path, ConfigDiagnostics& _diagnostics)
{
  const JsonValue member = _parent.Member(_name);
  if (!member.Valid())
  {
    return;
  }
  if (member.Kind() != JsonKind::String)
  {
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " must be text");
    return;
  }
  _value = std::string(member.AsString(_value));
}

void ReadDouble(const JsonValue& _parent, const char* _name, double& _value, double _min, double _max, std::string_view _path,
                ConfigDiagnostics& _diagnostics)
{
  const JsonValue member = _parent.Member(_name);
  if (!member.Valid())
  {
    return;
  }
  if (!member.IsNumeric())
  {
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " must be a number");
    return;
  }
  const double value = member.AsDouble(_value);
  if (value < _min || value > _max)
  {
    // Clamping silently would leave the file saying one thing and the game
    // doing another, which is the failure this whole section exists to avoid.
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " is out of range");
    return;
  }
  _value = value;
}

/// Integers are read through int64 and range-checked before narrowing, so a
/// port of 70000 is refused rather than wrapping to something plausible.
void ReadUInt(const JsonValue& _parent, const char* _name, std::uint32_t& _value, std::uint32_t _min, std::uint32_t _max,
              std::string_view _path, ConfigDiagnostics& _diagnostics)
{
  const JsonValue member = _parent.Member(_name);
  if (!member.Valid())
  {
    return;
  }
  if (member.Kind() != JsonKind::Integer)
  {
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " must be a whole number");
    return;
  }
  const std::int64_t value = member.AsInt64(_value);
  if (value < static_cast<std::int64_t>(_min) || value > static_cast<std::int64_t>(_max))
  {
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " is out of range");
    return;
  }
  _value = static_cast<std::uint32_t>(value);
}

void ReadPort(const JsonValue& _parent, const char* _name, std::uint16_t& _value, std::string_view _path, ConfigDiagnostics& _diagnostics)
{
  std::uint32_t wide = _value;
  ReadUInt(_parent, _name, wide, 0, 65535, _path, _diagnostics);
  _value = static_cast<std::uint16_t>(wide);
}

void ReadWindow(const JsonValue& _parent, WindowSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue window = _parent.Member("window");
  if (!window.Valid())
  {
    return;
  }
  WarnUnknownKeys(window, {"width", "height", "mode"}, "client.window", _diagnostics);
  ReadUInt(window, "width", _settings.width, 640, 16384, "client.window", _diagnostics);
  ReadUInt(window, "height", _settings.height, 480, 16384, "client.window", _diagnostics);
  ReadText(window, "mode", _settings.mode, "client.window", _diagnostics);
}

void ReadRenderer(const JsonValue& _parent, RendererSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue renderer = _parent.Member("renderer");
  if (!renderer.Valid())
  {
    return;
  }
  WarnUnknownKeys(renderer, {"vsync", "msaa", "frameCap", "hullScale", "uploadBytesPerFrame"}, "client.renderer", _diagnostics);
  ReadBool(renderer, "vsync", _settings.vsync, "client.renderer", _diagnostics);
  ReadUInt(renderer, "msaa", _settings.msaa, 1, 8, "client.renderer", _diagnostics);
  ReadUInt(renderer, "frameCap", _settings.frameCap, 0, 1000, "client.renderer", _diagnostics);
  // The floor is not zero: a scale of nothing is a fleet that does not draw,
  // which is a way to lose an evening. The ceiling is where a formation begins
  // to overlap itself.
  ReadDouble(renderer, "hullScale", _settings.hullScale, 0.25, 2.5, "client.renderer", _diagnostics);
  // Zero is the sentinel for "the client's own budget", so the range admits it
  // and the ceiling is a sanity bound rather than a tuning limit: 64 MiB is far
  // past any frame this renderer can compose and far short of a typo that
  // reserves a gigabyte of upload heap.
  ReadUInt(renderer, "uploadBytesPerFrame", _settings.uploadBytesPerFrame, 0, 64u * 1024u * 1024u, "client.renderer",
           _diagnostics);
}

void ReadAudio(const JsonValue& _parent, AudioSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue audio = _parent.Member("audio");
  if (!audio.Valid())
  {
    return;
  }
  WarnUnknownKeys(audio, {"enabled", "master", "world", "ui", "music", "alerts", "ambience", "voiceCap3D", "voiceCap2D"},
                  "client.audio", _diagnostics);
  ReadBool(audio, "enabled", _settings.enabled, "client.audio", _diagnostics);
  // Capped rather than open: these size fixed pools, and a bank that asked for
  // a thousand voices would be asking the mixer for the failure ADR-011 §3
  // exists to prevent.
  ReadUInt(audio, "voiceCap3D", _settings.voiceCap3D, 1, 256, "client.audio", _diagnostics);
  ReadUInt(audio, "voiceCap2D", _settings.voiceCap2D, 1, 128, "client.audio", _diagnostics);
  ReadDouble(audio, "master", _settings.master, 0.0, 1.0, "client.audio", _diagnostics);
  ReadDouble(audio, "world", _settings.world, 0.0, 1.0, "client.audio", _diagnostics);
  ReadDouble(audio, "ui", _settings.ui, 0.0, 1.0, "client.audio", _diagnostics);
  ReadDouble(audio, "music", _settings.music, 0.0, 1.0, "client.audio", _diagnostics);
  ReadDouble(audio, "alerts", _settings.alerts, 0.0, 1.0, "client.audio", _diagnostics);
  ReadDouble(audio, "ambience", _settings.ambience, 0.0, 1.0, "client.audio", _diagnostics);
}

void ReadUi(const JsonValue& _parent, UiSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue ui = _parent.Member("ui");
  if (!ui.Valid())
  {
    return;
  }
  WarnUnknownKeys(ui, {"scale", "palette", "font"}, "client.ui", _diagnostics);
  // 0.8-1.6 is the settings sheet's range, and the layout is scale-independent
  // rather than a scaled bitmap, so the floor is a real constraint.
  ReadDouble(ui, "scale", _settings.scale, 0.8, 1.6, "client.ui", _diagnostics);
  ReadText(ui, "palette", _settings.palette, "client.ui", _diagnostics);
  ReadText(ui, "font", _settings.font, "client.ui", _diagnostics);
}

void ReadDiagnostics(const JsonValue& _parent, DiagnosticsSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue diagnostics = _parent.Member("diagnostics");
  if (!diagnostics.Valid())
  {
    return;
  }
  WarnUnknownKeys(diagnostics, {"strip"}, "client.diagnostics", _diagnostics);
  ReadBool(diagnostics, "strip", _settings.strip, "client.diagnostics", _diagnostics);
}

/*
 * The wing call signs the player owns (ADR-012 §3, ADR-017 §6).
 *
 * A list rather than an object keyed by number, because the ids are sparse --
 * a player who composes wings 9 and 14 has named two things, and an object
 * would spell that as a file full of quoted integers. Each entry is a pair and
 * both halves are required: an id with no name names nothing, and a name with
 * no id belongs to nothing.
 *
 * Every refusal here leaves the *whole* list alone rather than dropping the bad
 * entry. A partial roster of call signs is the failure that is hardest to see:
 * the player finds one wing renamed and one not, and nothing says why. Losing
 * all of them says something is wrong with the file, which is true.
 */
void ReadWings(const JsonValue& _root, std::vector<WingName>& _value, ConfigDiagnostics& _diagnostics)
{
  const JsonValue wings = _root.Member("wings");
  if (!wings.Valid())
  {
    return;
  }
  if (!wings.IsArray())
  {
    _diagnostics.errors.push_back("wings must be a list of { id, name }");
    return;
  }

  std::vector<WingName> names;
  names.reserve(wings.Count());
  for (std::size_t i = 0; i < wings.Count(); ++i)
  {
    const JsonValue entry = wings.At(i);
    const std::string where = "wings[" + std::to_string(i) + "]";
    if (!entry.IsObject())
    {
      _diagnostics.errors.push_back(where + " must be an object with an id and a name");
      return;
    }
    WarnUnknownKeys(entry, {"id", "name"}, where, _diagnostics);

    const JsonValue id = entry.Member("id");
    if (id.Kind() != JsonKind::Integer)
    {
      _diagnostics.errors.push_back(where + ".id must be a whole number");
      return;
    }
    // 1..255 is `Game::WingId`'s whole usable range. Zero is INVALID_WING_ID --
    // the wing the stations are in, which draws no row -- so a name for it
    // would be a word nothing can ever show.
    const std::int64_t number = id.AsInt64(0);
    if (number < 1 || number > 255)
    {
      _diagnostics.errors.push_back(where + ".id is out of range");
      return;
    }

    const JsonValue name = entry.Member("name");
    if (name.Kind() != JsonKind::String)
    {
      _diagnostics.errors.push_back(where + ".name must be text");
      return;
    }
    const std::string_view text = name.AsString();
    if (text.empty())
    {
      // A wing named "" is worse than an unnamed one: the roster would draw a
      // row the player cannot read or point at.
      _diagnostics.errors.push_back(where + ".name is empty");
      return;
    }

    const auto duplicate =
      std::find_if(names.begin(), names.end(),
                   [number](const WingName& _existing) { return _existing.wing == static_cast<std::uint32_t>(number); });
    if (duplicate != names.end())
    {
      _diagnostics.errors.push_back(where + ".id names a wing that is already named");
      return;
    }
    names.push_back(WingName{static_cast<std::uint32_t>(number), std::string(text)});
  }

  _value = std::move(names);
}

/// A list of file names. Present-but-empty is an error rather than a fallback:
/// a config that says "no meshes" and gets nine of them is a config that lies.
void ReadTextList(const JsonValue& _parent, const char* _name, std::vector<std::string>& _value, std::string_view _path,
                  ConfigDiagnostics& _diagnostics)
{
  const JsonValue member = _parent.Member(_name);
  if (!member.Valid())
  {
    return;
  }
  if (!member.IsArray())
  {
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " must be a list of file names");
    return;
  }

  std::vector<std::string> names;
  names.reserve(member.Count());
  for (std::size_t i = 0; i < member.Count(); ++i)
  {
    const JsonValue entry = member.At(i);
    if (entry.Kind() != JsonKind::String)
    {
      _diagnostics.errors.push_back(std::string(_path) + "." + _name + " entry " + std::to_string(i) + " must be text");
      return;
    }
    names.emplace_back(entry.AsString());
  }

  if (names.empty())
  {
    _diagnostics.errors.push_back(std::string(_path) + "." + _name + " is empty");
    return;
  }
  _value = std::move(names);
}

void ReadContent(const JsonValue& _root, ContentSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue content = _root.Member("content");
  if (!content.Valid())
  {
    return;
  }
  WarnUnknownKeys(content, {"meshDirectory", "meshes", "audioDirectory", "soundBank"}, "content", _diagnostics);
  ReadText(content, "meshDirectory", _settings.meshDirectory, "content", _diagnostics);
  ReadTextList(content, "meshes", _settings.meshes, "content", _diagnostics);
  ReadText(content, "audioDirectory", _settings.audioDirectory, "content", _diagnostics);
  ReadText(content, "soundBank", _settings.soundBank, "content", _diagnostics);
}

void ReadClient(const JsonValue& _root, ClientSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue client = _root.Member("client");
  if (!client.Valid())
  {
    return;
  }
  WarnUnknownKeys(client, {"connect", "window", "renderer", "camera", "nebula", "audio", "ui", "diagnostics"}, "client",
                  _diagnostics);

  const JsonValue connect = client.Member("connect");
  if (connect.Valid())
  {
    WarnUnknownKeys(connect, {"host", "port"}, "client.connect", _diagnostics);
    ReadText(connect, "host", _settings.connectHost, "client.connect", _diagnostics);
    ReadPort(connect, "port", _settings.connectPort, "client.connect", _diagnostics);
  }

  ReadWindow(client, _settings.window, _diagnostics);
  ReadRenderer(client, _settings.renderer, _diagnostics);

  const JsonValue camera = client.Member("camera");
  if (camera.Valid())
  {
    WarnUnknownKeys(camera, {"zoomMetres", "yawSnapDegrees"}, "client.camera", _diagnostics);
    ReadDouble(camera, "zoomMetres", _settings.camera.zoomMetres, 500.0, 40000.0, "client.camera", _diagnostics);
    ReadDouble(camera, "yawSnapDegrees", _settings.camera.yawSnapDegrees, 0.0, 180.0, "client.camera", _diagnostics);
  }

  const JsonValue nebula = client.Member("nebula");
  if (nebula.Valid())
  {
    WarnUnknownKeys(nebula,
                    {"tintRed", "tintGreen", "tintBlue", "intensity", "tileMetres", "resolution", "octaves", "coverage",
                     "contrast", "seed"},
                    "client.nebula", _diagnostics);
    // The tint is linear rgb, so 1.0 is the ceiling and there is no headroom
    // above it to allow for: the pass adds, and the target is SDR (ADR-006 §2).
    ReadDouble(nebula, "tintRed", _settings.nebula.tintRed, 0.0, 1.0, "client.nebula", _diagnostics);
    ReadDouble(nebula, "tintGreen", _settings.nebula.tintGreen, 0.0, 1.0, "client.nebula", _diagnostics);
    ReadDouble(nebula, "tintBlue", _settings.nebula.tintBlue, 0.0, 1.0, "client.nebula", _diagnostics);
    ReadDouble(nebula, "intensity", _settings.nebula.intensity, 0.0, 1.0, "client.nebula", _diagnostics);
    // A tile smaller than the play area repeats inside one screen; the upper
    // bound just keeps a typo from flattening the field to a constant.
    ReadDouble(nebula, "tileMetres", _settings.nebula.tileMetres, 1000.0, 10000000.0, "client.nebula", _diagnostics);
    ReadUInt(nebula, "resolution", _settings.nebula.resolution, 16, 2048, "client.nebula", _diagnostics);
    ReadUInt(nebula, "octaves", _settings.nebula.octaves, 1, 8, "client.nebula", _diagnostics);
    ReadDouble(nebula, "coverage", _settings.nebula.coverage, 0.0, 0.99, "client.nebula", _diagnostics);
    ReadDouble(nebula, "contrast", _settings.nebula.contrast, 0.01, 8.0, "client.nebula", _diagnostics);
    ReadUInt(nebula, "seed", _settings.nebula.seed, 0, 0xffffffffu, "client.nebula", _diagnostics);
  }

  ReadAudio(client, _settings.audio, _diagnostics);
  ReadUi(client, _settings.ui, _diagnostics);
  ReadDiagnostics(client, _settings.diagnostics, _diagnostics);
}

} // namespace

const char* HostModeText(HostMode _mode) noexcept
{
  switch (_mode)
  {
    case HostMode::Host:
      return "host";
    case HostMode::Headless:
      return "headless";
    case HostMode::Client:
      return "client";
    case HostMode::Bake:
      return "bake";
  }
  return "?";
}

void ApplyConfigLayer(const JsonValue& _root, AppConfig& _config, ConfigDiagnostics& _diagnostics)
{
  if (!_root.IsObject())
  {
    _diagnostics.errors.push_back("the configuration root must be an object");
    return;
  }

  WarnUnknownKeys(_root,
                  {"mode", "selfTest", "logging", "universe", "economy", "persistence", "content", "server", "client",
                   "scenario"},
                  "",
                  _diagnostics);

  const JsonValue mode = _root.Member("mode");
  if (mode.Valid())
  {
    const std::string_view text = mode.AsString();
    if (text == "host")
    {
      _config.mode = HostMode::Host;
    }
    else if (text == "headless")
    {
      _config.mode = HostMode::Headless;
    }
    else if (text == "client")
    {
      _config.mode = HostMode::Client;
    }
    else if (text == "bake")
    {
      _config.mode = HostMode::Bake;
    }
    else
    {
      _diagnostics.errors.push_back("mode must be \"host\", \"headless\", \"client\" or \"bake\"");
    }
  }

  ReadBool(_root, "selfTest", _config.selfTest, "", _diagnostics);

  const JsonValue logging = _root.Member("logging");
  if (logging.Valid())
  {
    WarnUnknownKeys(logging, {"level", "file"}, "logging", _diagnostics);
    std::string level = "info";
    ReadText(logging, "level", level, "logging", _diagnostics);
    _config.logging.level = Neuron::Log::ParseLevel(level);
    ReadText(logging, "file", _config.logging.file, "logging", _diagnostics);
  }

  const JsonValue universe = _root.Member("universe");
  if (universe.Valid())
  {
    WarnUnknownKeys(universe, {"definition"}, "universe", _diagnostics);
    ReadText(universe, "definition", _config.universeDefinition, "universe", _diagnostics);
  }

  const JsonValue economy = _root.Member("economy");
  if (economy.Valid())
  {
    WarnUnknownKeys(economy, {"definition"}, "economy", _diagnostics);
    ReadText(economy, "definition", _config.economyDefinition, "economy", _diagnostics);
  }

  const JsonValue persistence = _root.Member("persistence");
  if (persistence.Valid())
  {
    WarnUnknownKeys(persistence, {"enabled", "directory"}, "persistence", _diagnostics);
    ReadBool(persistence, "enabled", _config.persistence.enabled, "persistence", _diagnostics);
    ReadText(persistence, "directory", _config.persistence.directory, "persistence", _diagnostics);
  }

  /*
   * The scenario knobs (`ScenarioSettings`), absent from the shipped file and
   * meaning "the world as it has always been" when they are.
   *
   * Read like every other section, which is the point: a state you cannot reach
   * is a state nobody checks, and the way to reach one here is a config layer
   * rather than a debug key or a build flag. A scratch directory holding only a
   * modified `Outpost.json` runs a variant without touching the repository.
   */
  const JsonValue scenario = _root.Member("scenario");
  if (scenario.Valid())
  {
    WarnUnknownKeys(scenario, {"secondFleetWings"}, "scenario", _diagnostics);
    // Capped at the starting fleet's own wing count: this spawns the same
    // authored fleet, so asking for more wings than there are is asking for
    // ships the table does not describe.
    ReadUInt(scenario, "secondFleetWings", _config.scenario.secondFleetWings, 0, 8, "scenario", _diagnostics);
  }

  const JsonValue server = _root.Member("server");
  if (server.Valid())
  {
    WarnUnknownKeys(server, {"port", "maxSessions"}, "server", _diagnostics);
    ReadPort(server, "port", _config.server.port, "server", _diagnostics);
    ReadUInt(server, "maxSessions", _config.server.maxSessions, 1, 1024, "server", _diagnostics);
  }

  ReadContent(_root, _config.content, _diagnostics);
  ReadClient(_root, _config.client, _diagnostics);
}

void ApplyUserLayer(const JsonValue& _root, AppConfig& _config, ConfigDiagnostics& _diagnostics)
{
  if (!_root.IsObject())
  {
    _diagnostics.warnings.push_back("the user settings file is not an object (ignored)");
    return;
  }

  // Only what the settings screen owns. A user layer cannot change the hosting
  // mode, the port or the universe -- those are deployment, not preference.
  WarnUnknownKeys(_root, {"client", "wings"}, "settings", _diagnostics);

  // Beside `client` rather than inside it, because it is not a client setting:
  // it is the player's own vocabulary, and ADR-012 §3 lists it as a family of
  // this file in its own right.
  ReadWings(_root, _config.wings, _diagnostics);

  const JsonValue client = _root.Member("client");
  if (!client.Valid())
  {
    return;
  }
  // Diagnostics is a user preference by design: the print's whole answer to
  // F11 is that the strip is a setting the settings screen owns.
  WarnUnknownKeys(client, {"window", "renderer", "audio", "ui", "diagnostics"}, "settings.client", _diagnostics);
  ReadWindow(client, _config.client.window, _diagnostics);
  ReadRenderer(client, _config.client.renderer, _diagnostics);
  ReadAudio(client, _config.client.audio, _diagnostics);
  ReadUi(client, _config.client.ui, _diagnostics);
  ReadDiagnostics(client, _config.client.diagnostics, _diagnostics);
}

namespace
{

/*
 * --- writing the user layer (ADR-012 §A3) ---------------------------------
 *
 * One key the player has changed, held back until its section knows it has any.
 *
 * The gathering pass exists because of what an empty section would mean. The
 * writer streams, so a section has to be opened before its members are known;
 * open one that turns out to have nothing in it and the file grows
 * `"renderer": {}` -- a line that says the player configured the renderer, when
 * what happened is that they did not. Collecting first is what lets a section be
 * *absent*, which is the only honest spelling of "unchanged".
 */
struct ChangedKey
{
  enum class Kind : std::uint8_t
  {
    Bool,
    Int,
    Number,
    Text
  };

  const char* name = nullptr;
  Kind kind = Kind::Bool;
  bool boolean = false;
  std::int64_t integer = 0;
  double number = 0.0;
  std::string text;
};

/*
 * `!=` on a double, deliberately, and it is the right comparison here rather
 * than a tolerance.
 *
 * Both sides came from the same load: `_shipped` is a copy taken before the
 * user layer was applied, so a value the player never touched is bit-identical
 * rather than merely close. A tolerance would do the opposite of what it looks
 * like -- it would discard a small change the player *did* make, which is the
 * one failure this function exists to prevent.
 */
void AddIfChanged(std::vector<ChangedKey>& _keys, const char* _name, double _value, double _shipped)
{
  if (_value != _shipped)
  {
    ChangedKey key;
    key.name = _name;
    key.kind = ChangedKey::Kind::Number;
    key.number = _value;
    _keys.push_back(std::move(key));
  }
}

void AddIfChanged(std::vector<ChangedKey>& _keys, const char* _name, bool _value, bool _shipped)
{
  if (_value != _shipped)
  {
    ChangedKey key;
    key.name = _name;
    key.kind = ChangedKey::Kind::Bool;
    key.boolean = _value;
    _keys.push_back(std::move(key));
  }
}

void AddIfChanged(std::vector<ChangedKey>& _keys, const char* _name, std::uint32_t _value, std::uint32_t _shipped)
{
  if (_value != _shipped)
  {
    ChangedKey key;
    key.name = _name;
    key.kind = ChangedKey::Kind::Int;
    key.integer = static_cast<std::int64_t>(_value);
    _keys.push_back(std::move(key));
  }
}

void AddIfChanged(std::vector<ChangedKey>& _keys, const char* _name, const std::string& _value, const std::string& _shipped)
{
  if (_value != _shipped)
  {
    ChangedKey key;
    key.name = _name;
    key.kind = ChangedKey::Kind::Text;
    key.text = _value;
    _keys.push_back(std::move(key));
  }
}

/// Emits one section, or nothing at all when the player changed none of it.
void WriteSection(Neuron::JsonWriter& _writer, const char* _name, const std::vector<ChangedKey>& _keys)
{
  if (_keys.empty())
  {
    return;
  }
  _writer.Key(_name);
  _writer.BeginObject();
  for (const ChangedKey& key : _keys)
  {
    switch (key.kind)
    {
    case ChangedKey::Kind::Bool:
      _writer.Member(key.name, key.boolean);
      break;
    case ChangedKey::Kind::Int:
      _writer.Member(key.name, key.integer);
      break;
    case ChangedKey::Kind::Number:
      _writer.Member(key.name, key.number);
      break;
    case ChangedKey::Kind::Text:
      _writer.Member(key.name, std::string_view(key.text));
      break;
    }
  }
  _writer.EndObject();
}

/*
 * The `client` object, present only if something under it changed.
 *
 * The key order is `ApplyUserLayer`'s reading order rather than the ADR's
 * sketch, so the two can be read side by side: a key that is written and never
 * read -- or the reverse -- shows up as a missing line instead of as a silent
 * asymmetry nobody notices until a setting stops surviving a restart.
 */
void WriteClient(Neuron::JsonWriter& _writer, const ClientSettings& _client, const ClientSettings& _shipped)
{
  std::vector<ChangedKey> window;
  AddIfChanged(window, "width", _client.window.width, _shipped.window.width);
  AddIfChanged(window, "height", _client.window.height, _shipped.window.height);
  AddIfChanged(window, "mode", _client.window.mode, _shipped.window.mode);

  std::vector<ChangedKey> renderer;
  AddIfChanged(renderer, "vsync", _client.renderer.vsync, _shipped.renderer.vsync);
  AddIfChanged(renderer, "msaa", _client.renderer.msaa, _shipped.renderer.msaa);
  AddIfChanged(renderer, "frameCap", _client.renderer.frameCap, _shipped.renderer.frameCap);
  AddIfChanged(renderer, "hullScale", _client.renderer.hullScale, _shipped.renderer.hullScale);
  AddIfChanged(renderer, "uploadBytesPerFrame", _client.renderer.uploadBytesPerFrame, _shipped.renderer.uploadBytesPerFrame);

  std::vector<ChangedKey> audio;
  AddIfChanged(audio, "enabled", _client.audio.enabled, _shipped.audio.enabled);
  AddIfChanged(audio, "voiceCap3D", _client.audio.voiceCap3D, _shipped.audio.voiceCap3D);
  AddIfChanged(audio, "voiceCap2D", _client.audio.voiceCap2D, _shipped.audio.voiceCap2D);
  AddIfChanged(audio, "master", _client.audio.master, _shipped.audio.master);
  AddIfChanged(audio, "world", _client.audio.world, _shipped.audio.world);
  AddIfChanged(audio, "ui", _client.audio.ui, _shipped.audio.ui);
  AddIfChanged(audio, "music", _client.audio.music, _shipped.audio.music);
  AddIfChanged(audio, "alerts", _client.audio.alerts, _shipped.audio.alerts);
  AddIfChanged(audio, "ambience", _client.audio.ambience, _shipped.audio.ambience);

  std::vector<ChangedKey> ui;
  AddIfChanged(ui, "scale", _client.ui.scale, _shipped.ui.scale);
  AddIfChanged(ui, "palette", _client.ui.palette, _shipped.ui.palette);
  AddIfChanged(ui, "font", _client.ui.font, _shipped.ui.font);

  std::vector<ChangedKey> diagnostics;
  AddIfChanged(diagnostics, "strip", _client.diagnostics.strip, _shipped.diagnostics.strip);

  if (window.empty() && renderer.empty() && audio.empty() && ui.empty() && diagnostics.empty())
  {
    return;
  }

  _writer.Key("client");
  _writer.BeginObject();
  WriteSection(_writer, "window", window);
  WriteSection(_writer, "renderer", renderer);
  WriteSection(_writer, "audio", audio);
  WriteSection(_writer, "ui", ui);
  WriteSection(_writer, "diagnostics", diagnostics);
  _writer.EndObject();
}

} // namespace

bool WriteUserLayer(const AppConfig& _config, const AppConfig& _shipped, std::string& _outText)
{
  _outText.clear();
  Neuron::JsonWriter writer(_outText);

  writer.BeginObject();
  WriteClient(writer, _config.client, _shipped.client);

  if (!_config.wings.empty())
  {
    /*
     * Sorted by wing number, and it is the write-elision that needs it rather
     * than tidiness.
     *
     * `SaveUserSettings` compares the composed text against the file already on
     * disk and writes nothing when they match. Names arrive here in the order
     * the session minted them, which differs run to run for the same set of
     * wings -- so without a canonical order the same settings would rewrite the
     * file on every exit, and a diff would show a reshuffle where nothing had
     * changed.
     */
    std::vector<WingName> ordered = _config.wings;
    std::sort(ordered.begin(), ordered.end(), [](const WingName& _a, const WingName& _b) { return _a.wing < _b.wing; });

    writer.Key("wings");
    writer.BeginArray();
    for (const WingName& wing : ordered)
    {
      writer.BeginObject();
      writer.Member("id", static_cast<std::int64_t>(wing.wing));
      writer.Member("name", std::string_view(wing.name));
      writer.EndObject();
    }
    writer.EndArray();
  }

  writer.EndObject();
  return writer.Complete();
}

} // namespace Outpost
