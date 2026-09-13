#include "CommandListTracker.h"
#include "EvaluateGpuGate.h"
#include "NrRouteProbe.h"
#include <cstring>
#include <mutex>

namespace DXL {

CommandListTracker& CommandListTracker::Get() noexcept {
	static CommandListTracker instance;
	return instance;
}

CommandListTracker::Entry* CommandListTracker::Find(
	ID3D12GraphicsCommandList* list) const noexcept {
	if (!list) return nullptr;
	const uint32_t count = _count.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count; ++i) {
		if (_entries[i].list.load(std::memory_order_relaxed) == list) {
			return &_entries[i];
		}
	}
	if (_overflowCount.load(std::memory_order_acquire)) {
		std::shared_lock lock(_overflowMutex);
		const auto it = _overflow.find(list);
		if (it != _overflow.end()) return it->second.get();
	}
	return nullptr;
}

CommandListTracker::Entry* CommandListTracker::FindOrAdd(
	ID3D12GraphicsCommandList* list) noexcept {
	if (_ignoreDepth || !list) return nullptr;
	if (Entry* existing = Find(list)) return existing;
	// 抢一个空槽。setter 会从多个渲染线程进来，所以用 CAS 而不是加锁 ——
	// 这几个函数在游戏每一次 SetDescriptorHeaps 上都会跑，非常热。
	for (uint32_t i = 0; i < MAX_LISTS; ++i) {
		ID3D12GraphicsCommandList* expected = nullptr;
		if (_entries[i].list.compare_exchange_strong(expected, list)) {
			_entries[i].state = CommandListState{};
			// _count 只增不减，用来给上面的线性查找划范围
			uint32_t seen = _count.load(std::memory_order_relaxed);
			while (seen < i + 1 &&
				!_count.compare_exchange_weak(seen, i + 1)) {
			}
			return &_entries[i];
		}
	}
	// Never evict another recording list to admit this one. Nodes stay at stable
	// addresses across rehashes; per-list Reset still clears the saved bindings.
	try {
		std::unique_lock lock(_overflowMutex);
		const auto found = _overflow.find(list);
		if (found != _overflow.end()) return found->second.get();
		auto entry = std::make_unique<Entry>();
		entry->list.store(list, std::memory_order_relaxed);
		auto* result = entry.get();
		_overflow.emplace(list, std::move(entry));
		_overflowCount.fetch_add(1, std::memory_order_release);
		return result;
	} catch (...) {
		return nullptr; // Allocation failure must retain the NR binding guard.
	}
}

void CommandListTracker::NoteDescriptorHeaps(
	ID3D12GraphicsCommandList* list,
	uint32_t count, ID3D12DescriptorHeap* const* heaps) noexcept {
	Entry* entry = FindOrAdd(list);
	if (!entry) return;
	// D3D12 允许一次绑最多两个（一个 CBV_SRV_UAV + 一个 SAMPLER）
	const uint32_t keep = count > 2 ? 2 : count;
	bool changed = entry->state.heapCount != keep;
	for (uint32_t i = 0; i < keep; ++i)
		changed = changed || entry->state.heaps[i] != (heaps ? heaps[i] : nullptr);
	if (changed) {
		// Switching shader-visible heaps invalidates descriptor tables, including
		// those belonging to the other pipeline. Never replay a stale heap handle.
		for (auto& arg : entry->state.compute) if (arg.kind == CommandListState::RootArgument::Table) arg = {};
		for (auto& arg : entry->state.graphics) if (arg.kind == CommandListState::RootArgument::Table) arg = {};
	}
	entry->state.heapCount = keep;
	entry->state.descriptorHeapsObserved = true;
	for (uint32_t i = 0; i < keep; ++i) {
		entry->state.heaps[i] = heaps ? heaps[i] : nullptr;
	}
}

void CommandListTracker::NotePipelineState(
	ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) noexcept {
	if (Entry* entry = FindOrAdd(list)) entry->state.pipelineState = pso;
}

void CommandListTracker::NoteComputeRootSignature(
	ID3D12GraphicsCommandList* list, ID3D12RootSignature* sig) noexcept {
	if (Entry* entry = FindOrAdd(list)) {
		if (entry->state.computeRootSignature != sig)
			for (auto& arg : entry->state.compute) arg = {};
		entry->state.computeRootSignature = sig;
	}
}

