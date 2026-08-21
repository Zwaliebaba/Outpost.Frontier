#include "pch.h"

#include "ConfigLoad.h"

#include "Json.h"

#include <ShlObj.h>

#include <cstdio>
#include <vector>

namespace Outpost
{
namespace
{

constexpr const char* BASE_CONFIG_NAME = "Outpost.json";
constexpr const char* USER_CONFIG_NAME = "Settings.json";

std::string ToUtf8(const std::wstring& _wide)
{
  if (_wide.empty())
  {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, _wide.c_str(), static_cast<int>(_wide.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, _wide.c_str(), static_cast<int>(_wide.size()), result.data(), bytes, nullptr, nullptr);
  return result;
}

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

/// Either kind. `ResolveContentPath` is asked about directories as often as
/// files, and a directory answering "no" would send it looking beside the
/// executable for something already in front of it.
[[nodiscard]] bool PathExists(const std::string& _path)
{
  return GetFileAttributesW(ToWide(_path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// Reports parse diagnostics with the file name attached, so a message names
/// the file, the line and the column a person can go and fix.
void ReportJsonErrors(const std::string& _path, const std::vector<Neuron::JsonError>& _errors, std::vector<std::string>& _outMessages)
{
  for (const Neuron::JsonError& error : _errors)
  {
    _outMessages.push_back(_path + "(" + std::to_string(error.line) + "," + std::to_string(error.column) + "): " + error.message);
  }
}

} // namespace

std::string ExecutableDirectory()
{
  std::wstring path(MAX_PATH, L'\0');
  for (;;)
  {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0)
    {
      return {};
    }
    if (length < path.size())
    {
      path.resize(length);
      break;
    }
    path.resize(path.size() * 2); // Long path: try again with room.
  }

  const std::size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? std::string{} : ToUtf8(path.substr(0, separator + 1));
}

std::string ResolveContentPath(const std::string& _path)
{
  if (_path.empty())
  {
    return {};
  }
  if (PathExists(_path))
  {
    return _path;
  }
  const std::string beside = ExecutableDirectory() + _path;
  return PathExists(beside) ? beside : std::string{};
}

std::string ResolveWritablePath(const std::string& _path)
{
  if (_path.empty())
  {
    return {};
  }
  // Absolute as given. The check is the same one `ResolveContentPath` makes
  // implicitly by trying the path first, spelled out here because there is no
  // existence test to hide behind.
  const bool absolute = _path.size() > 1 && (_path[1] == ':' || _path[0] == '\\' || _path[0] == '/');
  return absolute ? _path : ExecutableDirectory() + _path;
}

std::string UserSettingsDirectory()
{
  PWSTR folder = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder)))
  {
    return {};
  }
  std::wstring directory(folder);
  CoTaskMemFree(folder);
  directory += L"\\Outpost.Frontier\\";
  CreateDirectoryW(directory.c_str(), nullptr); // Already existing is not an error.
  return ToUtf8(directory);
}

bool ReadTextFile(const std::string& _path, std::string& _outText)
{
  const HANDLE file =
    CreateFileW(ToWide(_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    return false;
  }

  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart > 64 * 1024 * 1024)
  {
    CloseHandle(file);
    return false;
  }

  _outText.resize(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = _outText.empty() ? TRUE : ReadFile(file, _outText.data(), static_cast<DWORD>(_outText.size()), &read, nullptr);
  CloseHandle(file);

  if (ok == FALSE || read != _outText.size())
  {
    _outText.clear();
    return false;
  }
  return true;
}

bool LoadAppConfig(AppConfig& _outConfig, ConfigPaths& _outPaths, ConfigDiagnostics& _diagnostics)
{
  // Working directory first, then beside the executable. The directory is what
  // replaces a command line: a runner chooses a configuration by choosing where
  // it launches from.
  const std::string beside = ExecutableDirectory() + BASE_CONFIG_NAME;
  if (FileExists(BASE_CONFIG_NAME))
  {
    _outPaths.base = BASE_CONFIG_NAME;
  }
  else if (FileExists(beside))
  {
    _outPaths.base = beside;
  }
  else
  {
    _diagnostics.errors.push_back(std::string("no ") + BASE_CONFIG_NAME + " in the working directory or beside the executable");
    return false;
  }

  std::string text;
  if (!ReadTextFile(_outPaths.base, text))
  {
    _diagnostics.errors.push_back("could not read " + _outPaths.base);
    return false;
  }

  Neuron::JsonDocument document;
  std::vector<Neuron::JsonError> parseErrors;
  if (!Neuron::JsonDocument::Parse(text, document, parseErrors, {}))
  {
    ReportJsonErrors(_outPaths.base, parseErrors, _diagnostics.errors);
    return false;
  }

  ApplyConfigLayer(document.Root(), _outConfig, _diagnostics);
  if (!_diagnostics.Ok())
  {
    return false; // A shipped config with a wrong type is a build problem, not a runtime one.
  }

  // The user layer is optional in every way: absent, empty or broken all leave
  // the game starting on the shipped values.
  const std::string userDirectory = UserSettingsDirectory();
  if (!userDirectory.empty())
  {
    _outPaths.userLayer = userDirectory + USER_CONFIG_NAME;
    std::string userText;
    if (ReadTextFile(_outPaths.userLayer, userText))
    {
      Neuron::JsonDocument userDocument;
      std::vector<Neuron::JsonError> userErrors;
      if (Neuron::JsonDocument::Parse(userText, userDocument, userErrors, {}))
      {
        ConfigDiagnostics userDiagnostics;
        ApplyUserLayer(userDocument.Root(), _outConfig, userDiagnostics);

        // Preferences must never be fatal: a bad value here is reported and the
        // shipped default stands.
        for (std::string& warning : userDiagnostics.warnings)
        {
          _diagnostics.warnings.push_back(std::move(warning));
        }
        for (std::string& error : userDiagnostics.errors)
        {
          _diagnostics.warnings.push_back("user settings: " + error + " (ignored)");
        }
      }
      else
      {
        std::vector<std::string> messages;
        ReportJsonErrors(_outPaths.userLayer, userErrors, messages);
        for (std::string& message : messages)
        {
          _diagnostics.warnings.push_back("user settings ignored: " + message);
        }
      }
    }
  }

  return true;
}

} // namespace Outpost
