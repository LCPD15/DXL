#include "../src/common/SemanticExtension.h"
#include "../src/core/SegmentationRuntime.h"
#include <fstream>
#include <cstdio>
#include <stdexcept>
using namespace DXL;
void check(bool ok, const char* what) { if(!ok) throw std::runtime_error(what); }
int main() {
    const auto root=std::filesystem::temp_directory_path()/(L"DXL-extension-test-"+std::to_wstring(GetCurrentProcessId()));
    check(!std::filesystem::exists(root),"test path already exists");
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup(){std::error_code ec;std::filesystem::remove_all(root,ec);} } cleanup{root};
    auto touch=[](const auto& p, bool content=true){std::filesystem::create_directories(p.parent_path());std::ofstream f(p);if(content)f<<'x';};
    check(!SemanticExtensionInstalled(root),"empty base must be disabled");
    touch(root/L"models/yolo11n-seg.plan");
    check(!SemanticExtensionInstalled(root),"model without DLL must be disabled");
    touch(root/L"nvinfer_lean_11.dll");
    check(SemanticExtensionInstalled(root),"legacy Lean install");
    const auto addon=SemanticExtensionFolder(root);
    touch(addon/L"nvinfer_lean_11.dll",false);
    check(!SemanticExtensionInstalled(root),"broken addon must not use legacy files");
    touch(addon/L"nvinfer_lean_11.dll");
    check(!SemanticExtensionInstalled(root),"DLL without matching model must be disabled");
    touch(addon/L"models/yolo11n-seg.plan");
    check(SemanticExtensionInstalled(root),"single YOLO addon is sufficient without ADE");
    check(SelectSegmentationRuntime(root).library==addon/L"nvinfer_lean_11.dll","use addon DLL");
    check(!GetModuleHandleW(L"nvinfer_lean_11.dll"),"availability check must never load runtime");
    puts("PASS optional extension: missing/partial/empty files, legacy compatibility, addon precedence, no ADE dependency, no DLL loaded");
}
