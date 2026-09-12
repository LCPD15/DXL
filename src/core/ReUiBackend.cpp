// ReUiBackend.cpp — see ReUiBackend.h
#include "ReUiBackend.h"
#include "HookTeardown.h"
#include "PresentationFocus.h"
#include "PresentWriterTracker.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"
#include <d3d11_1.h>

#include <cstdint>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#else
#define _ReturnAddress() __builtin_return_address(0)
#endif

namespace {

constexpr UINT kSrvCapacity = 128;

struct Ctx {
    bool ok = false;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D11Device* device11 = nullptr;
    ID3D11DeviceContext1* context11 = nullptr;
    ID3DDeviceContextState* state11 = nullptr;
    std::atomic<HWND> hwnd{nullptr};
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;

    // imgui state
    ImGuiContext* imgui = nullptr;
    ImFont* uiFont = nullptr;
    ImFont* toastFont = nullptr;

    // descriptor heaps
    ID3D12DescriptorHeap* srvHeap = nullptr;  // shader-visible (font SRVs)
    ID3D12DescriptorHeap* rtvHeap = nullptr;  // CPU only
    uint32_t srvCount = 0;
    std::array<bool, kSrvCapacity> srvUsed{};
    UINT srvIncrement = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};

    // command list + sync
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    bool submittedOnce = false;
    bool syncFailed = false;
    ID3D12Resource* pendingBuffer = nullptr;

    // Window subclass state is independent of renderer/queue lifetime.
    std::atomic<std::atomic<bool>*> uiOpen{nullptr};
    std::atomic<int> wheelX{0}, wheelY{0};

    // cursor free-range hooks (user32, installed via MinHook)
    bool hooksUp = false;
    HMODULE selfMod = nullptr;
    bool haveAnchor = false;
    POINT anchor{};
    std::mutex anchorMutex;
    HCURSOR arrow = nullptr;
    std::atomic<bool> virtualCursor{false};
    bool inputSessionOpen = false;

};

// A foreign WndProc subclass can retain our forwarding callback after renderer
// shutdown. Its state must outlive CRT destructors, just like cursor detours.
Ctx& g = *new Ctx;

// ---- UI scale --------------------------------------------------------------
// Base font was 13 px; the panel is now drawn at 2x and the toast text at 3x.
// (Style is scaled by the same factor inside InitOnce.)
constexpr float kUiFontPx = 13.0f * 2.0f;
constexpr float kToastFontPx = 13.0f * 3.0f;

// ---- SRV allocator callbacks for the backend ----
void SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto* heap = static_cast<ID3D12DescriptorHeap*>(info->SrvDescriptorHeap);
    if (!heap) return;
    D3D12_CPU_DESCRIPTOR_HANDLE c = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gp = heap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = g.srvIncrement;
    UINT idx = 0;
    while (idx < kSrvCapacity && g.srvUsed[idx]) ++idx;
    if (idx == kSrvCapacity) {
        if (cpu) *cpu = {};
        if (gpu) *gpu = {};
        D5_LOG_ERROR(L"ImGui descriptor heap exhausted: %u live textures", kSrvCapacity);
        return;
    }
    g.srvUsed[idx] = true;
    g.srvCount = (std::max)(g.srvCount, idx + 1);
    c.ptr += idx * inc;
    gp.ptr += idx * inc;
    if (cpu) *cpu = c;
    if (gpu) *gpu = gp;
}
void SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
             D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    const auto* heap = info ? info->SrvDescriptorHeap : nullptr;
    if (!heap || heap != g.srvHeap || !g.srvIncrement) return;
    const auto firstCpu = g.srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    const auto firstGpu = g.srvHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < firstCpu || gpu.ptr < firstGpu) return;
    const auto offset = cpu.ptr - firstCpu;
    if (offset % g.srvIncrement || gpu.ptr - firstGpu != offset) return;
    const auto idx = offset / g.srvIncrement;
    // ImGui retires textures after its in-flight frame delay. Our present path
    // also fences each submission, so retired descriptors are safe to reuse.
    if (idx < kSrvCapacity) g.srvUsed[idx] = false;
}

// ---- input swallow ----
// A foreign subclass above ours retains a pointer to UiWndProc. Reinstalling
// above it would form UI -> foreign -> UI. Keep each window's original edge
// for as long as anybody can call it, including while the renderer is absent.
struct InputHook {
    WNDPROC previous = nullptr;
    bool unicode = true;
    bool enabled = false;
};
std::mutex& inputMutex = *new std::mutex;
std::unordered_map<HWND, InputHook>& inputHooks = *new std::unordered_map<HWND, InputHook>;
void EnsureInputHook(HWND want) noexcept;
void RemoveInputHook() noexcept;
void QueueRawMouse(HRAWINPUT input) noexcept;
void SyncCursorImage(bool virtualActive) noexcept;


static LRESULT CallProcVariant(WNDPROC proc, bool unicode, HWND hwnd, UINT msg,
                               WPARAM w, LPARAM l) {
    return unicode ? CallWindowProcW(proc, hwnd, msg, w, l)
                   : CallWindowProcA(proc, hwnd, msg, w, l);
}

