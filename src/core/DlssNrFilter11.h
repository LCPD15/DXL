#pragma once

// D3D11 / D3D10 Present bridge. NR runs on a private D3D12 device on
// the game's adapter. Shared colour/output textures and two GPU fences order
// copy-in -> NR -> copy-out without CPU readback or steady-state CPU waits.
//
// A D3D10 device can expose D3D11 interfaces while retaining D3D10 API mode.
// A scoped private D3D11 context state enables the bridge calls and restores
// the game's complete bindings/API mode on every exit, including failures.
//
// Shared textures use SHARED | SHARED_NTHANDLE with SHADER_RESOURCE binding.
// Resize/exit retirement includes the final D3D11 copy-out; its bounded CPU
// wait is separate from the steady-state GPU fence protocol. The underlying
// D3D12 filter supplies NR layers, optical flow and semantic-mask processing.

#include <windows.h>
#include <d3d11.h>
#include <d3d11_3.h>   // ID3D11Fence / ID3D11DeviceContext4
#include <d3d11_4.h>   // ID3D11Device5::OpenSharedFence
#include <d3d12.h>
#include <cstdint>

#include "DlssNrFilter.h"   // NrSettings + 复用的 D3D12 版滤镜

namespace DXL {

class DlssNrFilter11 final {
public:
	DlssNrFilter11() = default;
	~DlssNrFilter11();

	DlssNrFilter11(const DlssNrFilter11&) = delete;
	DlssNrFilter11& operator=(const DlssNrFilter11&) = delete;

	// device11 从 swapchain 的 GetDevice 拿（core 那边已经存好）。
	// 内部：同适配器上建私有 D3D12 设备 + DIRECT 队列 + 共享 fence ×2，
	// 然后 DlssNrFilter::Initialize（私有设备，整套 D3D12 路径）。
	bool Initialize(ID3D11Device* device11, HMODULE selfModule) noexcept;

	// 每帧一次（present hook 里，游戏的 immediate context 线程）：
	// 拷进 → fence → D3D12 滤镜 → fence → 拷回。全程 GPU 侧同步。
	bool Execute(ID3D11Texture2D* backBuffer, const NrSettings& settings) noexcept;

	// 退出清理（RequestTeardown 的回调）：先 DlssNrFilter::TeardownForExit
	// （等它自己的 GPU 活 + 释放 feature），再放共享资源。
	bool TeardownForExit() noexcept;

	/* ---------------- 状态（和 D3D12 版名字对齐） ---------------- */
	bool IsInitialized() const noexcept { return _initialized; }
	bool IsDisabled() const noexcept { return _disabled; }
	uint64_t EvaluateCount() const noexcept { return _evaluateCount; }
	const char* LastError() const noexcept { return _lastError; }
	// 耗时透传：真正计时的是桥接里的 D3D12 滤镜（浮层/OSD 显示用，
	// core 的 FeedOverlay 在 D3D11 上读这里而不是 g_state.nrFilter）。
	float RecentGpuMs() const noexcept { return _filter12.RecentGpuMs(); }
	float WorstGpuMs() const noexcept { return _filter12.WorstGpuMs(); }
	uint32_t TimingSamples() const noexcept { return _filter12.TimingSamples(); }
	// 浮层状态区（#63）也要透传：深度/矢量来源 + 分辨率 + 工作分辨率 + 帧数。
	// D3D11 上这些全在桥接里那套 D3D12 滤镜上（和耗时同理）。
	void InvalidateOpticalHistory() noexcept { _filter12.InvalidateOpticalHistory(); }
	bool UsingExternalMotion() const noexcept { return _filter12.UsingExternalMotion(); }
	bool UsingExternalDepth() const noexcept { return _filter12.UsingExternalDepth(); }
	uint32_t ExternalMotionWidth() const noexcept { return _filter12.ExternalMotionWidth(); }
	uint32_t ExternalMotionHeight() const noexcept { return _filter12.ExternalMotionHeight(); }
	uint32_t ExternalDepthWidth() const noexcept { return _filter12.ExternalDepthWidth(); }
	uint32_t ExternalDepthHeight() const noexcept { return _filter12.ExternalDepthHeight(); }
	uint32_t Width() const noexcept { return _filter12.Width(); }
	uint32_t Height() const noexcept { return _filter12.Height(); }
	// FeedOverlay 状态区按码选滤镜用（D3D11 分支读 f.Width() 等那一串）。
	// 返回 const 引用：调用方只读状态，不持有、不调非 const 方法。
	const DlssNrFilter& Filter12ForStatus() const noexcept { return _filter12; }

