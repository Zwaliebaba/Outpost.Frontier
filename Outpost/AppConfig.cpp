#include "pch.h"

#include "AppConfig.h"

#include <algorithm>
#include <array>

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
      _diagnostics.warnings.push_back(std::string(_path) + "." + std::string(name) + " is not a known setting (ignored)");
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
  WarnUnknownKeys(renderer, {"vsync", "msaa", "frameCap"}, "client.renderer", _diagnostics);
  ReadBool(renderer, "vsync", _settings.vsync, "client.renderer", _diagnostics);
  ReadUInt(renderer, "msaa", _settings.msaa, 1, 8, "client.renderer", _diagnostics);
  ReadUInt(renderer, "frameCap", _settings.frameCap, 0, 1000, "client.renderer", _diagnostics);
}

void ReadAudio(const JsonValue& _parent, AudioSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue audio = _parent.Member("audio");
  if (!audio.Valid())
  {
    return;
  }
  WarnUnknownKeys(audio, {"master", "world", "ui", "music", "alerts", "ambience"}, "client.audio", _diagnostics);
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
  WarnUnknownKeys(ui, {"scale", "palette"}, "client.ui", _diagnostics);
  // 0.8-1.6 is the settings sheet's range, and the layout is scale-independent
  // rather than a scaled bitmap, so the floor is a real constraint.
  ReadDouble(ui, "scale", _settings.scale, 0.8, 1.6, "client.ui", _diagnostics);
  ReadText(ui, "palette", _settings.palette, "client.ui", _diagnostics);
}

void ReadClient(const JsonValue& _root, ClientSettings& _settings, ConfigDiagnostics& _diagnostics)
{
  const JsonValue client = _root.Member("client");
  if (!client.Valid())
  {
    return;
  }
  WarnUnknownKeys(client, {"connect", "window", "renderer", "camera", "audio", "ui"}, "client", _diagnostics);

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

  ReadAudio(client, _settings.audio, _diagnostics);
  ReadUi(client, _settings.ui, _diagnostics);
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

  WarnUnknownKeys(_root, {"mode", "selfTest", "logging", "universe", "server", "client"}, "", _diagnostics);

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
    else
    {
      _diagnostics.errors.push_back("mode must be \"host\", \"headless\" or \"client\"");
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

  const JsonValue server = _root.Member("server");
  if (server.Valid())
  {
    WarnUnknownKeys(server, {"port", "transport", "maxSessions"}, "server", _diagnostics);
    ReadPort(server, "port", _config.server.port, "server", _diagnostics);
    ReadText(server, "transport", _config.server.transport, "server", _diagnostics);
    if (_config.server.transport != "udp" && _config.server.transport != "quic")
    {
      _diagnostics.errors.push_back("server.transport must be \"udp\" or \"quic\"");
    }
    ReadUInt(server, "maxSessions", _config.server.maxSessions, 1, 1024, "server", _diagnostics);
  }

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
  WarnUnknownKeys(_root, {"client"}, "settings", _diagnostics);

  const JsonValue client = _root.Member("client");
  if (!client.Valid())
  {
    return;
  }
  WarnUnknownKeys(client, {"window", "renderer", "audio", "ui"}, "settings.client", _diagnostics);
  ReadWindow(client, _config.client.window, _diagnostics);
  ReadRenderer(client, _config.client.renderer, _diagnostics);
  ReadAudio(client, _config.client.audio, _diagnostics);
  ReadUi(client, _config.client.ui, _diagnostics);
}

} // namespace Outpost
