// Ui: the NGUI-style UI runtime, DirectWrite text.
#pragma once

#include <string_view>

namespace Ui
{
    // The library's name; a placeholder that proves the library builds and links (Plan 0.2).
    [[nodiscard]] std::string_view ModuleName() noexcept;
}
