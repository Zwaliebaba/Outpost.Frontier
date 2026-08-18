#include "pch.h"

#include "UniverseLoad.h"

#include "ConfigLoad.h"

#include "UniverseParse.h"

#include <cstdint>

namespace Outpost
{

bool LoadUniverse(const std::string& _definitionPath, UniverseLoadResult& _outResult, std::vector<std::string>& _outErrors)
{
  _outResult = UniverseLoadResult{};

  // The configured path, then the same path beside the executable -- the rule
  // the base config is found by, and now literally the same function rather
  // than a second copy of it that was free to drift (ADR-012 §A2).
  const std::string path = ResolveContentPath(_definitionPath);
  if (path.empty())
  {
    _outErrors.push_back("universe definition not found: " + _definitionPath);
    return false;
  }

  std::string text;
  if (!ReadTextFile(path, text))
  {
    _outErrors.push_back("could not read the universe definition: " + path);
    return false;
  }

  std::vector<Game::UniverseDiagnostic> diagnostics;
  if (!Game::ParseUniverse(text, _outResult.universe, diagnostics))
  {
    for (const Game::UniverseDiagnostic& diagnostic : diagnostics)
    {
      _outErrors.push_back(diagnostic.Text(path));
    }
    return false;
  }

  _outResult.path = path;
  _outResult.universeHash = Game::ComputeUniverseHash(_outResult.universe);
  return true;
}

} // namespace Outpost