static LRESULT CallDefVariant(bool unicode, HWND hwnd, UINT msg, WPARAM w,
                              LPARAM l) {
    return unicode ? DefWindowProcW(hwnd, msg, w, l)
                   : DefWindowProcA(hwnd, msg, w, l);
}

LRESULT UiWndProcInner(const InputHook& hook, HWND hwnd, UINT msg, WPARAM wParam,
                       LPARAM lParam) {
    const bool unicode = hook.unicode;
    // Snapshot before calling outside code: WM_NCDESTROY must still reach the
    // previous procedure even if teardown changes our registration meanwhile.
    auto forward = [&]() -> LRESULT {
        if (hook.previous) return CallProcVariant(hook.previous, unicode, hwnd,
                                                  msg, wParam, lParam);
        return CallDefVariant(unicode, hwnd, msg, wParam, lParam);
    };
    if (msg == WM_NCDESTROY) {
        SyncCursorImage(false);
        const LRESULT result = forward();
        std::lock_guard<std::mutex> lock(inputMutex);
        inputHooks.erase(hwnd);
        HWND expected = hwnd;
        g.hwnd.compare_exchange_strong(expected, nullptr);
        return result;
    }
    if (!hook.enabled) return forward();
    auto* uiOpen = g.uiOpen.load(std::memory_order_acquire);

    // Moving focus between an embedded render surface and its host/control
    // stays in the game. Alt-Tab or a separate dialog releases the panel.
    if ((msg == WM_ACTIVATEAPP && wParam == FALSE) ||
        (msg == WM_KILLFOCUS && !DXL::SamePresentationWindowTree(
            g.hwnd.load(), reinterpret_cast<HWND>(wParam)))) {
        if (uiOpen) uiOpen->store(false, std::memory_order_relaxed);
        ClipCursor(nullptr); // render callbacks may stop immediately after Alt-Tab
    }

    const bool open = uiOpen && uiOpen->load(std::memory_order_acquire);
    SyncCursorImage(open && g.virtualCursor.load(std::memory_order_relaxed));
    if (!open) return forward();

    switch (msg) {
    case WM_INPUT:
        QueueRawMouse(reinterpret_cast<HRAWINPUT>(lParam));
        // raw input MUST still reach DefWindowProc for the OS-side cleanup even
        // when the game is not allowed to see it
        return CallDefVariant(unicode, hwnd, msg, wParam, lParam);
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
    case WM_SETCURSOR:
        return 0;   // stop the game fighting the UI
    case WM_MOUSEWHEEL:
        g.wheelY.fetch_add(GET_WHEEL_DELTA_WPARAM(wParam), std::memory_order_relaxed);
        return 0;
    case WM_MOUSEHWHEEL:
        g.wheelX.fetch_add(-GET_WHEEL_DELTA_WPARAM(wParam), std::memory_order_relaxed);
        return 0;
    default:
        return forward();
    }
}

// Legitimate nested messages (e.g. SendMessage from a window procedure) still
// follow the chain. An identical message re-entering our callback is a cycle;
// never call any saved foreign procedure to break it -- that can re-enter too.
LRESULT CALLBACK UiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    InputHook hook;
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        const auto it = inputHooks.find(hwnd);
        if (it != inputHooks.end()) hook = it->second;
        else hook.unicode = IsWindowUnicode(hwnd) != FALSE;
    }
    struct ActiveCall { HWND hwnd; UINT msg; WPARAM w; LPARAM l; ActiveCall* previous; };
    static thread_local ActiveCall* active = nullptr;
    unsigned depth = 0;
    for (auto* call = active; call; call = call->previous) {
        if (++depth >= 32 || (call->hwnd == hwnd && call->msg == msg &&
                             call->w == wParam && call->l == lParam)) {
            static std::atomic<unsigned> warnings{0};
            if (warnings.fetch_add(1) < 4)
                D5_LOG_WARN(L"ImGui: window message cycle stopped hwnd=%p msg=0x%X", hwnd, msg);
            return CallDefVariant(hook.unicode, hwnd, msg, wParam, lParam);
        }
    }
    ActiveCall call{hwnd, msg, wParam, lParam, active};
    active = &call;
    struct Restore { ActiveCall*& head; ActiveCall* previous; ~Restore() { head = previous; } } restore{active, call.previous};
    return UiWndProcInner(hook, hwnd, msg, wParam, lParam);
}

void DetachInputHook(HWND hwnd) noexcept {
    std::lock_guard<std::mutex> lock(inputMutex);
    const auto it = inputHooks.find(hwnd);
    if (it == inputHooks.end()) return;
    auto& hook = it->second;
    hook.enabled = false;
    if (!IsWindow(hwnd)) { inputHooks.erase(it); return; }
    const LONG_PTR current = hook.unicode ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
                                          : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (current != reinterpret_cast<LONG_PTR>(&UiWndProc) || !hook.previous) {
        // Another layer still calls us. Remain a transparent forwarding link.
        return;
    }
    SetLastError(0);
    const LONG_PTR replaced = hook.unicode
        ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hook.previous))
        : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hook.previous));
    if (replaced || GetLastError() == 0) inputHooks.erase(it);
}

