#pragma once

#include "../common/ColorGradingSettings.h"
#include "../common/PostFxCatalog.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace DXL {

// Owned by one caller command slot. Reuse/release only after that slot's GPU
// fence retires; descriptors and LUT uploads remain immutable while in flight.
struct ColorGradingFrame {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> lut;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    uint64_t lutRevision = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> fxHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> fxScratch[2];
    std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> fxPrograms;
    static constexpr uint32_t BloomLevels = 5;
    Microsoft::WRL::ComPtr<ID3D12Resource> bloomPyramid[BloomLevels];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> bloomHeap;
};

class ColorGrading {
public:
    bool Initialize(ID3D12Device* device) noexcept;
    void SetModule(HMODULE module) noexcept;
    bool IsReady() const noexcept { return _pso != nullptr; }
    const char* LastError() const noexcept { return _lastError.c_str(); }
    static bool ValidateLutFilename(const std::string& filename) noexcept;
    static bool InferLutLayout(uint32_t width, uint32_t height,
        uint32_t& cubeSize, uint32_t& tilesX) noexcept;

    // Returns true only if a dispatch was recorded. All disabled/neutral is
    // free: no descriptors, uploads, transitions or dispatch are recorded.
    // The source is an SRV in NON_PIXEL_SHADER_RESOURCE; the distinct target
    // is a UAV in UNORDERED_ACCESS. Caller owns transitions and subsequent
    // UAV visibility. Only the top-left width x height region is touched.
    // This replaces list PSO/root signature/heaps. inputLinear selects linear
    // HDR/scRGB input; display-encoded SDR uses false. Alpha is preserved.
    bool Record(ID3D12GraphicsCommandList* list,
        ID3D12Resource* source, ID3D12Resource* target,
        uint32_t width, uint32_t height, const ColorGradingSettings& settings,
        bool inputLinear, uint32_t frameIndex, ColorGradingFrame& retiredSlot) noexcept;

private:
    bool RecordBuiltins(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*,
        uint32_t, uint32_t, const ColorGradingSettings&, bool, uint32_t, ColorGradingFrame&);
    struct PostFxProgram {
        PostFxDefinition definition;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        std::string error;
    };
    PostFxProgram& PrepareFx(const std::string& filename);
    bool PrepareFxRoot();
    bool LoadLut(const std::string& filename);
    bool UploadLut(ID3D12GraphicsCommandList* list, ColorGradingFrame& slot);
    bool PrepareBloom(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
        uint32_t width, uint32_t height, const ColorGradingSettings& settings,
        bool inputLinear, ColorGradingFrame& slot);
    Microsoft::WRL::ComPtr<ID3D12Device> _device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _pso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _bloomRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _bloomPso;
    std::wstring _lutRoot;
    std::string _loadedFilename;
    std::string _lastError;
    std::string _lutLoadError;
    std::vector<uint8_t> _lutPixels;
    uint32_t _cubeSize = 0;
    uint64_t _lutRevision = 1;
    uint32_t _requestedRevision = 0;
    bool _lutAttempted = false;
    std::filesystem::path _fxRoot;
    uint32_t _fxRequestedRevision = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _fxSignature;
    std::unordered_map<std::string, PostFxProgram> _fxPrograms;
};

} // namespace DXL
