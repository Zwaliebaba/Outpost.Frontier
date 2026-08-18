#pragma once

/*
 * Assertions.
 *
 * Named Debug.h rather than Assert.h, which would shadow the CRT's <assert.h>
 * on a case-insensitive filesystem once this folder is on the include path.
 *
 * NEURON_ASSERT is debug-only and states an invariant the code believes. It is
 * not input validation: anything reading a file, a socket or a config reports a
 * diagnostic instead and keeps going (AGENTS.md §5) -- an assert on hostile
 * input is a crash with extra steps.
 *
 * NEURON_VERIFY evaluates its expression in every configuration and is for the
 * cases where the call itself must happen; only the check is compiled out.
 */

namespace Neuron
{

/// Reports a failed assertion and breaks into the debugger when one is attached.
void AssertFailed(const char* _expression, const char* _file, int _line, const char* _message) noexcept;

} // namespace Neuron

#if defined(_DEBUG)
#   define NEURON_ASSERT(_expr)                                                                                                            \
      do                                                                                                                                   \
      {                                                                                                                                    \
        if (!(_expr))                                                                                                                      \
        {                                                                                                                                  \
          ::Neuron::AssertFailed(#_expr, __FILE__, __LINE__, nullptr);                                                                     \
        }                                                                                                                                  \
      } while (false)

#   define NEURON_ASSERT_MSG(_expr, _message)                                                                                              \
      do                                                                                                                                   \
      {                                                                                                                                    \
        if (!(_expr))                                                                                                                      \
        {                                                                                                                                  \
          ::Neuron::AssertFailed(#_expr, __FILE__, __LINE__, (_message));                                                                   \
        }                                                                                                                                  \
      } while (false)

#   define NEURON_VERIFY(_expr) NEURON_ASSERT(_expr)
#else
#   define NEURON_ASSERT(_expr) ((void)0)
#   define NEURON_ASSERT_MSG(_expr, _message) ((void)0)
#   define NEURON_VERIFY(_expr) ((void)(_expr))
#endif