void EnsureInputHook(HWND want) noexcept {
    if (!want || !IsWindow(want)) return;
    const HWND old = g.hwnd.exchange(want);
    if (old && old != want) DetachInputHook(old);
    std::lock_guard<std::mutex> lock(inputMutex);
    const auto existing = inputHooks.find(want);
    if (existing != inputHooks.end()) {
        existing->second.enabled = true;
        // A different top callback means another subclass, not a lost hook.
        // Never reinsert ourselves above it, even during FG/renderer rebuilds.
        return;
    }
    const bool unicode = IsWindowUnicode(want) != FALSE;
    const auto prior = reinterpret_cast<WNDPROC>(unicode
        ? GetWindowLongPtrW(want, GWLP_WNDPROC) : GetWindowLongPtrA(want, GWLP_WNDPROC));
    if (prior == &UiWndProc) return;
    auto& hook = inputHooks[want];
    hook = {prior, unicode, true};
    SetLastError(0);
    const LONG_PTR previous = unicode
        ? SetWindowLongPtrW(want, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UiWndProc))
        : SetWindowLongPtrA(want, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UiWndProc));
    if (!previous && GetLastError() != 0) {
        inputHooks.erase(want);
        return;
    }
    if (previous != reinterpret_cast<LONG_PTR>(&UiWndProc))
        hook.previous = reinterpret_cast<WNDPROC>(previous);
    D5_LOG_INFO(L"ImGui input attached: hwnd=%p root=%p foreground=%p previous=%p unicode=%u",
        want, GetAncestor(want, GA_ROOT), GetForegroundWindow(), hook.previous, unsigned(unicode));
}

void RemoveInputHook() noexcept {
    const HWND hwnd = g.hwnd.exchange(nullptr);
    if (hwnd) DetachInputHook(hwnd);
}

bool WaitForGpu() noexcept {
    if (g.syncFailed) return false;
    if (!g.submittedOnce || !g.fence) return true;
    const uint64_t target = g.fenceValue;
    auto completed = g.fence->GetCompletedValue();
    if (completed == UINT64_MAX) return false;
    if (completed < target) {
        if (FAILED(g.fence->SetEventOnCompletion(target, g.fenceEvent)) ||
            WaitForSingleObject(g.fenceEvent, 100) != WAIT_OBJECT_0) return false;
        completed = g.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < target) return false;
    }
    if (g.pendingBuffer) { g.pendingBuffer->Release(); g.pendingBuffer = nullptr; }
    return true;
}

// ---- cursor free-range policy (ReShade-style) ----

// Panel open AND the game window is in the foreground. Alt-tabbing out of the
// game must release everything (no forced clip / no swallowed cursor).
bool UiActive() noexcept {
    auto* uiOpen = g.uiOpen.load(std::memory_order_acquire);
    if (!uiOpen || !uiOpen->load(std::memory_order_acquire)) return false;
    if (!g.hwnd) return true;
    return DXL::HasPresentationFocus(g.hwnd.load());
}

bool IsSelfCall(const void* caller) noexcept {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(caller), &m)) {
        return false;
    }
    return m == g.selfMod;
}

bool IsActiveForeignCall(const void* caller) noexcept {
    return UiActive() && !IsSelfCall(caller);
}

// The position the game is allowed to "see" while the panel is open. Seeded
// once (from the real cursor) the first time the game asks; then frozen so
// GetCursorPos-based mouse look produces zero deltas while we drive the UI.
POINT ForeignCursorPos() noexcept {
    std::lock_guard<std::mutex> lock(g.anchorMutex);
    if (!g.haveAnchor) {
        g.haveAnchor = true;
        if (g.hwnd) {
            POINT p{};
            GetCursorPos(&p);  // real position; no hook recursion for self
            g.anchor = p;
        }
    }
    return g.anchor;
}

// -- hook trampolines (MinHook original-function pointers) --
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using GetMessagePosFn = DWORD(WINAPI*)();
using ShowCursorFn = int(WINAPI*)(BOOL);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);

SetCursorPosFn o_SetCursorPos = nullptr;
ClipCursorFn o_ClipCursor = nullptr;
GetCursorPosFn o_GetCursorPos = nullptr;
GetMessagePosFn o_GetMessagePos = nullptr;
ShowCursorFn o_ShowCursor = nullptr;
SetCursorFn o_SetCursor = nullptr;

BOOL WINAPI hkSetCursorPos(int x, int y) {
    if (UiActive()) return TRUE;  // swallow the game's re-centring
    return o_SetCursorPos(x, y);
}
BOOL WINAPI hkClipCursor(const RECT* rect) {
    if (UiActive()) return TRUE;  // swallow the game's cursor confinement
    return o_ClipCursor(rect);
}
BOOL WINAPI hkGetCursorPos(LPPOINT pt) {
    if (IsActiveForeignCall(_ReturnAddress())) {
        if (pt) *pt = ForeignCursorPos();
        return TRUE;
    }
    return o_GetCursorPos(pt);
}
DWORD WINAPI hkGetMessagePos() {
    if (IsActiveForeignCall(_ReturnAddress())) {
        const POINT p = ForeignCursorPos();
        return MAKELONG(static_cast<SHORT>(p.x), static_cast<SHORT>(p.y));
    }
    return o_GetMessagePos();
}
int WINAPI hkShowCursor(BOOL show) {
    // Preserve loop termination without changing the thread's display count.
    // A game can legitimately loop until ShowCursor(FALSE) returns < 0.
    if (UiActive()) return show ? 0 : -1;
    return o_ShowCursor(show);
}
HCURSOR WINAPI hkSetCursor(HCURSOR c) {
    if (UiActive()) {
        if (g.virtualCursor.load(std::memory_order_relaxed)) return o_SetCursor(nullptr);
        if (!g.arrow) g.arrow = LoadCursorW(nullptr, IDC_ARROW);
        if (g.arrow) return o_SetCursor(g.arrow);
    }
    return o_SetCursor(c);
}

