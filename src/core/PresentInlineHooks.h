#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>
#include "MinHook.h"

namespace DXL {
// Keep the interface's method address stable for overlays that rediscover it
// when a swapchain is replaced. Each implementation has its own trampoline;
// forwarding must never jump back through a subsequently patched entry point.
template<class Function, size_t Capacity = 32> class PresentInlineHooks;

template<size_t Capacity, class Result, class... Args>
class PresentInlineHooks<Result (STDMETHODCALLTYPE*)(Args...), Capacity> {
public:
    using Function = Result (STDMETHODCALLTYPE*)(Args...);
    using Handler = Result (*)(Function, Args...);
private:
    struct Binding {
        void* target = nullptr;
        std::atomic<Function> original{nullptr};
        std::atomic<Handler> handler{nullptr};
    };
    struct State { std::mutex mutex; std::array<Binding, Capacity> bindings; size_t used = 0; };
    // Foreign overlays can retain a trampoline through process teardown.
    // Core's teardown gate makes handlers forward only; never free these.
    static State& Get() { static auto* state = new State; return *state; }
    template<size_t Slot> static Result STDMETHODCALLTYPE Dispatch(Args... args) {
        auto& binding = Get().bindings[Slot];
        return binding.handler.load(std::memory_order_acquire)(
            binding.original.load(std::memory_order_acquire), args...);
    }
    template<size_t... Slots> static constexpr auto Detours(std::index_sequence<Slots...>) {
        return std::array<Function, Capacity>{&Dispatch<Slots>...};
    }
public:
    static Function Install(void* target, Handler handler, const char** failure = nullptr) noexcept {
        if (failure) *failure = nullptr;
        const auto failed = [failure](const char* reason) -> Function {
            if (failure) *failure = reason; return nullptr;
        };
        if (!target || !handler) return failed("invalid target or handler");
        auto& state = Get();
        std::lock_guard<std::mutex> lock(state.mutex);
        for (size_t i = 0; i < state.used; ++i)
            if (state.bindings[i].target == target)
                return state.bindings[i].original.load(std::memory_order_acquire);
        if (state.used == Capacity) return failed("target capacity exhausted");
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
            return failed(MH_StatusToString(initialized));
        static constexpr auto detours = Detours(std::make_index_sequence<Capacity>{});
        Function original = nullptr;
        const auto created = MH_CreateHook(target, reinterpret_cast<void*>(detours[state.used]),
                reinterpret_cast<void**>(&original));
        if (created != MH_OK) return failed(MH_StatusToString(created));
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(detours[state.used]), &self)) {
            MH_RemoveHook(target); return failed("core module could not be pinned");
        }
        auto& binding = state.bindings[state.used];
        binding.target = target;
        binding.original.store(original, std::memory_order_release);
        binding.handler.store(handler, std::memory_order_release);
        const auto enabled = MH_EnableHook(target);
        if (enabled != MH_OK) {
            MH_RemoveHook(target); binding.target = nullptr;
            return failed(MH_StatusToString(enabled));
        }
        ++state.used;
        return original;
    }
};
} // namespace DXL
