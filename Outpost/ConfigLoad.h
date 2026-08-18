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

/*
 * A configured content path, made absolute: as given if it exists, otherwise
 * the same path beside the executable. Empty if it is in neither place.
 *
 * The rule is the base config's, deliberately (ADR-012 §A2) -- content and
 * configuration must never be found by different rules, or a build that starts
 * is not evidence that it will find its meshes. Accepts directories as well as
 * files, because `content.meshDirectory` is one.
 *
 * This exists because the working directory is a real variable: launching from
 * the project folder rather than from beside the executable is what Visual
 * Studio does by default, and a relative path handed straight to the engine
 * fails there while everything resolved through here keeps working.
 */
[[nodiscard]] std::string ResolveContentPath(const std::string& _path);

} // namespace Outpost
