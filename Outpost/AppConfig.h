#pragma once

#include "Json.h"
#include "Log.h"

#include <cstdint>
#include <string>
#include <vector>

/*
 * The configuration surface (ADR-012 §B).
 *
 * Plain structs with defaults, assembled here in the composition root and
 * handed to the libraries. No library reads a file, an environment variable or
 * a command line -- there is no command line (ADR-008 §4).
 *
 * Reading is layered: defaults, then the shipped config, then the user layer.
 * Each pass uses the value already in the struct as its fallback, so a key a
 * layer does not mention is simply left alone. That is the whole merge.
 */

namespace Outpost
{

enum class HostMode : std::uint8_t
{
  Host,     // Server and client in one process -- the MVP default.
  Headless, // Server only. The standing proof it has no client dependency.
  Client    // Client only, connecting to client.connect.
};

struct LogSettings
{
  Neuron::LogLevel level = Neuron::LogLevel::Info;
  std::string file = "Outpost.log";
};

struct ServerSettings
{
  std::uint16_t port = 7777;
  std::uint32_t maxSessions = 8;
};

struct WindowSettings
{
  std::uint32_t width = 1600;
  std::uint32_t height = 900;
  std::string mode = "windowed";
};

struct RendererSettings
{
  bool vsync = true;
  std::uint32_t msaa = 4;
  std::uint32_t frameCap = 0;
};

struct CameraSettings
{
  double zoomMetres = 8000.0;
  double yawSnapDegrees = 45.0;
};

/*
 * The ambient field behind the fleet (ADR-006 §1's Nebula node).
 *
 * Art direction, so it is content and not a rebuild (ADR-012). The defaults
 * here mirror `Neuron::NebulaSettings` -- the engine's own struct, which this
 * one is mapped onto in the composition root rather than shared, because a
 * library taking a config struct from the host is exactly what ADR-012 forbids.
 */
struct NebulaSettings
{
  double tintRed = 0.06;
  double tintGreen = 0.22;
  double tintBlue = 0.10;
  double intensity = 0.35;
  double tileMetres = 96000.0;
  std::uint32_t resolution = 256;
  std::uint32_t octaves = 4;
  double coverage = 0.55;
  double contrast = 2.0;
  std::uint32_t seed = 1;
};

struct AudioSettings
{
  double master = 1.0;
  double world = 1.0;
  double ui = 0.8;
  double music = 0.6;
  double alerts = 1.0;
  double ambience = 0.7;
};

struct UiSettings
{
  double scale = 1.0;
  std::string palette = "default";
  std::string font = "Consolas"; // A monospace face for the glyph atlas (ADR-006 §9).
};

/*
 * Where the client's boot-time content lives, and which meshes it loads.
 *
 * The mesh list is ordered, and the order *is* the classId the renderer draws
 * with: the engine is game-free (ADR-014), so it has no opinion about which
 * index is a Carrier. Ships come first, smallest to largest, and the structure
 * last -- and the parked-fleet placeholder S5 renders reads it that way.
 */
struct ContentSettings
{
  std::string meshDirectory = "GameData/Meshes";
  std::string shaderDirectory = "GameData/Shaders";
  std::vector<std::string> meshes = {"Interceptor.obj", "Bomber.obj",  "Corvette.obj",   "Frigate.obj",  "Hauler.obj",
                                     "Miner.obj",       "Carrier.obj", "Battleship.obj", "Structure.obj"};
};

struct ClientSettings
{
  std::string connectHost = "127.0.0.1";
  std::uint16_t connectPort = 7777;
  WindowSettings window;
  RendererSettings renderer;
  CameraSettings camera;
  NebulaSettings nebula;
  AudioSettings audio;
  UiSettings ui;
};

struct AppConfig
{
  HostMode mode = HostMode::Host;
  bool selfTest = false;
  LogSettings logging;
  std::string universeDefinition = "GameData/Universe/Frontier.json";
  ServerSettings server;
  ClientSettings client;
  ContentSettings content;
};

struct ConfigDiagnostics
{
  std::vector<std::string> warnings; // Unknown keys, ignored user-layer sections.
  std::vector<std::string> errors;   // Type mismatches and bad enum values: fatal.

  [[nodiscard]] bool Ok() const noexcept { return errors.empty(); }
};

/// Applies one JSON layer over the values already in _config.
/// Unknown keys warn; wrong types are errors, because silent coercion is how a
/// configuration bug hides (ADR-012 §A4).
void ApplyConfigLayer(const Neuron::JsonValue& _root, AppConfig& _config, ConfigDiagnostics& _diagnostics);

/// The user layer owns only what the settings screen writes; anything else there is ignored.
void ApplyUserLayer(const Neuron::JsonValue& _root, AppConfig& _config, ConfigDiagnostics& _diagnostics);

[[nodiscard]] const char* HostModeText(HostMode _mode) noexcept;

} // namespace Outpost