// Shared MinHook instance: cursor teardown must not disable NGX or graphics hooks.
static void* cursorTargets[6]{};
bool InstallCursorHooks() noexcept {
    if (g.hooksUp) return true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        D5_LOG_WARN(L"ImGui cursor hooks unavailable: MinHook status=%d; raw fallback remains available", int(init));
        return false;
    }

    struct { const char* name; void** orig; void* hook; } entries[] = {
        { "SetCursorPos", reinterpret_cast<void**>(&o_SetCursorPos),
          reinterpret_cast<void*>(&hkSetCursorPos) },
        { "ClipCursor", reinterpret_cast<void**>(&o_ClipCursor),
          reinterpret_cast<void*>(&hkClipCursor) },
        { "GetCursorPos", reinterpret_cast<void**>(&o_GetCursorPos),
          reinterpret_cast<void*>(&hkGetCursorPos) },
        { "GetMessagePos", reinterpret_cast<void**>(&o_GetMessagePos),
          reinterpret_cast<void*>(&hkGetMessagePos) },
        { "ShowCursor", reinterpret_cast<void**>(&o_ShowCursor),
          reinterpret_cast<void*>(&hkShowCursor) },
        { "SetCursor", reinterpret_cast<void**>(&o_SetCursor),
          reinterpret_cast<void*>(&hkSetCursor) },
    };

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    int made = 0;
    if (user32) {
        unsigned entryIndex = 0;
        for (auto& e : entries) {
            void*& ownedTarget = cursorTargets[entryIndex++];
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, e.name));
            if (!target) continue;
            const MH_STATUS created = ownedTarget == target && *e.orig
                ? MH_OK : MH_CreateHook(target, e.hook, e.orig);
            if (created == MH_OK) {
                ownedTarget = target;
                const MH_STATUS enabled = MH_EnableHook(target);
                if (enabled == MH_OK || enabled == MH_ERROR_ENABLED) ++made;
                else {
                    // Keep the trampoline alive: another thread may still be forwarding.
                    D5_LOG_WARN(L"ImGui cursor hook %S enable failed: %d", e.name, int(enabled));
                }
            } else {
                D5_LOG_WARN(L"ImGui cursor hook %S create failed: %d", e.name, int(created));
            }
        }
    }
    if (made == 0) {

        D5_LOG_WARN(L"ImGui cursor hooks unavailable; raw mouse fallback remains available");
        return false;
    }
    // caller module used to distinguish our own user32 calls from the game's
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&hkGetCursorPos), &g.selfMod);
    g.hooksUp = true;
    D5_LOG_INFO(L"ImGui cursor hooks ready: %d/6; adaptive raw mouse fallback enabled", made);
    return true;
}

void RemoveCursorHooks() noexcept {
    if (!g.hooksUp) return;
    g.hooksUp = false;
    // LoadCursor cursors are shared system cursors — never DestroyCursor them.
    g.arrow = nullptr;
    // Disable only hooks owned here. Retain their original call targets across
    // renderer restarts and for calls already inside a hook during shutdown.
    for (void* target : cursorTargets) if (target) MH_DisableHook(target);
}

// Native cursor imagery belongs to the window's input thread. Hide only the
// image while the fallback is active; never perturb ShowCursor's display count.
// Invoked from our WndProc, including focus loss / the final restore message.
void SyncCursorImage(bool virtualActive) noexcept {
    static thread_local HCURSOR previous = nullptr;
    static thread_local bool hidden = false;
    if (!o_SetCursor) return;
    if (virtualActive) {
        const HCURSOR prior = o_SetCursor(nullptr);
        if (!hidden) previous = prior;
        hidden = true;
    } else if (hidden) {
        o_SetCursor(previous);
        previous = nullptr;
        hidden = false;
    }
}

// We observe the game's existing WM_INPUT registration. RegisterRawInputDevices
// is process-wide, so taking ownership here would break another input consumer.
// WndProc queues data only; the render thread remains the sole ImGui owner.
struct RawMouseDelta {
    int64_t x = 0, y = 0;
    bool absolute = false;
};
std::mutex& rawMouseMutex = *new std::mutex;
RawMouseDelta rawMousePending;

void QueueRelativeMouse(const RAWMOUSE& mouse) noexcept {
    std::lock_guard<std::mutex> lock(rawMouseMutex);
    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        rawMousePending.absolute = true;
        return; // Tablet / RDP coordinates must not be integrated as deltas.
    }
    // Defensive saturation keeps prolonged stalls / malformed input bounded.
    constexpr int64_t maxPending = 1 << 20;
    rawMousePending.x = std::clamp(rawMousePending.x + mouse.lLastX, -maxPending, maxPending);
    rawMousePending.y = std::clamp(rawMousePending.y + mouse.lLastY, -maxPending, maxPending);
}

