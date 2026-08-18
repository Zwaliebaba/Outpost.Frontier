#include "pch.h"

#include "Debug.h"

#include "Log.h"

#include <cstdio>

namespace Neuron
{

void AssertFailed(const char* _expression, const char* _file, int _line, const char* _message) noexcept
{
  char text[1024];
  if (_message != nullptr)
  {
    snprintf(text, sizeof(text), "assertion failed: %s (%s) at %s:%d", _expression, _message, _file, _line);
  }
  else
  {
    snprintf(text, sizeof(text), "assertion failed: %s at %s:%d", _expression, _file, _line);
  }

  NEURON_LOG_ERROR("%s", text);

  // Break where the invariant broke, not in a handler two frames up. Without a
  // debugger attached, leave the process running: the log line is the record,
  // and a debug build that dies on the first assertion tells you about exactly
  // one of them.
  if (IsDebuggerPresent() != FALSE)
  {
    DebugBreak();
  }
}

} // namespace Neuron
