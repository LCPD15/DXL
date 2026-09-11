#pragma once
#include <windows.h>
#include <string_view>
namespace DXL::UiLanguage {
inline int System(LANGID language = GetUserDefaultUILanguage()) noexcept {
    return PRIMARYLANGID(language) == LANG_CHINESE ? 0 : 2;
}
inline int Resolve(std::string_view saved, LANGID system = GetUserDefaultUILanguage()) noexcept {
    if (saved == "zh") return 0;
    if (saved == "en") return 2;
    return System(system);
}
}
