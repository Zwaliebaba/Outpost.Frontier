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

/*
 * Resolves paths, reads both layers and merges them over the defaults.
 *
 * Returns false only when the base config is missing or unusable -- a missing
 * or corrupt user layer is a warning, because losing preferences must never
 * stop the game starting. A user layer that will not parse is **backed up
 * beside itself** before it is ignored (ADR-012 §A4): the file is the player's
 * own, and overwriting it on the next save without keeping a copy would throw
 * away settings a person could have recovered by hand.
 *
 * `_outShipped` is the same configuration with the user layer **not** applied.
 * It is what `SaveUserSettings` writes a difference against, so the file on
 * disk records what the player changed rather than a snapshot of the shipped
 * values -- see `WriteUserLayer`.
 */
[[nodiscard]] bool LoadAppConfig(AppConfig& _outConfig, AppConfig& _outShipped, ConfigPaths& _outPaths,
                                 ConfigDiagnostics& _diagnostics);

/*
 * Writes the user layer, and is the only place in this program that creates a
 * file the player owns (ADR-012 §A3).
 *
 * Two properties, both of which are the point rather than polish.
 *
 * **It is atomic.** The text goes to a temporary beside the target and is then
 * renamed over it, so a crash or a full disk mid-write leaves the previous
 * settings intact instead of a truncated file the next boot reports as corrupt.
 * A settings file is small enough that this costs nothing and valuable enough
 * that losing one to a power cut is a real complaint.
 *
 * **It writes nothing when nothing changed.** The composed text is compared
 * with what is already on disk first. Without that, every clean exit would
 * touch the file -- and on a first run, where the player has changed nothing at
 * all, `WriteUserLayer` composes `{}` and this declines to create the file,
 * which is what keeps an untouched installation free of one.
 *
 * False on a real failure, with `_outError` set. A caller logs it and carries
 * on: preferences that could not be saved are worth a line in the log and are
 * never worth failing a shutdown over.
 */
[[nodiscard]] bool SaveUserSettings(const AppConfig& _config, const AppConfig& _shipped, const ConfigPaths& _paths,
                                    std::string& _outError);

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

/*
 * `ResolveContentPath`'s sibling, for a path the process **writes** to
 * (ADR-025 §7).
 *
 * Same rule -- absolute as given, relative resolved beside the executable --
 * and one difference that is the whole reason it is a second function:
 * existence is not a condition. A shard's state directory does not exist until
 * the first shard runs, and a resolver that returned empty for "not there yet"
 * would make a first boot indistinguishable from a misconfigured one.
 *
 * **Not** the LocalAppData user layer, deliberately: that is one player's
 * settings on one machine, and this is a service's state.
 */
[[nodiscard]] std::string ResolveWritablePath(const std::string& _path);

} // namespace Outpost
