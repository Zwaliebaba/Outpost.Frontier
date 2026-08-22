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
  Client,   // Client only, connecting to client.connect.
  Bake      // Runs the universe bake, writes the JSON, exits (build order U1).
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

  /*
   * How big hulls are drawn, against the size the class table says they are.
   *
   * Art direction, which is why it is content and not a constant: 1.0 draws
   * every hull at exactly its own silhouette radius -- contact radius times
   * `Game::SILHOUETTE_RADIUS_PER_CONTACT_RADIUS` -- which is the size the sim
   * already treats it as having, and the size the two structures were authored
   * at. Above 1.0 trades physical honesty for presence, and the ceiling is
   * where a formation starts overlapping itself: the gap between neighbours at
   * their own spacing is 3/8 of it at 1.0, so it closes at about 2.6.
   *
   * It scales *hulls*, never chrome. Selection rings come off `pickRadius`,
   * lanes and pucks are screen-space, and neither reads this.
   */
  double hullScale = 1.0;
};

struct CameraSettings
{
  /// The ortho half-height the session opens at. A third of the original 8,000
  /// (2026-08-19): the starting fleet sits within about 1.4 km of the station,
  /// and from 8 km up that whole arrangement was a handful of specks.
  double zoomMetres = 2667.0;
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
  /// False runs the client silently without ever opening a device -- the same
  /// state a machine with no speakers reaches on its own (ADR-011).
  bool enabled = true;

  double master = 1.0;
  double world = 1.0;
  double ui = 0.8;
  double music = 0.6;
  double alerts = 1.0;
  double ambience = 0.7;

  /// Concurrent source voices per pool (ADR-011 §3). Content-side tuning: the
  /// caps are what stop a fleet from becoming a voice each.
  std::uint32_t voiceCap3D = 32;
  std::uint32_t voiceCap2D = 16;
};

struct UiSettings
{
  double scale = 1.0;
  std::string palette = "default";
  std::string font = "Consolas"; // A monospace face for the glyph atlas (ADR-006 §9).
};

/// The Diagnostics section (`debug-hud.png` §6): the Tier-1 counters strip
/// ships in every build, off by default, behind this setting -- the F1 key at
/// runtime is a shortcut to the same bit, not a second switch.
struct DiagnosticsSettings
{
  bool strip = false;
};

/*
 * A wing's call sign as the player left it (ADR-017 §6, ADR-012 §3).
 *
 * The one family in the user layer that is not a preference. A wing is a number
 * a ship carries and the shard has no name for it, so a *rename* and a name for
 * a wing the player composed are the same act -- presentation, client-side,
 * never on the wire -- and this is where both survive a restart.
 *
 * `wing` is a `Game::WingId` widened to a type this header can spell. The
 * configuration surface knows no game types on purpose: pulling `Ids.h` in here
 * would cost `Tests/OutpostTests` the one property that lets it compile this
 * file at all (Dependency-Map, "Outpost.exe -- composition root").
 */
struct WingName
{
  std::uint32_t wing = 0;
  std::string name;
};

/*
 * Where the client's boot-time content lives, and which meshes it loads.
 *
 * The mesh list is ordered, and the order *is* the classId the renderer draws
 * with: the engine is game-free (ADR-014), so it has no opinion about which
 * index is a Carrier. Ships come first, smallest to largest, then the two
 * things that never move -- the structure, and the stargate U4 added behind it
 * -- and the parked-fleet placeholder S5 renders reads it that way.
 */
struct ContentSettings
{
  std::string meshDirectory = "GameData/Meshes";
  std::vector<std::string> meshes = {"Interceptor.obj", "Bomber.obj",     "Corvette.obj", "Frigate.obj",
                                     "Hauler.obj",      "Miner.obj",      "Carrier.obj",  "Battleship.obj",
                                     "Structure.obj",   "Stargate.obj"};

  /// Where the WAVs are and which bank names them (ADR-011 §10-11). The bank
  /// lists the files; this says which directory they are relative to, the same
  /// arrangement the meshes have.
  std::string audioDirectory = "GameData/Audio";
  std::string soundBank = "GameData/Audio/SoundBank.json";
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
  DiagnosticsSettings diagnostics;
};

