#pragma once
#include <filesystem>
#include <windows.h>

namespace DXL {
inline std::filesystem::path SemanticModuleFolder(HMODULE module) {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    return length && length < std::size(path) ? std::filesystem::path(path).parent_path() : std::filesystem::path{};
}
// Checking availability never loads TensorRT or starts a worker.
inline bool SemanticFilePresent(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    const auto bytes = std::filesystem::file_size(path, ec);
    return !ec && bytes > 0;
}
inline std::filesystem::path SemanticExtensionFolder(const std::filesystem::path& toolRoot) {
    return toolRoot / L"extensions" / L"semantic";
}
inline std::filesystem::path SemanticRuntimeFolder(const std::filesystem::path& toolRoot) {
    const auto addon = SemanticExtensionFolder(toolRoot);
    std::error_code ec;
    // An incomplete addon is not silently replaced by an older root install.
    if (std::filesystem::exists(addon / L"nvinfer_lean_11.dll", ec) ||
        std::filesystem::exists(addon / L"nvinfer_11.dll", ec)) return addon;
    return toolRoot; // Compatibility with existing portable Full/Lean packages.
}
inline bool SemanticExtensionInstalled(const std::filesystem::path& toolRoot) {
    const auto folder = SemanticRuntimeFolder(toolRoot);
    std::error_code ec;
    const auto lean = folder / L"nvinfer_lean_11.dll";
    const auto library = std::filesystem::exists(lean, ec) ? lean : folder / L"nvinfer_11.dll";
    return SemanticFilePresent(library) && SemanticFilePresent(folder / L"models" / L"yolo11n-seg.plan");
}
} // namespace DXL
