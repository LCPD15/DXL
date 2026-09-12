// Exercise the production NR slot gate without loading NGX or injecting a game.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include "DlssNrFilter.h"

using Microsoft::WRL::ComPtr;
static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void HR(HRESULT value) { Check(SUCCEEDED(value), "D3D12 operation failed"); }

namespace DXL {
struct NrLayerTestAccess {
    static void Run(ID3D12Device* device, ID3D12CommandQueue* queue) {
        DlssNrFilter nr;
        nr._device = device;
        nr._queue = queue;
        queue->AddRef();
        Check(nr.CreateCommandObjects(), "NR command object creation failed");
        auto& slot = nr._slots[0];
        Check(nr.TryClaimSlot(slot), "unused slot must be available");

        slot.fenceValue = 1;
        Check(!nr.TryClaimSlot(slot), "incomplete slot must time out");
        HR(nr._fence->Signal(1));
        Check(WaitForSingleObject(nr._fenceEvent, 1000) == WAIT_OBJECT_0,
              "timed-out registration did not signal when the older work completed");
        // Restore the exact signaled state that exists when the render thread
        // has not yet consumed the old completion notification.
        Check(SetEvent(nr._fenceEvent) != FALSE, "could not restore stale event");
        slot.fenceValue = 2;
        const bool premature = nr.TryClaimSlot(slot);
        const auto incomplete = nr._fence->GetCompletedValue();
        // Drain every registration before destruction, even on the negative test.
        HR(nr._fence->Signal(2));
        Check(!premature && incomplete == 1,
              "stale completion event released an unfinished NR command slot");
        Check(nr.TryClaimSlot(slot), "completed slot must become available");
        Check(!nr._disabled, "ordinary slot timeout must not disable NR");
        printf("PASS NR slot gate: unused, incomplete, stale event, completed; skipped=%llu\n",
               static_cast<unsigned long long>(nr._skippedFrames));
    }
};
}

int main() try {
    ComPtr<ID3D12Debug> debug;
    HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    HR(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
    DXL::NrLayerTestAccess::Run(device.Get(), queue.Get());
    return 0;
} catch (const std::exception& error) {
    printf("FAIL: %s\n", error.what());
    return 1;
}
