#pragma once

// 记住游戏在**它自己的命令列表**上绑过什么，跑完我们的 dispatch 之后原样放回去。
//
// 为什么必须有这个（实测崩出来的，不是预防性设计）：
// 在游戏的 DLSS evaluate 之前插一个计算 pass，就必须 SetDescriptorHeaps 换成我们的堆。
// 我们不还回去的话，游戏后面的绘制拿着指向**它原来那个堆**的根描述符表句柄去采样 ——
// 落到我们那个只有 2 个描述符的堆外面 → GPU 越界 → 设备丢失 → 游戏闪退。
// 实测：注入成功后立刻闪退，日志正好停在第一次成功处理的那一帧上。
//
// 我曾经推理说"游戏紧接着自己也要调 NGX EvaluateFeature，所以它本来就预期这些状态被
// 打乱、我们蹭的是它已经付过的代价"。**那个推理是错的。** OptiScaler 有
// `RestoreComputeSignature` 这个配置项本身就是反证，而我把它解释掉了。
//
// D3D12 的命令列表**没有 getter** —— 拿不到"当前绑的是什么"。所以只能自己记：
// hook 那几个 setter，把游戏设过的值存下来（按命令列表分别存），要还的时候重放。
// OptiScaler 走的也是这条路（hooks/D3D12_Hooks.cpp）。
//
// 只跟这四样：
//   SetDescriptorHeaps        —— 不还会 GPU 越界，这是唯一会直接崩的
//   SetPipelineState          —— 还了才不至于让游戏的下一次 Dispatch 用我们的 PSO
//   SetComputeRootSignature   —— 同上
//   SetGraphicsRootSignature  —— 我们只碰计算管线，但顺手记着，将来要加图形 pass 就有了
//
// v0.41: 同时追踪 compute/graphics 根表、根常量及 CBV/SRV/UAV 根描述符。
// 更换根签名会使根参数失效，恢复签名后必须恢复参数；Reset 也必须清除旧记录。

#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace DXL {

// 一条命令列表上"游戏最后绑了什么"。
struct CommandListState {
	struct RootArgument {
		enum Kind { Empty, Table, Constants, CBV, SRV, UAV } kind = Empty;
		D3D12_GPU_DESCRIPTOR_HANDLE table{};
		D3D12_GPU_VIRTUAL_ADDRESS address = 0;
		uint64_t constantMask = 0;
		uint32_t constants[64]{};
	};
	RootArgument compute[64]{}, graphics[64]{};
	// 最多两个（CBV_SRV_UAV + SAMPLER），这是 D3D12 的上限
	ID3D12DescriptorHeap* heaps[2]{};
	uint32_t heapCount = 0;
	ID3D12PipelineState* pipelineState = nullptr;
	ID3D12RootSignature* computeRootSignature = nullptr;
	ID3D12RootSignature* graphicsRootSignature = nullptr;
	bool resetObserved = false;
	bool descriptorHeapsObserved = false;
	bool ComputeStateKnown() const noexcept {
		if (computeRootSignature) return true;
		if (!resetObserved) return false;
		for (const auto& arg : compute) if (arg.kind != RootArgument::Empty) return false;
		return true; // Observed Reset followed by no compute bindings is a known empty state.
	}
	bool KnownEmptyDescriptorHeaps() const noexcept {
		if (heapCount || (!resetObserved && !descriptorHeapsObserved)) return false;
		// Root descriptors/constants need no shader-visible heap. A descriptor
		// table without one is invalid or unobserved; never replay its stale handle.
		for (const auto& arg : compute) if (arg.kind == RootArgument::Table) return false;
		for (const auto& arg : graphics) if (arg.kind == RootArgument::Table) return false;
		return true;
	}
	bool RestorableComputeBindings() const noexcept {
		return ComputeStateKnown() && (heapCount || KnownEmptyDescriptorHeaps());
	}

	bool HasAnything() const noexcept {
		return heapCount || pipelineState || computeRootSignature ||
			graphicsRootSignature;
	}
};

class CommandListTracker {
public:
	class IgnoreScope {
	public:
		explicit IgnoreScope(bool enabled = true) noexcept : _enabled(enabled) { if (_enabled) ++_ignoreDepth; }
		~IgnoreScope() { if (_enabled) --_ignoreDepth; }
		IgnoreScope(const IgnoreScope&) = delete;
		IgnoreScope& operator=(const IgnoreScope&) = delete;
	private:
		bool _enabled;
	};
	static CommandListTracker& Get() noexcept;
	using PatchFn = void* (*)(void*, size_t, void*) noexcept;
	static bool InstallRootHooks(ID3D12GraphicsCommandList* list, PatchFn patch) noexcept;
	inline static std::atomic<bool> rootsReady{ false };
	void NoteRootTable(ID3D12GraphicsCommandList*, bool graphics, UINT index,
		D3D12_GPU_DESCRIPTOR_HANDLE) noexcept;
	void NoteRootConstants(ID3D12GraphicsCommandList*, bool graphics, UINT index,
		UINT count, const void* values, UINT offset) noexcept;
	void NoteRootAddress(ID3D12GraphicsCommandList*, bool graphics, UINT index,
		D3D12_GPU_VIRTUAL_ADDRESS, CommandListState::RootArgument::Kind) noexcept;

