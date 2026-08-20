#pragma once

#include "AppConfig.h"

/*
 * The bake host mode (build order U1).
 *
 * `GenerateUniverse` is pure and returns bytes; something has to write them to
 * a path, and that something is the composition root -- GameLogic stays
 * OS-free (ADR-009 §7), which is the same division `UniverseLoad` makes in the
 * other direction.
 *
 * Selected by `"mode": "bake"` in `Outpost.json`, because there is no argv to
 * pass a seed on (ADR-012). The mode writes the universe and exits: it is a
 * content tool that happens to ship inside the game's binary, which is the
 * cheapest way to guarantee the generator that made the content is exactly the
 * one the game links.
 */

namespace Outpost
{

/// Generates the universe from the config's recipe, writes the canonical JSON
/// beside the configured universe definition, and returns a process exit code.
[[nodiscard]] int RunBake(const AppConfig& _config);

} // namespace Outpost
