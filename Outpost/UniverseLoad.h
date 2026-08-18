#pragma once

#include "Universe.h"

#include <cstdint>
#include <string>
#include <vector>

/*
 * Reading the universe definition off disk (ADR-009 §7).
 *
 * The split is the point: **this** file opens files, and GameLogic's
 * `ParseUniverse` turns bytes into a `UniverseDef`. GameLogic never learns what
 * a path is, which is what keeps it OS-free, deterministic and testable without
 * a filesystem -- the same discipline `ConfigLoad` applies to configuration.
 *
 * Resolution mirrors `ConfigLoad` exactly, and for the same reason: a dev or CI
 * runner selects content by choosing a directory to launch from, never by
 * passing a flag (ADR-012 §A2).
 */

namespace Outpost
{

struct UniverseLoadResult
{
  Game::UniverseDef universe;
  std::uint64_t universeHash = 0;
  std::string path; // The file actually read, for the log.
};

/*
 * Loads and parses the universe named by the configuration.
 *
 * False on any problem, with every diagnostic the parser found in
 * `_outErrors` -- already formatted with the file, line and JSON path. There is
 * no partial success: a universe the two halves might disagree about is worse
 * than a refusal to start (ADR-009 §8).
 */
[[nodiscard]] bool LoadUniverse(const std::string& _definitionPath, UniverseLoadResult& _outResult, std::vector<std::string>& _outErrors);

} // namespace Outpost
