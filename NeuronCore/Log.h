#pragma once

#include <cstdint>
#include <string_view>

/*
 * Logging.
 *
 * One sink, opened by the composition root from config (ADR-012) and written to
 * by every thread. Formatting is printf-style and the call is locked: this is a
 * diagnostic channel, not a hot path, and interleaved half-lines from two
 * threads cost more debugging time than the lock ever costs in frames.
 *
 * Per-frame measurement belongs in Telemetry, not here.
 */

namespace Neuron
{

enum class LogLevel : std::uint8_t
{
  Trace,
  Debug,
  Info,
  Warning,
  Error
};

class Log
{
public:
  /// Opens the log file. An empty path logs to the debugger and console only.
  static void Initialise(std::string_view _filePath, LogLevel _minimumLevel) noexcept;
  static void Shutdown() noexcept;

  static void SetMinimumLevel(LogLevel _level) noexcept;
  [[nodiscard]] static LogLevel MinimumLevel() noexcept;

  /// Parses "trace"/"debug"/"info"/"warning"/"error"; unknown text yields Info.
  [[nodiscard]] static LogLevel ParseLevel(std::string_view _text) noexcept;

  static void Write(LogLevel _level, const char* _format, ...) noexcept;

private:
  static LogLevel sm_minimumLevel;
};

} // namespace Neuron

#define NEURON_LOG_TRACE(...) ::Neuron::Log::Write(::Neuron::LogLevel::Trace, __VA_ARGS__)
#define NEURON_LOG_DEBUG(...) ::Neuron::Log::Write(::Neuron::LogLevel::Debug, __VA_ARGS__)
#define NEURON_LOG_INFO(...) ::Neuron::Log::Write(::Neuron::LogLevel::Info, __VA_ARGS__)
#define NEURON_LOG_WARNING(...) ::Neuron::Log::Write(::Neuron::LogLevel::Warning, __VA_ARGS__)
#define NEURON_LOG_ERROR(...) ::Neuron::Log::Write(::Neuron::LogLevel::Error, __VA_ARGS__)
