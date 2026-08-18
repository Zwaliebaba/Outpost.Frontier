#pragma once

#include <cstdint>
#include <string>
#include <vector>

/*
 * What the client needs to start, as a plain struct assembled by the
 * composition root (ADR-008 §3). NeuronClient never reads a file, an
 * environment variable or a command line.
 */

namespace Neuron
{

struct ClientConfig
{
  std::uint32_t windowWidth = 1600;
  std::uint32_t windowHeight = 900;
  std::string windowTitle = "Outpost: Frontier";
  bool borderlessFullscreen = false;

  bool vsync = true;
  std::uint32_t frameCap = 0; // 0 = uncapped; vsync usually makes this moot.
  bool enableDebugLayer = false;

  std::string serverHost = "127.0.0.1";
  std::uint16_t serverPort = 7777;
  std::string playerName;

  /*
   * Boot content. The client opens these files itself -- assets are its own
   * business (ADR-013 §6) -- but it is told where they are and never goes
   * looking, and it is told which meshes to load rather than deciding.
   *
   * `meshFiles` is ordered, and the index is the classId that `InstanceRecord`
   * carries. The engine has no opinion about which index is a Carrier; the
   * composition root supplies the list, because the composition root is the
   * only thing that knows the game (ADR-014).
   */
  std::string meshDirectory = "GameData/Meshes";
  std::string shaderDirectory = "GameData/Shaders";
  std::vector<std::string> meshFiles;
  std::string fontFamily = "Consolas";

  /// Camera start state and detent spacing (ADR-006 §4).
  float cameraZoomMetres = 8000.0f;
  float cameraYawSnapDegrees = 45.0f;

  /// A multiplier on HUD sizes, honoured from day one because the settings
  /// sheet makes 0.8-1.6x a requirement rather than a nicety.
  float uiScale = 1.0f;

  /// The game's message layout and authored content, as the simulation reports
  /// them. Compared at the handshake so mismatched builds refuse each other
  /// rather than disagreeing quietly (ADR-004 §3). The composition root supplies
  /// both: it is the only place that knows the engine and the game (ADR-014).
  std::uint64_t schemaHash = 0;
  std::uint64_t contentHash = 0;
};

} // namespace Neuron
