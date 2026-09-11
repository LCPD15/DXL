#pragma once
#include <cstdint>

namespace DXL {
// Timing uses confirmed native SR calls, never generic NGX/FG parameter keys.
// The owner serializes this policy. Empty is a sampled-output heuristic, not a
// declaration that the entire texture is black or that a menu has been detected.
class NrRoutePolicy {
public:
    static constexpr uint64_t NATIVE_TIMEOUT_MS = 350;
    static constexpr uint64_t EMPTY_HOLD_MS = 600;
    static constexpr uint64_t SAMPLE_TIMEOUT_MS = 350;
    static constexpr uint64_t PRESENT_HANDOFF_MS = 200;
    void Native(uint64_t now, uintptr_t feature) {
        if (!_lastNative || now - _lastNative >= NATIVE_TIMEOUT_MS || feature != _feature) {
            ++_epoch; _emptySince = 0; _emptySamples = 0; _lastSample = 0;
        }
        _lastNative = now; _feature = feature;
    }
    void Sample(uint64_t at, uint64_t epoch, bool empty) {
        if (epoch != _epoch || at < _lastSample) return;
        _lastSample = at;
        if (!empty) { _emptySince = 0; _emptySamples = 0; return; }
        if (!_emptySamples) _emptySince = at;
        ++_emptySamples;
    }
    void Applied(uint64_t now) { _lastApplied = now; }
    bool NativeRecent(uint64_t now) const {
        return _lastNative && now - _lastNative < NATIVE_TIMEOUT_MS;
    }
    bool Dormant(uint64_t now) const {
        return _emptySamples >= 4 && _lastSample && now - _lastSample < SAMPLE_TIMEOUT_MS &&
            _lastSample - _emptySince >= EMPTY_HOLD_MS;
    }
    bool UseEvaluate(uint64_t now, bool fg) const { return fg || !Dormant(now); }
    bool AutomaticHandoffBlocked(bool uncertainFgChain) const {
        return _lastNative != 0 && uncertainFgChain;
    }
    bool UsePresent(uint64_t now, bool fg, bool uncertainFgChain = false) const {
        return !fg && !AutomaticHandoffBlocked(uncertainFgChain) &&
            (!NativeRecent(now) || Dormant(now)) &&
            (!_lastApplied || now - _lastApplied >= PRESENT_HANDOFF_MS);
    }
    uint64_t Epoch() const { return _epoch; }
private:
    uintptr_t _feature = 0;
    uint64_t _epoch = 0, _lastNative = 0, _lastApplied = 0;
    uint64_t _lastSample = 0, _emptySince = 0, _emptySamples = 0;
};
}
