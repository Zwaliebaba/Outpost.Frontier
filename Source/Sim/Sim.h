// Sim: the simulation: the port of BBI.Game.
#pragma once

#include <string_view>

namespace Sim
{
    // The library's name; a placeholder that proves the library builds and links (Plan 0.2).
    [[nodiscard]] std::string_view ModuleName() noexcept;
}