    using ResetObserver = void (*)(void*, ID3D12GraphicsCommandList*) noexcept;
    static void SetResetObserver(ResetObserver observer, void* ctx) noexcept {
        _resetContext.store(ctx, std::memory_order_relaxed);
        _resetObserver.store(observer, std::memory_order_release);
    }
    static void NotifyResetObserver(ID3D12GraphicsCommandList* list) noexcept {
        if (auto observer = _resetObserver.load(std::memory_order_acquire))
            observer(_resetContext.load(std::memory_order_relaxed), list);
    }

	// 从 hook 里调。**不持有引用** —— 记录的是"游戏绑过这个指针"，
	// AddRef 会妨碍它释放（swapchain 那次已经教过一遍了），而且这些对象活得比
	// 一帧长得多。指针悬空的唯一后果是我们回放一个已死的对象，
	// 而那只会在"游戏释放了 PSO 但还在用同一条列表画"时发生 —— 那本身就是它的 bug。
	void NoteDescriptorHeaps(
		ID3D12GraphicsCommandList* list,
		uint32_t count, ID3D12DescriptorHeap* const* heaps) noexcept;
	void NotePipelineState(
		ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) noexcept;
	void NoteComputeRootSignature(
		ID3D12GraphicsCommandList* list, ID3D12RootSignature* sig) noexcept;
	void NoteGraphicsRootSignature(
		ID3D12GraphicsCommandList* list, ID3D12RootSignature* sig) noexcept;
	// Reset 之后列表上什么都没绑了，记录必须跟着清 —— 否则会把上一轮的东西
	// 回放到一条全新的列表上。
	void NoteReset(ID3D12GraphicsCommandList* list) noexcept;

	// 取一份快照（我们插入之前的状态）。没记录过就返回一个空的。
	CommandListState Snapshot(ID3D12GraphicsCommandList* list) const noexcept;

	// 把快照重新绑回去。空的字段跳过 —— 游戏没绑过的东西我们也没资格替它绑。
	static void Restore(
		ID3D12GraphicsCommandList* list, const CommandListState& state) noexcept;

	// 跟了多少条命令列表。诊断用：0 说明 setter 的 hook 没装上，
	// 那"还原"就是个空动作，evaluate 点那条路必须拒绝执行。
	uint32_t TrackedCount() const noexcept {
		return _count.load(std::memory_order_relaxed) + _overflowCount.load(std::memory_order_relaxed);
	}
	bool IsTracked(ID3D12GraphicsCommandList* list) const noexcept { return Find(list) != nullptr; }
	static constexpr uint32_t Capacity() noexcept { return 0; } // 0 = grows on demand

private:
	inline static thread_local unsigned _ignoreDepth = 0;
    inline static std::atomic<ResetObserver> _resetObserver{nullptr};
    inline static std::atomic<void*> _resetContext{nullptr};
	CommandListTracker() = default;

	// Keep the existing small-game fast path; larger games use stable overflow nodes.
	static constexpr uint32_t MAX_LISTS = 64;

	struct Entry {
		std::atomic<ID3D12GraphicsCommandList*> list{ nullptr };
		CommandListState state;
	};

	Entry* Find(ID3D12GraphicsCommandList* list) const noexcept;
	Entry* FindOrAdd(ID3D12GraphicsCommandList* list) noexcept;

	mutable Entry _entries[MAX_LISTS];
	std::atomic<uint32_t> _count{ 0 };
	mutable std::shared_mutex _overflowMutex;
	std::unordered_map<ID3D12GraphicsCommandList*, std::unique_ptr<Entry>> _overflow;
	std::atomic<uint32_t> _overflowCount{ 0 };
};

// Capture at the GAME -> SR boundary, not after SR has left its temporary heaps
// and root arguments on the list. SR and NR must not overwrite the game's record.
class CommandListStateScope {
public:
	explicit CommandListStateScope(ID3D12GraphicsCommandList* list, bool enabled = true) noexcept
		: _list(list), _saved(enabled ? CommandListTracker::Get().Snapshot(list) : CommandListState{}),
		  _ready(enabled && CommandListTracker::rootsReady.load() && _saved.RestorableComputeBindings()),
		  _ignore(_ready) {}
	~CommandListStateScope() { if (_ready && _restore) CommandListTracker::Restore(_list, _saved); }
	void CancelRestore() noexcept { _restore = false; }
	bool Ready() const noexcept { return _ready; }
	// Diagnostics must inspect the pre-SR snapshot, not SR's temporary bindings.
	const CommandListState& SavedState() const noexcept { return _saved; }
	CommandListStateScope(const CommandListStateScope&) = delete;
	CommandListStateScope& operator=(const CommandListStateScope&) = delete;
private:
	ID3D12GraphicsCommandList* _list;
	CommandListState _saved;
	bool _ready;
	bool _restore = true;
	CommandListTracker::IgnoreScope _ignore;
};

}  // namespace DXL
