#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace DXL {
enum class GamePathState { Exists, Missing, Unknown };

inline bool IsMissingPathError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Failure to reach a disk/network share is not evidence that a game was removed.
// This function only classifies paths; it never deletes a profile or game file.
inline GamePathState InspectGameExecutable(const std::wstring& pathText) {
    try {
        const std::filesystem::path path(pathText);
        if (path.empty() || !path.is_absolute() ||
            _wcsicmp(path.extension().c_str(), L".exe") != 0)
            return GamePathState::Unknown;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? GamePathState::Unknown : GamePathState::Exists;
        const DWORD pathError = GetLastError();
        if (!IsMissingPathError(pathError)) return GamePathState::Unknown;
        const std::wstring root = path.root_path().wstring();
        const UINT drive = GetDriveTypeW(root.c_str());
        if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK)
            return GamePathState::Unknown;
        DWORD serial = 0, maxComponent = 0, flags = 0;
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, &maxComponent,
            &flags, nullptr, 0)) return GamePathState::Unknown;
        return GamePathState::Missing;
    } catch (...) {
        return GamePathState::Unknown;
    }
}
}
