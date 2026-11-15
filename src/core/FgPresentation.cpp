#include "FgPresentation.h"
#include "PresentGeometry.h"
#include "DlssFgBackend.h"
#include "../common/Log.h"
#include <dcomp.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <new>

#pragma comment(lib, "dcomp.lib")

namespace DXL {
using Microsoft::WRL::ComPtr;
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
thread_local bool privateThread = false;
using Alloc = ComPtr<ID3D12CommandAllocator>;
using List = ComPtr<ID3D12GraphicsCommandList>;

void LogGeometry(IDXGISwapChain3* source, HWND hwnd, UINT bw, UINT bh, const PresentGeometry& g) {
    static thread_local PresentGeometry last{};
    static thread_local HWND lastWindow = nullptr;
    if (hwnd == lastWindow && g == last) return;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    source->GetDesc1(&desc);
    UINT sw = bw, sh = bh;
    source->GetSourceSize(&sw,&sh);
    RECT client{};
    const auto old = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
    GetClientRect(hwnd,&client);
    if (old) SetThreadDpiAwarenessContext(old);
    D5_LOG_INFO(L"DLSS FG geometry: buffer=%ux%u source=%ux%u client=%ux%u crop=(%u,%u,%u,%u) scaling=%u effect=%u dpi=%u scale=(%.4f,%.4f) offset=(%.1f,%.1f)",
        bw,bh,sw,sh,UINT(client.right),UINT(client.bottom),g.x,g.y,g.width,g.height,unsigned(desc.Scaling),unsigned(desc.SwapEffect),
        GetDpiForWindow(hwnd),g.sx,g.sy,g.offsetX,g.offsetY);
    last = g; lastWindow = hwnd;
}
bool NewList(ID3D12Device* d, Alloc& alloc, List& list) {
    return SUCCEEDED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) &&
        SUCCEEDED(list->Close());
}
bool Copy(ID3D12CommandQueue* queue, Alloc& alloc, List& list, ID3D12Resource* dst, ID3D12Resource* src, const PresentGeometry* crop = nullptr) {
    if (FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.Get(), nullptr))) return false;
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Transition = {src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, barriers);
    if (crop) {
        D3D12_TEXTURE_COPY_LOCATION from{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION to{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_BOX box{crop->x,crop->y,0,crop->x+crop->width,crop->y+crop->height,1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    } else list->CopyResource(dst, src);
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(2, barriers);
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    return true;
}
bool WaitForGpu(ID3D12Fence* fence, UINT64 value, HANDLE event) {
    if (!value) return true;
    const auto done = fence->GetCompletedValue();
    if (done == UINT64_MAX) return false;
    if (done >= value) return true;
    if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
    // Never release submitted resources on a timer. Device removal signals
    // outstanding fence events as well.
    return WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX;
}
}

void FgPresentation::RateWindow::Clear() noexcept {
    start_ = last_ = {}; intervals_ = latencySamples_ = 0;
    latencySum_ = 0; fps_ = 0; latencyMs_ = -1;
}
void FgPresentation::RateWindow::Reset() noexcept {
    std::lock_guard guard(mutex_);
    Clear();
}
void FgPresentation::RateWindow::Record(Time now, double latencyMs) noexcept {
    std::lock_guard guard(mutex_);
    if (last_ != Time{} && now - last_ > 1s) Clear();
    if (last_ == Time{}) start_ = now;
    else ++intervals_;
    last_ = std::max(now, last_); // two native Present callbacks can arrive out of order
    if (latencyMs >= 0) { latencySum_ += latencyMs; ++latencySamples_; }
    const auto elapsed = last_ - start_;
    if (elapsed < 750ms) return;
    fps_ = float(intervals_ / std::chrono::duration<double>(elapsed).count());
    latencyMs_ = latencySamples_ ? float(latencySum_ / latencySamples_) : -1.f;
    start_ = last_; intervals_ = latencySamples_ = 0; latencySum_ = 0;
}
float FgPresentation::RateWindow::Read(bool latency) const noexcept {
    std::lock_guard guard(mutex_);
    if (last_ == Time{} || Clock::now() - last_ > 1s) return latency ? -1.f : 0.f;
    return latency ? latencyMs_ : fps_;
}
void FgPresentation::ObserveInput(IUnknown* identity, Time received) noexcept {
    if (inputIdentity_ != identity) {
        inputIdentity_ = identity;
        input_.Reset();
    }
    input_.Record(received);
}
float FgPresentation::InputFps() const noexcept { return input_.Read(false); }
float FgPresentation::OutputFps() const noexcept { return output_.Read(false); }
float FgPresentation::OutputLatencyMs() const noexcept { return output_.Read(true); }

struct FgPresentation::Session {
    HWND hwnd;
    UINT width, height;
    DXGI_FORMAT format;
    DXGI_COLOR_SPACE_TYPE space;
    const void* owner;
    HMODULE self;
    RateWindow& outputMetrics;
    std::atomic<uint32_t>& maxMultiplier;
    std::atomic<uint32_t>& status;
    std::atomic<uint64_t>& presented;
    Session(HWND w, UINT x, UINT y, DXGI_FORMAT f, DXGI_COLOR_SPACE_TYPE cs, const void* id, HMODULE dll,
        RateWindow& metrics, std::atomic<uint32_t>& maxM, std::atomic<uint32_t>& s, std::atomic<uint64_t>& count)
        : hwnd(w), width(x), height(y), format(f), space(cs), owner(id), self(dll), outputMetrics(metrics), maxMultiplier(maxM), status(s), presented(count) {}
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> inputFence11;
    ComPtr<ID3D12Fence> inputFence, displayFence;
    HANDLE inputEvent = nullptr, displayEvent = nullptr;
    UINT64 captureSerial = 0, displaySerial = 0;
    struct Slot {
        ComPtr<ID3D12Resource> frame;
        ComPtr<ID3D11Texture2D> shared;
        Alloc alloc;
        List list;
        PresentGeometry geom{};
        UINT multiplier = 2;
        UINT64 serial = 0;
        Clock::time_point time{};
        enum { Free, Pending, Reading } state = Free;
    } slots[2];
    std::mutex lock;
    std::condition_variable wake;
    std::atomic<bool> quit{false}, failed{false};
    std::thread worker;
    DlssFgBackend backend;
    Alloc displayAlloc;
    List displayList;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<IDCompositionDevice> comp;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionTarget> target;
    bool visible = false;
    HRESULT lastHr = S_OK; // Worker only.
    PresentGeometry lastGeometry{};
    std::atomic<UINT> requestedMultiplier{2};

    bool Init(ID3D12CommandQueue* writer, ID3D11Device* d11) {
        ComPtr<ID3D11Device5> device5;
        if (writer) {
            if (FAILED(writer->GetDevice(IID_PPV_ARGS(&dev)))) return false;
        } else {
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<ID3D11DeviceContext> context;
            d11->GetImmediateContext(&context);
            if (!context || FAILED(context.As(&context11)) || FAILED(d11->QueryInterface(IID_PPV_ARGS(&device5))) ||
                FAILED(d11->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
                FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        if (FAILED(dev->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) ||
            FAILED(dev->CreateFence(0, d11 ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&inputFence))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&displayFence))) ||
            !NewList(dev.Get(), displayAlloc, displayList)) return false;
        inputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        displayEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!inputEvent || !displayEvent) return false;
        if (d11) {
            HANDLE handle{};
            if (FAILED(dev->CreateSharedHandle(inputFence.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) return false;
            const HRESULT hr = device5->OpenSharedFence(handle, IID_PPV_ARGS(&inputFence11));
            CloseHandle(handle);
            if (FAILED(hr)) return false;
        }
        for (auto& slot : slots) {
            if (d11) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
                d.Format = format;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
                ComPtr<IDXGIResource1> res;
                HANDLE handle{};
                if (FAILED(d11->CreateTexture2D(&d, nullptr, &slot.shared)) || FAILED(slot.shared.As(&res)) ||
                    FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) return false;
                const HRESULT hr = dev->OpenSharedHandle(handle, IID_PPV_ARGS(&slot.frame));
                CloseHandle(handle);
                if (FAILED(hr)) return false;
            } else {
                D3D12_RESOURCE_DESC d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = width; d.Height = height;
                d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1; d.Format = format;
                D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                heap.CreationNodeMask = heap.VisibleNodeMask = 1;
                if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                        nullptr, IID_PPV_ARGS(&slot.frame))) || !NewList(dev.Get(), slot.alloc, slot.list)) return false;
            }
        }
        worker = std::thread([this] {
            try { Run(); }
            catch (...) {
                // An optional provider or an allocation failure must not terminate the game.
                Fail("presentation worker exception", E_FAIL);
                Hide();
                if (displayFence) WaitForGpu(displayFence.Get(), displaySerial, displayEvent);
                backend.Reset();
            }
        });
        return true;
    }

    void Submit(Time received, ID3D12CommandQueue* writer, bool creationQueue, ID3D12Resource* src, ID3D11Texture2D* src11, const PresentGeometry& geom, UINT mult) {
        std::unique_lock guard(lock);
        if (failed || quit) return;
        Slot* available = nullptr;
        const auto retired = inputFence->GetCompletedValue();
        for (auto& candidate : slots) {
            if (candidate.state == Slot::Free && retired >= candidate.serial) { available = &candidate; break; }
            // Overwrite a queued frame once its copy/upload allocator is idle.
            // This keeps the first pair after a lengthy cold start current.
            if (candidate.state == Slot::Pending && retired >= candidate.serial &&
                (!available || candidate.serial < available->serial)) available = &candidate;
        }
        if (available) {
            auto& slot = *available;
            const UINT64 seq = captureSerial + 1;
            HRESULT captureHr;
            if (src11) {
                const D3D11_BOX box{geom.x,geom.y,0,geom.x+geom.width,geom.y+geom.height,1};
                context11->CopySubresourceRegion(slot.shared.Get(), 0, 0, 0, 0, src11, 0, &box);
                captureHr = context11->Signal(inputFence11.Get(), seq);
                context11->Flush();
            } else {
                if (!Copy(writer, slot.alloc, slot.list, slot.frame.Get(), src, &geom)) { Fail("capture copy", E_FAIL); return; }
                captureHr = writer->Signal(inputFence.Get(), seq);
            }
            if (FAILED(captureHr)) { Fail("capture signal", captureHr); return; }
            captureSerial = seq;
            // A verified last writer may differ from DXGI's presentation queue.
            if (writer && !creationQueue && !WaitForGpu(inputFence.Get(), seq, inputEvent)) {
                Fail("capture retire", E_FAIL); return;
            }
            slot.geom = geom; slot.multiplier = mult;
            slot.serial = seq; slot.time = received; slot.state = Slot::Pending;
            guard.unlock();
            wake.notify_one();
            return;
        }
    }

    void Fail(const char* stage, HRESULT hr) {
        if (!failed.exchange(true)) D5_LOG_WARN(L"DLSS FG: %hs failed (0x%08X)", stage, (unsigned)hr);
        status.store(4); wake.notify_all();
    }
    bool Output() {
        ComPtr<IDXGIFactory2> factory;
        ComPtr<IDXGISwapChain1> output;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = width; d.Height = height; d.Format = format; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 3;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(lastHr = CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(lastHr = factory->CreateSwapChainForComposition(queue.Get(), &d, nullptr, &output)) ||
            FAILED(lastHr = output.As(&chain)) || FAILED(lastHr = chain->SetColorSpace1(space)) ||
            FAILED(lastHr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&comp))) ||
            FAILED(lastHr = comp->CreateTargetForHwnd(hwnd, FALSE, &target)) ||
            FAILED(lastHr = comp->CreateVisual(&visual)) || FAILED(lastHr = visual->SetContent(chain.Get()))) return false;
        return true;
    }
    void Hide() {
        if (visible) {
            target->SetRoot(nullptr); comp->Commit(); visible = false;
            outputMetrics.Reset();
        }
    }
    bool Show(const PresentGeometry& g) {
        if (visible && g == lastGeometry) return true;
        const D2D_MATRIX_3X2_F transform{g.sx, 0, 0, g.sy, g.offsetX, g.offsetY};
        if (FAILED(lastHr = visual->SetTransform(transform)) || FAILED(lastHr = target->SetRoot(visual.Get())) ||
            FAILED(lastHr = comp->Commit())) return false;
        visible = true; lastGeometry = g;
        return true;
    }
    bool Display(ID3D12Resource* frame) {
        if (!WaitForGpu(displayFence.Get(), displaySerial, displayEvent)) { lastHr = DXGI_ERROR_DEVICE_REMOVED; return false; }
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(lastHr = chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))) ||
            !Copy(queue.Get(), displayAlloc, displayList, buffer.Get(), frame)) {
            if (SUCCEEDED(lastHr)) lastHr = E_FAIL;
            return false;
        }
        UINT64 next = displaySerial + 1;
        if (FAILED(lastHr = queue->Signal(displayFence.Get(), next))) return false;
        displaySerial = next;
        // Nonblocking Present keeps a stopped/occluded compositor from wedging the host on exit.
        lastHr = chain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        return lastHr == S_OK;
    }

    void Run() {
        privateThread = true;
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!Output()) Fail("composition output", lastHr);
        Clock::time_point prev{};
        UINT previousMultiplier = 0;
        while (!quit && !failed) {
            Slot* slot = nullptr;
            {
                std::unique_lock guard(lock);
                auto next = [&] {
                    for (auto& s : slots) if (s.state == Slot::Pending && (!slot || s.serial < slot->serial)) slot = &s;
                    return slot || quit || failed;
                };
                if (!wake.wait_for(guard, 200ms, next)) {
                    guard.unlock(); Hide(); prev = {}; status.store(1); continue;
                }
                if (quit || failed) break;
                if (slot->multiplier != requestedMultiplier.load()) {
                    // Stale queued configuration. The producer checks its capture
                    // fence before recycling the allocator or shared texture.
                    slot->state = Slot::Free;
                    continue;
                }
                slot->state = Slot::Reading;
            }
            auto interval = slot->time - prev;
            if (prev == Clock::time_point{} || interval > 200ms) {
                Hide();
                backend.ResetHistory();
                interval = std::chrono::duration_cast<Clock::duration>(16ms);
            }
            prev = slot->time;
            unsigned shown = 0;
            ID3D12Resource* generated[3]{};
            if (FAILED(queue->Wait(inputFence.Get(), slot->serial))) { Fail("input queue wait", E_FAIL); break; }
            const UINT mult = slot->multiplier;
            if (mult != previousMultiplier) {
                outputMetrics.Reset();
                previousMultiplier = mult;
            }
            const bool generatedBatch = backend.Generate(queue.Get(), slot->frame.Get(), self, generated,
                space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, mult);
            maxMultiplier.store(backend.MaxMultiplier());
            if (generatedBatch) {
                const auto begin = Clock::now();
                const auto step = std::clamp(interval / mult, Clock::duration(1ms), Clock::duration(40ms));
                for (UINT index = 0; index < mult-1 && !quit && requestedMultiplier.load() == mult; ++index) {
                    // Each texture is the provider's corresponding temporal phase of this source interval.
                    if (Display(generated[index])) {
                        const auto submitted = Clock::now();
                        if (!Show(slot->geom)) { Fail("attach visual", lastHr); break; }
                        outputMetrics.Record(submitted);
                        ++shown;
                        ++presented;
                    } else if (lastHr != DXGI_ERROR_WAS_STILL_DRAWING && lastHr != DXGI_STATUS_OCCLUDED) {
                        Fail("present generated", lastHr); break;
                    }
                    std::unique_lock guard(lock);
                    wake.wait_until(guard, begin + step * (index+1), [&] { return quit.load() || requestedMultiplier.load() != mult; });
                }
                if (failed) break;
            } else if (const char* error = backend.Error(); error && *error) {
                Fail(error, E_FAIL); break;
            }
            if (!quit && visible) {
                const bool real = Display(slot->frame.Get());
                if (real) {
                    const auto submitted = Clock::now();
                    outputMetrics.Record(submitted,
                        std::chrono::duration<double, std::milli>(submitted - slot->time).count());
                }
                if (!real && lastHr != DXGI_ERROR_WAS_STILL_DRAWING && lastHr != DXGI_STATUS_OCCLUDED) {
                    Fail("present real", lastHr); break;
                }
                if (!real) Hide();
                if (real && shown) {
                    status.store(2);
                    const auto n = presented.load();
                    if (n <= 6 || (n-shown)/300 != n/300) D5_LOG_INFO(L"DLSS FG: presented interpolated + real (%llu generated, %u/%u this interval, %ux%u format=%u)",
                        (unsigned long long)n, shown, mult-1, width, height, (unsigned)format);
                }
            }
            // Both producer texture and NGX output can only be recycled after copy-out.
            if (!WaitForGpu(displayFence.Get(), displaySerial, displayEvent)) { Fail("display retire", E_FAIL); break; }
            std::lock_guard guard(lock);
            slot->state = Slot::Free;
        }
        Hide();
        WaitForGpu(displayFence.Get(), displaySerial, displayEvent);
        backend.Reset();
        target.Reset(); visual.Reset(); comp.Reset(); chain.Reset();
        if (SUCCEEDED(co)) CoUninitialize();
        privateThread = false;
    }
    ~Session() {
        quit.store(true); wake.notify_all();
        if (worker.joinable()) worker.join();
        if (captureSerial) WaitForGpu(inputFence.Get(), captureSerial, inputEvent);
        if (inputEvent) CloseHandle(inputEvent);
        if (displayEvent) CloseHandle(displayEvent);
    }
};

