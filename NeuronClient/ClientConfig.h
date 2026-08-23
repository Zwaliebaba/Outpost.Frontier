#pragma once

#include "NebulaField.h"
#include "SignalLamp.h"

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

/*
 * Audio, as the settings sheet describes it (ADR-011 §2, ADR-012).
 *
 * Linear gains, one per submix plus the master, because the sliders a settings
 * screen draws are linear and two scales would eventually be converted wrongly
 * in one direction. Named fields rather than an array indexed by
 * `SoundCategory`: this struct is configuration, and configuration that has to
 * agree with an enum's ordering is configuration with a silent way to be wrong.
 */
struct AudioSettings
{
  bool enabled = true;

  float master = 1.0f;
  float world = 1.0f;
  float ambience = 0.7f;
  float ui = 0.8f;
  float music = 0.6f;
  float alerts = 1.0f;

  /// Concurrent source voices, split by what they cost (ADR-011 §3). The 3D
  /// pool is the one that meets a fleet, and its cap is the reason a 200-ship
  /// scene does not become 200 voices.
  std::uint32_t voiceCap3D = 32;
  std::uint32_t voiceCap2D = 16;
};

struct ClientConfig
{
  std::uint32_t windowWidth = 1600;
  std::uint32_t windowHeight = 900;
  std::string windowTitle = "Outpost: Frontier";
  bool borderlessFullscreen = false;

  bool vsync = true;
  std::uint32_t frameCap = 0; // 0 = uncapped; vsync usually makes this moot.
  bool enableDebugLayer = false;

  /// World-pass MSAA (S14): 1 disables, 4 is the shipped default. An
  /// unsupported count falls back to 1 at swapchain creation with a log line.
  std::uint32_t msaaSamples = 4;

  /*
   * The upload ring's per-frame segment, in bytes (ADR-018 A20).
   *
   * **Zero means the budget this client derives for itself** -- `UploadBudget.h`
   * sized from what the renderer is built to draw, which is the answer a
   * deployment should almost always want. The same sentence `ServerConfig`'s
   * tick budget carries, and for the same reason: a config written before this
   * existed says zero, and gets the right number.
   *
   * A non-zero value below `MIN_UPLOAD_BYTES_PER_FRAME` is raised to it and
   * logged. That floor is what the tactical view alone costs; below it the
   * client cannot draw a full grid at all, which is a broken game rather than a
   * retuned one.
   */
  std::uint32_t uploadBytesPerFrame = 0;

  /// Whether the Tier-1 diagnostics strip starts visible (`debug-hud.png`).
  /// Off by default and toggled at runtime by F1 -- the setting is where the
  /// toggle *lives*, the key is merely a shortcut to it.
  bool diagnosticsStrip = false;

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

  /// Where the WAVs live and which bank names them (ADR-011 §10-11). Told,
  /// like the meshes, rather than discovered: the client opens its own assets
  /// but never goes looking for them.
  std::string audioDirectory = "GameData/Audio";
  std::string soundBankFile = "GameData/Audio/SoundBank.json";
  AudioSettings audio;

  /// Camera start state and detent spacing (ADR-006 §4).
  float cameraZoomMetres = 8000.0f;

  /*
   * How wide each mesh in `meshFiles` should be drawn on the plane, in metres
   * and in the same order.
   *
   * The engine cannot work this out: a mesh arrives at whatever scale it was
   * modelled at, and only the game knows that one of these files is a 17 m
   * interceptor and another a 200 m station. Empty, or a zero in a slot, loads
   * that file exactly as authored.
   */
  std::vector<float> meshPlaneRadiiMetres;

  /*
   * Which signal-light fixture each mesh carries, in the same order
   * (ADR-006 §6a).
   *
   * The engine cannot work this out either, and for the same reason the radii
   * are here: "this file is a station and that one is a strike craft" is a
   * sentence only the game can say (ADR-014). What it says is a *rig* and not a
   * list of lamps -- where the lamps actually go is derived from the mesh's own
   * bounding box, so a re-export moves them with the art.
   *
   * Empty, or a shorter list than `meshFiles`, means `None` for the classes it
   * does not reach: a client whose game has no opinion about signal lights
   * draws none and is missing nothing.
   */
  std::vector<LampRig> meshLampRigs;

  float cameraYawSnapDegrees = 45.0f;

  /// A multiplier on HUD sizes, honoured from day one because the settings
  /// sheet makes 0.8-1.6x a requirement rather than a nicety.
  float uiScale = 1.0f;

  /// Which HUD colour table to resolve (`HudPalette`). "default" is the
  /// prints' phosphor table; the settings sheet's colour-vision palettes
  /// arrive as more names, which is why this is a string and not a bool.
  std::string uiPalette = "default";

  /*
   * Which of the game's per-entity status bits are worth a mark on the plane,
   * as a mask over `SceneEntity::statusBits` (ADR-014 4).
   *
   * **Zero by default, and the default is the point.** The engine carries that
   * byte and has no opinion about a single bit in it; the sentence "bit zero
   * means a ship is under undock protection, and a protected ship shimmers" is
   * one game's, so it is written where the composition root builds this struct
   * and nowhere in this library. A client whose game sets nothing here draws no
   * status marks and is missing nothing.
   */
  std::uint8_t statusMarkBits = 0;

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
