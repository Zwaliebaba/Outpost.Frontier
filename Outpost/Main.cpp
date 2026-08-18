#include "pch.h"

#include "AppConfig.h"
#include "ConfigLoad.h"
#include "ParkedFleetView.h"
#include "SelfTest.h"
#include "UniverseLoad.h"

// GameLogic, reached only from here: the executable is the one project
// entitled to know both halves (ADR-014 §1).
#include "ShipClass.h"
#include "World.h"

#include "ClientApp.h"
#include "ClientConfig.h"

#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"

#include "Clock.h"
#include "Log.h"
#include "Telemetry.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

/*
 * The composition root (ADR-008).
 *
 * Its whole job: load configuration, start the pieces the mode asks for, and
 * shut them down in the right order. No game logic, no rendering, no
 * networking of its own -- and, deliberately, no argument parsing: wWinMain
 * ignores what it is handed (ADR-012 §A1).
 */

namespace
{

volatile bool g_stopRequested = false;

BOOL WINAPI ConsoleHandler(DWORD _type)
{
  if (_type == CTRL_C_EVENT || _type == CTRL_CLOSE_EVENT || _type == CTRL_BREAK_EVENT)
  {
    g_stopRequested = true; // Let the loop unwind rather than dying mid-tick.
    return TRUE;
  }
  return FALSE;
}

/// Before the log file exists, a fatal problem still has to reach a person.
void ReportFatal(const std::string& _text)
{
  OutputDebugStringA(_text.c_str());
  std::fputs(_text.c_str(), stderr);

  const int wide = MultiByteToWideChar(CP_UTF8, 0, _text.c_str(), -1, nullptr, 0);
  std::wstring message(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _text.c_str(), -1, message.data(), wide);
  MessageBoxW(nullptr, message.c_str(), L"Outpost: Frontier", MB_OK | MB_ICONERROR);
}

void ReportStartupFailure(const Outpost::ConfigDiagnostics& _diagnostics)
{
  std::string text = "Outpost could not start.\n\n";
  for (const std::string& error : _diagnostics.errors)
  {
    text += error;
    text += '\n';
  }
  ReportFatal(text);
}

void LogResolvedConfig(const Outpost::AppConfig& _config, const Outpost::ConfigPaths& _paths)
{
  NEURON_LOG_INFO("config: %s", _paths.base.c_str());
  if (!_paths.userLayer.empty())
  {
    NEURON_LOG_INFO("user settings: %s", _paths.userLayer.c_str());
  }
  NEURON_LOG_INFO("mode: %s%s", Outpost::HostModeText(_config.mode), _config.selfTest ? " (self test)" : "");
  NEURON_LOG_INFO("server: port %u, max sessions %u", static_cast<unsigned>(_config.server.port),
                  static_cast<unsigned>(_config.server.maxSessions));
  NEURON_LOG_INFO("universe: %s", _config.universeDefinition.c_str());
  NEURON_LOG_INFO("content: %u meshes from %s, shaders from %s", static_cast<unsigned>(_config.content.meshes.size()),
                  _config.content.meshDirectory.c_str(), _config.content.shaderDirectory.c_str());
  NEURON_LOG_INFO("window: %ux%u %s, vsync %s, ui scale %.2f", static_cast<unsigned>(_config.client.window.width),
                  static_cast<unsigned>(_config.client.window.height), _config.client.window.mode.c_str(),
                  _config.client.renderer.vsync ? "on" : "off", _config.client.ui.scale);
}

/*
 * The simulation the server hosts until GameLogic supplies a real one (S5c).
 *
 * From S6 it advances a real `Game::World`: the tick loop the server has been
 * driving since S3 now moves ships. What it does not yet do is *say* so --
 * `WriteSnapshot` is still empty, because the snapshot format and its wire
 * schema are S7 -- so the fleet moves on the server and nothing sees it. That
 * is the honest state of the seam, and it is one function away from not being.
 *
 * The adapter holds the vtable and forwards; the simulation is GameLogic's
 * (ADR-014 §2a). Everything in this class is a line of wiring.
 *
 * `SchemaHash` stays zero on purpose: that field means "the layout of the
 * *game's* wire types", and there are none until S7 writes snapshots. Returning
 * something plausible would be worse than returning nothing -- two builds would
 * agree about a layout that does not exist.
 */
class UniverseSimulation final : public Neuron::Simulation
{
public:
  UniverseSimulation(std::uint64_t _universeHash, Neuron::WorldMeta _worldMeta, Game::World _world) noexcept
    : m_universeHash(_universeHash),
      m_worldMeta(_worldMeta),
      m_world(std::move(_world))
  {
  }

