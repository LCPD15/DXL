#include "../src/core/DirectQueueCandidate.h"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
ComPtr<ID3D12CommandQueue> Queue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) {
    D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = type;
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "CreateCommandQueue");
    return queue;
}
ComPtr<ID3D12Device> Device(IDXGIAdapter* adapter) {
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "D3D12CreateDevice");
    return device;
}
bool SameObject(IUnknown* left, IUnknown* right) {
    ComPtr<IUnknown> a, b;
    return left && right && SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}
void CheckCandidate(DXL::DirectQueueCandidate& candidate, ID3D12CommandQueue* expected) {
    ComPtr<ID3D12CommandQueue> acquired;
    acquired.Attach(candidate.Acquire());
    Check(acquired.Get() == expected, "candidate identity");
    if (acquired) Check(acquired->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT, "candidate type");
}
void AssertNoDeviceErrors(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> messages;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&messages)))) return;
    const UINT64 count = messages->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T bytes = 0;
        messages->GetMessage(i, nullptr, &bytes);
        auto storage = std::make_unique<unsigned char[]>(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
        Check(SUCCEEDED(messages->GetMessage(i, message, &bytes)), "GetMessage");
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            fprintf(stderr, "D3D12 validation: %s\n", message->pDescription);
            Check(false, "D3D12 validation error");
        }
    }
}
}

int main() {
    try {
        ComPtr<ID3D12Debug> debug;
        const bool validation = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (validation) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter> warp;
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "EnumWarpAdapter");
        const auto selected = Device(warp.Get());
        // D3D12 returns one device per adapter in a process. Use a real hardware
        // adapter plus WARP to exercise rejection of a different COM device.
        ComPtr<ID3D12Device> other;
        for (UINT i = 0; !other; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other));
        }
        Check(other && !SameObject(selected.Get(), other.Get()), "distinct hardware and WARP devices required");
        const auto directA = Queue(selected.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        const auto directB = Queue(selected.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        const auto copy = Queue(selected.Get(), D3D12_COMMAND_LIST_TYPE_COPY);
        const auto compute = Queue(selected.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
        const auto foreign = Queue(other.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        DXL::DirectQueueCandidate candidate;
        Check(!candidate.Observe(directA.Get()), "queue cannot claim unselected device");
        Check(!candidate.SetDevice(nullptr), "null device rejected");
        Check(candidate.SetDevice(selected.Get()) && candidate.SetDevice(selected.Get()), "same device selection is idempotent");
        Check(!candidate.SetDevice(other.Get()), "device replacement rejected");
        Check(!candidate.Observe(copy.Get()) && !candidate.Observe(compute.Get()), "COPY and COMPUTE rejected");
        Check(!candidate.Observe(foreign.Get()), "foreign DIRECT queue rejected");
        CheckCandidate(candidate, nullptr);
        Check(candidate.Observe(directA.Get()), "selected DIRECT accepted");
        for (unsigned i = 0; i < 1000; ++i) Check(candidate.Observe(directA.Get()), "cached queue observation");
        Check(!candidate.Observe(foreign.Get()) && !candidate.Observe(copy.Get()), "rejected observation preserves candidate");
        CheckCandidate(candidate, directA.Get());
        Check(candidate.Observe(directB.Get()), "new selected-device DIRECT accepted");
        CheckCandidate(candidate, directB.Get());
        DXL::DirectQueueCandidate hardware;
        Check(hardware.SetDevice(other.Get()) && hardware.Observe(foreign.Get()), "hardware queue canonical device identity");
        Check(!hardware.Observe(directA.Get()), "hardware selection rejects WARP queue");
        CheckCandidate(hardware, foreign.Get());
        hardware.Clear();
        puts("PASS direct queue device/type rejection, stable observation and exchange");

        // The observer's retained reference is now the queue's only external
        // reference. Acquire must keep it usable across the observer's Clear.
        DXL::DirectQueueCandidate lifetime;
        Check(lifetime.SetDevice(selected.Get()), "lifetime selected device");
        auto transient = Queue(selected.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        Check(lifetime.Observe(transient.Get()), "transient observation");
        auto* identity = transient.Get();
        transient.Reset();
        ComPtr<ID3D12CommandQueue> retained;
        retained.Attach(lifetime.Acquire());
        Check(retained.Get() == identity, "retained after caller Release");
        lifetime.Clear();
        Check(!lifetime.Acquire() && !lifetime.SetDevice(selected.Get()) && !lifetime.Observe(directA.Get()), "Clear permanently closes observer");
        ComPtr<ID3D12Fence> fence;
        Check(SUCCEEDED(selected->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence");
        Check(SUCCEEDED(retained->Signal(fence.Get(), 1)), "retained queue real GPU Signal after Clear");
        HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(completed != nullptr, "fence event");
        const HRESULT signal = fence->SetEventOnCompletion(1, completed);
        const DWORD wait = SUCCEEDED(signal) ? WaitForSingleObject(completed, 5000) : WAIT_FAILED;
        CloseHandle(completed);
        Check(wait == WAIT_OBJECT_0 && fence->GetCompletedValue() == 1, "retained queue fence completed");
        puts("PASS direct queue retained lifetime across caller Release and observer Clear");

        DXL::DirectQueueCandidate concurrent;
        Check(concurrent.SetDevice(selected.Get()) && concurrent.Observe(directA.Get()), "concurrent setup");
        std::atomic<unsigned> observed{0}, acquired{0}, faults{0};
        std::atomic<bool> cleared{false};
        std::thread producer([&] {
            for (unsigned i = 0; i < 8000; ++i) {
                concurrent.Observe(i & 1 ? directA.Get() : directB.Get());
                ++observed;
                if ((i & 15) == 0) std::this_thread::yield();
            }
        });
        std::thread consumer([&] {
            for (unsigned i = 0; i < 10000; ++i) {
                ComPtr<ID3D12CommandQueue> queue;
                queue.Attach(concurrent.Acquire());
                if (queue) {
                    if ((queue.Get() != directA.Get() && queue.Get() != directB.Get()) ||
                        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) ++faults;
                    ++acquired;
                }
                if ((i & 15) == 0) std::this_thread::yield();
            }
        });
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while ((observed < 100 || acquired < 100) && GetTickCount64() < deadline) std::this_thread::yield();
        concurrent.Clear();
        cleared = true;
        producer.join(); consumer.join();
        Check(observed >= 100 && acquired >= 100 && faults == 0 && cleared, "concurrent observation/acquire validation");
        Check(!concurrent.Acquire() && !concurrent.Observe(directA.Get()) && !concurrent.SetDevice(selected.Get()), "concurrent teardown cannot resurrect");
        candidate.Clear();
        AssertNoDeviceErrors(selected.Get()); AssertNoDeviceErrors(other.Get());
        printf("PASS direct queue concurrency observed=%u acquired=%u validation=%u\n", observed.load(), acquired.load(), unsigned(validation));
        puts("PASS DirectQueueCandidate real D3D12 tests");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL DirectQueueCandidate: %s\n", error.what());
        return 1;
    }
}
