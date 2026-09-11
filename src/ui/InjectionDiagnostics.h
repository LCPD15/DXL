#pragma once

#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace DXL::InjectionDiagnostics {

enum class ModuleState { Confirmed, Absent, Unknown };
struct ModuleInspection {
    ModuleState state = ModuleState::Unknown;
    DWORD error = ERROR_SUCCESS;
    DWORD count = 0;
    DWORD unreadablePaths = 0;
    uintptr_t base = 0;
    std::wstring path;
};

inline const wchar_t* StateName(ModuleState state) noexcept {
    switch (state) {
    case ModuleState::Confirmed: return L"confirmed";
    case ModuleState::Absent: return L"absent";
    default: return L"unknown";
    }
}

// Compare complete Win32 paths, never just a DLL basename. Ignore only normal
// DOS path spelling differences; this does not search for alternate DLLs.
inline std::wstring PathKey(const std::wstring& path, DWORD& error) {
    error = ERROR_SUCCESS;
    if (path.empty()) { error = ERROR_INVALID_NAME; return {}; }
    std::wstring normal = path;
    std::replace(normal.begin(), normal.end(), L'/', L'\\');
    if (normal.rfind(L"\\\\?\\UNC\\", 0) == 0) normal = L"\\\\" + normal.substr(8);
    else if (normal.rfind(L"\\\\?\\", 0) == 0) normal.erase(0, 4);
    const DWORD needed = GetFullPathNameW(normal.c_str(), 0, nullptr, nullptr);
    if (!needed) { error = GetLastError(); return {}; }
    std::vector<wchar_t> buffer(size_t(needed) + 1);
    const DWORD copied = GetFullPathNameW(normal.c_str(), DWORD(buffer.size()), buffer.data(), nullptr);
    if (!copied) { error = GetLastError(); return {}; }
    if (copied >= buffer.size()) { error = ERROR_INSUFFICIENT_BUFFER; return {}; }
    return std::wstring(buffer.data(), copied);
}

inline bool SamePath(const std::wstring& a, const std::wstring& b) noexcept {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

// Read-only confirmation. An unavailable/incomplete module list is UNKNOWN,
// not proof that the DLL was absent. A full-path match is positive evidence
// even if a different, unrelated module's path could not be queried.
inline ModuleInspection InspectModule(HANDLE process, const std::wstring& expectedPath) {
    ModuleInspection result;
    const auto expected = PathKey(expectedPath, result.error);
    if (expected.empty()) return result;
    std::vector<HMODULE> modules(128);
    DWORD needed = 0;
    bool complete = false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const BOOL queried = EnumProcessModulesEx(process, modules.data(),
            DWORD(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL);
        const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
        if (!queried) { result.error = queryError; return result; }
        if (needed <= modules.size() * sizeof(HMODULE)) { complete = true; break; }
        if (needed > 1024 * 1024) { result.error = ERROR_INSUFFICIENT_BUFFER; return result; }
        modules.resize(size_t(needed) / sizeof(HMODULE) + 32);
    }
    if (!complete || needed % sizeof(HMODULE)) {
        result.error = ERROR_INSUFFICIENT_BUFFER;
        return result;
    }
    result.count = needed / sizeof(HMODULE);
    std::vector<wchar_t> path(32768);
    for (DWORD i = 0; i < result.count; ++i) {
        const DWORD length = GetModuleFileNameExW(process, modules[i], path.data(), DWORD(path.size()));
        const DWORD pathError = length ? ERROR_SUCCESS : GetLastError();
        if (!length || length >= path.size()) {
            ++result.unreadablePaths;
            if (!result.error) result.error = length ? ERROR_INSUFFICIENT_BUFFER : pathError;
            continue;
        }
        DWORD keyError = 0;
        const std::wstring actual(path.data(), length);
        const auto key = PathKey(actual, keyError);
        if (key.empty()) {
            ++result.unreadablePaths;
            if (!result.error) result.error = keyError;
        } else if (SamePath(expected, key)) {
            result.state = ModuleState::Confirmed;
            result.base = reinterpret_cast<uintptr_t>(modules[i]);
            result.path = actual;
            return result;
        }
    }
    result.state = result.unreadablePaths ? ModuleState::Unknown : ModuleState::Absent;
    return result;
}

inline bool LoadConfirmed(DWORD wait, bool exitCodeRead, const ModuleInspection& module) noexcept {
    // A DWORD thread exit code cannot carry a complete x64 HMODULE and can also
    // represent an abnormal thread termination. It is diagnostic data, not the
    // loading-success predicate. Even a zero low DWORD needs module inspection.
    return wait == WAIT_OBJECT_0 && exitCodeRead && module.state == ModuleState::Confirmed;
}

// The remote thread can keep reading its LoadLibrary argument after a timeout.
// Keep the small allocation until target exit when completion is unknown. This
// helper owns no thread, never terminates one, and never schedules another load.
class RemoteArgument {
public:
    using FreeFn = BOOL(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD);
    RemoteArgument(HANDLE process, void* memory, FreeFn freeFn = &VirtualFreeEx) noexcept
        : process_(process), memory_(memory), free_(freeFn) {}
    ~RemoteArgument() { if (CanRelease()) free_(process_, memory_, 0, MEM_RELEASE); }
    RemoteArgument(const RemoteArgument&) = delete;
    RemoteArgument& operator=(const RemoteArgument&) = delete;
    void ThreadStarted() noexcept { pending_ = true; }
    void ThreadStopped() noexcept { pending_ = false; }
    bool CanRelease() const noexcept { return memory_ && !pending_; }
    bool Retained() const noexcept { return memory_ && pending_; }
    BOOL Release(DWORD& error) noexcept {
        error = ERROR_SUCCESS;
        if (!CanRelease()) return FALSE;
        const BOOL freed = free_(process_, memory_, 0, MEM_RELEASE);
        error = freed ? ERROR_SUCCESS : GetLastError();
        // The destructor must not issue a second free with a closed process
        // handle if the first call failed; leave target-exit cleanup in charge.
        memory_ = nullptr;
        return freed;
    }
private:
    HANDLE process_ = nullptr;
    void* memory_ = nullptr;
    FreeFn free_ = nullptr;
    bool pending_ = false;
};

} // namespace DXL::InjectionDiagnostics
