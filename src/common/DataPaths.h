#pragma once
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace DXL::DataPaths {
namespace fs = std::filesystem;

inline bool RegularFile(const fs::path& path) {
    std::error_code ec;
    return !fs::is_symlink(path, ec) && fs::is_regular_file(path, ec);
}
inline bool HasSettings(const fs::path& root) {
    std::error_code ec;
    return RegularFile(root / L"settings.json") || RegularFile(root / L"launcher.json") ||
        fs::is_directory(root / L"profiles", ec);
}
// Copy only configuration, never executable files, diagnostics or browser caches.
// Existing destination files win. Keep the source intact for rollback.
inline bool ImportSettings(const fs::path& source, const fs::path& target) {
    std::error_code ec;
    if (source.empty() || target.empty() || source == target || !HasSettings(source)) return false;
    fs::create_directories(target, ec);
    if (ec) return false;
    auto copy = [&](const fs::path& from, const fs::path& to) {
        if (!RegularFile(from)) return true;
        std::error_code error;
        fs::create_directories(to.parent_path(), error);
        if (error) return false;
        fs::copy_file(from, to, fs::copy_options::skip_existing, error);
        return !error;
    };
    if (!copy(source / L"launcher.json", target / L"launcher.json")) return false;
    const auto profiles = source / L"profiles";
    if (fs::is_directory(profiles, ec) && !fs::is_symlink(profiles, ec)) {
        for (fs::directory_iterator it(profiles, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == L".json" &&
                !copy(it->path(), target / L"profiles" / it->path().filename())) return false;
        }
        if (ec) return false;
    }
    // Write the main settings last, so interrupted imports can be retried.
    return copy(source / L"settings.json", target / L"settings.json");
}
inline fs::path ResolveConfigRoot(const fs::path& local, const fs::path& beside) {
    if (local.empty()) return {};
    const auto target = local / L"DXL";
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) return {};
    const auto marker = target / L"config-migration-v1.txt";
    if (RegularFile(target / L"settings.json") || RegularFile(marker)) return target;
    // The old product name is deliberately retained only for data migration.
    for (const auto& candidate : {beside, local / L"DLSS5-Quick"}) {
        if (!candidate.empty() && candidate != target && HasSettings(candidate)) {
            if (ImportSettings(candidate, target)) {
                std::ofstream(marker) << "DXL configuration imported; original files retained.\n";
            }
            break;
        }
    }
    return target;
}
inline fs::path ConfigRoot(HMODULE module) noexcept {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return {};
    const fs::path localPath(local);
    CoTaskMemFree(local);
    wchar_t buffer[32768]{};
    const DWORD count = GetModuleFileNameW(module, buffer, DWORD(std::size(buffer)));
    const auto beside = count && count < std::size(buffer) ? fs::path(buffer).parent_path() : fs::path{};
    // Launcher and injected DLL can start together; serialize migration only.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\DXL.ConfigMigration");
    if (!mutex) return localPath / L"DXL";
    const DWORD wait = WaitForSingleObject(mutex, 5000);
    fs::path result = localPath / L"DXL";
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        result = ResolveConfigRoot(localPath, beside);
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
    return result;
}
} // namespace DXL::DataPaths