  void AdvanceTick(std::uint32_t _tick) override
  {
    // No orders yet: `ApplyOrderBytes` refuses everything, and the scripted
    // patrol that gives these ships somewhere to go arrives with S7.
    m_world.Tick(_tick, std::span<const Game::ScriptedMove>{});
  }

  void WriteSnapshot(std::uint32_t, Neuron::ByteWriter&) override {}

  [[nodiscard]] Neuron::OrderVerdict ApplyOrderBytes(std::uint32_t, std::span<const std::uint8_t>) override
  {
    return Neuron::OrderVerdict{}; // No order format to decode until S9.
  }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return 0; }
  [[nodiscard]] std::uint64_t ContentHash() const override { return m_universeHash; }
  [[nodiscard]] Neuron::WorldMeta World() const override { return m_worldMeta; }

private:
  std::uint64_t m_universeHash = 0;
  Neuron::WorldMeta m_worldMeta;
  Game::World m_world;
};

/*
 * The starting fleet, on the grid the universe definition anchored.
 *
 * Deliberately modest and deliberately here: what ships a session begins with
 * is a scenario, not a rule, and a scenario belongs in the composition root
 * until there is a save file to read one from. The layout mirrors the client's
 * placeholder -- one wing per playable hull -- so that when S7 replaces the
 * invented fleet with the replicated one, the picture on screen changes as
 * little as possible and any difference is a real difference.
 */
[[nodiscard]] Game::World MakeStartingWorld(std::uint64_t _seed)
{
  Game::World world;
  world.Reset(_seed);

  constexpr Game::HullClass FLEET[] = {Game::HullClass::Interceptor, Game::HullClass::Bomber,  Game::HullClass::Corvette,
                                       Game::HullClass::Frigate,     Game::HullClass::Hauler,  Game::HullClass::Miner,
                                       Game::HullClass::Carrier,     Game::HullClass::Battleship};
  constexpr std::uint32_t SHIPS_PER_WING = 5;
  constexpr float WING_RADIUS_METRES = 1400.0f;

  std::uint32_t wing = 0;
  for (const Game::HullClass hullClass : FLEET)
  {
    const float wingAngle = (static_cast<float>(wing) / static_cast<float>(std::size(FLEET))) * DirectX::XM_2PI;
    const float spacing = Game::ShipClass(hullClass).formationSpacingMetres;

    for (std::uint32_t index = 0; index < SHIPS_PER_WING; ++index)
    {
      const float offset = (static_cast<float>(index) - 0.5f * static_cast<float>(SHIPS_PER_WING - 1)) * spacing;

      Game::ShipSpawn spawn;
      spawn.hullClass = hullClass;
      spawn.wing = static_cast<Game::WingId>(wing + 1);
      // Ships face the anchor, which is where the station is -- a fleet parked
      // facing outward would read as a fleet about to leave.
      spawn.headingRadians = wingAngle + DirectX::XM_PI;
      spawn.xMetres = std::cos(wingAngle) * WING_RADIUS_METRES - std::sin(wingAngle) * offset;
      spawn.yMetres = std::sin(wingAngle) * WING_RADIUS_METRES + std::cos(wingAngle) * offset;
      (void)world.Spawn(spawn);
    }
    ++wing;
  }
  return world;
}

/// The universe's world meta, in the neutral terms the engine speaks
/// (ADR-009 §8): which world, and where its tactical grid is anchored.
Neuron::WorldMeta MakeWorldMeta(const Game::UniverseDef& _universe)
{
  const Game::GridAnchor anchor = _universe.StartAnchor();
  return Neuron::WorldMeta{anchor.system, anchor.origin.x, anchor.origin.y};
}

