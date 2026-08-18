#include "pch.h"

#include "AppConfig.h"
#include "ConfigLoad.h"

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

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
  Neuron::Clock::Initialise();

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

  // Slices S1 and S3 hang the client and the server off this switch; until then
  // the boot path itself is what is being proved.
  switch (config.mode)
  {
    case Outpost::HostMode::Host:
      NEURON_LOG_INFO("host mode: server and client will start here (slices S1, S3)");
      break;
    case Outpost::HostMode::Headless:
      NEURON_LOG_INFO("headless mode: server only (slice S3)");
      break;
    case Outpost::HostMode::Client:
      NEURON_LOG_INFO("client mode: connecting to %s:%u (slice S4)", config.client.connectHost.c_str(),
                      static_cast<unsigned>(config.client.connectPort));
      break;
  }

  NEURON_LOG_INFO("Outpost: Frontier exiting cleanly");
  Neuron::Log::Shutdown();
  return 0;
}
