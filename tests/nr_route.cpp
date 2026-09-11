#include "NrRouteProbe.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <stdexcept>
#include <cstdio>
#include <vector>
#include <string>
using namespace DXL;
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT hr) { Check(SUCCEEDED(hr), "D3D12 call failed"); }
int main(int argc, char** argv) try {
    const bool typeless = argc > 1 && std::string(argv[1]) == "--typeless-color";
    NrRoutePolicy policy;
    Check(policy.UsePresent(1000, false), "startup fallback");
    policy.Native(1000, 1); policy.Applied(1000);
    Check(!policy.UsePresent(1100, false), "double processing after Evaluate");
    Check(policy.UsePresent(1350, false), "DLSS-off timeout");
    Check(!policy.UsePresent(2000, true) && policy.UseEvaluate(2000, true), "FG guard");
    Check(!policy.UsePresent(2000,false,true),"uncertain FG chain admitted automatic menu handoff");
    Check(policy.UseEvaluate(2000,false),"uncertain handoff disabled valid Evaluate");
    for (uint64_t t=2000; t<=2700; t+=100) { policy.Native(t, 1); policy.Sample(t,policy.Epoch(),true); }
    Check(policy.Dormant(2700) && policy.UsePresent(2700,false), "empty native SR/menu fallback");
    Check(!policy.UsePresent(2700,false,true),"empty SR samples proved FG was disabled");
    Check(!policy.UseEvaluate(2700,false), "both routes selected for empty SR");
    policy.Sample(2750,policy.Epoch(),false);
    Check(policy.UseEvaluate(2750,false) && !policy.UsePresent(2750,false), "visible SR recovery");
    const auto oldEpoch=policy.Epoch(); policy.Native(4000,2);
    for(uint64_t t=4000;t<=4800;t+=100) policy.Sample(t,oldEpoch,true);
    Check(!policy.Dormant(4800), "stale readbacks from old SR session");

    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; HR(device.As(&info));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> alloc; HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> list; HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list))); list->Close();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=td.Height=64;
    td.DepthOrArraySize=td.MipLevels=1; td.Format=typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R11G11B10_FLOAT; td.SampleDesc.Count=1;
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> color; HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&color)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> heap; HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = typeless ? DXGI_FORMAT_R8G8B8A8_UNORM : td.Format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(color.Get(),&rtv,heap->GetCPUDescriptorHandleForHeapStart());
    ComPtr<ID3D12Fence> fence; HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); UINT64 value=0;
    auto flush=[&] { HR(queue->Signal(fence.Get(),++value)); HR(fence->SetEventOnCompletion(value,event)); Check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU timeout"); };
    auto& probe=NrRouteProbe::Get(); probe.Enable(true); probe.SetSubmissionHookReady(true);
    auto record=[&](UINT64 at,float red) {
        HR(alloc->Reset()); HR(list->Reset(alloc.Get(),nullptr)); probe.Reset(list.Get());
        D3D12_RESOURCE_BARRIER b{}; b.Transition={color.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET};
        list->ResourceBarrier(1,&b); float clear[4]{red,0,0,1}; list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),clear,0,nullptr);
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; list->ResourceBarrier(1,&b);
        probe.Observe(list.Get(),color.Get(),reinterpret_cast<void*>(1),at,true);
        HR(list->Close());
    };
    // An unsubmitted copy must never be mapped or counted as black content.
    record(10000,0);
    Check(probe.Snapshot(10000,false).samples==0,"read unsubmitted probe");
    probe.Reset(list.Get()); // discard this recording
    Check(probe.Snapshot(10000,false).samples==0,"discarded probe counted");
    for(UINT64 t=10100;t<=11000;t+=100) {
        record(t,0); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
        probe.Submitted(queue.Get(),1,lists); flush();
    }
    auto status=probe.Snapshot(11000,false);
    Check(status.samples==10 && status.dormant && status.present,"GPU black samples did not select fallback");
    record(11100,typeless ? 1.0f/255.0f : .0001f); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
    probe.Submitted(queue.Get(),1,lists); flush();
    status=probe.Snapshot(11100,false);
    Check(!status.dormant && !status.present && probe.UseEvaluate(11100,false),"near-black SR was misclassified");
    status=probe.Snapshot(12000,false,true);
    Check(!status.present&&status.handoffBlocked,"real native probe timeout bypassed uncertain-chain guard");
    probe.Enable(false);probe.Enable(true);
    Check(!probe.Snapshot(12000,false,true).present,"settings reload bypassed menu handoff protection");
    record(12100,.25f);queue->ExecuteCommandLists(1,lists);probe.Submitted(queue.Get(),1,lists);flush();
    status=probe.Snapshot(12100,false,true);
    Check(!status.handoffBlocked&&!status.present&&probe.UseEvaluate(12100,false),"SR recovery stayed latched after disable/re-enable");
    Check(probe.Snapshot(13000,false,false).present,"safe replacement could not restore automatic Present");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T n=0; info->GetMessage(i,nullptr,&n); std::vector<unsigned char> bytes(n);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); info->GetMessage(i,m,&n);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { puts(m->pDescription); throw std::runtime_error("probe validation error"); }
    }
    CloseHandle(event);
    printf("Color resource format=%u\n", unsigned(td.Format));
    puts("PASS automatic route: startup, DLSS-off, empty SR/menu, valid SR recovery, FG uncertainty guard, disable/re-enable, stale epoch rejection; actual GPU probe 12 samples, no pre-fence read, Reset discard, near-black stays Evaluate; API errors=0");
    return 0;
} catch(const std::exception& e) { printf("FAIL: %s\n",e.what()); return 1; }
