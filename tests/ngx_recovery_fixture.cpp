// Synthetic exports only: no game, Vulkan driver, or NVIDIA model is loaded.
#include <windows.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
struct ID3D12GraphicsCommandList;
static volatile LONG calls[3]{};
static const void* lastHandle = nullptr;
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
    InterlockedIncrement(&calls[0]); lastHandle = handle;
    return NVSDK_NGX_Result_Success;
}
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**) {
    InterlockedIncrement(&calls[1]); return NVSDK_NGX_Result_Success;
}
// The named Vulkan export intentionally has a distinct test ABI. If recovery
// incorrectly redirects it through the D3D12 wrapper, its sentinel/count fail.
extern "C" __declspec(dllexport) unsigned NVSDK_NGX_VULKAN_EvaluateFeature(unsigned value) {
    InterlockedIncrement(&calls[2]); return value ^ 0x7351a9b2u;
}
extern "C" __declspec(dllexport) LONG FixtureCalls(unsigned kind) { return kind < 3 ? calls[kind] : -1; }
extern "C" __declspec(dllexport) const void* FixtureLastHandle() { return lastHandle; }
