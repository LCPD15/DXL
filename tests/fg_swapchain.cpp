#include "../src/core/FrameGenSwapChains.h"
#include "../src/core/NrRoutePolicy.h"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
#include <source_location>
using namespace DXL;
using Microsoft::WRL::ComPtr;
static void Check(bool b, const char* msg) { if (!b) throw std::runtime_error(msg); }
static void HR(HRESULT h, std::source_location at = std::source_location::current()) {
    if (FAILED(h)) { printf("HRESULT %08X at line %u\n", unsigned(h), at.line()); throw std::runtime_error("DXGI/D3D12 call failed"); }
}
int main() try {
    auto& state = FrameGenSwapChains::Get();
    for (int invalid : {-1, 0, 1, 17, 999})
        Check(NormalizeFgBufferThreshold(invalid) == 4, "invalid threshold must retain default protection");
    Check(NormalizeFgBufferThreshold(8) == 8 && NormalizeFgBufferThreshold(16) == 16, "valid overrides");
    state.SetThreshold(NormalizeFgBufferThreshold(4));
    {
        FrameGenSwapChains::Creation six(6);
        Check(state.HasCandidate(), "six buffers trigger default guard");
        state.SetThreshold(NormalizeFgBufferThreshold(8));
        Check(!state.HasCandidate(), "threshold eight excludes a six-buffer candidate");
        { FrameGenSwapChains::Creation eight(8); Check(state.HasCandidate(), "threshold remains inclusive"); }
        Check(!state.HasCandidate(), "high-buffer candidate removal restores override");
        state.SetThreshold(4);
        Check(state.HasCandidate(), "default restores six-buffer guard");
    }
    Check(!state.HasCandidate(), "initial state");
    { FrameGenSwapChains::Creation pending(4); Check(state.HasCandidate(), "FG creation gap"); }
    Check(!state.HasCandidate(), "failed FG creation left guard latched");
    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; HR(device.As(&info));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName=L"NrFgLifetimeTest";
    RegisterClassW(&wc);
    auto window = [&] { HWND h=CreateWindowW(wc.lpszClassName,L"NR test",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,wc.hInstance,nullptr); Check(h!=nullptr,"window"); return h; };
    HWND a=window(), b=window(), c=window();
    auto create = [&](HWND h, UINT buffers) {
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width=d.Height=64; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count=1; d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount=buffers; d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        FrameGenSwapChains::Creation pending(buffers);
        ComPtr<IDXGISwapChain1> chain; HR(factory->CreateSwapChainForHwnd(queue.Get(),h,&d,nullptr,nullptr,&chain));
        Check(state.Observe(chain.Get(), buffers),"cookie registration");
        return chain;
    };
    // Real yysls sequence: SR->NR runs on a six-buffer chain, then SR stops in
    // a menu. Raising the configured heuristic to eight does not establish FG
    // ownership. Reproduce the old admission without submitting invalid work.
    state.SetThreshold(8);
    {
        auto six=create(a,6);
        auto replacement=create(b,3);
        Check(!state.HasCandidate(),"test requires the configured-threshold blind spot");
        UINT buffers=0;
        const bool uncertain=state.AutomaticHandoffUncertain(six.Get(),&buffers);
        Check(uncertain&&buffers==6,"six-buffer uncertainty must survive threshold override");
        NrRoutePolicy startup, route;
        Check(startup.UsePresent(1000,false,uncertain),"FG preload alone disabled standalone startup Present");
        route.Native(1000,1);route.Applied(1000);
        Check(route.UsePresent(2000,false),"negative control did not reproduce old menu admission");
        Check(!route.UsePresent(2000,false,uncertain),"menu admitted Present on uncertain six-buffer chain");
        Check(route.UsePresent(2000,false,false),"absence of FG module blocked normal menu fallback");
        Check(!state.AutomaticHandoffUncertain(replacement.Get()),"replacement three-buffer chain inherited old uncertainty");
        Check(route.UsePresent(2000,false,state.AutomaticHandoffUncertain(replacement.Get())),
            "current low-buffer replacement must recover even while old chain remains alive");
        HR(six->ResizeBuffers(3,80,80,DXGI_FORMAT_UNKNOWN,0));state.Observe(six.Get(),3,true);
        Check(route.UsePresent(2000,false,state.AutomaticHandoffUncertain(six.Get())),"successful low-buffer resize did not recover");
        HR(six->ResizeBuffers(6,80,80,DXGI_FORMAT_UNKNOWN,0));state.Observe(six.Get(),6,true);
        Check(!route.UsePresent(2000,false,state.AutomaticHandoffUncertain(six.Get())),"FG-capable resize lost automatic handoff protection");
        route.Native(3000,1);
        Check(route.UseEvaluate(3000,false)&&!route.UsePresent(3000,false,uncertain),"native SR recovery stayed paused");
        // Old private cookies own no game object: destruction allows immediate
        // recreation at the same HWND and cannot leave a permanent menu latch.
        six.Reset();auto ordinary=create(a,2);
        Check(route.UsePresent(4000,false,state.AutomaticHandoffUncertain(ordinary.Get())),"destroy/recreate did not restore automatic Present");
    }
    state.SetThreshold(4);
    auto four=create(a,4);
    auto three=create(b,3); // exact player sequence: FG chain then another native chain
    Check(state.HasCandidate() && state.CandidateCount()==1,"3-buffer creation cleared live FG chain");
    { FrameGenSwapChains::Creation failed(3); Check(state.HasCandidate(),"failed small creation cleared FG"); }
    ComPtr<IDXGISwapChain3> alias; HR(four.As(&alias));
    Check(state.Observe(alias.Get(),4) && state.CandidateCount()==1,"interface alias double-counted chain");
    state.Observe(alias.Get(),3);
    Check(state.CandidateCount()==1,"smaller inner-chain observation erased outer FG count");
    ComPtr<ID3D12Resource> heldBuffer; HR(four->GetBuffer(0,IID_PPV_ARGS(&heldBuffer)));
    {
        FrameGenSwapChains::Creation pending(3);
        const HRESULT failed = four->ResizeBuffers(3,80,80,DXGI_FORMAT_UNKNOWN,0);
        Check(FAILED(failed),"resize with held backbuffer should fail");
        if (SUCCEEDED(failed)) state.Observe(four.Get(),3,true);
    }
    heldBuffer.Reset();
    Check(state.CandidateCount()==1,"failed resize cleared live FG chain");
    auto second=create(c,4);
    // Private cookies must survive buffer recreation; the registry changes only
    // after successful API calls. Resize count 0 preserves the previous count.
    HR(four->ResizeBuffers(0,80,80,DXGI_FORMAT_UNKNOWN,0));
    state.Observe(four.Get(),0,true); Check(state.CandidateCount()==2,"ResizeBuffers(0) dropped FG");
    const UINT masks[]{1,1,1};
    IUnknown* queues[]{queue.Get(),queue.Get(),queue.Get()};
    HR(alias->ResizeBuffers1(3,80,80,DXGI_FORMAT_UNKNOWN,0,masks,queues));
    state.Observe(alias.Get(),3,true); Check(state.CandidateCount()==1,"resize cleared another FG chain");
    second.Reset(); Check(!state.HasCandidate(),"destroyed FG chain did not restore Present fallback");
    // No hidden AddRef on the chain: last reference destruction releases the
    // cookie. Recreate on the SAME HWND, which fails if a chain was leaked.
    alias.Reset(); four.Reset();
    for (int i=0;i<48;++i) {
        auto fg=create(a,4); Check(state.CandidateCount()==1,"recreated FG chain not registered");
        std::thread observer([&] { for(int j=0;j<100;++j) Check(state.Contains(fg.Get()),"cookie lost"); });
        for(int j=0;j<100;++j) state.Observe(fg.Get(),4);
        observer.join(); fg.Reset(); Check(!state.HasCandidate(),"FG release leaked/latch stuck");
    }
    three.Reset();
    DestroyWindow(a); DestroyWindow(b); DestroyWindow(c);
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T n=0; info->GetMessage(i,nullptr,&n); std::vector<unsigned char> data(n);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data()); info->GetMessage(i,m,&n);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { puts(m->pDescription); throw std::runtime_error("D3D12 API error"); }
    }
    puts("PASS FG DXGI lifecycle: actual 6-buffer threshold8 menu blind spot, protected automatic handoff, SR recovery, current-chain replacement/resize/destruction recovery, standalone Present; 4 then3 buffers, aliases, 48 recreations, concurrent observation; API errors=0");
    return 0;
} catch(const std::exception& e) { printf("FAIL %s\n",e.what()); return 1; }
