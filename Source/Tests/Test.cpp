#include "Test.h"

#include <print>
#include <vector>

namespace
{
    struct Entry
    {
        std::string_view name;
        Tests::TestFunction function;
    };

    std::vector<Entry>& Registry()
    {
        static std::vector<Entry> registry;
        return registry;
    }

    int g_failures = 0;
}

namespace Tests
{
    Registration::Registration(std::string_view _name, TestFunction _function) noexcept
    {
        Registry().push_back({_name, _function});
    }

    void Fail(std::string_view _expression, std::source_location _where)
    {
        ++g_failures;
        std::println("  FAILED {} ({}:{})", _expression, _where.file_name(), _where.line());
    }
}

int main()
{
    for (const Entry& entry : Registry())
    {
        const int before = g_failures;
        entry.function();
        std::println("{} {}", g_failures == before ? "ok    " : "FAILED", entry.name);
    }
    std::println("{} tests, {} failures", Registry().size(), g_failures);
    return g_failures;
}
