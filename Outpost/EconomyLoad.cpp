#include "pch.h"

#include "EconomyLoad.h"

#include "ConfigLoad.h"

#include "EconomyParse.h"

#include <cstdint>

namespace Outpost
{

bool LoadEconomy(const std::string& _definitionPath, EconomyLoadResult& _outResult, std::vector<std::string>& _outErrors)
{
  _outResult = EconomyLoadResult{};

  const std::string path = ResolveContentPath(_definitionPath);
  if (path.empty())
  {
    _outErrors.push_back("economy definition not found: " + _definitionPath);
    return false;
  }

  std::string text;
  if (!ReadTextFile(path, text))
  {
    _outErrors.push_back("could not read the economy definition: " + path);
    return false;
  }

  std::vector<Game::EconomyDiagnostic> diagnostics;
  if (!Game::ParseEconomy(text, _outResult.economy, diagnostics))
  {
    for (const Game::EconomyDiagnostic& diagnostic : diagnostics)
    {
      _outErrors.push_back(diagnostic.Text(path));
    }
    return false;
  }

  _outResult.path = path;
  _outResult.economyHash = Game::ComputeEconomyHash(_outResult.economy);
  return true;
}

} // namespace Outpost