void QueueRawMouse(HRAWINPUT input) noexcept {
    RAWINPUT raw{};
    UINT bytes = sizeof(raw);
    const UINT copied = GetRawInputData(input, RID_INPUT, &raw, &bytes, sizeof(RAWINPUTHEADER));
    if (copied != UINT(-1) && copied >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) &&
        raw.header.dwType == RIM_TYPEMOUSE) QueueRelativeMouse(raw.data.mouse);
}

RawMouseDelta ConsumeRawMouse() noexcept {
    std::lock_guard<std::mutex> lock(rawMouseMutex);
    const RawMouseDelta result = rawMousePending;
    rawMousePending = {};
    return result;
}

// Normal games continue to use the OS pointer (including acceleration). Only
// repeated relative hardware motion with a stationary interior pointer enables
// the fallback. Once enabled, retain it until the panel closes to avoid jumps.
struct CursorFallback {
    bool session = false, active = false, absolute = false;
    POINT lastReal{}, pinnedReal{};
    float x = 0, y = 0;
    unsigned pinnedSamples = 0;
    uint64_t lastMovementMs = 0;

    void Reset() noexcept { *this = {}; }
    POINT Update(POINT real, LONG width, LONG height, RawMouseDelta raw,
                 uint64_t nowMs) noexcept {
        width = (std::max)(width, 1L); height = (std::max)(height, 1L);
        if (!session) {
            session = true;
            lastReal = real;
            x = float(real.x); y = float(real.y);
        }
        if (raw.absolute) {
            absolute = true;
            active = false;
            pinnedSamples = 0;
        }
        const bool moved = raw.x != 0 || raw.y != 0;
        const POINT reference = pinnedSamples ? pinnedReal : lastReal;
        const bool realMoved = std::abs(real.x - reference.x) > 2 || std::abs(real.y - reference.y) > 2;
        const bool atEdge = real.x <= 1 || real.y <= 1 || real.x >= width - 2 || real.y >= height - 2;
        if (!absolute) {
            if (!active && (realMoved || atEdge || (nowMs - lastMovementMs > 250))) {
                x = float(real.x); y = float(real.y);
                pinnedSamples = 0;
            }
            // While not yet pinned, ordinary OS movement remains authoritative.
            if (active || !realMoved) {
                x = std::clamp(x + float(raw.x), 0.0f, float(width - 1));
                y = std::clamp(y + float(raw.y), 0.0f, float(height - 1));
            }
            if (moved) {
                lastMovementMs = nowMs;
                if (!active && !realMoved && !atEdge) {
                    if (!pinnedSamples) pinnedReal = real;
                    if (++pinnedSamples >= 5 &&
                        (std::abs(x - float(pinnedReal.x)) > 4 || std::abs(y - float(pinnedReal.y)) > 4)) active = true;
                }
            }
        }
        lastReal = real;
        return active ? POINT{LONG(x), LONG(y)} : real;
    }
};
CursorFallback cursorFallback;

void ResetCursorSession() noexcept {
    cursorFallback.Reset();
    ConsumeRawMouse();
    if (g.virtualCursor.exchange(false)) {
        if (const HWND hwnd = g.hwnd.load()) PostMessageW(hwnd, WM_NULL, 0, 0);
    }
    std::lock_guard<std::mutex> lock(g.anchorMutex);
    g.haveAnchor = false;
}

// While the panel is open: confine the OS cursor to the game window so it can
// roam freely across the UI but can not leave the window. (Native cursor image
// is handled by the hooks; imgui draws its own cursor while the panel is open,
// so we deliberately do NOT hammer ShowCursor here — counters would grow.)
void MaintainFreeCursor(bool open) noexcept {
    if (!open || !g.hwnd || !o_ClipCursor) return;
    if (!DXL::HasPresentationFocus(g.hwnd.load())) return;

    RECT client{};
    if (GetClientRect(g.hwnd, &client)) {
        POINT tl{ client.left, client.top }, br{ client.right, client.bottom };
        ClientToScreen(g.hwnd, &tl);
        ClientToScreen(g.hwnd, &br);
        RECT scr{ tl.x, tl.y, br.x, br.y };
        o_ClipCursor(&scr);  // via the trampoline so our own clip is not swallowed
    }
}

