#pragma once
#include <cstdint>

namespace DXL {
// A physical native Present can forward to Present1/Present on the same COM
// object. Only an ancestor operation suppresses that forwarding. After it
// returns, a sibling physical Present is a new output even in one FG wrapper.
class FinalPresentScope {
public:
    FinalPresentScope() noexcept : _parent(_current) { _current = this; }
    ~FinalPresentScope() { _current = _parent; }
    bool Outer() const noexcept { return _parent == nullptr; }
    bool EnterNative(uintptr_t identity) noexcept {
        if (!identity) return false;
        for (auto* p = _parent; p; p = p->_parent) if (p->_nativeIdentity == identity) return false;
        _nativeIdentity = identity;
        return true;
    }
    bool AdvanceInputOnce() noexcept {
        auto& root = Root();
        if (root._inputAdvanced) return false;
        root._inputAdvanced = true;
        return true;
    }
    void SetCloseOnEscape(bool value) noexcept { Root()._closeOnEscape = value; }
    bool CloseOnEscape() noexcept { return Root()._closeOnEscape; }
    void MarkUpstreamUi() noexcept { Root()._upstreamUi = true; }
    bool UpstreamUiDrawn() noexcept { return Root()._upstreamUi; }
    FinalPresentScope(const FinalPresentScope&) = delete;
    FinalPresentScope& operator=(const FinalPresentScope&) = delete;
private:
    FinalPresentScope& Root() noexcept { auto* p = this; while (p->_parent) p = p->_parent; return *p; }
    inline static thread_local FinalPresentScope* _current = nullptr;
    FinalPresentScope* _parent = nullptr;
    uintptr_t _nativeIdentity = 0;
    bool _inputAdvanced = false, _closeOnEscape = false, _upstreamUi = false;
};

// Discovery never permanently removes the only controls that can disable an
// unsupported effect. A recovered native callback skips a tree whose fallback
// overlay was already drawn upstream, then uses final-before-UI next time.
class FinalPresentationPolicy {
public:
    void Begin(bool enabled, uint64_t now) noexcept {
        if (!enabled) { _enabledAt = 0; return; }
        if (!_enabledAt) _enabledAt = now;
    }
    void NativeReady(uint64_t now) noexcept { _nativeAt = now; }
    bool ProxyUiFallback(uint64_t now) const noexcept {
        return _enabledAt && now - _enabledAt >= 1000 && (!_nativeAt || now - _nativeAt >= 1000);
    }
private:
    uint64_t _enabledAt = 0, _nativeAt = 0;
};
}
