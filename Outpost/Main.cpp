#include "pch.h"

#include "AppConfig.h"
#include "ConfigLoad.h"
#include "ReplicatedWorldView.h"
#include "SelfTest.h"
#include "UniverseLoad.h"

// GameLogic, reached only from here: the executable is the one project
// entitled to know both halves (ADR-014 §1).
#include "SchemaHash.h"
#include "ShipClass.h"
#include "Snapshot.h"
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
#include <string_view>
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
 * From S6 it advances a real `Game::World`; from S7 it also says so, emitting a
 * full quantised snapshot every tick for `ServerHost` to fan out. That closes
 * the loop the whole build order has been assembling: the world moves, and
 * someone can see it.
 *
 * The adapter holds the vtable and forwards; the simulation is GameLogic's
 * (ADR-014 §2a). Everything in this class is a line of wiring.
 */
class UniverseSimulation final : public Neuron::Simulation
{
public:
  UniverseSimulation(std::uint64_t _universeHash, Neuron::WorldMeta _worldMeta, Game::World _world,
                     std::vector<Game::ShipId> _patrolShips)
    : m_universeHash(_universeHash),
      m_worldMeta(_worldMeta),
      m_world(std::move(_world)),
      m_patrolShips(std::move(_patrolShips))
  {
  }

  /*
   * The scripted patrol (Build Order S7).
   *
   * Every wing is sent to a waypoint, and the waypoints rotate every few
   * seconds. It exists so the fleet moves without anyone touching an input
   * device -- which is what makes "is the motion smooth at 144 Hz against 20 Hz
   * snapshots?" a question that can be answered by looking at the screen.
   *
   * It is scripted from the tick index and nothing else, so it replays exactly
   * (ADR-005 §5) and a desync between two runs of the same build would be a
   * real defect rather than an artefact of when someone clicked.
   */
  void AdvanceTick(std::uint32_t _tick) override
  {
    constexpr std::uint32_t LEG_TICKS = 200; // Ten seconds at 20 Hz.
    if (_tick % LEG_TICKS == 1 && !m_patrolShips.empty())
    {
      const std::uint32_t leg = (_tick / LEG_TICKS) % 4;
      constexpr float WAYPOINTS[4][2] = {{6000.0f, 0.0f}, {0.0f, 6000.0f}, {-6000.0f, 0.0f}, {0.0f, -6000.0f}};

      Game::ScriptedMove move;
      move.shipIds = m_patrolShips.data();
      move.shipCount = static_cast<std::uint32_t>(m_patrolShips.size());
      move.targetXMetres = WAYPOINTS[leg][0];
      move.targetYMetres = WAYPOINTS[leg][1];
      move.arrivalFacingRadians = static_cast<float>(leg) * DirectX::XM_PIDIV2;
      m_world.Tick(_tick, std::span<const Game::ScriptedMove>{&move, 1});
      return;
    }

    m_world.Tick(_tick, std::span<const Game::ScriptedMove>{});
  }

  /// The tick argument is the loop's; the world knows its own and they are the
  /// same number. Writing the world's is the one that cannot drift.
  [[nodiscard]] bool WriteSnapshot(std::uint32_t, Neuron::ByteWriter& _writer) override
  {
    return Game::WriteSnapshot(m_world, _writer);
  }

  [[nodiscard]] Neuron::OrderVerdict ApplyOrderBytes(std::uint32_t, std::span<const std::uint8_t>) override
  {
    return Neuron::OrderVerdict{}; // No order format to decode until S9.
  }

  [[nodiscard]] std::uint64_t SchemaHash() const override { return Game::GameSchemaHash(); }
  [[nodiscard]] std::uint64_t ContentHash() const override { return m_universeHash; }
  [[nodiscard]] Neuron::WorldMeta World() const override { return m_worldMeta; }

private:
  std::uint64_t m_universeHash = 0;
  Neuron::WorldMeta m_worldMeta;
  Game::World m_world;

  /// The mobile half of the fleet. The station is deliberately absent: sending
  /// a `Structure` a waypoint is harmless -- it has no speed -- but listing it
  /// would imply it might move.
  std::vector<Game::ShipId> m_patrolShips;
};

/*
 * Spawns the authored stations into the world, in the grid's local frame.
 *
 * Until S7 these were `ScenePlacement`s handed to the renderer -- authored
 * scenery on one side of the seam and an invented fleet on the other. They are
 * ships now: a `Structure` has zero speed and zero turn rate (ADR-005 §1), so a
 * station is a ship-table entry that never moves, and it replicates through the
 * same twenty bytes as everything else. One path instead of two, and a station
 * that can be selected and targeted by the code that already does those things.
 *
 * This remains the one place the universe's exact integer metres become the
 * sim's local floats, and it is deliberately in the composition root: GameLogic
 * owns the coordinates, the engine owns the drawing, and the conversion belongs
 * to the thing that knows both (ADR-014).
 */
