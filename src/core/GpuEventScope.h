#pragma once
#include <d3d12.h>
#define USE_PIX_RETAIL
#include "../../third_party/pix/Include/WinPixEventRuntime/pix3.h"

namespace DXL {
// Use Microsoft's PIX encoder and GPU transport only, with no ETW runtime DLL.
class GpuEventScope {
public:
    GpuEventScope(ID3D12GraphicsCommandList* list, const char* label) : _list(list) {
        if (!_list) return;
        UINT64 data[PIXEventsGraphicsRecordSpaceQwords]{};
        auto* end = PixEventsLegacy::EncodeBeginEventForContext(data, UINT64(0xff38a7ff), label);
        PIXBeginGPUEventOnContext(_list, data, UINT(reinterpret_cast<BYTE*>(end)-reinterpret_cast<BYTE*>(data)));
    }
    ~GpuEventScope() { if (_list) _list->EndEvent(); }
private:
    ID3D12GraphicsCommandList* _list;
};
}
