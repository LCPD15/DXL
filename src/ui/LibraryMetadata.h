#pragma once

// Metadata enrichment never searches game executables or changes the library.
// Only installed platform roots are matched to the exact profile paths supplied.
#include "GameScan.h"
#include "LaunchArguments.h"
#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")

namespace DXL::LibraryMetadata {
namespace fs = std::filesystem;
struct Install { fs::path root; std::wstring source, steamAppId; };

inline bool IsAppId(const std::wstring& id) {
    return !id.empty() && id.size() <= 12 &&
        std::all_of(id.begin(), id.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
inline std::wstring Wide(std::string_view text) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count > 0 ? count : 0, L'\0');
    if (count > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
inline std::wstring NormalPath(const fs::path& path) {
    if (!path.is_absolute()) return {};
    std::wstring value = path.lexically_normal().wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return scan_detail::Lowered(value);
}
inline bool Within(const fs::path& child, const fs::path& root) {
    const auto c = NormalPath(child), r = NormalPath(root);
    // A platform root must be a game directory, not an entire volume/share.
    return !c.empty() && !r.empty() && r != NormalPath(root.root_path()) &&
        c.size() > r.size() && c.compare(0, r.size(), r) == 0 && c[r.size()] == L'\\';
}
inline const Install* Match(const fs::path& exe, const std::vector<Install>& installs) {
    const Install* best = nullptr;
    size_t longest = 0;
    for (const auto& install : installs) {
        const size_t length = NormalPath(install.root).size();
        if (length > longest && Within(exe, install.root)) {
            best = &install; longest = length;
        }
    }
    return best;
}
inline void ReadSteamRoots(const std::vector<std::wstring>& libraries, std::vector<Install>& out) {
    for (const auto& library : libraries) {
        const fs::path apps = fs::path(library) / L"steamapps";
        std::error_code ec;
        for (fs::directory_iterator it(apps, ec), end; !ec && it != end; it.increment(ec)) {
            const auto leaf = it->path().filename().wstring();
            if (leaf.rfind(L"appmanifest_", 0) || it->path().extension() != L".acf") continue;
            const auto text = scan_detail::ReadAllText(it->path());
            const auto id = scan_detail::ExtractVdfValue(text, "appid");
            const auto directory = scan_detail::ExtractVdfValue(text, "installdir");
            if (!IsAppId(id) || directory.empty()) continue;
            const fs::path common = apps / L"common", root = common / directory;
            std::error_code check;
            if (Within(root, common) && fs::is_directory(root, check)) out.push_back({ root, L"Steam", id });
        }
    }
}
inline void ReadEpicRoots(const fs::path& manifests, std::vector<Install>& out) {
    std::error_code ec;
    for (fs::directory_iterator it(manifests, ec), end; !ec && it != end; it.increment(ec)) {
        if (scan_detail::Lowered(it->path().extension().wstring()) != L".item") continue;
        const auto text = scan_detail::ReadAllText(it->path());
        const auto category = text.find("\"AppCategories\"");
        const auto close = category == std::string::npos ? category : text.find(']', category);
        const auto games = category == std::string::npos ? category : text.find("\"games\"", category);
        if (close == std::string::npos || games == std::string::npos || games > close) continue;
        const fs::path root(Wide(LaunchArguments::ReadField(text, "InstallLocation")));
        std::error_code check;
        if (root.is_absolute() && fs::is_directory(root, check)) out.push_back({ root, L"Epic", {} });
    }
}
inline std::vector<Install> Collect(const std::vector<std::wstring>& libraries) {
    std::vector<Install> out;
    ReadSteamRoots(libraries, out);
    PWSTR data = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &data))) {
        const fs::path manifests = fs::path(data) / L"Epic/EpicGamesLauncher/Data/Manifests";
        CoTaskMemFree(data);
        ReadEpicRoots(manifests, out);
    }
    for (const wchar_t* keyName : { L"SOFTWARE\\GOG.com\\Games", L"SOFTWARE\\WOW6432Node\\GOG.com\\Games" }) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyName, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for (DWORD index = 0;; ++index) {
            wchar_t name[256]{}; DWORD size = 256;
            if (RegEnumKeyExW(key, index, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const auto child = std::wstring(keyName) + L"\\" + name;
            const fs::path root(scan_detail::ReadRegString(HKEY_LOCAL_MACHINE, child.c_str(), L"PATH"));
            std::error_code ec;
            if (root.is_absolute() && fs::is_directory(root, ec)) out.push_back({ root, L"GOG", {} });
        }
        RegCloseKey(key);
    }
    return out;
}

inline bool ImageExtension(const fs::path& path) {
    const auto ext = scan_detail::Lowered(path.extension().wstring());
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
        ext == L".webp" || ext == L".bmp" || ext == L".gif";
}
inline bool ValidateImage(const fs::path& path) {
    std::error_code ec;
    if (!ImageExtension(path) || !fs::is_regular_file(path, ec)) return false;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes == 0 || bytes > 32 * 1024 * 1024) return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT width = 0, height = 0;
    return SUCCEEDED(frame->GetSize(&width, &height)) && width && height && width <= 16384 && height <= 16384;
}
inline std::wstring CacheName(const fs::path& source, std::wstring_view identity) {
    // Never interpolate a profile id or user filename into a destination path.
    // The source mtime changes the URL when a player replaces the same artwork.
    uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&](std::wstring_view part) {
        for (wchar_t c : part) { hash ^= static_cast<uint16_t>(c); hash *= 1099511628211ULL; }
    };
    mix(identity); mix(NormalPath(source));
    std::error_code ec;
    const auto modified = fs::last_write_time(source, ec);
    if (!ec) { hash ^= static_cast<uint64_t>(modified.time_since_epoch().count()); hash *= 1099511628211ULL; }
    wchar_t leaf[48]{};
    swprintf_s(leaf, L"cover_%016llx", static_cast<unsigned long long>(hash));
    return std::wstring(leaf) + scan_detail::Lowered(source.extension().wstring());
}
inline std::wstring CacheImage(const fs::path& source, const fs::path& cache, std::wstring_view identity) {
    if (!ValidateImage(source)) return {};
    const auto leaf = CacheName(source, identity);
    const auto destination = cache / leaf;
    if (!Within(destination, cache)) return {};
    std::error_code ec;
    fs::create_directories(cache, ec);
    if (ec) return {};
    if (NormalPath(source) != NormalPath(destination)) {
        const fs::path temporary = destination.wstring() + L".tmp." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId());
        if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE)) return {};
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return {};
        }
    }
    return L"covercache/" + leaf;
}
inline fs::path FindSteamCover(const std::vector<std::wstring>& libraries, const std::wstring& appId) {
    if (!IsAppId(appId)) return {};
    for (const auto& library : libraries) {
        const fs::path cache = fs::path(library) / L"appcache/librarycache";
        // Both old appid_library_600x900.jpg and new appid/library_600x900.jpg layouts.
        for (const auto& folder : { cache / appId, cache }) {
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
                const auto leaf = scan_detail::Lowered(it->path().filename().wstring());
                const auto prefix = folder == cache ? appId + L"_library_600x900" : L"library_600x900";
                if (leaf.rfind(prefix, 0) == 0 && ImageExtension(it->path())) return it->path();
            }
        }
    }
    return {};
}
inline std::wstring PickCover(HWND owner) {
    wchar_t file[32768]{};
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"封面图片 / Cover image\0*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif\0";
    dialog.lpstrFile = file; dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrTitle = L"选择游戏封面 / Choose game cover";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameW(&dialog) ? file : L"";
}
} // namespace DXL::LibraryMetadata
