#pragma once
#include <windows.h>
#include <filesystem>
#include "../common/SemanticExtension.h"

namespace DXL {
struct SegmentationRuntime {
    bool lean = false;
    std::filesystem::path library;
    const wchar_t* Name() const noexcept { return lean ? L"Lean" : L"Full"; }
};

// A package selects its backend before loading anything. An explicitly supplied
// lean runtime is never replaced with Full on a load/deserialization failure.
// This keeps A/B packages deterministic and makes incomplete Lean installs clear.
inline SegmentationRuntime SelectSegmentationRuntime(const std::filesystem::path& beside) {
    const auto folder = SemanticRuntimeFolder(beside);
    const auto lean = folder / L"nvinfer_lean_11.dll";
    const DWORD attributes = GetFileAttributesW(lean.c_str());
    const bool supplied = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    return {supplied, supplied ? lean : folder / L"nvinfer_11.dll"};
}
} // namespace DXL