/*
 * Where a shard's durable state lives (ADR-025 §7).
 *
 * A configured directory, JSON like everything else -- and **not** the
 * LocalAppData user layer, which is one player's settings on one machine while
 * this is a service's state. Resolved beside the executable when it is
 * relative, because that is where a shard's own files belong.
 *
 * `enabled = false` is the headless and `selfTest` posture: a shard that
 * persists nothing and says so at boot. It is **not a fallback** -- a shard
 * configured to persist that cannot open its directory refuses to start, since
 * the alternative is a service that looks healthy and is quietly amnesiac.
 */
struct PersistenceSettings
{
  bool enabled = false;
  std::string directory = "ShardState";
};

/*
 * States the shipped world does not produce, asked for on purpose (ADR-012).
 *
 * **Why this exists.** A handful of the client's behaviours only ever happen in
 * a world the default scenario never builds -- following a fleet across grids,
 * the settle after a switch, a location block for a place you are not standing
 * -- so they could be built, unit-tested, and still never once be *looked at*.
 * That is precisely the gap R1 records three defects escaping through, and a
 * feature nobody can reach is a feature nobody can check.
 *
 * **Knobs rather than named scenarios**, because a preset list is a vocabulary
 * that drifts from what it sets up: six months on, "two-grids" means whatever
 * the function does, and the name stops being a description. Each field here is
 * one fact about the world, and they compose.
 *
 * **Every field is off by default, and that is load-bearing rather than
 * polite.** The committed `Outpost.json` must keep producing exactly the world
 * it produced yesterday: the replay hash is a property of the shipped scenario
 * (ADR-005), and a knob that changed it by existing would make every future
 * determinism failure ambiguous. Off means "identical to before".
 */
struct ScenarioSettings
{
  /*
   * Wings of a *second* fleet for the same commander, on a grid of its own.
   * Zero means none, which is the shipped world.
   *
   * The anchor is not configured and deliberately so: it is chosen by the same
   * deterministic rule that places a second commander -- the next anchor in
   * bake order nobody is standing on -- so the config says *how much, elsewhere*
   * and never has to carry an id a person would have to look up, or that a
   * re-bake could move under them.
   *
   * It stands where it is put, like every fleet this shard spawns -- the
   * scripted patrol that used to fly the boot fleet a circuit has been removed,
   * because a fleet moving on anything but its commander's orders is a fleet
   * whose owner cannot tell their own commands from the simulation's.
   */
  std::uint32_t secondFleetWings = 0;
};

struct AppConfig
{
  HostMode mode = HostMode::Host;
  bool selfTest = false;
  LogSettings logging;
  std::string universeDefinition = "GameData/Universe/Frontier.json";
  std::string economyDefinition = "GameData/Economy/Economy.json";
  PersistenceSettings persistence;
  ServerSettings server;
  ClientSettings client;
  ContentSettings content;
  ScenarioSettings scenario;

  /*
   * Call signs the player has given wings (ADR-012 §3).
   *
   * **The user layer's, and only the user layer's.** The starting fleet's names
   * are content in the composition root, so a `wings` key in `Outpost.json` is
   * an unknown key and warns -- which is the right answer for a shipped file
   * reaching for a player's vocabulary.
   */
  std::vector<WingName> wings;
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

/*
 * The user layer as text, ready for `SaveUserSettings` to put on disk
 * (ADR-012 §A3).
 *
 * `ApplyUserLayer` read backwards: it emits exactly the keys that function
 * accepts, so what this writes is what the next boot reads back. That symmetry
 * is the thing worth testing, and it is why the two live beside each other
 * rather than either living next to the file handle.
 *
 * **A preference is emitted only where `_config` differs from `_shipped`.**
 * A settings file that captured the shipped values would go on overriding them
 * after the shipped file moved on -- the player would be pinned to last year's
 * defaults by a file they never edited. So the file records what was *changed*,
 * and a section with nothing changed in it is not written at all. `wings` is
 * the exception and not one: the shipped layer never carries wing names, so
 * every one of them is a difference by construction.
 *
 * False if the writer was misused. `_outText` must not reach disk in that case:
 * half-valid JSON is worse than no file, because the next boot backs it up as
 * corrupt and the player silently loses the settings that were fine.
 */
[[nodiscard]] bool WriteUserLayer(const AppConfig& _config, const AppConfig& _shipped, std::string& _outText);

[[nodiscard]] const char* HostModeText(HostMode _mode) noexcept;

} // namespace Outpost