/*
 * Authored placements, converted into the grid's local frame for the renderer.
 *
 * This is the one place the universe's integer metres become the client's local
 * floats, and it is deliberately in the composition root: GameLogic owns the
 * exact coordinates, the engine owns the rendering, and the conversion between
 * them belongs to the thing that knows both (ADR-014).
 */
std::vector<Neuron::ScenePlacement> BuildScenery(const Game::UniverseDef& _universe, std::uint16_t _structureClassId)
{
  std::vector<Neuron::ScenePlacement> scenery;

  const Game::GridAnchor anchor = _universe.StartAnchor();
  const Game::SolarSystem* system = _universe.FindSystem(anchor.system);
  if (system == nullptr)
  {
    return scenery;
  }

  for (const Game::Station& station : system->stations)
  {
    Game::LocalOffsetCm local;
    if (!Game::LocalFromUniverse(anchor.origin, station.position, local))
    {
      // Refused rather than wrapped (ADR-009 §2). A station further than the
      // grid's half-extent from the anchor is a content error, and drawing it
      // at a folded coordinate would hide that.
      NEURON_LOG_WARNING("station '%s' is outside the tactical grid and was not placed", station.name.c_str());
      continue;
    }

    Neuron::ScenePlacement placement;
    placement.xMetres = static_cast<float>(local.x) * 0.01f;
    placement.yMetres = static_cast<float>(local.y) * 0.01f;
    placement.classId = _structureClassId;
    scenery.push_back(placement);
  }
  return scenery;
}

void LogResolvedUniverse(const Outpost::UniverseLoadResult& _universe)
{
  const Game::GridAnchor anchor = _universe.universe.StartAnchor();
  NEURON_LOG_INFO("universe: '%s' from %s (%u system(s), hash %016llx)", _universe.universe.name.c_str(), _universe.path.c_str(),
                  static_cast<unsigned>(_universe.universe.systems.size()), static_cast<unsigned long long>(_universe.universeHash));

  const Game::SolarSystem* system = _universe.universe.FindSystem(anchor.system);
  NEURON_LOG_INFO("start: system %u '%s', grid anchored at (%lld, %lld)", static_cast<unsigned>(anchor.system),
                  system != nullptr ? system->name.c_str() : "?", static_cast<long long>(anchor.origin.x),
                  static_cast<long long>(anchor.origin.y));
}

/*
 * The client's half of the seam (ADR-014 §2a).
 *
 * The placeholder world, assembled from the same universe definition the server
 * simulates against. Both halves load the identical file, so the client hashes
 * what it read rather than being told (ADR-009 §8) -- in `mode: "client"` that
 * is the whole safety property, because a client whose content drifted is
 * refused at the door instead of rendering a world nobody is simulating.
 */
Outpost::ParkedFleetView::Desc MakeWorldViewDesc(const Outpost::AppConfig& _config, const Outpost::UniverseLoadResult& _universe)
{
  // The structure mesh is the last entry in the content list by convention
  // (AppConfig.h), which is the only place that convention is spelled -- the
  // engine is handed a classId and never learns what it stands for.
  const auto classCount = static_cast<std::uint32_t>(_config.content.meshes.size());
  const auto structureClassId = static_cast<std::uint16_t>(classCount == 0 ? 0 : classCount - 1);

  Outpost::ParkedFleetView::Desc desc;
  desc.scenery = BuildScenery(_universe.universe, structureClassId);
  desc.classCount = classCount;
  desc.contentHash = _universe.universeHash;
  return desc;
}

/// The server takes the same treatment: a plain struct, assembled here.
Neuron::ServerConfig MakeServerConfig(const Outpost::AppConfig& _config)
{
  Neuron::ServerConfig server;
  server.port = _config.server.port;
  server.maxSessions = _config.server.maxSessions;
  return server;
}

