#include "LegacyGraphics.h"
#include "HookTeardown.h"
#include "MinHook.h"
#include <d3d9.h>
#include <dxgi1_2.h>
#include <GL/gl.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "d3d9.lib")

namespace DXL {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned Slots = 3;
constexpr auto Api9 = static_cast<Ipc::GraphicsApi>(9);
// A thread can already be inside a retained trampoline when CRT teardown
// begins. Keep its lookup/locking state alive just like the trampoline itself.
// GPU resources are explicitly drained by StopLegacyGraphicsHooks; the shared
// accelerator is deliberately retained until the OS reclaims this process.
std::recursive_mutex& mutex = *new std::recursive_mutex;
LegacyGraphicsCallbacks callbacks;
LegacyGraphicsStats stats;
std::atomic<bool> stopped{true};
thread_local bool inside = false;
HWND selectedWindow = nullptr;
ComPtr<ID3D11Device>& accelerator = *new ComPtr<ID3D11Device>;
ComPtr<ID3D11DeviceContext>& context11 = *new ComPtr<ID3D11DeviceContext>;
bool acceleratorTried = false;
std::atomic<bool> d3d9DeviceObserved{false};
std::vector<void*>& inlineHooks = *new std::vector<void*>;
std::mutex& hookMutex = *new std::mutex;
struct MethodHook { void* target; void* original; };
std::vector<MethodHook>& methodHooks = *new std::vector<MethodHook>;

void Error(const char* text) {
    if (stats.error != text) D5_LOG_WARN(L"Legacy graphics: %hs", text);
    stats.error = text;
}
struct Reentry {
    Reentry() { inside = true; }
    ~Reentry() { inside = false; }
};
bool SelectWindow(HWND window) {
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) return false;
    if (selectedWindow && selectedWindow != window && IsWindow(selectedWindow)) return false;
    selectedWindow = window;
    return true;
}
bool EnsureAccelerator() {
    if (accelerator) return true;
    if (acceleratorTried) return false;
    acceleratorTried = true;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (!adapter || FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                &accelerator, nullptr, &context11))) {
            D5_LOG_INFO(L"Legacy graphics: private NVIDIA D3D11 accelerator: %ls", desc.Description);
            return true;
        }
    }
    Error("No NVIDIA D3D11 accelerator is available for legacy NR");
    return false;
}

// Copy-only private context: never touches the application's D3D9/GL state.
// Every output slot owns an event query; no Map is attempted while GPU work
// remains outstanding. Flush submits cross-API fences, but does not wait.
struct Transfer {
    struct Slot { ComPtr<ID3D11Texture2D> staging; ComPtr<ID3D11Query> ready;
        uint64_t serial = 0, generation = 0, submittedAt = 0; };
    ComPtr<ID3D11Texture2D> image;
    std::array<Slot, Slots> slots;
    UINT width = 0, height = 0;
    uint64_t serial = 0, displayedSerial = 0;
    uint64_t lastOutputAt = 0;
    std::vector<unsigned char> pixels;
    bool valid = false;

    void Discard() { valid = false; ++stats.generation; }
    bool Resize(UINT w, UINT h) {
        if (image && width == w && height == h) return true;
        if (!w || !h || w > 16384 || h > 16384 || !EnsureAccelerator()) return false;
        *this = Transfer{};
        width = w; height = h;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(accelerator->CreateTexture2D(&desc, nullptr, &image))) return false;
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        for (auto& slot : slots)
            if (FAILED(accelerator->CreateTexture2D(&desc, nullptr, &slot.staging)) ||
                FAILED(accelerator->CreateQuery(&query, &slot.ready))) return false;
        pixels.resize(size_t(w) * h * 4);
        ++stats.generation;
        return true;
    }
    bool Poll() {
        bool changed = false;
        // A failed or stalled processor must not freeze the game indefinitely
        // on a cached frame. Resume native presentation after a bounded age.
        constexpr uint64_t MaximumFrameAgeMs = 500;
        const auto now = GetTickCount64();
        if (valid && now - lastOutputAt > MaximumFrameAgeMs) valid = false;
        for (auto& slot : slots) {
            if (!slot.serial) continue;
            BOOL done = FALSE;
            HRESULT hr = context11->GetData(slot.ready.Get(), &done, sizeof(done),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE || (hr == S_OK && !done)) continue;
            if (FAILED(hr)) { slot.serial = 0; Error("D3D11 output completion query failed"); continue; }
            if (slot.generation == stats.generation && slot.serial > displayedSerial &&
                now - slot.submittedAt <= MaximumFrameAgeMs) {
                D3D11_MAPPED_SUBRESOURCE map{};
                hr = context11->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
                    D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
                if (SUCCEEDED(hr)) {
                    for (UINT y = 0; y < height; ++y)
                        memcpy(pixels.data() + size_t(y) * width * 4,
                            static_cast<const char*>(map.pData) + size_t(y) * map.RowPitch, width * 4);
                    context11->Unmap(slot.staging.Get(), 0);
                    displayedSerial = slot.serial; valid = changed = true;
                    lastOutputAt = slot.submittedAt;
                } else Error("D3D11 output map failed");
            }
            slot.serial = 0;
        }
        return changed;
    }
    bool HasRoom() const {
        for (const auto& slot : slots) if (!slot.serial) return true;
        return false;
    }
    bool Process(const void* input, UINT pitch, Ipc::GraphicsApi api, HWND window) {
        Slot* slot = nullptr;
        for (auto& candidate : slots) if (!candidate.serial) { slot = &candidate; break; }
        if (!slot || !callbacks.process) { ++stats.skipped; return false; }
        context11->UpdateSubresource(image.Get(), 0, nullptr, input, pitch, 0);
        const bool ran = callbacks.process(accelerator.Get(), image.Get(), api, window);
        if (ran) {
            context11->CopyResource(slot->staging.Get(), image.Get());
            context11->End(slot->ready.Get());
            slot->serial = ++serial; slot->generation = stats.generation;
            slot->submittedAt = GetTickCount64();
            ++stats.processed;
        }
        context11->Flush();
        return ran;
    }
};

