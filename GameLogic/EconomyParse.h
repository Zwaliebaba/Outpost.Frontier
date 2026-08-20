#pragma once

#include "EconomyDef.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * The economy definition: JSON bytes in, `EconomyDef` out (ADR-024 §7,
 * build order E1a).
 *
 * A pure function, shaped deliberately like `ParseUniverse` -- it takes bytes
 * rather than a path, so GameLogic never learns what a filesystem is and the
 * suite needs nothing to mock; it never throws and never asserts, because this
 * reads a file a person edits and every way it can be wrong is a diagnostic
 * with a line and a JSON path on it; and it reports **every** problem it finds
 * rather than the first (ADR-012 §C8), since authoring balance means fixing
 * five things at once and a parser that surfaces them one run at a time turns
 * that into five runs.
 *
 * **What this refuses and what it leaves to the suite.** Here: structure,
 * ranges, and referential integrity -- a recipe naming an ore that does not
 * exist, a composition that does not sum to 100, a hull that cannot carry
 * cargo. Those are content that two halves could disagree about, and content
 * both halves must agree on has no degraded mode. *Not* here: balance shape --
 * that alloys compress, that rarer ore is slower to mine, that higher tiers
 * dominate. Those are assertions in `EconomyParseTests`, deliberately, so
 * retuning the economy is editing a file rather than editing a parser.
 */

namespace Game
{

/// Where the content stopped making sense. `path` is the JSON path
/// (`alloys[4].inputs[1].ore`) rather than a byte offset, because that is what
/// a person editing the file can act on.
struct EconomyDiagnostic
{
  std::uint32_t line = 0; // 1-based; 0 when the problem is not at one line.
  std::string path;
  std::string message;

  /// "Economy.json(51) alloys[1].inputs[0]: 'astracyte' is not a known ore"
  [[nodiscard]] std::string Text(std::string_view _file = {}) const;
};

/*
 * Parses an economy definition.
 *
 * False when the result is unusable, with everything wrong recorded in
 * `_outDiagnostics`. A true return with a non-empty diagnostics list does not
 * happen: anything worth reporting here is fatal, for the reason the universe
 * parser gives -- a definition the two halves might disagree about is worse
 * than a refusal to start.
 */
[[nodiscard]] bool ParseEconomy(std::string_view _json, EconomyDef& _outEconomy, std::vector<EconomyDiagnostic>& _outDiagnostics);

} // namespace Game
