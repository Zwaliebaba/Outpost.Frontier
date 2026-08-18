#include "pch.h"

#include "Log.h"

#include "Clock.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace Neuron
{
namespace
{

std::mutex g_logMutex;
std::FILE* g_logFile = nullptr;

const char* LevelText(LogLevel _level) noexcept
{
  switch (_level)
  {
    case LogLevel::Trace:
      return "trace";
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info ";
    case LogLevel::Warning:
      return "warn ";
    case LogLevel::Error:
      return "error";
  }
  return "?????";
}

} // namespace

LogLevel Log::sm_minimumLevel = LogLevel::Info;

void Log::Initialise(std::string_view _filePath, LogLevel _minimumLevel) noexcept
{
  const std::lock_guard<std::mutex> lock(g_logMutex);
  sm_minimumLevel = _minimumLevel;

  if (g_logFile != nullptr)
  {
    std::fclose(g_logFile);
    g_logFile = nullptr;
  }
  if (!_filePath.empty())
  {
    const std::string path(_filePath);
    if (fopen_s(&g_logFile, path.c_str(), "wb") != 0)
    {
      g_logFile = nullptr; // Losing the file is not worth failing a launch over.
    }
  }
}

void Log::Shutdown() noexcept
{
  const std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_logFile != nullptr)
  {
    std::fflush(g_logFile);
    std::fclose(g_logFile);
    g_logFile = nullptr;
  }
}

void Log::SetMinimumLevel(LogLevel _level) noexcept
{
  sm_minimumLevel = _level;
}

LogLevel Log::MinimumLevel() noexcept
{
  return sm_minimumLevel;
}

LogLevel Log::ParseLevel(std::string_view _text) noexcept
{
  if (_text == "trace")
  {
    return LogLevel::Trace;
  }
  if (_text == "debug")
  {
    return LogLevel::Debug;
  }
  if (_text == "warning")
  {
    return LogLevel::Warning;
  }
  if (_text == "error")
  {
    return LogLevel::Error;
  }
  return LogLevel::Info;
}

void Log::Write(LogLevel _level, const char* _format, ...) noexcept
{
  if (_level < sm_minimumLevel || _format == nullptr)
  {
    return;
  }

  char message[1024];
  va_list args;
  va_start(args, _format);
  const int written = vsnprintf(message, sizeof(message), _format, args);
  va_end(args);
  if (written < 0)
  {
    return;
  }

  char line[1152];
  const int lineLength =
    snprintf(line, sizeof(line), "[%8.3f] %s | %s\n", Clock::SecondsSinceStart(), LevelText(_level), message);
  if (lineLength < 0)
  {
    return;
  }

  const std::lock_guard<std::mutex> lock(g_logMutex);
  OutputDebugStringA(line);
  std::fputs(line, _level >= LogLevel::Warning ? stderr : stdout);
  if (g_logFile != nullptr)
  {
    std::fputs(line, g_logFile);
    if (_level >= LogLevel::Warning)
    {
      std::fflush(g_logFile); // A crash after a warning should still have the warning on disk.
    }
  }
}

} // namespace Neuron
