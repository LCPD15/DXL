#pragma once
#include <d3d12.h>
#include <memory>

namespace DXL {
// The owner serializes calls, and drains all consumers of the previous output
// before Reset. Input and borrowed outputs are in COMMON. Provide multiplier-1
// output slots in playback order; only consume them on success.
class DlssFgBackend {
public:
    DlssFgBackend();
    ~DlssFgBackend();
    bool Generate(ID3D12CommandQueue* queue, ID3D12Resource* color, HMODULE self,
                  ID3D12Resource** generated, bool hdr10 = false, unsigned multiplier = 2);
    unsigned MaxMultiplier() const noexcept;
    const char* Error() const noexcept;
    void Reset() noexcept;
    void ResetHistory() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> session_;
};
}