/// Maps the file's settings onto what the client library asks for. The client
/// never sees AppConfig: libraries take plain structs from the composition root.
Neuron::ClientConfig MakeClientConfig(const Outpost::AppConfig& _config)
{
  Neuron::ClientConfig client;
  client.windowWidth = _config.client.window.width;
  client.windowHeight = _config.client.window.height;
  client.windowTitle = "Outpost: Frontier";
  client.borderlessFullscreen = _config.client.window.mode == "borderless";
  client.vsync = _config.client.renderer.vsync;
  client.frameCap = _config.client.renderer.frameCap;
  client.serverHost = _config.client.connectHost;
  client.serverPort = _config.client.connectPort;

  // Content: where it is and which meshes to load, in classId order. The client
  // opens the files, but it is told what to open -- the engine has no opinion
  // about which index is a Carrier (ADR-014).
  client.meshDirectory = _config.content.meshDirectory;
  client.shaderDirectory = _config.content.shaderDirectory;
  client.meshFiles = _config.content.meshes;
  client.fontFamily = _config.client.ui.font;

  client.cameraZoomMetres = static_cast<float>(_config.client.camera.zoomMetres);

  // Two structs describing the same thing, mapped rather than shared: the
  // engine may not take a type from the host, and the host may not take one
  // from the engine's config layer either (ADR-012, ADR-014). The cost is this
  // block; the benefit is that NeuronClient has no idea Outpost.json exists.
  client.nebula.tintRed = static_cast<float>(_config.client.nebula.tintRed);
  client.nebula.tintGreen = static_cast<float>(_config.client.nebula.tintGreen);
  client.nebula.tintBlue = static_cast<float>(_config.client.nebula.tintBlue);
  client.nebula.intensity = static_cast<float>(_config.client.nebula.intensity);
  client.nebula.tileMetres = static_cast<float>(_config.client.nebula.tileMetres);
  client.nebula.resolution = _config.client.nebula.resolution;
  client.nebula.octaves = _config.client.nebula.octaves;
  client.nebula.coverage = static_cast<float>(_config.client.nebula.coverage);
  client.nebula.contrast = static_cast<float>(_config.client.nebula.contrast);
  client.nebula.seed = _config.client.nebula.seed;
  client.cameraYawSnapDegrees = static_cast<float>(_config.client.camera.yawSnapDegrees);
  client.uiScale = static_cast<float>(_config.client.ui.scale);

  // The world is not here any more. Scenery, the grid anchor and the handshake
  // hashes went behind `Neuron::WorldView` with S5c: configuration is how the
  // client is set up, not what it is looking at.
#if defined(_DEBUG)
  client.enableDebugLayer = true; // Every debug run gets the validation layer.
#endif
  return client;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
  // Before anything computes with DirectXMath: the library is compiled for the
  // instruction set /arch selects, and running it on a CPU without that set is
  // an illegal instruction somewhere far from here (ADR-010, Risk R11).
  // Called directly rather than through a helper -- DirectXMath is used
  // natively, and boot is the use site.
  if (!DirectX::XMVerifyCPUSupport())
  {
    MessageBoxW(nullptr, L"This CPU does not support the instruction set this build requires.", L"Outpost: Frontier", MB_OK | MB_ICONERROR);
    return 4;
  }

  Neuron::Clock::Initialise();
  SetConsoleCtrlHandler(&ConsoleHandler, TRUE);

  // The composition root owns the Main lane so headless mode -- which never
  // constructs a ClientApp -- still has one (ADR-007 §8).
  (void)Neuron::Telemetry::RegisterLane("Main");

  Outpost::AppConfig config;
  Outpost::ConfigPaths paths;
  Outpost::ConfigDiagnostics diagnostics;
  if (!Outpost::LoadAppConfig(config, paths, diagnostics))
  {
    ReportStartupFailure(diagnostics);
    return 1;
  }

  Neuron::Log::Initialise(config.logging.file, config.logging.level);
  NEURON_LOG_INFO("Outpost: Frontier starting");
  for (const std::string& warning : diagnostics.warnings)
  {
    NEURON_LOG_WARNING("%s", warning.c_str());
  }
  LogResolvedConfig(config, paths);

  // The universe, read here and parsed by GameLogic (ADR-009 §7). Both halves
  // load the identical definition, so this happens once and feeds both -- and
  // it happens before anything starts, because a universe that will not parse
  // is not a degraded mode, it is a refusal to run.
  Outpost::UniverseLoadResult universe;
  std::vector<std::string> universeErrors;
  if (!Outpost::LoadUniverse(config.universeDefinition, universe, universeErrors))
  {
    Outpost::ConfigDiagnostics universeDiagnostics;
    universeDiagnostics.errors = universeErrors;
    for (const std::string& error : universeErrors)
    {
      NEURON_LOG_ERROR("%s", error.c_str());
    }
    ReportStartupFailure(universeDiagnostics);
    Neuron::Log::Shutdown();
    return 1;
  }
  LogResolvedUniverse(universe);

  // GameLogic implements Simulation and the composition root injects it
  // (ADR-014 §2). Until it does (S5c), the server hosts one that advances
  // nothing but knows its content hash and where its world is anchored -- which
  // is enough to prove the loop, the sessions, the wire and the handshake.
  UniverseSimulation simulation{universe.universeHash, MakeWorldMeta(universe.universe),
                                MakeStartingWorld(universe.universeHash)};

  // Before anything opens a window: the self test is a diagnostic, and its
  // answer is an exit code (Build Order S4).
  if (config.selfTest)
  {
    const int result = Outpost::RunSelfTest(config, simulation);
    Neuron::Log::Shutdown();
    return result;
  }

  // Boot order is normative (ADR-008 §5): the server starts before the client,
  // and the client is torn down first.
  Neuron::ServerHost server;

  int exitCode = 0;
  const bool hostsServer = config.mode != Outpost::HostMode::Client;
  if (hostsServer && !server.Start(MakeServerConfig(config), simulation))
  {
    NEURON_LOG_ERROR("server failed to start");
    Neuron::Log::Shutdown();
    return 2;
  }

  // check_hresult failures (device, swapchain, command lists) and assertion
  // fatals arrive here as exceptions; the boundary turns them into a log line
  // and a message box instead of a silent crash, and still stops the server.
  try
  {
    switch (config.mode)
    {
    case Outpost::HostMode::Host:
    case Outpost::HostMode::Client:
    {
      Neuron::ClientConfig clientConfig = MakeClientConfig(config);
      if (config.mode == Outpost::HostMode::Host)
      {
        // Port 0 asked the OS to choose, so the client is told what it chose.
        // This convenience dies with the split, which is the point of it being
        // the only thing the two halves share besides the socket.
        clientConfig.serverHost = "127.0.0.1";
        clientConfig.serverPort = server.BoundPort();
      }

      // Engine meets game here and nowhere else (ADR-014 §6). The client gets
      // a world view by reference and never learns what is behind it; hosting
      // exercises exactly the handshake a separate client would, because both
      // sides state their own hashes rather than sharing a variable.
      Outpost::ParkedFleetView worldView{MakeWorldViewDesc(config, universe)};

      Neuron::ClientApp client;
      if (!client.Initialise(clientConfig, worldView))
      {
        NEURON_LOG_ERROR("client failed to initialise");
        exitCode = 2;
        break;
      }
      exitCode = client.Run();
      // Client first, always: it must never render against a server that has
      // already gone (ADR-008 §6).
      client.Shutdown();
      break;
    }

    case Outpost::HostMode::Headless:
    {
      NEURON_LOG_INFO("headless: serving on port %u until Ctrl-C", static_cast<unsigned>(server.BoundPort()));
      // The standing proof that the server needs no client at all.
      while (server.Running() && !g_stopRequested)
      {
        Sleep(100);
      }
      break;
    }
    }
  }
  catch (const winrt::hresult_error& error)
  {
    const std::string text = std::format("fatal error 0x{:08x}: {}", static_cast<std::uint32_t>(static_cast<std::int32_t>(error.code())),
                                         winrt::to_string(error.message()));
    NEURON_LOG_ERROR("%s", text.c_str());
    ReportFatal(text);
    exitCode = 5;
  }
  catch (const std::exception& error)
  {
    const std::string text = std::string("fatal error: ") + error.what();
    NEURON_LOG_ERROR("%s", text.c_str());
    ReportFatal(text);
    exitCode = 5;
  }

  if (hostsServer)
  {
    server.Stop();
    server.Join();
    NEURON_LOG_INFO("server ran %u ticks (%u overruns)", server.TickCount(), server.OverrunCount());
  }

  NEURON_LOG_INFO("Outpost: Frontier exiting cleanly (%d)", exitCode);
  Neuron::Log::Shutdown();
  return exitCode;
}
