#include "../src/core/SegmentationRuntime.h"
#include <fstream>
#include <cstdio>
#include <stdexcept>
namespace fs = std::filesystem;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() try {
    const auto temp = fs::temp_directory_path();
    const auto root = (temp / (L"DXL.RuntimeSelection." + std::to_wstring(GetCurrentProcessId()))).lexically_normal();
    Check(root.is_absolute() && fs::equivalent(root.parent_path(), temp), "fixture must stay in temporary directory");
    fs::create_directories(root);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    auto choice = DXL::SelectSegmentationRuntime(root);
    Check(!choice.lean && choice.library == root / L"nvinfer_11.dll", "legacy Full package path");
    std::ofstream(root / L"nvinfer_11.dll") << "test fixture";
    Check(!DXL::SelectSegmentationRuntime(root).lean, "Full-only selected Lean");
    std::ofstream(root / L"nvinfer_lean_11.dll") << "invalid DLL intentionally still selects Lean";
    choice = DXL::SelectSegmentationRuntime(root);
    Check(choice.lean && choice.library == root / L"nvinfer_lean_11.dll", "Lean is explicit even when invalid and Full is present");
    fs::remove(root / L"nvinfer_11.dll");
    Check(DXL::SelectSegmentationRuntime(root).lean, "Lean package requires Full DLL");
    puts("PASS deterministic package runtime selection: legacy Full, Lean-only, both present and invalid Lean without fallback");
    return 0;
} catch (const std::exception& e) { printf("FAIL %s\n", e.what()); return 1; }