void SpawnStations(const Game::UniverseDef& _universe, Game::World& _world)
{
  const Game::GridAnchor anchor = _universe.StartAnchor();
  const Game::SolarSystem* system = _universe.FindSystem(anchor.system);
  if (system == nullptr)
  {
    return;
  }

  for (const Game::Station& station : system->stations)
  {
    Game::LocalOffsetCm local;
    if (!Game::LocalFromUniverse(anchor.origin, station.position, local))
    {
      // Refused rather than wrapped (ADR-009 §2). A station further than the
      // grid's half-extent from the anchor is a content error, and placing it
      // at a folded coordinate would hide that.
      NEURON_LOG_WARNING("station '%s' is outside the tactical grid and was not spawned", station.name.c_str());
      continue;
    }

    Game::ShipSpawn spawn;
    spawn.hullClass = Game::HullClass::Structure;
    spawn.xMetres = static_cast<float>(local.x) * 0.01f;
    spawn.yMetres = static_cast<float>(local.y) * 0.01f;
    (void)_world.Spawn(spawn);
  }
}

/*
 * The starting fleet, on the grid the universe definition anchored.
 *
 * Deliberately modest and deliberately here: what ships a session begins with
 * is a scenario, not a rule, and a scenario belongs in the composition root
 * until there is a save file to read one from.
 *
 * The layout -- one wing per playable hull -- was chosen in S6 to match the
 * client's placeholder, so that when the replicated fleet replaced the invented
 * one the picture would change as little as possible and any difference would be
 * a real difference. The placeholder is gone and this is now the only fleet
 * there is, but the layout stays: it puts every hull class on screen at once,
 * which is what makes a rendering or replication fault obvious rather than
 * subtle.
 */
[[nodiscard]] Game::World MakeStartingWorld(const Game::UniverseDef& _universe, std::uint64_t _seed,
                                            std::vector<Game::ShipId>& _outPatrolShips)
{
  Game::World world;
  world.Reset(_seed);

  // Stations first, so they hold the low ids: nothing depends on it, but a
  // stable ordering makes a snapshot easier to read by eye when something is
  // wrong.
  SpawnStations(_universe, world);

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

      const Game::ShipId id = world.Spawn(spawn);
      if (id != Game::INVALID_SHIP_ID)
      {
        _outPatrolShips.push_back(id);
      }
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
 * The one table that maps the game's hull taxonomy onto the renderer's mesh
 * ids. They are different orderings of overlapping sets on purpose: `HullClass`
 * is the icon sheet's closed eleven, ordered so wire values never renumber
 * (ADR-009 §6), while the mesh list in `Outpost.json` runs smallest hull to
 * largest. Neither side should learn the other's, so the translation lives
 * here, in the only project entitled to know both.
 *
 * The content hash is the universe hash. Both halves load the identical file
 * and each states what it read rather than being told (ADR-009 §8) -- in
 * `mode: "client"` that is the whole safety property, because a client whose
 * content drifted is refused at the door instead of rendering a world nobody
 * is simulating.
 */
Outpost::ReplicatedWorldView::Desc MakeWorldViewDesc(const Outpost::AppConfig& _config,
                                                     const Outpost::UniverseLoadResult& _universe)
{
  // The mesh list, by name, is the renderer's classId order. Matching on the
  // authored file name rather than on position means reordering the list in
  // `Outpost.json` reorders the meshes and nothing breaks.
  static constexpr struct
  {
    Game::HullClass hullClass;
    std::string_view meshFile;
  } MESH_FOR_HULL[] = {
      {Game::HullClass::Interceptor, "Interceptor.obj"}, {Game::HullClass::Bomber, "Bomber.obj"},
      {Game::HullClass::Corvette, "Corvette.obj"},       {Game::HullClass::Frigate, "Frigate.obj"},
      {Game::HullClass::Hauler, "Hauler.obj"},           {Game::HullClass::Miner, "Miner.obj"},
      {Game::HullClass::Carrier, "Carrier.obj"},         {Game::HullClass::Battleship, "Battleship.obj"},
      {Game::HullClass::Structure, "Structure.obj"},
  };

  Outpost::ReplicatedWorldView::Desc desc;
  desc.renderClassByHull.assign(Game::HULL_CLASS_COUNT, Outpost::ReplicatedWorldView::INVALID_RENDER_CLASS);
  desc.contentHash = _universe.universeHash;

  for (const auto& mapping : MESH_FOR_HULL)
  {
    for (std::size_t index = 0; index < _config.content.meshes.size(); ++index)
    {
      if (_config.content.meshes[index] == mapping.meshFile)
      {
        desc.renderClassByHull[static_cast<std::size_t>(mapping.hullClass)] = static_cast<std::uint16_t>(index);
        break;
      }
    }
  }

  // Fighter and Cruiser stay INVALID_RENDER_CLASS: reserved ids with no mesh
  // and no content, which the view draws as nothing rather than as a stand-in
  // (ADR-009 §6).
  for (std::size_t hull = 0; hull < desc.renderClassByHull.size(); ++hull)
  {
    const auto hullClass = static_cast<Game::HullClass>(hull);
    if (Game::HullClassHasContent(hullClass) && desc.renderClassByHull[hull] == Outpost::ReplicatedWorldView::INVALID_RENDER_CLASS)
    {
      NEURON_LOG_WARNING("no mesh configured for hull class '%s'; it will not be drawn",
                         std::string(Game::HullClassName(hullClass)).c_str());
    }
  }
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
  std::vector<Game::ShipId> patrolShips;
  UniverseSimulation simulation{universe.universeHash, MakeWorldMeta(universe.universe),
                                MakeStartingWorld(universe.universe, universe.universeHash, patrolShips),
                                std::move(patrolShips)};

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
      Outpost::ReplicatedWorldView worldView{MakeWorldViewDesc(config, universe)};

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