	// ---- #87 语义蒙版（D3D11 路径的接线，之前只接了 D3D12）----
	// worker 挂桥接的私有 D3D12 设备/队列：readback 和滤镜活在同一条队列上
	// 天然串行（fence 序见 .cpp Execute 里钩子处 的注释）。
	ID3D12Device* BridgeDevice12() const noexcept { return _device12; }
	ID3D12CommandQueue* BridgeQueue12() const noexcept { return _queue12; }
	// provider 转发给桥接里的那套 D3D12 滤镜 —— 语义 mask 是 _filter12 消费的
	//（g_state.nrFilter 在 D3D11 上从没跑过 evaluate）。
	void SetSemanticMaskProvider(
		const DlssNrFilter::SemanticMaskProvider& provider) noexcept {
		_filter12.SetSemanticMaskProvider(provider);
	}
	// 读回钩子：Execute 里"滤镜活之后、Signal(f12to11) 之前"调
	// RequestFrame(共享颜色, COMMON)。**时机是 fence 序的关键**，core 不能在
	// present 线程自己调 —— 挪出 Execute 就丢了和下一帧拷进之间的互斥。
	using SemanticReadbackFn = void(*)(ID3D12Resource* sharedColor, void* ctx);
	void SetSemanticReadbackHook(SemanticReadbackFn fn, void* ctx) noexcept {
		_semanticReadback = fn;
		_semanticReadbackCtx = ctx;
	}

private:
	friend struct NrBridgeFailureTestAccess;
	bool CreateSharedTextures(uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept;
	void ReleaseShared() noexcept;
	bool WaitForSharedWorkIdle() noexcept;
	void Fail(const char* what) noexcept;

	// 游戏侧（引用由我们持有，teardown 时释放）
	ID3D11Device* _device11 = nullptr;
	ID3D11DeviceContext* _context = nullptr;        // immediate（GetImmediateContext 有 AddRef）
	ID3D11DeviceContext1* _context1 = nullptr;
	ID3DDeviceContextState* _state11 = nullptr;    // private D3D11 API mode + bindings
	ID3D11DeviceContext4* _context4 = nullptr;      // Signal/Wait（共享 fence 用）
	// 私有 D3D12
	ID3D12Device* _device12 = nullptr;
	ID3D12CommandQueue* _queue12 = nullptr;
	DlssNrFilter _filter12;                         // 复用的整套 D3D12 滤镜
	// 共享纹理（D3D11 建 / D3D12 打开）
	ID3D11Texture2D* _color11 = nullptr;
	ID3D12Resource* _color12 = nullptr;
	ID3D11Texture2D* _out11 = nullptr;
	ID3D12Resource* _out12 = nullptr;
	// 共享 fence（D3D12 建 / D3D11 打开）
	ID3D12Fence* _fence11to12 = nullptr;
	ID3D11Fence* _fence11to12_11 = nullptr;
	ID3D12Fence* _fence12to11 = nullptr;
	ID3D11Fence* _fence12to11_11 = nullptr;
	uint64_t _fenceValue = 0;
	uint64_t _lastSharedWork = 0; // odd=copy-in, even=copy-out retired on D3D11
	HANDLE _retireEvent = nullptr;
	bool _bridgeSyncFailed = false;

	uint32_t _width = 0;
	uint32_t _height = 0;
	DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;

	bool _initialized = false;
	bool _disabled = false;
	uint32_t _failureCount = 0;   // Fail() 里 8 次停用（一次失败可能是暂态）
	uint64_t _evaluateCount = 0;
	const char* _lastError = "";

	// #87 语义读回钩子（见 public 区 SetSemanticReadbackHook 的说明）
	SemanticReadbackFn _semanticReadback = nullptr;
	void* _semanticReadbackCtx = nullptr;
};

}  // namespace DXL