// GPU lifetime only. FG switches the queue/RTV format without replacing the
// game's HWND. Keep the ImGui context, input chain and cursor hooks intact.
bool ReleaseRenderer() noexcept {
    if (!WaitForGpu()) return false;
    g.ok = false;
    if (g.imgui) {
        ImGui::SetCurrentContext(g.imgui);
        if (ImGui::GetIO().BackendRendererUserData) {
            if (g.device11) ImGui_ImplDX11_Shutdown();
            else ImGui_ImplDX12_Shutdown();
        }
    }
    if (g.fenceEvent) { CloseHandle(g.fenceEvent); g.fenceEvent = nullptr; }
    if (g.list) { g.list->Release(); g.list = nullptr; }
    if (g.allocator) { g.allocator->Release(); g.allocator = nullptr; }
    if (g.fence) { g.fence->Release(); g.fence = nullptr; }
    if (g.srvHeap) { g.srvHeap->Release(); g.srvHeap = nullptr; }
    if (g.rtvHeap) { g.rtvHeap->Release(); g.rtvHeap = nullptr; }
    if (g.queue) { g.queue->Release(); g.queue = nullptr; }
    if (g.device) { g.device->Release(); g.device = nullptr; }
    if (g.state11) { g.state11->Release(); g.state11 = nullptr; }
    if (g.context11) { g.context11->Release(); g.context11 = nullptr; }
    if (g.device11) { g.device11->Release(); g.device11 = nullptr; }
    g.rtvFormat = DXGI_FORMAT_UNKNOWN;
    g.rtvHandle = {};
    g.srvCount = 0;
    g.srvUsed.fill(false);
    g.srvIncrement = 0;
    g.fenceValue = 0;
    g.submittedOnce = false;
    g.syncFailed = false;
    return true;
}

}  // namespace

namespace ReUi {

void SetUiOpen(std::atomic<bool>* uiOpen) noexcept {
    g.uiOpen = uiOpen;
    const bool open = uiOpen && uiOpen->load(std::memory_order_acquire);
    // The caller also polls this on frames with no visible panel/toast. Closing
    // must release the pointer even if FramePresent is skipped entirely.
    if (g.inputSessionOpen && !open) {
        ResetCursorSession();
        if (o_ClipCursor) o_ClipCursor(nullptr);
    }
    g.inputSessionOpen = open;
}

// 加载支持中文的字体（tooltip 里有中英双语文案，imgui 自带的 ProggyClean 没有
// 中文字形，必须用系统字体）。逐个候选尝试，全失败才退回内置默认字体。
ImFont* LoadScalableUiFont(float px) {
    ImGuiIO& io = ImGui::GetIO();
    const char* kCandidates[] = {
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyhl.ttc",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\simfang.ttf",
    };
    for (const char* p : kCandidates) {
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig cfg{};
        cfg.SizePixels = px;
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(p, px, &cfg,
                                 io.Fonts->GetGlyphRangesChineseFull())) {
            return f;
        }
    }
    ImFontConfig cfg{};
    cfg.SizePixels = px;
    return io.Fonts->AddFontDefault(&cfg);
}

static void EnsureContext() {
    if (!g.imgui) {
        g.imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(g.imgui);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        g.uiFont = LoadScalableUiFont(kUiFontPx);
        g.toastFont = LoadScalableUiFont(kToastFontPx);
        ImGui::StyleColorsDark();
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 4.0f;
        st.Alpha = 0.94f;
        st.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.95f, 0.90f, 1.00f);
        st.ScaleAllSizes(2.0f);
    }
    ImGui::SetCurrentContext(g.imgui);

}

bool InitOnce(ID3D12Device* device, ID3D12CommandQueue* queue,
              HWND hwnd, DXGI_FORMAT rtvFormat) {
    if (!device || !queue || !rtvFormat) return false;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    if (g.ok) {
        if (g.device == device && g.queue == queue && g.rtvFormat == rtvFormat) {
            if (hwnd) EnsureInputHook(hwnd);
            return true;
        }
        // Rebinding the renderer must not rebuild the Win32 subclass chain.
        const auto* oldQueue = g.queue;
        const auto oldFormat = g.rtvFormat;
        if (!ReleaseRenderer()) return false;
        D5_LOG_INFO(L"ImGui renderer rebind: queue=%p -> %p format=%u -> %u; input chain preserved",
                    oldQueue, queue, unsigned(oldFormat), unsigned(rtvFormat));
    }

    g.device = device;
    g.queue = queue;
    device->AddRef(); queue->AddRef();
    g.fenceValue = 0; g.submittedOnce = false; g.syncFailed = false;
    struct InitGuard { ~InitGuard() { if (!g.ok) ReleaseRenderer(); } } initGuard;
    g.rtvFormat = rtvFormat;

    D3D12_DESCRIPTOR_HEAP_DESC shDesc{};
    shDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shDesc.NumDescriptors = kSrvCapacity;
    shDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&shDesc, IID_PPV_ARGS(&g.srvHeap))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC rDesc{};
    rDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rDesc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&rDesc, IID_PPV_ARGS(&g.rtvHeap)))) {
        g.srvHeap->Release(); g.srvHeap = nullptr;
        return false;
    }
    g.rtvHandle = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g.srvIncrement =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.srvCount = 0;
    g.srvUsed.fill(false);

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         g.allocator, nullptr,
                                         IID_PPV_ARGS(&g.list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        return false;
    }
    g.list->Close();
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.fenceEvent) return false;

    EnsureContext();

    ImGui_ImplDX12_InitInfo info{};
    info.Device = device;
    info.CommandQueue = queue;
    info.NumFramesInFlight = 2;
    info.RTVFormat = rtvFormat;
    info.SrvDescriptorHeap = g.srvHeap;
    info.SrvDescriptorAllocFn = SrvAlloc;
    info.SrvDescriptorFreeFn = SrvFree;
    if (!ImGui_ImplDX12_Init(&info)) return false;

    // swallow input while the panel is open (guarded subclass — see UiWndProc)
    if (hwnd) EnsureInputHook(hwnd);

    // stop the game from re-centring / confining / hiding the OS cursor while
    // our panel is open (ReShade-style). If MinHook fails we keep going: the
    // per-frame clip/show inside FramePresent still helps for clip-only games.
    InstallCursorHooks();

    g.ok = true;
    return true;
}

