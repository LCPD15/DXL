#pragma once

// 找出游戏的主场景深度缓冲。
//
// 思路和 ReShade 的 Generic-Depth 一样：游戏不会告诉我们哪张是"主深度"，只能观察
// 它每帧怎么用这些深度缓冲，然后按启发式挑一张。所以必然存在挑错的游戏 —— 因此
// 候选列表和 debug 视图是这套东西的**必要组成部分**，不是附加功能。
//
// 观察哪三件事：
//   Device::CreateDepthStencilView   建立 DSV 句柄 -> 深度资源 的映射。
//                                    这一步不可省：命令列表里只能拿到描述符句柄，
//                                    从句柄反查不到资源。
//   List::OMSetRenderTargets         深度被绑定为渲染目标 —— 每帧绑定次数是主要信号
//   List::ClearDepthStencilView      主深度基本都是每帧清一次
//
// 刻意**不** hook Draw*：绘制调用一帧几千次，给每一次都加一层间接调用是实打实的
// 开销。绑定次数已经能区分主深度和影子贴图，不值得为此拖慢游戏。
//
// 尺寸筛选是最有力的一条：主场景深度的尺寸等于渲染分辨率，而影子贴图通常是
// 正方形且边长是 2 的幂。

#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>

namespace DXL {

// 一个深度缓冲候选。给 UI / debug 视图看的。
struct DepthCandidate {
	ID3D12Resource* resource = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	uint32_t bindsLastFrame = 0;
	uint32_t clearsLastFrame = 0;
	// 资源当前处于什么状态。靠观察 ResourceBarrier 维护 —— 我们要在 present 时
	// 拷走它，而拷贝必须知道它此刻的状态，否则调试层会报错、拿到的也是垃圾。
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	bool stateKnown = false;
};

class DepthTracker {
public:
	// 真实游戏的深度缓冲数量远超预期（鬼武者实测把 32 个槽全占满了），
	// 表满之后新的进不来，真正的场景深度可能根本登记不上。放宽 + 支持淘汰。
	static constexpr uint32_t MAX_CANDIDATES = 96;

	// 装 hook 之前会先做 vtable 自检，不过就返回 false 且什么都不装。
	// probeCommandList 只用来自检，我们不持有它。
	bool Install(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* probeCommandList) noexcept;

	bool IsInstalled() const noexcept { return _installed; }

	// 每帧在 present 时调一次：把本帧的计数归档，重新挑选主深度。
	// renderWidth/Height 是我们期望的渲染分辨率，尺寸筛选要用。
	void EndFrame(uint32_t renderWidth, uint32_t renderHeight) noexcept;

	// 当前挑中的那张。没有就返回 nullptr。
	ID3D12Resource* Selected() const noexcept { return _selected; }
	uint32_t SelectedWidth() const noexcept { return _selectedWidth; }
	uint32_t SelectedHeight() const noexcept { return _selectedHeight; }
	DXGI_FORMAT SelectedFormat() const noexcept { return _selectedFormat; }
	// 挑中那张在 present 时的资源状态。stateKnown 为 false 时不要去拷它。
	bool SelectedState(D3D12_RESOURCE_STATES& state) const noexcept;

	// 用户在 UI 里手动指定第几个候选（启发式挑错时的出路）。-1 = 自动。
	void SetManualIndex(int index) noexcept { _manualIndex = index; }
	int ManualIndex() const noexcept { return _manualIndex; }

	// 给 UI / 日志用的快照。返回写入的条数。
	uint32_t Snapshot(DepthCandidate* out, uint32_t capacity) const noexcept;
	uint32_t CandidateCount() const noexcept { return _count; }

	// 游戏重建了 swapchain / 改了分辨率：整张候选表连同 DSV 映射全部作废。
	// 那一刻游戏几乎肯定也重建了深度缓冲，留着旧条目只会让我们拿旧尺寸去匹配
	// 新资源（地址复用），得出错误结论。
	void Invalidate() noexcept;

private:
	friend struct DepthTrackerHooks;

	struct Entry {
		// **刻意不 AddRef**。持有引用会把游戏的深度缓冲钉住不放，妨碍它自己回收
		// 显存，甚至改变它的行为 —— 一个只读的探测器绝不该做这种事。
		//
		// 代价是这个指针会变成悬垂的：游戏改画质设置时会销毁并重建深度缓冲。
		// 我们**从不解引用它**（只当身份标识比较，尺寸/格式都是发现时抄下来的），
		// 但还有一个坑：新资源可能正好分配在刚释放的旧地址上，于是我们拿旧尺寸
		// 去匹配新资源。对策是 staleFrames —— 连续多帧没被用到就丢掉，让表跟着
		// 游戏的重建走。
		ID3D12Resource* resource = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		std::atomic<uint32_t> binds{ 0 };
		std::atomic<uint32_t> clears{ 0 };
		uint32_t bindsLastFrame = 0;
		uint32_t clearsLastFrame = 0;
		uint32_t staleFrames = 0;      // 连续多少帧既没被绑定也没被清除
		std::atomic<uint32_t> state{ D3D12_RESOURCE_STATE_COMMON };
		std::atomic<bool> stateKnown{ false };
	};

	// 连续这么多帧没被用到就把候选丢掉。取值只要远大于"偶尔跳过几帧"的正常波动，
	// 又远小于玩家察觉时间即可。
	static constexpr uint32_t MAX_STALE_FRAMES = 240;

	// DSV 句柄 -> 候选下标。句柄数量很少，线性查找足够，还省掉一把锁。
	struct DsvMapping {
		SIZE_T handle = 0;
		int candidate = -1;
	};

	int FindOrAddCandidate(ID3D12Resource* resource) noexcept;
	int CandidateForHandle(SIZE_T handle) const noexcept;

	Entry _entries[MAX_CANDIDATES]{};
	std::atomic<uint32_t> _count{ 0 };

	static constexpr uint32_t MAX_DSV = 128;
	DsvMapping _dsvMap[MAX_DSV]{};
	std::atomic<uint32_t> _dsvCount{ 0 };

	ID3D12Resource* _selected = nullptr;
	uint32_t _selectedWidth = 0;
	uint32_t _selectedHeight = 0;
	DXGI_FORMAT _selectedFormat = DXGI_FORMAT_UNKNOWN;
	int _selectedIndex = -1;
	int _manualIndex = -1;
	bool _installed = false;
	// 深度缓冲变化的"密集度"检测：短时间内发现大量新候选 = 换场景（进/退场景），
	// 稀疏的深度缓冲轮换（移动时）不算，避免移动时频繁触发暂停导致画面闪烁。
	uint64_t _burstWindowStart = 0;   // 当前爆发窗口开始时间（GetTickCount64 毫秒）
	uint32_t _burstCount = 0;         // 窗口内新候选数
};

// 进程内只有一个游戏设备，单例够用
DepthTracker& GetDepthTracker() noexcept;

}  // namespace DXL
