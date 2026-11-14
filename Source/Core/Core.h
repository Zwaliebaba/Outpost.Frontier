// Core: Fixed64, vectors, Random64, Checksum, IDs, the .NET-compatible collections and hashes, events, JSON, logging, file I/O.
#pragma once

#include <string_view>

namespace Core
{
    // The library's name; a placeholder that proves the library builds and links (Plan 0.2).
    [[nodiscard]] std::string_view ModuleName() noexcept;
}
