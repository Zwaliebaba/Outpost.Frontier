#include "pch.h"

#include "UniverseBake.h"

#include "ConfigLoad.h"

#include "UniverseGen.h"
#include "UniverseParse.h"

#include "Clock.h"
#include "Log.h"

#include <fstream>
#include <string>
#include <vector>

namespace Outpost
{

int RunBake(const AppConfig& _config)
{
  const Game::UniverseGenConfig recipe; // The committed universe's recipe (U1).
  NEURON_LOG_INFO("bake: seed %llu, %u systems across %u regions", static_cast<unsigned long long>(recipe.seed),
                  static_cast<unsigned>(recipe.systemCount), static_cast<unsigned>(recipe.regionCount));

  const std::int64_t generateStart = Neuron::Clock::Counter();
  Game::UniverseDef universe;
  if (!Game::GenerateUniverse(recipe, universe))
  {
    NEURON_LOG_ERROR("bake: the recipe cannot be satisfied");
    return 1;
  }
  const double generateMs = Neuron::Clock::MillisecondsBetween(generateStart, Neuron::Clock::Counter());

  std::string json;
  if (!Game::WriteUniverseJson(universe, json))
  {
    NEURON_LOG_ERROR("bake: the universe would not serialise");
    return 1;
  }

  /*
   * Read it straight back before it is trusted.
   *
   * A bake that writes something the game cannot load is worse than a bake that
   * fails: the file is committed, the failure surfaces at someone else's boot,
   * and the diagnostic points at content rather than at the tool. Parsing the
   * bytes here and comparing the hash makes the round trip the bake's own
   * acceptance rather than a test's -- and it produces R17's parse number as a
   * side effect, which is the number that decides whether the content ever has
   * to split per region.
   */
  const std::int64_t parseStart = Neuron::Clock::Counter();
  Game::UniverseDef reloaded;
  std::vector<Game::UniverseDiagnostic> diagnostics;
  const bool parsed = Game::ParseUniverse(json, reloaded, diagnostics);
  const double parseMs = Neuron::Clock::MillisecondsBetween(parseStart, Neuron::Clock::Counter());
  if (!parsed)
  {
    for (const Game::UniverseDiagnostic& diagnostic : diagnostics)
    {
      NEURON_LOG_ERROR("bake: %s", diagnostic.Text("(baked)").c_str());
    }
    return 1;
  }

  const std::int64_t hashStart = Neuron::Clock::Counter();
  const std::uint64_t reloadedHash = Game::ComputeUniverseHash(reloaded);
  const double hashMs = Neuron::Clock::MillisecondsBetween(hashStart, Neuron::Clock::Counter());
  if (reloadedHash != Game::ComputeUniverseHash(universe))
  {
    NEURON_LOG_ERROR("bake: the universe does not survive its own round trip");
    return 1;
  }

  const std::string path = ResolveContentPath(_config.universeDefinition);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
  {
    NEURON_LOG_ERROR("bake: could not open %s for writing", path.c_str());
    return 1;
  }
  out.write(json.data(), static_cast<std::streamsize>(json.size()));
  if (!out)
  {
    NEURON_LOG_ERROR("bake: could not write %s", path.c_str());
    return 1;
  }

  std::size_t anchors = 0;
  std::size_t gates = 0;
  for (const Game::SolarSystem& system : universe.systems)
  {
    anchors += system.anchors.size();
    gates += system.gates.size();
  }

  NEURON_LOG_INFO("bake: %zu regions, %zu constellations, %zu systems, %zu gates, %zu anchors", universe.regions.size(),
                  universe.constellations.size(), universe.systems.size(), gates, anchors);
  NEURON_LOG_INFO("bake: universeHash %016llx, %.2f MB", static_cast<unsigned long long>(reloadedHash),
                  static_cast<double>(json.size()) / (1024.0 * 1024.0));
  NEURON_LOG_INFO("bake: generate %.0f ms, parse %.0f ms, hash %.0f ms -- R17's threshold is ~1000 ms on parse", generateMs, parseMs,
                  hashMs);
  NEURON_LOG_INFO("bake: wrote %s", path.c_str());
  return 0;
}

} // namespace Outpost
