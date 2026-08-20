#pragma once

#include "EconomyDef.h"

#include <cstdint>
#include <string>
#include <vector>

/*
 * Reading the economy definition off disk (ADR-024 §7, build order E1a).
 *
 * The split is `UniverseLoad`'s, for the same reason: **this** file opens
 * files, and GameLogic's `ParseEconomy` turns bytes into an `EconomyDef`.
 * GameLogic never learns what a path is, which is what keeps it OS-free,
 * deterministic, and testable without a filesystem.
 *
 * Resolution goes through the same `ResolveContentPath` the universe and the
 * base config use, so a dev or CI runner selects content by choosing the
 * directory it launches from rather than by passing a flag (ADR-012 §A2).
 */

namespace Outpost
{

struct EconomyLoadResult
{
  Game::EconomyDef economy;
  std::uint64_t economyHash = 0;
  std::string path; // The file actually read, for the log.
};

/*
 * Loads and parses the economy named by the configuration.
 *
 * False on any problem, with every diagnostic the parser found in `_outErrors`
 * -- already formatted with the file, line and JSON path. There is no partial
 * success: an economy the two halves might disagree about is worse than a
 * refusal to start, which is the posture the universe takes and for a sharper
 * reason here, since a disagreement about a litre is a disagreement about when
 * a hold is full.
 */
[[nodiscard]] bool LoadEconomy(const std::string& _definitionPath, EconomyLoadResult& _outResult, std::vector<std::string>& _outErrors);

} // namespace Outpost
