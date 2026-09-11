// Synthetic NGX ABI fixture. No NVIDIA code/model is loaded by identity tests.
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
struct ID3D12GraphicsCommandList;
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
    return NVSDK_NGX_Result_Success;
}
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**) {
    return NVSDK_NGX_Result_Success;
}
