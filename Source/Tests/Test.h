// A minimal test runner (the library policy rules out test frameworks): TEST registers a function,
// CHECK records a failure with its location, and main() runs every test and returns the failure count.
#pragma once

#include <source_location>
#include <string_view>

namespace Tests
{
    using TestFunction = void (*)();

    struct Registration
    {
        Registration(std::string_view _name, TestFunction _function) noexcept;
    };

    void Fail(std::string_view _expression, std::source_location _where = std::source_location::current());
}

#define OUTPOST_TEST_CONCAT2(a, b) a##b
#define OUTPOST_TEST_CONCAT(a, b) OUTPOST_TEST_CONCAT2(a, b)

#define TEST(name)                                                                               \
    static void name();                                                                          \
    static const ::Tests::Registration OUTPOST_TEST_CONCAT(g_registration_, name){#name, &name}; \
    static void name()

#define CHECK(expression)                    \
    do                                       \
    {                                        \
        if (!(expression))                   \
            ::Tests::Fail(#expression);      \
    } while (false)