void CommandListTracker::NoteGraphicsRootSignature(
	ID3D12GraphicsCommandList* list, ID3D12RootSignature* sig) noexcept {
	if (Entry* entry = FindOrAdd(list)) {
		if (entry->state.graphicsRootSignature != sig)
			for (auto& arg : entry->state.graphics) arg = {};
		entry->state.graphicsRootSignature = sig;
	}
}

void CommandListTracker::NoteReset(ID3D12GraphicsCommandList* list) noexcept {
	if (_ignoreDepth) return;
	// **不能只是"清空记录"，要连槽位一起留着。** 同一条列表会被反复 Reset 复用，
	// 留着槽位省掉一次 CAS；但状态必须清 —— Reset 之后列表上确实什么都没绑，
	// 把上一轮的东西回放上去是凭空替游戏做决定。
	if (Entry* entry = FindOrAdd(list)) {
		entry->state = CommandListState{};
		entry->state.resetObserved = true;
	}
}

CommandListState CommandListTracker::Snapshot(
	ID3D12GraphicsCommandList* list) const noexcept {
	if (const Entry* entry = Find(list)) return entry->state;
	return CommandListState{};
}

void CommandListTracker::Restore(
	ID3D12GraphicsCommandList* list, const CommandListState& state) noexcept {
	if (!list) return;
	// Restore heaps, signatures and PSO. An observed Reset makes a null root
	// signature known: clear NR's signature too. Unknown null fields stay untouched.
	if (state.heapCount && state.heaps[0]) {
		list->SetDescriptorHeaps(state.heapCount, state.heaps);
	} else if (state.KnownEmptyDescriptorHeaps()) {
		// SetDescriptorHeaps unsets all prior heaps, including the NR heap.
		// Leaving NR's heap bound would not restore an observed empty state.
		// The D3D12 debug layer still requires a non-null array at count zero.
		list->SetDescriptorHeaps(0, state.heaps);
	}
	if (state.computeRootSignature || state.resetObserved) {
		list->SetComputeRootSignature(state.computeRootSignature);
	}
	if (state.graphicsRootSignature || (state.resetObserved && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)) {
		list->SetGraphicsRootSignature(state.graphicsRootSignature);
	}
	if (state.pipelineState) {
		list->SetPipelineState(state.pipelineState);
	}
	for (UINT stage = 0; stage < 2; ++stage) {
		const bool graphics = stage != 0;
		const auto* args = graphics ? state.graphics : state.compute;
		for (UINT i = 0; i < 64; ++i) {
			const auto& a = args[i];
			using A = CommandListState::RootArgument;
			switch (a.kind) {
			case A::Table:
				if (graphics) list->SetGraphicsRootDescriptorTable(i, a.table);
				else list->SetComputeRootDescriptorTable(i, a.table);
				break;
			case A::Constants:
				for (UINT c = 0; c < 64; ++c) if (a.constantMask & (uint64_t(1) << c)) {
					if (graphics) list->SetGraphicsRoot32BitConstant(i, a.constants[c], c);
					else list->SetComputeRoot32BitConstant(i, a.constants[c], c);
				}
				break;
			case A::CBV:
				if (graphics) list->SetGraphicsRootConstantBufferView(i, a.address);
				else list->SetComputeRootConstantBufferView(i, a.address);
				break;
			case A::SRV:
				if (graphics) list->SetGraphicsRootShaderResourceView(i, a.address);
				else list->SetComputeRootShaderResourceView(i, a.address);
				break;
			case A::UAV:
				if (graphics) list->SetGraphicsRootUnorderedAccessView(i, a.address);
				else list->SetComputeRootUnorderedAccessView(i, a.address);
				break;
			default: break;
			}
		}
	}
}


