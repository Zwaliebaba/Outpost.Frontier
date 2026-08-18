#pragma once

#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <tuple>

#if defined(_DEBUG)
#   include <crtdbg.h>
#   define NEW new (_NORMAL_BLOCK, __FILE__, __LINE__)
#else
#   define NEW new
#endif

namespace Neuron
{
template <class... Types> void DebugTrace(const std::string_view _fmt, Types&&... _args)
{
#ifdef _DEBUG
  const std::string message = vformat(_fmt, std::make_format_args(_args...));
  OutputDebugStringA(message.c_str());
#else
  __noop(_fmt);
#endif
}

template <class... Types> void DebugTrace(const std::wstring_view _fmt, Types&&... _args)
{
#ifdef _DEBUG
  const std::wstring message = vformat(_fmt, std::make_wformat_args(_args...));
  OutputDebugStringW(message.c_str());
#else
  __noop(_fmt);
#endif
}

template <class... Types> void __declspec(noreturn) Fatal(const std::string_view _fmt, Types&&... _args)
{
  std::ignore = vformat(_fmt, std::make_format_args(_args...));
  __debugbreak();
  throw std::exception("Fatal Error");
}

template <class... Types> void __declspec(noreturn) Fatal(const std::wstring_view _fmt, Types&&... _args)
{
  std::ignore = vformat(_fmt, std::make_wformat_args(_args...));
  __debugbreak();
  throw std::exception("Fatal Error");
}
} // namespace Neuron

#define ASSERT(expression) (void)((!!(expression)) || (Neuron::Fatal(_CRT_WIDE("Assert Failure")), 0))
#define ASSERT_TEXT(expression, ...) (void)((!!(expression)) || (Neuron::Fatal(__VA_ARGS__), 0))

#ifdef _DEBUG
#   define DEBUG_ASSERT(expression) ASSERT(expression)
#   define DEBUG_ASSERT_TEXT(expression, ...) ASSERT_TEXT(expression, __VA_ARGS__)
#   define DEBUG_WARNING(expression, ...) (void)((!(expression)) || (DebugTrace(__VA_ARGS__), 0))

#else
#   define DEBUG_ASSERT(expression) (__noop(expression))
#   define DEBUG_ASSERT_TEXT(expression, ...) (__noop(expression, __VA_ARGS__))
#   define DEBUG_WARNING(expression, ...) (__noop(expression, __VA_ARGS__))

#endif