namespace GL {
constexpr GLenum ReadFbo = 0x8CA8, DrawFbo = 0x8CA9, ReadFboBinding = 0x8CAA,
    DrawFboBinding = 0x8CA6, Color0 = 0x8CE0, Complete = 0x8CD5,
    PackBuffer = 0x88EB, UnpackBuffer = 0x88EC, PackBinding = 0x88ED,
    UnpackBinding = 0x88EF, StreamRead = 0x88E1, ReadOnly = 0x88B8,
    SyncComplete = 0x9117, AlreadySignaled = 0x911A, ConditionSatisfied = 0x911C,
    WaitFailed = 0x911D, FramebufferSrgb = 0x8DB9;
// GL_SYNC_GPU_COMMANDS_COMPLETE is 0x9117. The reference bridge declared
// 0x911F (GL_MAX_SERVER_WAIT_TIMEOUT), which explains its broken PBO fences.
using Sync = void*;
struct Functions {
    void (APIENTRY* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void (APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
    void (APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum (APIENTRY* CheckFramebufferStatus)(GLenum) = nullptr;
    void (APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
    void (APIENTRY* GenBuffers)(GLsizei, GLuint*) = nullptr;
    void (APIENTRY* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void (APIENTRY* BindBuffer)(GLenum, GLuint) = nullptr;
    void (APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum) = nullptr;
    void* (APIENTRY* MapBuffer)(GLenum, GLenum) = nullptr;
    GLboolean (APIENTRY* UnmapBuffer)(GLenum) = nullptr;
    Sync (APIENTRY* FenceSync)(GLenum, GLbitfield) = nullptr;
    GLenum (APIENTRY* ClientWaitSync)(Sync, GLbitfield, unsigned long long) = nullptr;
    void (APIENTRY* DeleteSync)(Sync) = nullptr;
    bool Load() {
#define LOAD_GL(name) do { auto p = wglGetProcAddress("gl" #name); \
    if (!p || uintptr_t(p) <= 3 || uintptr_t(p) == UINTPTR_MAX) return false; \
    name = reinterpret_cast<decltype(name)>(p); } while (false)
        LOAD_GL(GenFramebuffers); LOAD_GL(DeleteFramebuffers); LOAD_GL(BindFramebuffer);
        LOAD_GL(FramebufferTexture2D); LOAD_GL(CheckFramebufferStatus); LOAD_GL(BlitFramebuffer);
        LOAD_GL(GenBuffers); LOAD_GL(DeleteBuffers); LOAD_GL(BindBuffer); LOAD_GL(BufferData);
        LOAD_GL(MapBuffer); LOAD_GL(UnmapBuffer); LOAD_GL(FenceSync);
        LOAD_GL(ClientWaitSync); LOAD_GL(DeleteSync);
#undef LOAD_GL
        return true;
    }
};
struct StateGuard {
    Functions& fn;
    GLint readFbo, drawFbo, texture, packBuffer, unpackBuffer, defaultRead, defaultDraw;
    std::array<GLint, 8> pixel{};
    GLboolean scissor, srgb;
    static constexpr GLenum names[8] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
        GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT,
        GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS};
    explicit StateGuard(Functions& f) : fn(f) {
        glGetIntegerv(ReadFboBinding, &readFbo); glGetIntegerv(DrawFboBinding, &drawFbo);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture); glGetIntegerv(PackBinding, &packBuffer);
        glGetIntegerv(UnpackBinding, &unpackBuffer);
        for (unsigned i = 0; i < pixel.size(); ++i) glGetIntegerv(names[i], &pixel[i]);
        scissor = glIsEnabled(GL_SCISSOR_TEST); srgb = glIsEnabled(FramebufferSrgb);
        fn.BindFramebuffer(ReadFbo, 0); glGetIntegerv(GL_READ_BUFFER, &defaultRead);
        fn.BindFramebuffer(DrawFbo, 0); glGetIntegerv(GL_DRAW_BUFFER, &defaultDraw);
    }
    void TightPixels() {
        for (unsigned i = 0; i < pixel.size(); ++i) glPixelStorei(names[i], i % 4 ? 0 : 1);
    }
    ~StateGuard() {
        fn.BindFramebuffer(ReadFbo, 0); glReadBuffer(defaultRead);
        fn.BindFramebuffer(DrawFbo, 0); glDrawBuffer(defaultDraw);
        fn.BindFramebuffer(ReadFbo, readFbo); fn.BindFramebuffer(DrawFbo, drawFbo);
        glBindTexture(GL_TEXTURE_2D, texture);
        fn.BindBuffer(PackBuffer, packBuffer); fn.BindBuffer(UnpackBuffer, unpackBuffer);
        for (unsigned i = 0; i < pixel.size(); ++i) glPixelStorei(names[i], pixel[i]);
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (srgb) glEnable(FramebufferSrgb); else glDisable(FramebufferSrgb);
    }
};
struct State {
    HGLRC owner = nullptr;
    Functions fn;
    Transfer transfer;
    std::array<GLuint, Slots> pbos{};
    std::array<Sync, Slots> fences{};
    std::array<uint64_t, Slots> generations{}, serials{};
    uint64_t serial = 0;
    GLuint texture = 0, fbo = 0;
    bool textureValid = false, loaded = false, failed = false, enabled = false;
    std::vector<unsigned char> input;
    void ReleaseImages() {
        if (owner == wglGetCurrentContext() && loaded) {
            for (auto& fence : fences) if (fence) fn.DeleteSync(fence);
            fn.DeleteBuffers(Slots, pbos.data());
            if (fbo) fn.DeleteFramebuffers(1, &fbo);
            if (texture) glDeleteTextures(1, &texture);
        }
        fences = {}; pbos = {}; texture = fbo = 0; textureValid = false;
        transfer = Transfer{};
    }
    bool Resize(UINT w, UINT h) {
        if (transfer.image && transfer.width == w && transfer.height == h) return true;
        ReleaseImages();
        if (!transfer.Resize(w, h)) return false;
        StateGuard guard(fn); guard.TightPixels();
        fn.BindBuffer(UnpackBuffer, 0);
        glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        fn.GenFramebuffers(1, &fbo); fn.BindFramebuffer(ReadFbo, fbo);
        fn.FramebufferTexture2D(ReadFbo, Color0, GL_TEXTURE_2D, texture, 0);
        if (fn.CheckFramebufferStatus(ReadFbo) != Complete) return false;
        fn.GenBuffers(Slots, pbos.data());
        for (auto pbo : pbos) {
            fn.BindBuffer(PackBuffer, pbo);
            fn.BufferData(PackBuffer, ptrdiff_t(w) * h * 4, nullptr, StreamRead);
        }
        input.resize(size_t(w) * h * 4);
        return true;
    }
    void Present(HDC dc, HWND window) {
        const bool active = callbacks.begin && callbacks.begin(Ipc::GraphicsApi::OpenGL, window);
        if (!active) {
            if (enabled) { transfer.Discard(); textureValid = false; enabled = false; }
            return;
        }
        enabled = true;
        if (failed) return;
        if (!loaded) {
            owner = wglGetCurrentContext();
            int major = 0, minor = 0;
            const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            if (!version || sscanf_s(version, "%d.%d", &major, &minor) != 2 ||
                major < 3 || (major == 3 && minor < 2)) {
                Error("OpenGL 3.2 or newer is required by the native capture backend"); failed = true; return;
            }
            if (!fn.Load()) { Error("OpenGL 3.2 / ARB_sync and framebuffer functions are required"); failed = true; return; }
            loaded = true;
            PIXELFORMATDESCRIPTOR pfd{};
            DescribePixelFormat(dc, GetPixelFormat(dc), sizeof(pfd), &pfd);
            if (!(pfd.dwFlags & PFD_DOUBLEBUFFER) || (pfd.dwFlags & PFD_STEREO)) {
                Error("OpenGL capture requires a non-stereo double-buffered window"); failed = true; return;
            }
            if (pfd.iPixelType != PFD_TYPE_RGBA || pfd.cRedBits != 8 ||
                pfd.cGreenBits != 8 || pfd.cBlueBits != 8) {
                Error("OpenGL capture requires an 8-bit RGB window framebuffer"); failed = true; return;
            }
        }
        RECT rect{};
        if (!GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0) return;
        if (!Resize(rect.right, rect.bottom)) { Error("OpenGL bridge texture allocation failed"); failed = true; return; }
        StateGuard guard(fn); guard.TightPixels();
        const UINT w = transfer.width, h = transfer.height;
        bool outputChanged = transfer.Poll();
        // Consume the oldest completed capture first to preserve optical-flow
        // history. A full queue skips capture instead of waiting on the GPU.
        unsigned oldest = Slots;
        for (unsigned i = 0; i < Slots; ++i)
            if (fences[i] && (oldest == Slots || serials[i] < serials[oldest])) oldest = i;
        if (oldest != Slots && transfer.HasRoom()) {
            const GLenum ready = fn.ClientWaitSync(fences[oldest], 0, 0);
            if (ready == AlreadySignaled || ready == ConditionSatisfied) {
                fn.BindBuffer(PackBuffer, pbos[oldest]);
                const auto* mapped = static_cast<const unsigned char*>(fn.MapBuffer(PackBuffer, ReadOnly));
                if (mapped && generations[oldest] == stats.generation) {
                    for (UINT y = 0; y < h; ++y)
                        memcpy(input.data() + size_t(y) * w * 4, mapped + size_t(h - 1 - y) * w * 4, w * 4);
                }
                const bool intact = mapped && fn.UnmapBuffer(PackBuffer);
                fn.DeleteSync(fences[oldest]); fences[oldest] = nullptr;
                if (intact && generations[oldest] == stats.generation)
                    transfer.Process(input.data(), w * 4, Ipc::GraphicsApi::OpenGL, window);
            } else if (ready == WaitFailed) {
                Error("OpenGL asynchronous capture fence failed"); failed = true; return;
            }
        }
        for (unsigned i = 0; i < Slots; ++i) if (!fences[i]) {
            fn.BindFramebuffer(ReadFbo, 0); glReadBuffer(GL_BACK);
            fn.BindBuffer(PackBuffer, pbos[i]);
            LARGE_INTEGER start{}, end{}, frequency{};
            QueryPerformanceCounter(&start);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            fences[i] = fn.FenceSync(SyncComplete, 0);
            glFlush(); // submit the GL fence even when the app suppresses SwapBuffers
            QueryPerformanceCounter(&end); QueryPerformanceFrequency(&frequency);
            stats.lastCaptureMs = 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart;
            if (!fences[i]) { Error("OpenGL capture could not create its completion fence"); failed = true; return; }
            generations[i] = stats.generation; serials[i] = ++serial;
            ++stats.captures; break;
        }
        if (outputChanged) {
            fn.BindBuffer(UnpackBuffer, 0); glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, transfer.pixels.data());
            textureValid = true;
        }
        if (textureValid && transfer.valid) {
            fn.BindFramebuffer(ReadFbo, fbo); glReadBuffer(Color0);
            fn.BindFramebuffer(DrawFbo, 0); glDrawBuffer(GL_BACK);
            glDisable(GL_SCISSOR_TEST); glDisable(FramebufferSrgb);
            fn.BlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            ++stats.displayed;
        }
    }
};
std::vector<std::unique_ptr<State>>& states = *new std::vector<std::unique_ptr<State>>;
HGLRC selectedContext = nullptr;
State* Find(HGLRC owner) {
    for (auto& state : states) if (state->owner == owner) return state.get();
    auto state = std::make_unique<State>(); state->owner = owner;
    states.push_back(std::move(state)); return states.back().get();
}
} // namespace GL

struct State9 {
    IDirect3DDevice9* owner = nullptr; // never dereferenced outside a live Present
    ComPtr<IDirect3DSurface9> readback, upload, resolve;
    ComPtr<IDirect3DTexture9> outputTexture;
    ComPtr<IDirect3DSurface9> outputSurface;
    Transfer transfer;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    D3DMULTISAMPLE_TYPE samples = D3DMULTISAMPLE_NONE;
    DWORD sampleQuality = 0;
    bool enabled = false, failed = false;
    std::vector<unsigned char> input;
    void Release() { readback.Reset(); upload.Reset(); resolve.Reset();
        outputSurface.Reset(); outputTexture.Reset(); samples = D3DMULTISAMPLE_NONE; sampleQuality = 0;
        transfer = Transfer{}; format = D3DFMT_UNKNOWN; failed = false; ++stats.generation; }
    bool DrawMultisampleOutput(IDirect3DDevice9* device, IDirect3DSurface9* backbuffer) {
        if (FAILED(device->UpdateSurface(upload.Get(), nullptr, outputSurface.Get(), nullptr))) return false;
        // D3DSBT_ALL omits render targets and the depth-stencil surface. Save
        // those explicitly; SetRenderTarget also changes the viewport.
        ComPtr<IDirect3DStateBlock9> block;
        std::array<ComPtr<IDirect3DSurface9>, 4> targets;
        ComPtr<IDirect3DSurface9> depth;
        D3DVIEWPORT9 viewport{}; D3DCAPS9 caps{};
        if (FAILED(device->GetDeviceCaps(&caps)) || FAILED(device->GetViewport(&viewport)) ||
            FAILED(device->GetRenderTarget(0, &targets[0])) ||
            FAILED(device->CreateStateBlock(D3DSBT_ALL, &block)) || FAILED(block->Capture())) return false;
        const UINT targetCount = (caps.NumSimultaneousRTs < 4) ? caps.NumSimultaneousRTs : 4;
        for (UINT i = 1; i < targetCount; ++i) device->GetRenderTarget(i, &targets[i]);
        device->GetDepthStencilSurface(&depth);
        // Present of an additional chain may occur inside a game's scene.
        // In that case leave its native frame untouched rather than nesting.
        if (FAILED(device->BeginScene())) return false;
        bool okay = true;
        auto set = [&](HRESULT result) { okay &= SUCCEEDED(result); };
        set(device->SetDepthStencilSurface(nullptr));
        for (UINT i = 1; i < targetCount; ++i) set(device->SetRenderTarget(i, nullptr));
        set(device->SetRenderTarget(0, backbuffer));
        const D3DVIEWPORT9 outputViewport{0, 0, transfer.width, transfer.height, 0, 1};
        set(device->SetViewport(&outputViewport));
        set(device->SetVertexShader(nullptr)); set(device->SetPixelShader(nullptr));
        set(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
        const std::pair<D3DRENDERSTATETYPE, DWORD> render[] = {
            {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
            {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
            {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
            {D3DRS_FOGENABLE, FALSE}, {D3DRS_LIGHTING, FALSE}, {D3DRS_SPECULARENABLE, FALSE},
            {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_DITHERENABLE, FALSE},
            {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_CULLMODE, D3DCULL_NONE},
            {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_COLORWRITEENABLE, 15},
            {D3DRS_WRAP0, 0}, {D3DRS_VERTEXBLEND, D3DVBF_DISABLE}, {D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE},
            {D3DRS_MULTISAMPLEANTIALIAS, TRUE}, {D3DRS_MULTISAMPLEMASK, 0xffffffffu}
        };
        for (const auto& value : render) set(device->SetRenderState(value.first, value.second));
        set(device->SetTexture(0, outputTexture.Get()));
        set(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        set(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        set(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        set(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
        set(device->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_CURRENT));
        set(device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0));
        set(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
        set(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
        set(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
        set(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
        set(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
        set(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
        set(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        set(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
        struct Vertex { float x, y, z, rhw, u, v; };
        const float right = float(transfer.width) - .5f, bottom = float(transfer.height) - .5f;
        const Vertex vertices[] = {{-.5f,-.5f,0,1,0,0}, {right,-.5f,0,1,1,0},
            {-.5f,bottom,0,1,0,1}, {right,bottom,0,1,1,1}};
        if (okay) set(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
        set(device->EndScene());
        // Restore while the temporary state block is alive, then immediately
        // release its references to game textures/shaders/buffers (no retention
        // until the next frame or until Reset).
        device->SetDepthStencilSurface(nullptr);
        for (UINT i = 1; i < targetCount; ++i) device->SetRenderTarget(i, nullptr);
        set(device->SetRenderTarget(0, targets[0].Get()));
        for (UINT i = 1; i < targetCount; ++i) set(device->SetRenderTarget(i, targets[i].Get()));
        set(device->SetDepthStencilSurface(depth.Get()));
        set(block->Apply()); set(device->SetViewport(&viewport));
        return okay;
    }
    void Present(IDirect3DDevice9* device, IDirect3DSurface9* backbuffer, HWND window) {
        const bool active = callbacks.begin && callbacks.begin(Api9, window);
        if (!active) { if (enabled) { transfer.Discard(); enabled = false; } return; }
        enabled = true;
        if (failed || !backbuffer) return;
        D3DSURFACE_DESC desc{};
        if (FAILED(backbuffer->GetDesc(&desc))) return;
        if (owner != device || transfer.width != desc.Width || transfer.height != desc.Height || format != desc.Format ||
            samples != desc.MultiSampleType || sampleQuality != desc.MultiSampleQuality) {
            Release(); owner = device; format = desc.Format;
            samples = desc.MultiSampleType; sampleQuality = desc.MultiSampleQuality;
            if (format != D3DFMT_A8R8G8B8 && format != D3DFMT_X8R8G8B8) {
                Error("D3D9 capture supports A8R8G8B8 / X8R8G8B8 backbuffers"); failed = true; return;
            }
            if (!transfer.Resize(desc.Width, desc.Height) ||
                FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, format, D3DPOOL_SYSTEMMEM, &readback, nullptr)) ||
                FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, format, D3DPOOL_SYSTEMMEM, &upload, nullptr))) {
                Error("D3D9 bridge surface allocation failed"); failed = true; return;
            }
            if (samples != D3DMULTISAMPLE_NONE &&
                (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, format, D3DMULTISAMPLE_NONE,
                    0, FALSE, &resolve, nullptr)) ||
                 FAILED(device->CreateTexture(desc.Width, desc.Height, 1, 0, format,
                    D3DPOOL_DEFAULT, &outputTexture, nullptr)) ||
                 FAILED(outputTexture->GetSurfaceLevel(0, &outputSurface)))) {
                Error("D3D9 MSAA resolve/output texture allocation failed"); failed = true; return;
            }
            input.resize(size_t(desc.Width) * desc.Height * 4);
            D5_LOG_INFO(L"Legacy D3D9: CPU capture %ux%u; GetRenderTargetData may stall the game thread", desc.Width, desc.Height);
        }
        transfer.Poll();
        if (transfer.HasRoom()) {
            LARGE_INTEGER start{}, end{}, frequency{};
            QueryPerformanceCounter(&start);
            HRESULT hr = S_OK;
            if (resolve) hr = device->StretchRect(backbuffer, nullptr, resolve.Get(), nullptr, D3DTEXF_NONE);
            if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(resolve ? resolve.Get() : backbuffer, readback.Get());
            QueryPerformanceCounter(&end); QueryPerformanceFrequency(&frequency);
            stats.lastCaptureMs = 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart;
            if (FAILED(hr)) { Error("D3D9 native backbuffer readback failed"); return; }
            D3DLOCKED_RECT map{};
            if (FAILED(readback->LockRect(&map, nullptr, D3DLOCK_READONLY))) return;
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* source = static_cast<const unsigned char*>(map.pBits) + size_t(y) * map.Pitch;
                auto* dest = input.data() + size_t(y) * desc.Width * 4;
                for (UINT x = 0; x < desc.Width; ++x) {
                    dest[4*x] = source[4*x+2]; dest[4*x+1] = source[4*x+1];
                    dest[4*x+2] = source[4*x]; dest[4*x+3] = format == D3DFMT_A8R8G8B8 ? source[4*x+3] : 255;
                }
            }
            readback->UnlockRect(); ++stats.captures;
            transfer.Process(input.data(), desc.Width * 4, Api9, window);
        } else ++stats.skipped;
        if (transfer.valid) {
            D3DLOCKED_RECT map{};
            if (FAILED(upload->LockRect(&map, nullptr, 0))) return;
            for (UINT y = 0; y < desc.Height; ++y) {
                auto* dest = static_cast<unsigned char*>(map.pBits) + size_t(y) * map.Pitch;
                const auto* source = transfer.pixels.data() + size_t(y) * desc.Width * 4;
                for (UINT x = 0; x < desc.Width; ++x) {
                    dest[4*x] = source[4*x+2]; dest[4*x+1] = source[4*x+1];
                    dest[4*x+2] = source[4*x]; dest[4*x+3] = format == D3DFMT_A8R8G8B8 ? source[4*x+3] : 255;
                }
            }
            upload->UnlockRect();
            const bool written = outputTexture ? DrawMultisampleOutput(device, backbuffer) :
                SUCCEEDED(device->UpdateSurface(upload.Get(), nullptr, backbuffer, nullptr));
            if (written) ++stats.displayed;
            else Error("D3D9 native backbuffer writeback failed");
        }
    }
};
State9& state9 = *new State9;

// Native D3D9Ex can allocate a separate vtable for each device. Patching a
// dummy device's table neither reaches existing devices nor survives release.
// Detour shared implementation addresses instead; preserve each trampoline by
// target so native D3D9, D3D9Ex and wrapper implementations can coexist.
template<class T> T Original(void* object, size_t index) {
    void* target = (*reinterpret_cast<void***>(object))[index];
    std::lock_guard<std::mutex> lock(hookMutex);
    for (const auto& hook : methodHooks)
        if (hook.target == target) return reinterpret_cast<T>(hook.original);
    return reinterpret_cast<T>(target);
}
void Patch(void* object, size_t index, void* hook) {
    if (!object || stopped || IsTeardownRequested()) return;
    void* target = (*reinterpret_cast<void***>(object))[index];
    std::lock_guard<std::mutex> lock(hookMutex);
    for (const auto& entry : methodHooks) if (entry.target == target) return;
    void* original = nullptr;
    if (MH_CreateHook(target, hook, &original) != MH_OK) return;
    // Keep the method's code module alive for the lifetime of the trampoline,
    // including after the private probe device releases its own driver refs.
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(target), &owner);
    methodHooks.push_back({target, original});
    inlineHooks.push_back(target);
    if (MH_EnableHook(target) != MH_OK) Error("D3D9 method detour could not be enabled");
}
void Frame9(IDirect3DDevice9* device, IDirect3DSwapChain9* chain, HWND overrideWindow) try {
    if (stopped || IsTeardownRequested()) return;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (stopped) return;
    ComPtr<IDirect3DSwapChain9> implicit;
    if (!chain && SUCCEEDED(device->GetSwapChain(0, &implicit))) chain = implicit.Get();
    if (!chain) return;
    D3DPRESENT_PARAMETERS present{};
    if (FAILED(chain->GetPresentParameters(&present))) return;
    const HWND window = overrideWindow ? overrideWindow : present.hDeviceWindow;
    if (!SelectWindow(window)) return;
    ComPtr<IDirect3DSurface9> backbuffer;
    chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    state9.Present(device, backbuffer.Get(), window);
} catch (...) {
    Error("D3D9 bridge allocation failed; native frame retained");
}
using Present9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*,const RECT*,const RECT*,HWND,const RGNDATA*,DWORD);
using ChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*,const RECT*,const RECT*,HWND,const RGNDATA*,DWORD);
HRESULT STDMETHODCALLTYPE Present9(IDirect3DDevice9* d,const RECT* a,const RECT* b,HWND w,const RGNDATA* r) {
    auto original = Original<Present9Fn>(d, 17);
    if (inside) return original(d,a,b,w,r);
    Reentry guard; Frame9(d, nullptr, w);
    return original(d,a,b,w,r);
}
HRESULT STDMETHODCALLTYPE PresentEx(IDirect3DDevice9Ex* d,const RECT* a,const RECT* b,HWND w,const RGNDATA* r,DWORD flags) {
    auto original = Original<PresentExFn>(d, 121);
    if (inside) return original(d,a,b,w,r,flags);
    Reentry guard;
    if (!(flags & D3DPRESENT_DONOTWAIT)) Frame9(d, nullptr, w);
    return original(d,a,b,w,r,flags);
}
HRESULT STDMETHODCALLTYPE ChainPresent(IDirect3DSwapChain9* c,const RECT* a,const RECT* b,HWND w,const RGNDATA* r,DWORD flags) {
    auto original = Original<ChainPresentFn>(c, 3);
    if (inside) return original(c,a,b,w,r,flags);
    Reentry guard;
    ComPtr<IDirect3DDevice9> device;
    if (SUCCEEDED(c->GetDevice(&device)) && !(flags & D3DPRESENT_DONOTWAIT)) Frame9(device.Get(), c, w);
    return original(c,a,b,w,r,flags);
}
using Reset9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*,D3DPRESENT_PARAMETERS*,D3DDISPLAYMODEEX*);
HRESULT STDMETHODCALLTYPE Reset9(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p) {
    auto original = Original<Reset9Fn>(d, 16);
    { std::lock_guard<std::recursive_mutex> lock(mutex); if (state9.owner == d) state9.Release(); }
    return original(d,p);
}
HRESULT STDMETHODCALLTYPE ResetEx(IDirect3DDevice9Ex* d,D3DPRESENT_PARAMETERS* p,D3DDISPLAYMODEEX* mode) {
    auto original = Original<ResetExFn>(d, 132);
    { std::lock_guard<std::recursive_mutex> lock(mutex); if (state9.owner == d) state9.Release(); }
    return original(d,p,mode);
}
void TrackChain(IDirect3DSwapChain9* chain) { Patch(chain, 3, reinterpret_cast<void*>(ChainPresent)); }
using ExtraChainFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*,IDirect3DSwapChain9**);
HRESULT STDMETHODCALLTYPE ExtraChain(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p,IDirect3DSwapChain9** chain) {
    auto original = Original<ExtraChainFn>(d, 13);
    const HRESULT result = original(d,p,chain);
    if (SUCCEEDED(result) && chain && *chain) TrackChain(*chain);
    return result;
}
void TrackDevice(IDirect3DDevice9* device) {
    d3d9DeviceObserved = true;
    Patch(device, 17, reinterpret_cast<void*>(Present9)); Patch(device, 16, reinterpret_cast<void*>(Reset9));
    Patch(device, 13, reinterpret_cast<void*>(ExtraChain));
    ComPtr<IDirect3DDevice9Ex> ex;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&ex)))) {
        Patch(ex.Get(), 121, reinterpret_cast<void*>(PresentEx)); Patch(ex.Get(), 132, reinterpret_cast<void*>(ResetEx));
    }
    for (UINT i = 0; i < device->GetNumberOfSwapChains(); ++i) {
        ComPtr<IDirect3DSwapChain9> chain;
        if (SUCCEEDED(device->GetSwapChain(i, &chain))) TrackChain(chain.Get());
    }
}
using CreateDevice9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,D3DDISPLAYMODEEX*,IDirect3DDevice9Ex**);
HRESULT STDMETHODCALLTYPE CreateDevice9(IDirect3D9* d,UINT a,D3DDEVTYPE t,HWND w,DWORD f,D3DPRESENT_PARAMETERS* p,IDirect3DDevice9** out) {
    auto original = Original<CreateDevice9Fn>(d, 16);
    HRESULT hr = original(d,a,t,w,f,p,out);
    if (SUCCEEDED(hr) && out && *out) TrackDevice(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateDeviceEx(IDirect3D9Ex* d,UINT a,D3DDEVTYPE t,HWND w,DWORD f,D3DPRESENT_PARAMETERS* p,D3DDISPLAYMODEEX* m,IDirect3DDevice9Ex** out) {
    auto original = Original<CreateDeviceExFn>(d, 20);
    HRESULT hr = original(d,a,t,w,f,p,m,out);
    if (SUCCEEDED(hr) && out && *out) TrackDevice(*out);
    return hr;
}
void TrackFactory(IDirect3D9* factory) {
    Patch(factory, 16, reinterpret_cast<void*>(CreateDevice9));
    ComPtr<IDirect3D9Ex> ex;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&ex)))) Patch(ex.Get(), 20, reinterpret_cast<void*>(CreateDeviceEx));
}
using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using Create9ExFn = HRESULT(WINAPI*)(UINT,IDirect3D9Ex**);
Create9Fn originalCreate9 = nullptr;
Create9ExFn originalCreate9Ex = nullptr;
IDirect3D9* WINAPI Create9(UINT sdk) {
    auto* result = originalCreate9(sdk); if (result) TrackFactory(result); return result;
}
HRESULT WINAPI Create9Ex(UINT sdk, IDirect3D9Ex** out) {
    const HRESULT result = originalCreate9Ex(sdk,out);
    if (SUCCEEDED(result) && out && *out) TrackFactory(*out);
    return result;
}
using SwapFn = BOOL(WINAPI*)(HDC);
using DeleteContextFn = BOOL(WINAPI*)(HGLRC);
SwapFn originalSwap = nullptr;
DeleteContextFn originalDeleteContext = nullptr;
BOOL WINAPI Swap(HDC dc) {
    try { if (!inside && !stopped && !IsTeardownRequested()) {
        Reentry guard;
        std::lock_guard<std::recursive_mutex> lock(mutex);
        const HGLRC gl = wglGetCurrentContext();
        const HWND window = WindowFromDC(dc);
        if (!stopped && gl && dc == wglGetCurrentDC() && SelectWindow(window)) {
            // A window can be presented by auxiliary contexts. Keep its NR
            // history on one context; do not interleave unrelated frame rings.
            if (!GL::selectedContext) GL::selectedContext = gl;
            if (GL::selectedContext == gl) GL::Find(gl)->Present(dc, window);
        }
    } } catch (...) { Error("OpenGL bridge allocation failed; native frame retained"); }
    return originalSwap(dc);
}
BOOL WINAPI DeleteContext(HGLRC gl) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto it = GL::states.begin(); it != GL::states.end(); ++it) if ((*it)->owner == gl) {
        (*it)->ReleaseImages(); GL::states.erase(it); ++stats.generation; break;
    }
    if (GL::selectedContext == gl) GL::selectedContext = nullptr;
    return originalDeleteContext(gl);
}
bool Hook(HMODULE module, const char* name, void* detour, void** original) {
    if (!module) return false;
    std::lock_guard<std::mutex> lock(hookMutex);
    auto* address = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!address) return false;
    for (auto target : inlineHooks) if (target == address) return true;
    if (MH_CreateHook(address, detour, original) != MH_OK) return false;
    if (MH_EnableHook(address) != MH_OK) return false;
    inlineHooks.push_back(address); return true;
}
bool d3d9Probed = false;
BOOL CALLBACK FindVisibleGameWindow(HWND window, LPARAM opaque) {
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    RECT client{};
    if (process == GetCurrentProcessId() && IsWindowVisible(window) && !IsIconic(window) &&
        GetClientRect(window, &client) && client.right >= 64 && client.bottom >= 64) {
        *reinterpret_cast<bool*>(opaque) = true; return FALSE;
    }
    return TRUE;
}
} // namespace

