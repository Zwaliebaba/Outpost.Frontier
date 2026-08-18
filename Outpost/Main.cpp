#include "pch.h"

#include "AppConfig.h"
#include "ConfigLoad.h"
#include "SelfTest.h"

#include "ClientApp.h"
#include "ClientConfig.h"

#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"

#include "Clock.h"
#include "Log.h"

#include <cstdio>
#include <string>

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
void ReportStartupFailure(const Outpost::ConfigDiagnostics& _diagnostics)
{
  std::string text = "Outpost could not start.\n\n";
  for (const std::string& error : _diagnostics.errors)
  {
    text += error;
    text += '\n';
  }
  OutputDebugStringA(text.c_str());
  std::fputs(text.c_str(), stderr);

  const int wide = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring message(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, message.data(), wide);
  MessageBoxW(nullptr, message.c_str(), L"Outpost: Frontier", MB_OK | MB_ICONERROR);
}

void LogResolvedConfig(const Outpost::AppConfig& _config, const Outpost::ConfigPaths& _paths)
{
  NEURON_LOG_INFO("config: %s", _paths.base.c_str());
  if (!_paths.userLayer.empty())
  {
    NEURON_LOG_INFO("user settings: %s", _paths.userLayer.c_str());
  }
  NEURON_LOG_INFO("mode: %s%s", Outpost::HostModeText(_config.mode), _config.selfTest ? " (self test)" : "");
  NEURON_LOG_INFO("server: port %u, transport %s, max sessions %u", static_cast<unsigned>(_config.server.port),
                  _config.server.transport.c_str(), static_cast<unsigned>(_config.server.maxSessions));
  NEURON_LOG_INFO("universe: %s", _config.universeDefinition.c_str());
  NEURON_LOG_INFO("window: %ux%u %s, vsync %s, ui scale %.2f", static_cast<unsigned>(_config.client.window.width),
                  static_cast<unsigned>(_config.client.window.height), _config.client.window.mode.c_str(),
                  _config.client.renderer.vsync ? "on" : "off", _config.client.ui.scale);
}

/// The server takes the same treatment: a plain struct, assembled here.
Neuron::ServerConfig MakeServerConfig(const Outpost::AppConfig& _config)
{
  Neuron::ServerConfig server;
  server.port = _config.server.port;
  server.transport = _config.server.transport;
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
#if defined(_DEBUG)
  client.enableDebugLayer = true; // Every debug run gets the validation layer.
#endif
  return client;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
  Neuron::Clock::Initialise();
  SetConsoleCtrlHandler(&ConsoleHandler, TRUE);

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

  // GameLogic implements Simulation and the composition root injects it
  // (ADR-014 §2). Until that exists, the server hosts a simulation that does
  // nothing -- which is enough to prove the loop, the sessions and the wire.
  Neuron::NullSimulation simulation;

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
        // The handshake compares these against the simulation's own values, and
        // in one process there is exactly one simulation to ask.
        clientConfig.schemaHash = simulation.SchemaHash();
        clientConfig.contentHash = simulation.ContentHash();
      }

      Neuron::ClientApp client;
      if (!client.Initialise(clientConfig))
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