bool FgPresentation::Internal() noexcept { return privateThread; }
void FgPresentation::Stop() noexcept {
    if (session_) {
        delete session_; session_ = nullptr;
        output_.Reset();
    }
    pendingWidth_ = pendingHeight_ = 0;
    stableSince_ = 0;
}

void FgPresentation::Capture(Time received, IDXGISwapChain3* source, HWND hwnd, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space, ID3D12Resource* frame,
    ID3D12CommandQueue* writer, bool creationQueue, ID3D11Texture2D* frame11, HMODULE self,
    UINT mult, std::atomic<uint32_t>& maxMultiplier, std::atomic<uint32_t>& status, std::atomic<uint64_t>& presented) noexcept {
    ComPtr<ID3D11Device> d11;
    UINT w = 0, h = 0, samples = 0;
    if (frame11) {
        D3D11_TEXTURE2D_DESC d{}; frame11->GetDesc(&d); w = d.Width; h = d.Height; fmt = d.Format; samples = d.SampleDesc.Count;
        frame11->GetDevice(&d11);
    } else if (writer && frame) {
        auto d = frame->GetDesc(); w = (UINT)d.Width; h = d.Height; fmt = d.Format; samples = d.SampleDesc.Count;
    }
    if (!w || !h || samples != 1 || (fmt != DXGI_FORMAT_R8G8B8A8_UNORM &&
        fmt != DXGI_FORMAT_B8G8R8A8_UNORM && fmt != DXGI_FORMAT_R10G10B10A2_UNORM && fmt != DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        Stop(); status.store(3); return;
    }
    PresentGeometry g;
    if (!ReadPresentGeometry(source, hwnd, w, h, g)) { Stop(); status.store(3); return; }
    LogGeometry(source, hwnd, w, h, g);
    w = g.width; h = g.height;
    mult = std::clamp(mult, 2u, 4u);
    const void* owner = writer ? (void*)writer : (void*)d11.Get();
    if (session_ && (session_->hwnd != hwnd || session_->width != w || session_->height != h ||
        session_->format != fmt || session_->space != space || session_->owner != owner ||
        (session_->failed && session_->requestedMultiplier.load() != mult))) Stop();
    if (!session_) {
        status.store(1);
        // Live window drags can change the valid source rectangle on every
        // callback. Wait for a stable size before creating costly NGX resources.
        const auto now = GetTickCount64();
        if (w != pendingWidth_ || h != pendingHeight_) {
            pendingWidth_ = w; pendingHeight_ = h; stableSince_ = now;
        }
        if (now - stableSince_ < 100) return;
        session_ = new(std::nothrow) Session(hwnd, w, h, fmt, space, owner, self, output_, maxMultiplier, status, presented);
        if (!session_) { status.store(4); return; }
        try {
            if (!session_->Init(writer, d11.Get())) { session_->Fail("initialize capture", E_FAIL); return; }
        } catch (...) { session_->Fail("start worker", E_FAIL); return; }
        D5_LOG_INFO(L"DLSS FG: capturing D3D%u native final frame %ux%u format=%u space=%u", frame11?11:12, w,h,fmt,space);
    }
    session_->requestedMultiplier.store(mult);
    session_->Submit(received, writer, creationQueue, frame, frame11, g, mult);
}
} // namespace DXL
