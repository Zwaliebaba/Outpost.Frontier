#pragma once

#include "NebulaField.h"

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
   * Shaders are not here. They are compiled into the executable and handed to
   * `ClientApp::Initialise` as bytes, so there is no path to configure and no
   * way to point a build at a shader it was not built with.
   *
   * `meshFiles` is ordered, and the index is the classId that `InstanceRecord`
   * carries. The engine has no opinion about which index is a Carrier; the
   * composition root supplies the list, because the composition root is the
   * only thing that knows the game (ADR-014).
   */
  std::string meshDirectory = "GameData/Meshes";
  std::vector<std::string> meshFiles;
  std::string fontFamily = "Consolas";

  /// Camera start state and detent spacing (ADR-006 §4).
  float cameraZoomMetres = 8000.0f;
  float cameraYawSnapDegrees = 45.0f;

  /// A multiplier on HUD sizes, honoured from day one because the settings
  /// sheet makes 0.8-1.6x a requirement rather than a nicety.
  float uiScale = 1.0f;

  // The world used to be here: authored scenery, a world id and a grid anchor,
  // passed in as configuration. S5c moved all three behind `Neuron::WorldView`,
  // where they belong -- configuration is how the client is set up, not what it
  // is looking at, and an engine carrying world data in its config struct is an
  // engine that has an opinion about worlds (ADR-014).

  /// The ambient field behind the fleet (ADR-006 §1). A field that will not
  /// bake costs the frame its haze and nothing else -- the pass draws nothing
  /// rather than the client refusing to start.
  NebulaSettings nebula;

  // The handshake hashes used to be here. S5c moved them behind
  // `Neuron::WorldView`, which is asked for them at connect time: they describe
  // the *game* the client loaded, and a client told its own content hash by its
  // configuration cannot notice that its content changed underneath it.
};

} // namespace Neuron
