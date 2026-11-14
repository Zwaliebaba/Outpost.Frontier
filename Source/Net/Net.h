// Net: lockstep sessions over LAN.
#pragma once

#include <string_view>

namespace Net
{
    // The library's name; a placeholder that proves the library builds and links (Plan 0.2).
    [[nodiscard]] std::string_view ModuleName() noexcept;
}
