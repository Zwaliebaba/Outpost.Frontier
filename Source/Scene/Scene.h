// Scene: the runtime scene: cooked hierarchies, components, prefabs, LOD.
#pragma once

#include <string_view>

namespace Scene
{
    // The library's name; a placeholder that proves the library builds and links (Plan 0.2).
    [[nodiscard]] std::string_view ModuleName() noexcept;
}
