#pragma once
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace DXL {
// A launch owns all same-name replacement processes until its result is applied.
// Watch notifications are rechecked on the UI thread before touching a process.
class InjectionCoordination {
    struct WatchState {
        std::wstring name;
        bool queued = false, handled = false;
        uint64_t visibleSince = 0, loaderChangedAt = 0;
        uint32_t moduleCount = 0;
    };
    std::mutex mutex_;
    std::set<std::wstring> launches_;
    std::unordered_map<uint32_t, WatchState> watches_;
    static std::wstring Key(std::wstring name) {
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
        return name;
    }
public:
    static constexpr uint64_t WindowSettleMs = 2000;
    static constexpr uint64_t LoaderQuietMs = 700;
    bool BeginLaunch(const std::wstring& name) {
        std::lock_guard lock(mutex_);
        return launches_.insert(Key(name)).second;
    }
    void EndLaunch(const std::wstring& name) {
        std::lock_guard lock(mutex_); launches_.erase(Key(name));
    }
    void FinishLaunch(const std::wstring& name, uint32_t connectedPid) {
        std::lock_guard lock(mutex_);
        const auto key = Key(name);
        // The launch already dealt with these processes, including startup
        // shells. Reconnecting after its result would lose launch ownership.
        for (auto& [pid, state] : watches_) if (state.name == key) {
            state.handled = true; state.queued = false;
        }
        if (connectedPid) watches_[connectedPid] = {key, false, true, 0};
        launches_.erase(key);
    }
    bool NeedsWatch(uint32_t pid, const std::wstring& name) {
        std::lock_guard lock(mutex_);
        auto& state = watches_[pid];
        state.name = Key(name);
        return !state.queued && !state.handled && !launches_.count(state.name);
    }
    // Module stability is a startup heuristic, not a guarantee that game code
    // is idle. Explicit Early mode still races an externally started process.
    bool ObserveLoader(uint32_t pid, uint32_t count, uint64_t now) {
        std::lock_guard lock(mutex_);
        auto it = watches_.find(pid);
        if (it == watches_.end()) return false;
        auto& state = it->second;
        if (!count || count != state.moduleCount) {
            state.moduleCount = count; state.loaderChangedAt = now;
            return false;
        }
        return now - state.loaderChangedAt >= LoaderQuietMs;
    }
    bool QueueWatch(uint32_t pid, const std::wstring& name, bool late, bool visible, uint64_t now) {
        std::lock_guard lock(mutex_);
        auto& state = watches_[pid];
        state.name = Key(name);
        if (launches_.count(Key(name))) { state.visibleSince = 0; return false; }
        if (state.queued || state.handled) return false;
        if (late) {
            if (!visible) { state.visibleSince = 0; return false; }
            if (!state.visibleSince) state.visibleSince = now;
            if (now - state.visibleSince < WindowSettleMs) return false;
        }
        state.queued = true;
        return true;
    }
    bool BeginWatch(uint32_t pid, const std::wstring& name, bool late, bool visible, uint64_t now) {
        std::lock_guard lock(mutex_);
        auto it = watches_.find(pid);
        if (it == watches_.end() || !it->second.queued) return false;
        auto& state = it->second;
        state.queued = false;
        if (state.handled || launches_.count(Key(name))) { state.visibleSince = 0; return false; }
        if (late && (!visible || !state.visibleSince || now - state.visibleSince < WindowSettleMs)) {
            state.visibleSince = 0; return false;
        }
        state.handled = true;
        return true;
    }
    void CancelWatch(uint32_t pid) {
        std::lock_guard lock(mutex_);
        auto it = watches_.find(pid); if (it != watches_.end()) it->second.queued = false;
    }
    void Prune(const std::vector<uint32_t>& alive) {
        std::lock_guard lock(mutex_);
        for (auto it = watches_.begin(); it != watches_.end();) {
            if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) it = watches_.erase(it);
            else ++it;
        }
    }
};
}
