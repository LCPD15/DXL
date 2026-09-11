#pragma once
#include <string_view>

namespace DXL {
// Match only dedicated, known crash-report executables. Directory names and
// command-line arguments are not identities; renderer/helper children remain
// eligible for launcher handoff, including Vulkan renderers.
inline bool IsDedicatedCrashReporter(std::wstring_view imagePath) noexcept {
    const auto slash=imagePath.find_last_of(L"\\/");
    const auto leaf=imagePath.substr(slash==imagePath.npos ? 0 : slash+1);
    constexpr std::wstring_view names[]={
        L"crashpad_handler.exe", L"crashreportclient.exe", L"crashreportclienteditor.exe",
        L"unitycrashhandler32.exe", L"unitycrashhandler64.exe",
        L"werfault.exe", L"werfaultsecure.exe"
    };
    for (const auto name : names) {
        if (leaf.size()!=name.size()) continue;
        bool match=true;
        for (size_t i=0; i<name.size(); ++i) {
            const auto c=leaf[i]>=L'A' && leaf[i]<=L'Z' ? leaf[i]+(L'a'-L'A') : leaf[i];
            if (c!=name[i]) { match=false; break; }
        }
        if (match) return true;
    }
    return false;
}
} // namespace DXL