void CommandListTracker::NoteRootTable(ID3D12GraphicsCommandList* list, bool graphics,
    UINT index, D3D12_GPU_DESCRIPTOR_HANDLE value) noexcept {
    if (index >= 64) return;
    if (Entry* e = FindOrAdd(list)) {
        auto& a = graphics ? e->state.graphics[index] : e->state.compute[index];
        a = {}; a.kind = CommandListState::RootArgument::Table; a.table = value;
    }
}
void CommandListTracker::NoteRootConstants(ID3D12GraphicsCommandList* list, bool graphics,
    UINT index, UINT count, const void* values, UINT offset) noexcept {
    if (index >= 64 || offset >= 64 || count > 64 - offset || !values) return;
    if (Entry* e = FindOrAdd(list)) {
        auto& a = graphics ? e->state.graphics[index] : e->state.compute[index];
        if (a.kind != CommandListState::RootArgument::Constants) a = {};
        a.kind = CommandListState::RootArgument::Constants;
        for (UINT i = 0; i < count; ++i) {
            a.constants[offset + i] = static_cast<const UINT*>(values)[i];
            a.constantMask |= uint64_t(1) << (offset + i);
        }
    }
}
void CommandListTracker::NoteRootAddress(ID3D12GraphicsCommandList* list, bool graphics,
    UINT index, D3D12_GPU_VIRTUAL_ADDRESS value, CommandListState::RootArgument::Kind kind) noexcept {
    if (index >= 64) return;
    if (Entry* e = FindOrAdd(list)) {
        auto& a = graphics ? e->state.graphics[index] : e->state.compute[index];
        a = {}; a.kind = kind; a.address = value;
    }
}
namespace {
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
ResetFn originalReset = nullptr;
HRESULT STDMETHODCALLTYPE TrackReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso) {
    const HRESULT hr = originalReset(list, alloc, pso);
    if (SUCCEEDED(hr)) {
        // Observers retire discarded upload caches before the gate admits a
        // new recording that may reuse the same filter resources.
        CommandListTracker::NotifyResetObserver(list);
        EvaluateGpuGate::Get().Reset(list);
        NrRouteProbe::Get().Reset(list);
        CommandListTracker::Get().NoteReset(list);
        CommandListTracker::Get().NotePipelineState(list, pso);
    }
    return hr;
}
using Root0Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE value);
Root0Fn originalRoot0 = nullptr;
void STDMETHODCALLTYPE TrackRoot0(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE value) {
    CommandListTracker::Get().NoteRootTable(list, false, index, value);
    originalRoot0(list, index, value);
}
using Root1Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE value);
Root1Fn originalRoot1 = nullptr;
void STDMETHODCALLTYPE TrackRoot1(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE value) {
    CommandListTracker::Get().NoteRootTable(list, true, index, value);
    originalRoot1(list, index, value);
}
using Root2Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT value, UINT offset);
Root2Fn originalRoot2 = nullptr;
void STDMETHODCALLTYPE TrackRoot2(ID3D12GraphicsCommandList* list, UINT index, UINT value, UINT offset) {
    CommandListTracker::Get().NoteRootConstants(list, false, index, 1, &value, offset);
    originalRoot2(list, index, value, offset);
}
using Root3Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT value, UINT offset);
Root3Fn originalRoot3 = nullptr;
void STDMETHODCALLTYPE TrackRoot3(ID3D12GraphicsCommandList* list, UINT index, UINT value, UINT offset) {
    CommandListTracker::Get().NoteRootConstants(list, true, index, 1, &value, offset);
    originalRoot3(list, index, value, offset);
}
using Root4Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT count, const void* values, UINT offset);
Root4Fn originalRoot4 = nullptr;
void STDMETHODCALLTYPE TrackRoot4(ID3D12GraphicsCommandList* list, UINT index, UINT count, const void* values, UINT offset) {
    CommandListTracker::Get().NoteRootConstants(list, false, index, count, values, offset);
    originalRoot4(list, index, count, values, offset);
}
using Root5Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT count, const void* values, UINT offset);
Root5Fn originalRoot5 = nullptr;
void STDMETHODCALLTYPE TrackRoot5(ID3D12GraphicsCommandList* list, UINT index, UINT count, const void* values, UINT offset) {
    CommandListTracker::Get().NoteRootConstants(list, true, index, count, values, offset);
    originalRoot5(list, index, count, values, offset);
}
using Root6Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS value);
Root6Fn originalRoot6 = nullptr;
void STDMETHODCALLTYPE TrackRoot6(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS value) {
    CommandListTracker::Get().NoteRootAddress(list, false, index, value, CommandListState::RootArgument::CBV);
    originalRoot6(list, index, value);
}
using Root7Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS value);
Root7Fn originalRoot7 = nullptr;
void STDMETHODCALLTYPE TrackRoot7(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS value) {
    CommandListTracker::Get().NoteRootAddress(list, true, index, value, CommandListState::RootArgument::CBV);
    originalRoot7(list, index, value);
}
using Root8Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS value);
Root8Fn originalRoot8 = nullptr;
void STDMETHODCALLTYPE TrackRoot8(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS value) {
    CommandListTracker::Get().NoteRootAddress(list, false, index, value, CommandListState::RootArgument::SRV);
    originalRoot8(list, index, value);
}
using Root9Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS value);
Root9Fn originalRoot9 = nullptr;
void STDMETHODCALLTYPE TrackRoot9(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS value) {
    CommandListTracker::Get().NoteRootAddress(list, true, index, value, CommandListState::RootArgument::SRV);
    originalRoot9(list, index, value);
}
using Root10Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS value);
Root10Fn originalRoot10 = nullptr;
void STDMETHODCALLTYPE TrackRoot10(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS value) {
    CommandListTracker::Get().NoteRootAddress(list, false, index, value, CommandListState::RootArgument::UAV);
    originalRoot10(list, index, value);
}
using Root11Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS value);
Root11Fn originalRoot11 = nullptr;
void STDMETHODCALLTYPE TrackRoot11(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS value) {
    CommandListTracker::Get().NoteRootAddress(list, true, index, value, CommandListState::RootArgument::UAV);
    originalRoot11(list, index, value);
}
}
bool CommandListTracker::InstallRootHooks(ID3D12GraphicsCommandList* list, PatchFn patch) noexcept {
    originalReset = reinterpret_cast<ResetFn>(patch(list, 10, reinterpret_cast<void*>(&TrackReset)));
    bool ok = originalReset != nullptr;
    originalRoot0 = reinterpret_cast<Root0Fn>(patch(list, 31, reinterpret_cast<void*>(&TrackRoot0)));
    ok = ok && originalRoot0 != nullptr;
    originalRoot1 = reinterpret_cast<Root1Fn>(patch(list, 32, reinterpret_cast<void*>(&TrackRoot1)));
    ok = ok && originalRoot1 != nullptr;
    originalRoot2 = reinterpret_cast<Root2Fn>(patch(list, 33, reinterpret_cast<void*>(&TrackRoot2)));
    ok = ok && originalRoot2 != nullptr;
    originalRoot3 = reinterpret_cast<Root3Fn>(patch(list, 34, reinterpret_cast<void*>(&TrackRoot3)));
    ok = ok && originalRoot3 != nullptr;
    originalRoot4 = reinterpret_cast<Root4Fn>(patch(list, 35, reinterpret_cast<void*>(&TrackRoot4)));
    ok = ok && originalRoot4 != nullptr;
    originalRoot5 = reinterpret_cast<Root5Fn>(patch(list, 36, reinterpret_cast<void*>(&TrackRoot5)));
    ok = ok && originalRoot5 != nullptr;
    originalRoot6 = reinterpret_cast<Root6Fn>(patch(list, 37, reinterpret_cast<void*>(&TrackRoot6)));
    ok = ok && originalRoot6 != nullptr;
    originalRoot7 = reinterpret_cast<Root7Fn>(patch(list, 38, reinterpret_cast<void*>(&TrackRoot7)));
    ok = ok && originalRoot7 != nullptr;
    originalRoot8 = reinterpret_cast<Root8Fn>(patch(list, 39, reinterpret_cast<void*>(&TrackRoot8)));
    ok = ok && originalRoot8 != nullptr;
    originalRoot9 = reinterpret_cast<Root9Fn>(patch(list, 40, reinterpret_cast<void*>(&TrackRoot9)));
    ok = ok && originalRoot9 != nullptr;
    originalRoot10 = reinterpret_cast<Root10Fn>(patch(list, 41, reinterpret_cast<void*>(&TrackRoot10)));
    ok = ok && originalRoot10 != nullptr;
    originalRoot11 = reinterpret_cast<Root11Fn>(patch(list, 42, reinterpret_cast<void*>(&TrackRoot11)));
    ok = ok && originalRoot11 != nullptr;
    rootsReady.store(ok);
    return ok;
}
}  // namespace DXL