bool InstallLegacyGraphicsHooks(const LegacyGraphicsCallbacks& value) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    callbacks = value; stopped = false;
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    const bool installed = Hook(GetModuleHandleW(L"gdi32.dll"), "SwapBuffers", reinterpret_cast<void*>(Swap), reinterpret_cast<void**>(&originalSwap));
    Hook(GetModuleHandleW(L"opengl32.dll"), "wglDeleteContext", reinterpret_cast<void*>(DeleteContext), reinterpret_cast<void**>(&originalDeleteContext));
    // Export installation is safe during initialization. A private device is
    // reserved for a later worker tick after core has selected the actual API.
    PollLegacyGraphicsHooks(false);
    return installed;
}
void PollLegacyGraphicsHooks(bool allowDeviceProbe) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (stopped || IsTeardownRequested()) return;
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9 || d3d9Probed || d3d9DeviceObserved) return;
    Hook(d3d9, "Direct3DCreate9", reinterpret_cast<void*>(Create9), reinterpret_cast<void**>(&originalCreate9));
    Hook(d3d9, "Direct3DCreate9Ex", reinterpret_cast<void*>(Create9Ex), reinterpret_cast<void**>(&originalCreate9Ex));
    if (!allowDeviceProbe) return;
    // Export hooks cover early startup without creating a graphics device.
    // Only make the bounded late-injection probe after a game window is live.
    bool visible = false;
    EnumWindows(FindVisibleGameWindow, reinterpret_cast<LPARAM>(&visible));
    if (!visible) return;
    d3d9Probed = true;
    // Late injection: discover the runtime tables using a private hidden probe.
    // No exports are called under loader lock; the probe never presents.
    Reentry guard;
    HWND window = CreateWindowExW(0, L"STATIC", L"DXL D3D9 hook probe", WS_POPUP, 0,0,8,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!window) return;
    D3DPRESENT_PARAMETERS present{};
    present.Windowed = TRUE; present.hDeviceWindow = window; present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferWidth = present.BackBufferHeight = 8; present.BackBufferFormat = D3DFMT_X8R8G8B8;
    const DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    HRESULT probe9 = E_NOINTERFACE, probeEx = E_NOINTERFACE;
    if (originalCreate9) {
        ComPtr<IDirect3D9> factory; factory.Attach(originalCreate9(D3D_SDK_VERSION));
        if (factory) { TrackFactory(factory.Get()); ComPtr<IDirect3DDevice9> device;
            probe9 = factory->CreateDevice(0,D3DDEVTYPE_HAL,window,flags,&present,&device);
            if (SUCCEEDED(probe9)) TrackDevice(device.Get()); }
    }
    if (originalCreate9Ex) {
        ComPtr<IDirect3D9Ex> factory;
        if (SUCCEEDED(originalCreate9Ex(D3D_SDK_VERSION, &factory))) { TrackFactory(factory.Get()); ComPtr<IDirect3DDevice9Ex> device;
            probeEx = factory->CreateDeviceEx(0,D3DDEVTYPE_HAL,window,flags,&present,nullptr,&device);
            if (SUCCEEDED(probeEx)) TrackDevice(device.Get()); }
    }
    DestroyWindow(window);
    stats.d3d9Probe = probe9; stats.d3d9ExProbe = probeEx;
    D5_LOG_INFO(L"Legacy graphics: factory hooks installed; late-device probe D3D9=0x%08X D3D9Ex=0x%08X", probe9, probeEx);
}
void StopLegacyGraphicsHooks() noexcept {
    stopped = true;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    callbacks = {}; state9.Release();
    for (auto& state : GL::states) state->ReleaseImages();
    GL::states.clear();
    // Disable only this module's inline hooks. Do not free trampolines or call
    // MH_Uninitialize: another hook thread may already be executing one.
    std::lock_guard<std::mutex> hooksLock(hookMutex);
    for (auto address : inlineHooks) MH_DisableHook(address);
    // Accelerator lifetime follows the NR bridge's borrowed device pointer.
    // It stays alive through core teardown; the OS reclaims it on process exit.
}
LegacyGraphicsStats GetLegacyGraphicsStats() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex); return stats;
}
} // namespace DXL
