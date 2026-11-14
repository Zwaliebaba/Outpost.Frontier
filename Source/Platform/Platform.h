// Platform: Win32 window, input, timing, file system.
#pragma once

#include <string_view>

namespace Platform
{
    // The library's name; a placeholder that proves the library builds and links (Plan 0.2).
    [[nodiscard]] std::string_view ModuleName() noexcept;
}
