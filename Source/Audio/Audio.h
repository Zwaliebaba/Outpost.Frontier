// Audio: XAudio2 graph and the Fabric-style event runtime.
#pragma once

#include <string_view>

namespace Audio
{
    // The library's name; a placeholder that proves the library builds and links (Plan 0.2).
    [[nodiscard]] std::string_view ModuleName() noexcept;
}
