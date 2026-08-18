#pragma once

#include "Universe.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * Universe definition: JSON bytes in, UniverseDef out (ADR-009 §7, ADR-012 §D).
 *
 * A pure function. It takes bytes, not a path -- the hosts open files, which is
 * what keeps GameLogic free of the OS and free of anything to mock. It never
 * throws and never asserts: this reads a file a person edited, so every way it
 * can be wrong is a diagnostic with a line and a JSON path on it.
 *
 * It reports **every** problem it finds rather than the first (ADR-012 §C8).
 * Authoring a universe means fixing five things at once, and a parser that
 * surfaces them one run at a time turns that into five runs.
 */

namespace Game
{

/// Where the content stopped making sense. `path` is the JSON path
/// (`systems[0].stations[1].position.x`) rather than a byte offset, because
/// that is what a person editing the file can act on.
struct UniverseDiagnostic
{
  std::uint32_t line = 0; // 1-based; 0 when the problem is not at one line.
  std::string path;
  std::string message;

  /// "Frontier.json(42) systems[0].stations[1]: duplicate station id 1"
  [[nodiscard]] std::string Text(std::string_view _file = {}) const;
};

/*
 * Parses a universe definition.
 *
 * False when the result is unusable, with everything wrong recorded in
 * `_outDiagnostics`. A true return with a non-empty diagnostics list does not
 * happen: anything worth reporting here is fatal, because a universe both
 * halves must agree on has no degraded mode.
 */
[[nodiscard]] bool ParseUniverse(std::string_view _json, UniverseDef& _outUniverse, std::vector<UniverseDiagnostic>& _outDiagnostics);

} // namespace Game
