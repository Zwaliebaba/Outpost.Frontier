#pragma once

#include "AppConfig.h"

#include <string>

/*
 * Finding and loading the configuration (ADR-012 §A).
 *
 * There is no command line and no environment variable. A configuration is
 * selected by the directory the executable is launched from, which is what CI
 * uses to pick the self-test config, and the user layer lives where a settings
 * screen can actually write it.
 */

namespace Outpost
{

struct ConfigPaths
{
  std::string base;     // The Outpost.json actually used, or empty if none was found.
  std::string userLayer; // The user settings file, whether or not it exists yet.
};

/// Resolves paths, reads both layers and merges them over the defaults.
/// Returns false only when the base config is missing or unusable -- a missing
/// or corrupt user layer is a warning, because losing preferences must never
/// stop the game starting.
[[nodiscard]] bool LoadAppConfig(AppConfig& _outConfig, ConfigPaths& _outPaths, ConfigDiagnostics& _diagnostics);

/// Directory of the running executable, with a trailing separator.
[[nodiscard]] std::string ExecutableDirectory();

/// %LOCALAPPDATA%\Outpost.Frontier\ via the known-folder API -- not an
/// environment variable, and created on demand.
[[nodiscard]] std::string UserSettingsDirectory();

[[nodiscard]] bool ReadTextFile(const std::string& _path, std::string& _outText);

} // namespace Outpost
