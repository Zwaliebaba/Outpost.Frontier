#include "pch.h"

#include "UniverseLoad.h"

#include "UniverseParse.h"

#include <cstdint>

namespace Outpost
{
namespace
{

std::wstring ToWide(const std::string& _utf8)
{
  if (_utf8.empty())
  {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), static_cast<int>(_utf8.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, _utf8.c_str(), static_cast<int>(_utf8.size()), result.data(), count);
  return result;
}

[[nodiscard]] bool FileExists(const std::string& _path)
{
  const DWORD attributes = GetFileAttributesW(ToWide(_path).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] bool ReadWholeFile(const std::string& _path, std::string& _outText)
{
  const HANDLE file =
      CreateFileW(ToWide(_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    return false;
  }

  LARGE_INTEGER size{};
  // A universe file is hand-authored content; 64 MiB is far past anything
  // reasonable and well short of anything that would hurt to reject.
  if (GetFileSizeEx(file, &size) == 0 || size.QuadPart < 0 || size.QuadPart > 64 * 1024 * 1024)
  {
    CloseHandle(file);
    return false;
  }

  _outText.resize(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = _outText.empty() ? TRUE : ReadFile(file, _outText.data(), static_cast<DWORD>(_outText.size()), &read, nullptr);
  CloseHandle(file);
  return ok != 0 && read == _outText.size();
}

/// The configured path, then the same path beside the executable. Identical to
/// how the base configuration is found, so content and config are never located
/// by different rules (ADR-012 §A2).
[[nodiscard]] std::string ResolvePath(const std::string& _definitionPath)
{
  if (FileExists(_definitionPath))
  {
    return _definitionPath;
  }

  wchar_t modulePath[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
  {
    return {};
  }

  std::wstring directory(modulePath, length);
  const std::size_t slash = directory.find_last_of(L"\\/");
  if (slash == std::wstring::npos)
  {
    return {};
  }
  directory.resize(slash + 1);

  const int bytes = WideCharToMultiByte(CP_UTF8, 0, directory.c_str(), static_cast<int>(directory.size()), nullptr, 0, nullptr, nullptr);
  std::string beside(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, directory.c_str(), static_cast<int>(directory.size()), beside.data(), bytes, nullptr, nullptr);
  beside += _definitionPath;

  return FileExists(beside) ? beside : std::string{};
}

} // namespace

bool LoadUniverse(const std::string& _definitionPath, UniverseLoadResult& _outResult, std::vector<std::string>& _outErrors)
{
  _outResult = UniverseLoadResult{};

  const std::string path = ResolvePath(_definitionPath);
  if (path.empty())
  {
    _outErrors.push_back("universe definition not found: " + _definitionPath);
    return false;
  }

  std::string text;
  if (!ReadWholeFile(path, text))
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