bool InitOnce11(ID3D11Device* device, ID3D11DeviceContext* context,
                HWND hwnd, DXGI_FORMAT rtvFormat) {
    if (!device || !context || !rtvFormat ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    if (g.ok) {
        if (g.device11 == device && g.context11 == context &&
            g.rtvFormat == rtvFormat) {
            if (hwnd) EnsureInputHook(hwnd);
            return true;
        }
        if (!ReleaseRenderer()) return false;
    }
    g.device11 = device;
    device->AddRef();
    struct InitGuard { ~InitGuard() { if (!g.ok) ReleaseRenderer(); } } initGuard;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&g.context11)))) return false;
    ID3D11Device1* device1 = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return false;
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel() > D3D_FEATURE_LEVEL_11_1
        ? D3D_FEATURE_LEVEL_11_1 : device->GetFeatureLevel();
    const UINT stateFlags = device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED
        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    const HRESULT stateResult = device1->CreateDeviceContextState(
        stateFlags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1),
        nullptr, &g.state11);
    device1->Release();
    if (FAILED(stateResult)) return false;
    g.rtvFormat = rtvFormat;
    EnsureContext();
    if (!ImGui_ImplDX11_Init(device, context)) return false;
    if (hwnd) EnsureInputHook(hwnd);
    InstallCursorHooks();
    g.ok = true;
    D5_LOG_INFO(L"ImGui D3D11 renderer ready: device=%p format=%u; isolated context state",
                device, unsigned(rtvFormat));
    return true;
}

void Shutdown() noexcept {
    if (!ReleaseRenderer()) return;
    ResetCursorSession();
    g.inputSessionOpen = false;
    // Restore the image on its owning input thread before removing our hook.
    if (const HWND hwnd = g.hwnd.load()) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &ignored);
    }
    if (o_ClipCursor) o_ClipCursor(nullptr);  // release any clip we forced
    RemoveCursorHooks();
    RemoveInputHook();   // only restores when we're still on top of the chain
    if (g.imgui) { ImGui::DestroyContext(g.imgui); g.imgui = nullptr; }
    g.uiFont = g.toastFont = nullptr;
    g.wheelX = 0; g.wheelY = 0;
}

bool WaitIdle() noexcept { return WaitForGpu(); }

