#pragma once
#include <string>
#include <string_view>
#include <cwctype>

namespace DXL {
enum class NgxModuleKind { Other, Sr, Rr, Fg, CachedSr, CachedRr, CachedFg };

// NGX can load executable PE images named e.g. 160_E658700.bin. The SAME
// basename occurs in dlss, dlssd AND dlssg, so the basename is not an identity.
inline NgxModuleKind ClassifyNgxModule(std::wstring_view path) {
    std::wstring p(path);
    for (auto& c : p) c = c == L'/' ? L'\\' : wchar_t(towlower(c));
    const auto slash = p.find_last_of(L'\\');
    const std::wstring_view leaf(p.data() + (slash == p.npos ? 0 : slash + 1),
        p.size() - (slash == p.npos ? 0 : slash + 1));
    // Preserve the existing exclusion of FG debug/variant filenames as well.
    if (leaf.find(L"nvngx_dlssg") != leaf.npos || leaf.find(L"sl.dlss_g") != leaf.npos) return NgxModuleKind::Fg;
    if (leaf == L"nvngx_dlss.dll") return NgxModuleKind::Sr;
    if (leaf == L"nvngx_dlssd.dll") return NgxModuleKind::Rr;
    if (!leaf.ends_with(L".bin") || leaf.size() <= 4) return NgxModuleKind::Other;
    if (p.find(L"\\..\\") != p.npos || p.find(L"\\.\\") != p.npos) return NgxModuleKind::Other;
    constexpr std::wstring_view root = L"\\nvidia\\ngx\\models\\";
    const auto begin = p.rfind(root);
    if (begin == p.npos) return NgxModuleKind::Other;
    std::wstring_view tail(p.data() + begin + root.size(), p.size() - begin - root.size());
    const auto modelEnd = tail.find(L'\\');
    if (modelEnd == tail.npos) return NgxModuleKind::Other;
    const auto model = tail.substr(0, modelEnd);
    tail.remove_prefix(modelEnd + 1);
    if (!tail.starts_with(L"versions\\")) return NgxModuleKind::Other;
    tail.remove_prefix(9);
    const auto versionEnd = tail.find(L'\\');
    if (versionEnd == 0 || versionEnd == tail.npos) return NgxModuleKind::Other;
    tail.remove_prefix(versionEnd + 1);
    if (tail != std::wstring(L"files\\") + std::wstring(leaf)) return NgxModuleKind::Other;
    if (model == L"dlssg") return NgxModuleKind::CachedFg;
    if (model == L"dlss") return NgxModuleKind::CachedSr;
    if (model == L"dlssd") return NgxModuleKind::CachedRr;
    return NgxModuleKind::Other;
}
inline bool IsNativeUpscaler(NgxModuleKind k) noexcept {
    return k == NgxModuleKind::Sr || k == NgxModuleKind::Rr ||
        k == NgxModuleKind::CachedSr || k == NgxModuleKind::CachedRr;
}
inline bool IsFrameGen(NgxModuleKind k) noexcept {
    return k == NgxModuleKind::Fg || k == NgxModuleKind::CachedFg;
}
inline const wchar_t* NgxModuleKindName(NgxModuleKind k) noexcept {
    switch (k) {
    case NgxModuleKind::Sr: return L"SR";
    case NgxModuleKind::Rr: return L"RR";
    case NgxModuleKind::Fg: return L"FG";
    case NgxModuleKind::CachedSr: return L"cache-SR";
    case NgxModuleKind::CachedRr: return L"cache-RR";
    case NgxModuleKind::CachedFg: return L"cache-FG";
    default: return L"generic";
    }
}
} // namespace DXL
