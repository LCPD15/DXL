#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include "ComputePasses.h"
namespace DXL {
struct OpticalFlowStatus {
    bool active=false, ready=false, reset=true;
    uint32_t width=0,height=0;
    uint64_t dispatches=0;
    double gpuMs=0;
    const char* error=nullptr;
};
// One provider per NR feature. All calls except Status require the owner's NR
// lock. Destroy requires completion of all command lists referencing its data.
class OpticalFlow {
public:
    OpticalFlow();
    ~OpticalFlow();
    OpticalFlow(const OpticalFlow&)=delete;
    OpticalFlow& operator=(const OpticalFlow&)=delete;
    void Destroy() noexcept;
    void Suspend() noexcept; // CPU only; no allocation/dispatch while native MV is used.
    // Input is NR's UNMODIFIED display/encoded input, in NON_PIXEL_SHADER_RESOURCE.
    // Caller restores the game's bindings. No command list submission or CPU wait.
    ID3D12Resource* Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
        ComputePasses& passes, ID3D12Resource* input, int quality, bool reset,
        bool previousGpuComplete, uint64_t timestampFrequency) noexcept;
    OpticalFlowStatus Status() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
