#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <optional>
#include <future>
#include <vector>
#include "CommandListTracker.h"
#include "ComputePasses.h"
#include "EvaluateGpuGate.h"
using Microsoft::WRL::ComPtr;
using namespace DXL;
using SetHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
static SetHeapsFn originalSetHeaps = nullptr;
static UINT lastHeapCallCount = UINT_MAX;
void STDMETHODCALLTYPE SpySetHeaps(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps) {
    lastHeapCallCount = count;
    originalSetHeaps(list, count, heaps);
}
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void HR(HRESULT hr) { Check(SUCCEEDED(hr), "D3D12 operation failed"); }
void* Patch(void* object, size_t index, void* replacement) noexcept {
    auto** v = *reinterpret_cast<void***>(object); DWORD old;
    if (!VirtualProtect(v + index, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* original = v[index]; v[index] = replacement;
    VirtualProtect(v + index, sizeof(void*), old, &old); return original;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* d, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
    ComPtr<ID3D12Resource> r; HR(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &desc, state, nullptr, IID_PPV_ARGS(&r))); return r;
}
void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after; l->ResourceBarrier(1, &b);
}
int main(int argc, char** argv) try {
    const bool omitRestore = argc > 1 && !strcmp(argv[1], "--omit-restore");
    ComPtr<ID3D12Debug> debug;
    const bool hasDebug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (hasDebug) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp; HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device; HR(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{}; HR(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> alloc; HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> list;
    HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)));
    Check(CommandListTracker::InstallRootHooks(list.Get(), &Patch), "root hooks");
    auto& gate = EvaluateGpuGate::Get(); Check(gate.Initialize(device.Get()), "gate init");
    gate.SetSubmissionHookReady(true);
    Check(gate.Begin(list.Get()), "first admission");
    Check(!gate.Begin(list.Get()), "unsubmitted work was reused");
    HR(list->Close()); HR(list->Reset(alloc.Get(), nullptr));
    Check(gate.Begin(list.Get()), "discarded list did not release admission");

    // Exercise sparse constants, a descriptor table, CBV, SRV and UAV roots.
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 3};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[2].Descriptor.ShaderRegister = 1;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[4].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 5; rd.pParameters = params;
    ComPtr<ID3DBlob> serialized, errors, shader;
    HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ComPtr<ID3D12RootSignature> root;
    HR(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char* hlsl = "RWByteAddressBuffer dst:register(u0); RWByteAddressBuffer dst2:register(u1);"
        "ByteAddressBuffer src:register(t0); cbuffer A:register(b0){uint a;uint unused;uint b;}"
        "cbuffer B:register(b1){uint c;} [numthreads(1,1,1)] void main(){"
        "uint v=a+b+c+src.Load(4);dst.Store(0,v);dst2.Store(0,v+1);}";
    HR(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get();
    pd.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso; HR(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    auto upload = Buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped; HR(upload->Map(0, nullptr, &mapped));
    static_cast<UINT*>(mapped)[0] = 7; static_cast<UINT*>(mapped)[1] = 11; upload->Unmap(0, nullptr);
    auto output = Buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto output2 = Buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = Buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap; HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 64; ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &ud, heap->GetCPUDescriptorHandleForHeapStart());
    auto& tracker = CommandListTracker::Get(); ID3D12DescriptorHeap* heaps[]{heap.Get()};
    list->SetDescriptorHeaps(1, heaps); tracker.NoteDescriptorHeaps(list.Get(), 1, heaps);
    list->SetComputeRootSignature(root.Get()); tracker.NoteComputeRootSignature(list.Get(), root.Get());
    list->SetPipelineState(pso.Get()); tracker.NotePipelineState(list.Get(), pso.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstant(1, 5, 0); UINT b = 9;
    list->SetComputeRoot32BitConstants(1, 1, &b, 2);
    list->SetComputeRootConstantBufferView(2, upload->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, upload->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, output2->GetGPUVirtualAddress());
    const auto saved = tracker.Snapshot(list.Get());
    std::optional<CommandListStateScope> gameEnvelope;
    gameEnvelope.emplace(list.Get(), !omitRestore);
    // Simulate SR leaving temporary state. It must not replace the game record.
    if (!omitRestore) {
        tracker.NoteComputeRootSignature(list.Get(), nullptr);
        tracker.NoteDescriptorHeaps(list.Get(), 0, nullptr);
        Check(tracker.Snapshot(list.Get()).computeRootSignature == saved.computeRootSignature,
            "SR polluted the game root record");
        Check(tracker.Snapshot(list.Get()).heapCount == saved.heapCount,
            "SR polluted the game heap record");
    }
    // Run the production encode pass, which replaces heaps, PSO, root signature,
    // descriptor tables and constants, before the game's follow-up dispatch.
    ComputePasses passes; Check(passes.Initialize(device.Get()), "compute passes init");
    D3D12_HEAP_PROPERTIES th{}; th.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 8; td.Height = 8; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> texIn, texOut;
    HR(device->CreateCommittedResource(&th, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texIn)));
    HR(device->CreateCommittedResource(&th, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texOut)));
    Check(passes.RecordEncode(list.Get(), texIn.Get(), texOut.Get(), 8, 8, 4, 2.2f), "production encode");
    // NR changes root state. Rebinding the old signature alone cannot restore it.
    if (omitRestore) {
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(pso.Get());
    // Reproduce the old incomplete restoration, while keeping the negative
    // control valid for the GPU: only the value of one root is deliberately wrong.
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1, 1, &b, 2);
    list->SetComputeRootConstantBufferView(2, upload->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3, upload->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, output2->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstant(1, 1000, 0);
    }
    if (!omitRestore) CommandListTracker::Restore(list.Get(), saved);
    gameEnvelope.reset();
    // The next operation in the user's DRED was ExecuteIndirect. Verify that
    // a real indirect dispatch still sees every restored root after the envelope.
    D3D12_INDIRECT_ARGUMENT_DESC argument{}; argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC command{}; command.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS);
    command.NumArgumentDescs=1; command.pArgumentDescs=&argument;
    ComPtr<ID3D12CommandSignature> signature;
    HR(device->CreateCommandSignature(&command,nullptr,IID_PPV_ARGS(&signature)));
    auto indirect=Buffer(device.Get(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(indirect->Map(0,nullptr,&mapped)); *static_cast<D3D12_DISPATCH_ARGUMENTS*>(mapped)={1,1,1}; indirect->Unmap(0,nullptr);
    list->ExecuteIndirect(signature.Get(),1,indirect.Get(),0,nullptr,0);
    Barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list.Get(), output2.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 4);
    list->CopyBufferRegion(readback.Get(), 4, output2.Get(), 0, 4);
    HR(list->Close());
    ComPtr<ID3D12Fence> blocker, done;
    HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)));
    HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
    HR(queue->Wait(blocker.Get(), 1));
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    gate.Submitted(queue.Get(), 1, lists);
    Check(!gate.Begin(list.Get()), "GPU timeout allowed unsafe reuse");
    Check(gate.Snapshot().timeouts == 1, "GPU timeout was not counted");
    gate.Reset(list.Get()); // Must NOT release a submitted, GPU-pending list.
    auto releaseGpu = std::async(std::launch::async, [&] {
        Sleep(20); return blocker->Signal(1);
    });
    Check(gate.Begin(list.Get()), "submitted GPU work was skipped instead of awaited");
    HR(releaseGpu.get());
    const auto gateStats = gate.Snapshot();
    Check(gateStats.waits >= 2 && gateStats.timeouts == 1 && gateStats.unsubmitted == 1,
        "GPU wait/retry or unsubmitted protection regressed");
    HR(queue->Signal(done.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HR(done->SetEventOnCompletion(1, event));
    Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU timeout"); CloseHandle(event);
    HR(readback->Map(0, nullptr, &mapped)); UINT a = static_cast<UINT*>(mapped)[0], c = static_cast<UINT*>(mapped)[1];
    readback->Unmap(0, nullptr);
    Check(a == 32 && c == 33, "restored dispatch produced wrong values");
    // TLOU1 exceeds the former 64-list table before its first SR invocation.
    // Keep all lists alive to force distinct identities; verify old and new
    // snapshots survive overflow growth without evicting another recording.
    std::vector<ComPtr<ID3D12GraphicsCommandList>> manyLists;
    for (UINT i = 0; i < 256; ++i) {
        ComPtr<ID3D12GraphicsCommandList> extra;
        HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&extra)));
        extra->SetDescriptorHeaps(1, heaps); tracker.NoteDescriptorHeaps(extra.Get(), 1, heaps);
        extra->SetComputeRootSignature(root.Get()); tracker.NoteComputeRootSignature(extra.Get(), root.Get());
        extra->SetComputeRoot32BitConstant(1, i, 0);
        Check(tracker.Snapshot(extra.Get()).compute[1].constants[0] == i, "overflow root argument missing");
        { CommandListStateScope scope(extra.Get()); Check(scope.Ready(), "over-64 list rejected"); }
        HR(extra->Close()); manyLists.push_back(extra);
    }
    for (UINT i = 0; i < manyLists.size(); ++i) {
        const auto saved = tracker.Snapshot(manyLists[i].Get());
        Check(saved.heapCount == 1 && saved.computeRootSignature == root.Get() &&
            saved.compute[1].constants[0] == i, "growth evicted or mixed list state");
    }
    auto* last = manyLists.back().Get();
    HR(last->Reset(alloc.Get(), nullptr));
    Check(!tracker.Snapshot(last).HasAnything(), "overflow Reset retained bindings");
    { CommandListStateScope scope(last); Check(scope.Ready(), "observed Reset-empty overflow state rejected"); }
    HR(last->Close());
    printf("PASS tracker overflow: 256 distinct D3D12 lists, preserved roots/constants, Reset and empty-state guard; tracked=%u\n", tracker.TrackedCount());
    // A heap-only list is eligible only when its Reset was actually observed.
    ComPtr<ID3D12GraphicsCommandList> emptyList;
    HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&emptyList)));
    emptyList->SetDescriptorHeaps(1, heaps); tracker.NoteDescriptorHeaps(emptyList.Get(), 1, heaps);
    { CommandListStateScope scope(emptyList.Get()); Check(!scope.Ready(), "unknown empty state admitted"); }
    HR(emptyList->Close()); HR(emptyList->Reset(alloc.Get(), nullptr));
    emptyList->SetDescriptorHeaps(1, heaps); tracker.NoteDescriptorHeaps(emptyList.Get(), 1, heaps);
    {
        CommandListStateScope scope(emptyList.Get());
        Check(scope.Ready() && scope.SavedState().resetObserved && !scope.SavedState().computeRootSignature,
            "observed empty state rejected");
        Check(passes.RecordEncode(emptyList.Get(), texIn.Get(), texOut.Get(), 8, 8, 4, 2.2f), "empty-state encode");
    }
    Check(!tracker.Snapshot(emptyList.Get()).computeRootSignature, "NR contaminated empty game snapshot");
    // The game supplies its own compute state when it first dispatches.
    CommandListTracker::Restore(emptyList.Get(), saved);
    Barrier(emptyList.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(emptyList.Get(), output2.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    emptyList->Dispatch(1, 1, 1);
    Barrier(emptyList.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    emptyList->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 4);
    HR(emptyList->Close()); ID3D12CommandList* emptySubmit[]{emptyList.Get()}; queue->ExecuteCommandLists(1, emptySubmit);
    HR(queue->Signal(done.Get(), 2));
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr); HR(done->SetEventOnCompletion(2, event));
    Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "empty-state GPU completion"); CloseHandle(event);
    HR(readback->Map(0, nullptr, &mapped)); a = static_cast<UINT*>(mapped)[0]; readback->Unmap(0, nullptr);
    Check(a == 32, "follow-up dispatch after known-empty envelope failed");
    puts("PASS known-empty GPU: unknown state rejected, Reset+heaps accepted, encode, null-root restore, subsequent game dispatch=32");
    // The yysls trace intermittently has Reset+root signatures but no heaps.
    // Reproduce with actual constants/CBV/SRV/UAV root bindings, an injected
    // production pass and a following game dispatch that does not rebind them.
    D3D12_ROOT_PARAMETER directParams[4]{};
    directParams[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    directParams[0].Constants={0,0,1};
    directParams[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; directParams[1].Descriptor.ShaderRegister=1;
    directParams[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    directParams[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC directDesc{}; directDesc.NumParameters=4; directDesc.pParameters=directParams;
    ComPtr<ID3DBlob> directBlob, directShader;
    HR(D3D12SerializeRootSignature(&directDesc,D3D_ROOT_SIGNATURE_VERSION_1,&directBlob,&errors));
    ComPtr<ID3D12RootSignature> directRoot;
    HR(device->CreateRootSignature(0,directBlob->GetBufferPointer(),directBlob->GetBufferSize(),IID_PPV_ARGS(&directRoot)));
    const char* directHlsl="RWByteAddressBuffer dst:register(u0);ByteAddressBuffer src:register(t0);"
        "cbuffer A:register(b0){uint a;}cbuffer B:register(b1){uint b;}"
        "[numthreads(1,1,1)]void main(){dst.Store(0,a+b+src.Load(4));}";
    HR(D3DCompile(directHlsl,strlen(directHlsl),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&directShader,&errors));
    D3D12_COMPUTE_PIPELINE_STATE_DESC directPd{}; directPd.pRootSignature=directRoot.Get();
    directPd.CS={directShader->GetBufferPointer(),directShader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> directPso; HR(device->CreateComputePipelineState(&directPd,IID_PPV_ARGS(&directPso)));
    originalSetHeaps=reinterpret_cast<SetHeapsFn>(Patch(list.Get(),28,reinterpret_cast<void*>(&SpySetHeaps)));
    Check(originalSetHeaps != nullptr,"heap-call spy");
    for (UINT mode=0;mode<3;++mode) {
        HR(alloc->Reset()); HR(list->Reset(alloc.Get(),nullptr));
        if (mode==1) { // Explicitly unbind, even without relying on Reset metadata.
            list->SetDescriptorHeaps(1,heaps); tracker.NoteDescriptorHeaps(list.Get(),1,heaps);
            list->SetDescriptorHeaps(0,heaps); tracker.NoteDescriptorHeaps(list.Get(),0,nullptr);
        }
        list->SetComputeRootSignature(directRoot.Get()); tracker.NoteComputeRootSignature(list.Get(),directRoot.Get());
        list->SetPipelineState(directPso.Get()); tracker.NotePipelineState(list.Get(),directPso.Get());
        list->SetComputeRoot32BitConstant(0,24+mode,0);
        list->SetComputeRootConstantBufferView(1,upload->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2,upload->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3,output->GetGPUVirtualAddress());
        auto directState=tracker.Snapshot(list.Get());
        Check(directState.RestorableComputeBindings() && directState.KnownEmptyDescriptorHeaps(),"known heapless bindings rejected");
        auto unknown=directState; unknown.resetObserved=false; unknown.descriptorHeapsObserved=false;
        Check(!unknown.RestorableComputeBindings(),"unknown heapless bindings admitted");
        unknown.descriptorHeapsObserved=true;
        Check(unknown.RestorableComputeBindings(),"explicit heap unbind requires unrelated Reset");
        auto stale=directState; stale.compute[2].kind=CommandListState::RootArgument::Table;
        Check(!stale.RestorableComputeBindings(),"compute table without heap admitted");
        stale=directState; stale.graphics[0].kind=CommandListState::RootArgument::Table;
        Check(!stale.RestorableComputeBindings(),"graphics table without heap admitted");
        {
            CommandListStateScope scope(list.Get()); Check(scope.Ready(),"heapless game envelope rejected");
            Check(passes.RecordEncode(list.Get(),texIn.Get(),texOut.Get(),8,8,4,2.2f),"heapless-state encode");
            Check(lastHeapCallCount>0,"encode did not bind its temporary heap");
        }
        Check(lastHeapCallCount==0,"empty heap was not explicitly restored via SetDescriptorHeaps(0)");
        Check(tracker.Snapshot(list.Get()).heapCount==0,"NR polluted zero-heap game snapshot");
        Barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->Dispatch(1,1,1);
        Barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(),0,output.Get(),0,4); HR(list->Close());
        ID3D12CommandList* directSubmit[]{list.Get()}; queue->ExecuteCommandLists(1,directSubmit);
        HR(queue->Signal(done.Get(),3+mode));
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); HR(done->SetEventOnCompletion(3+mode,event));
        Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"heapless dispatch completion"); CloseHandle(event);
        HR(readback->Map(0,nullptr,&mapped)); a=static_cast<UINT*>(mapped)[0]; readback->Unmap(0,nullptr);
        Check(a==42+mode,"heapless root bindings not restored to the game dispatch");
    }
    Patch(list.Get(),28,reinterpret_cast<void*>(originalSetHeaps));
    puts("PASS heapless GPU: Reset/root constants/CBV/SRV/UAV -> production encode -> explicit zero-heap restoration -> game results 42/43/44; unknown and heapless tables rejected");
    // Exit must stop admissions but must not destroy an unsubmitted recording.
    // Separate process-lifetime gates leave the original admission regression
    // above intact and cover each actual fence state independently.
    auto* idleExit = new EvaluateGpuGate;
    Check(idleExit->Initialize(device.Get()), "idle exit gate init");
    idleExit->SetSubmissionHookReady(true);
    Check(idleExit->DrainForExit(), "idle exit rejected");
    Check(!idleExit->Begin(list.Get()), "exit allowed a new NR admission");
    auto* unsubmittedExit = new EvaluateGpuGate;
    Check(unsubmittedExit->Initialize(device.Get()), "unsubmitted exit init");
    unsubmittedExit->SetSubmissionHookReady(true);
    Check(unsubmittedExit->Begin(list.Get()), "exit fixture admission");
    const auto unsubmittedStart = GetTickCount64();
    Check(!unsubmittedExit->DrainForExit(), "unsubmitted exit freed referenced resources");
    Check(GetTickCount64() - unsubmittedStart < EvaluateGpuGate::WAIT_BUDGET_MS,
        "unsubmitted exit waited for a queue submission");
    unsubmittedExit->Reset(list.Get());
    Check(unsubmittedExit->DrainForExit(), "discarded recording retained on exit");

    auto* submittedExit = new EvaluateGpuGate;
    Check(submittedExit->Initialize(device.Get()), "submitted exit init");
    submittedExit->SetSubmissionHookReady(true);
    ComPtr<ID3D12GraphicsCommandList> exitList;
    HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&exitList)));
    HR(exitList->Close());
    Check(submittedExit->Begin(exitList.Get()), "submitted exit admission");
    HR(queue->Wait(blocker.Get(), 2));
    ID3D12CommandList* exitLists[]{exitList.Get()};
    queue->ExecuteCommandLists(1, exitLists);
    submittedExit->Submitted(queue.Get(), 1, exitLists);
    const auto submittedStart = GetTickCount64();
    Check(!submittedExit->DrainForExit(20), "unfinished GPU exit freed resources");
    Check(GetTickCount64() - submittedStart < 500, "exit drain was unbounded");
    auto finishExit = std::async(std::launch::async, [&] { Sleep(10); return blocker->Signal(2); });
    Check(submittedExit->DrainForExit(100), "completed submitted work not retired on exit");
    HR(finishExit.get());
    Check(!submittedExit->Begin(exitList.Get()), "drained exit gate reopened");
    puts("PASS exit GPU gate: idle, reject new admissions, immediate unsubmitted retention, Reset discard, bounded in-flight retention, completed drain");
    unsigned validationErrors = 0;
    if (info) for (UINT64 i=0; i<info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes=0; info->GetMessage(i,nullptr,&bytes); auto* msg = static_cast<D3D12_MESSAGE*>(malloc(bytes));
        info->GetMessage(i,msg,&bytes);
        if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { ++validationErrors; puts(msg->pDescription); }
        free(msg);
    }
    Check(validationErrors == 0, "D3D12 validation errors");
    tracker.NoteDescriptorHeaps(list.Get(),1,heaps);
    tracker.NoteRootTable(list.Get(),false,0,heap->GetGPUDescriptorHandleForHeapStart());
    tracker.NoteDescriptorHeaps(list.Get(),0,nullptr);
    Check(tracker.Snapshot(list.Get()).compute[0].kind == CommandListState::RootArgument::Empty,
        "heap switch retained invalid table");
    printf("PASS: WARP pre-SR state envelope -> encode -> ExecuteIndirect (32,33), submitted GPU wait/retry, bounded timeout, Reset discard protection, heap invalidation; debug=%s\n", hasDebug ? "on" : "unavailable");
    // Device removal is a separate safe terminal condition: the driver cannot
    // execute even a previously unsubmitted NR list. Do this last so the regular
    // graphics/state validation above still runs on a healthy real device.
    auto* removedExit = new EvaluateGpuGate;
    Check(removedExit->Initialize(device.Get()), "removed exit init");
    removedExit->SetSubmissionHookReady(true);
    Check(removedExit->Begin(exitList.Get()), "removed exit admission");
    ComPtr<ID3D12Device5> removable; HR(device.As(&removable));
    removable->RemoveDevice();
    Check(FAILED(device->GetDeviceRemovedReason()) && removedExit->DrainForExit(),
        "removed device did not allow safe terminal cleanup");
    puts("PASS exit GPU gate: deliberately removed device permits terminal cleanup");
    return 0;
} catch (const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
