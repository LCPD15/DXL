#pragma once
#include <cmath>
#include <string_view>
namespace DXL {
// Exact executable basename only: display names and similarly named launchers
// must not inherit game-specific graphics compatibility defaults.
constexpr unsigned DefaultFgBufferThreshold(std::wstring_view executable) noexcept {
    executable.remove_prefix(executable.find_last_of(L"\\/") == std::wstring_view::npos
        ? 0 : executable.find_last_of(L"\\/") + 1);
    constexpr std::wstring_view yysls = L"yysls.exe";
    if (executable.size() != yysls.size()) return 4u;
    for (size_t i = 0; i < yysls.size(); ++i) {
        const auto c = executable[i] >= L'A' && executable[i] <= L'Z'
            ? executable[i] + (L'a' - L'A') : executable[i];
        if (c != yysls[i]) return 4u;
    }
    return 8u;
}
inline unsigned NormalizeFgBufferThreshold(double value, unsigned fallback = 4u) noexcept {
    return std::isfinite(value) && value >= 2 && value <= 16 && std::floor(value) == value
        ? static_cast<unsigned>(value) : fallback;
}
}