static ImDrawData* BuildDrawData(uint32_t w, uint32_t h,
                                   const std::function<void()>& buildUi) {
    // input (polled; no win32 backend needed)
    auto* uiOpen = g.uiOpen.load(std::memory_order_acquire);
    const bool open = uiOpen && uiOpen->load(std::memory_order_acquire);
    const bool fg = !g.hwnd || DXL::HasPresentationFocus(g.hwnd.load());
    MaintainFreeCursor(open);

    // Also cover a toggle that occurred after SetUiOpen earlier in this frame.
    if (g.inputSessionOpen && !open) {
        ResetCursorSession();
        if (o_ClipCursor) o_ClipCursor(nullptr);
    }
    g.inputSessionOpen = open;

    ImGui::SetCurrentContext(g.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    // draw our own cursor only when the OS cursor is not showing (gameplay);
    // in menus the game draws one at the same spot and double cursors look bad
    bool nativeShown = false;
    if (open) {
        CURSORINFO ci{ sizeof(ci) };
        if (GetCursorInfo(&ci)) nativeShown = (ci.flags & CURSOR_SHOWING) != 0;
    }
    if (open && fg && g.hwnd) {
        POINT pt{};
        if (o_GetCursorPos) o_GetCursorPos(&pt); // always read the real pointer
        else GetCursorPos(&pt);
        ScreenToClient(g.hwnd, &pt);
        RECT client{};
        GetClientRect(g.hwnd, &client);
        const POINT cursor = cursorFallback.Update(pt, client.right, client.bottom,
                                                   ConsumeRawMouse(), GetTickCount64());
        if (g.virtualCursor.exchange(cursorFallback.active) != cursorFallback.active) {
            PostMessageW(g.hwnd, WM_NULL, 0, 0);
            D5_LOG_INFO(L"ImGui cursor: %s (relative hardware motion while OS pointer is pinned)",
                        cursorFallback.active ? L"raw fallback active" : L"absolute input restored");
        }
        // A window's client extent can differ from its render resolution.
        io.MousePos = ImVec2(float(cursor.x) * float(w) / float((std::max)(client.right, 1L)),
                             float(cursor.y) * float(h) / float((std::max)(client.bottom, 1L)));
    } else {
        ResetCursorSession();
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    }
    io.MouseDrawCursor = open && fg && (cursorFallback.active || !nativeShown);
    const bool buttonsAlive = open && fg;
    io.MouseDown[0] = buttonsAlive && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    io.MouseDown[1] = buttonsAlive && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    io.MouseDown[2] = buttonsAlive && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    // WndProc only queues deltas; ImGui stays confined to the rendering thread.
    const int wheelX = g.wheelX.exchange(0, std::memory_order_relaxed);
    const int wheelY = g.wheelY.exchange(0, std::memory_order_relaxed);
    if (buttonsAlive && (wheelX || wheelY))
        io.AddMouseWheelEvent(float(wheelX) / WHEEL_DELTA, float(wheelY) / WHEEL_DELTA);

    if (g.device11) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (buildUi) buildUi();
    ImGui::Render();
    return ImGui::GetDrawData();

}

void FramePresent(IDXGISwapChain* swapChain,
                  const std::function<void()>& buildUi) {
    DXL::PresentWriterTracker::IgnoreScope ignoreOwnWriterEvidence;
    if (g.device11) { FramePresent11(swapChain, buildUi); return; }
    if (!g.ok || !swapChain) return;
    // teardown 闸门：游戏在退/设备没了之后，叠加层立刻停止一切 D3D 工作
    // （绘制、命令提交都别再发生）—— 只差这一步就会在退出半路上崩溃
    if (DXL::IsTeardownRequested()) return;
    if (g.hwnd == nullptr) {
        // late hwnd discovery via the swapchain
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(swapChain->GetDesc(&d)) && d.OutputWindow) {
            EnsureInputHook(d.OutputWindow);
        }
    }
    // Attach once per HWND; preserve any overlay that subclasses above us.
    if (g.hwnd) EnsureInputHook(g.hwnd);

    // QueryInterface is required: an FG/proxy chain need not share interface layout.
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    // Wait before NewFrame too: ImGui can update font/vertex resources there.
    if (!WaitForGpu()) return;
    ID3D12Resource* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&backbuffer))))
        return;
    D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    const uint32_t w = (uint32_t)desc.Width;
    const uint32_t h = desc.Height;

    ImDrawData* draw = BuildDrawData(w, h, buildUi);

    // GPU work
    if (!WaitForGpu()) { backbuffer->Release(); return; }
    if (FAILED(g.allocator->Reset()) || FAILED(g.list->Reset(g.allocator, nullptr))) {
        backbuffer->Release(); return;
    }

    g.device->CreateRenderTargetView(backbuffer, nullptr, g.rtvHandle);

    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = backbuffer;
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->ResourceBarrier(1, &b0);

    g.list->OMSetRenderTargets(1, &g.rtvHandle, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g.srvHeap };
    g.list->SetDescriptorHeaps(1, heaps);

    D3D12_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc{ 0, 0, (LONG)w, (LONG)h };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);

    ImGui_ImplDX12_RenderDrawData(draw, g.list);

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = backbuffer;
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->ResourceBarrier(1, &b1);

    if (FAILED(g.list->Close())) { backbuffer->Release(); return; }
    ID3D12CommandList* lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    ++g.fenceValue;
    g.syncFailed = FAILED(g.queue->Signal(g.fence, g.fenceValue));
    g.submittedOnce = true;
    g.pendingBuffer = backbuffer;
    // Leave no overlay work referencing old DXGI buffers when Present returns
    // and the game immediately turns FG on/off or resizes its window.
    if (!WaitForGpu()) D5_LOG_WARN(L"ImGui: waiting for present-queue fence; resources retained");
}

void FramePresent11(IDXGISwapChain* swapChain,
                    const std::function<void()>& buildUi) {
    if (!g.ok || !g.device11 || !g.context11 || !g.state11 || !swapChain ||
        DXL::IsTeardownRequested()) return;
    DXGI_SWAP_CHAIN_DESC chainDesc{};
    if (SUCCEEDED(swapChain->GetDesc(&chainDesc)) && chainDesc.OutputWindow)
        EnsureInputHook(chainDesc.OutputWindow);
    // Flip-model DX11 chains rotate like DX12. Blt-model chains expose only 0.
    UINT index = 0;
    IDXGISwapChain3* chain3 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
        index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    }
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) return;
    FrameTexture11(backbuffer, buildUi);
    backbuffer->Release();
}

void FrameTexture11(ID3D11Texture2D* backbuffer,
                    const std::function<void()>& buildUi) {
    if (!g.ok || !g.device11 || !g.context11 || !g.state11 || !backbuffer ||
        DXL::IsTeardownRequested()) return;
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g.device11->CreateRenderTargetView(backbuffer, nullptr, &rtv))) {
        return;
    }
    ImDrawData* draw = BuildDrawData(desc.Width, desc.Height, buildUi);
    // ImGui's DX11 backend saves common draw state, but it also clears HS/DS/CS
    // without restoring them and does not own OM targets/UAVs or predication.
    // Swap the full runtime context state rather than guess the game's bindings.
    ID3DDeviceContextState* previous = nullptr;
    g.context11->SwapDeviceContextState(g.state11, &previous);
    g.context11->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(draw);
    // Never retain a backbuffer through our saved state: ResizeBuffers and FG
    // replacement must be free to release the chain immediately after Present.
    g.context11->ClearState();
    g.context11->SwapDeviceContextState(previous, nullptr);
    if (previous) previous->Release();
    rtv->Release();
}

ImFont* FontUi() noexcept { return g.uiFont; }
ImFont* FontToast() noexcept { return g.toastFont; }
float ToastFontSize() noexcept { return kToastFontPx; }

}  // namespace ReUi
