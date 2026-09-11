#pragma once

// 在游戏自己的 D3D12 设备和队列上跑 DLSS Super Resolution。
//
// 两种模式共用同一套骨架，唯一区别是输入尺寸：
//   DLAA    : render == output（不压低游戏分辨率）
//   真超分  : render <  output（配合 swapchain 尺寸拦截）
//
// 每帧的工作（全部录在一条命令列表里，提交到游戏的队列，所以和游戏的渲染天然串行）：
//   backbuffer --copy--> colorIn --NGX evaluate--> output --copy--> backbuffer
//
// 为什么要中转两次而不直接原地做：backbuffer 通常不支持 UAV，而 NGX 的输出是用
// UAV 写的；backbuffer 的格式也可能不是 NGX 接受的那几种。

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

#include "GpuImage.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace DXL {

enum class SrMode {
	Dlaa,      // render == output
	Upscale,   // render < output
};

// 对应 NGX 的 perf/quality 档。倍率就是 DLSS 官方的那几档比例，选了档位就等于
// 同时决定了渲染分辨率和 NGX 用哪个模型。
enum class SrQuality {
	Quality,           // 0.667
	Balanced,          // 0.580
	Performance,       // 0.500
	UltraPerformance,  // 0.333
	Custom,            // 用 inputMultiplier
};

struct SrSettings {
	bool enabled = false;
	SrMode mode = SrMode::Dlaa;
	SrQuality quality = SrQuality::Quality;
	float inputMultiplier = 1.0f;   // 真超分时的渲染倍率
	float sharpness = 0.0f;
};

// 档位对应的渲染倍率。Custom 由调用方自己填。
float QualityMultiplier(SrQuality quality) noexcept;

class DlssSrUpscaler {
public:
	~DlssSrUpscaler();

	DlssSrUpscaler(const DlssSrUpscaler&) = delete;
	DlssSrUpscaler& operator=(const DlssSrUpscaler&) = delete;
	DlssSrUpscaler() = default;

	// device/queue 来自 hook（queue 是从 CreateSwapChainForHwnd 抓的）
	bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept;

	// 尺寸或设置变了就重建 feature。返回 false 表示这一帧别做。
	// render 和 output 由调用方明确给出：DLAA 时两者相等；真超分时 render 是代理
	// backbuffer 的尺寸，output 是真 swapchain 的尺寸。
	bool Prepare(
		uint32_t renderWidth,
		uint32_t renderHeight,
		uint32_t outputWidth,
		uint32_t outputHeight,
		DXGI_FORMAT colorFormat,
		const SrSettings& settings) noexcept;

	// 在 present 之前调用。
	//   colorSource  这一帧要放大的图。可能是代理 backbuffer、真 backbuffer，
	//                也可能是 DLSSNR 的输出（滤镜串联时）。
	//   dest         结果拷到哪里，通常就是真 backbuffer。
	// 两者是同一块资源时会多一次中转拷贝（backbuffer 不能直接当 NGX 的输入读），
	// 不同时直接把 colorSource 喂给 NGX，省掉一次全分辨率拷贝。
	// 两者的 state 字段既是进入前的状态、也是我们还原到的状态。
	bool Execute(const GpuImage& colorSource, const GpuImage& dest) noexcept;

	bool IsReady() const noexcept { return _feature != nullptr; }
	uint32_t RenderWidth() const noexcept { return _renderWidth; }
	uint32_t RenderHeight() const noexcept { return _renderHeight; }
	uint32_t OutputWidth() const noexcept { return _outputWidth; }
	uint32_t OutputHeight() const noexcept { return _outputHeight; }
	uint64_t EvaluateCount() const noexcept { return _evaluateCount; }
	uint64_t FailureCount() const noexcept { return _failureCount; }
	uint64_t SkippedFrames() const noexcept { return _skippedFrames; }
	const char* LastError() const noexcept { return _lastError; }

private:
	// 4 个槽而不是 3 个：槽越多，正常的帧间抖动越不容易撞上"上一轮还没做完"，
	// 也就越少跳帧。代价只是多一份命令分配器。
	static constexpr uint32_t SLOT_COUNT = 4;
	// 取槽时允许等待的上限。只用来吸收抖动 —— 见 .cpp 里 TryClaimSlot 的说明，
	// 这个值绝不能大到让"死锁"变成"可感知的卡顿"。
	static constexpr DWORD SLOT_WAIT_BUDGET_MS = 4;

	struct Slot {
		ID3D12CommandAllocator* allocator = nullptr;
		ID3D12GraphicsCommandList* commandList = nullptr;
		uint64_t fenceValue = 0;
	};

	bool CreateCommandObjects() noexcept;
	bool CreateTextures(DXGI_FORMAT colorFormat) noexcept;
	bool CreateFeature() noexcept;
	void DestroyFeature() noexcept;
	void ReleaseTextures() noexcept;
	// 返回 false = 这一帧跳过（不是错误，也不停用自己）。调用方直接放弃这一帧。
	bool TryClaimSlot(Slot& slot) noexcept;
	void Fail(const char* what) noexcept;

	ID3D12Device* _device = nullptr;
	ID3D12CommandQueue* _queue = nullptr;

	NVSDK_NGX_Handle* _feature = nullptr;
	NVSDK_NGX_Parameter* _parameters = nullptr;

	// 我们自己的中转资源
	// _colorIn 只在没有代理 backbuffer（DLAA 模式）时才需要
	ID3D12Resource* _colorIn = nullptr;    // NGX 输入（render 尺寸）
	ID3D12Resource* _output = nullptr;     // NGX 输出（output 尺寸，UAV）
	ID3D12Resource* _zeroDepth = nullptr;  // 里程碑 3 前用零深度
	ID3D12Resource* _zeroMotion = nullptr;
	// 只为了清零那两张纹理。ClearUnorderedAccessViewFloat 要**两个**句柄，而且
	// 必须来自两个不同的堆：GPU 句柄来自当前绑定的 shader-visible 堆，CPU 句柄
	// 必须来自 non-shader-visible 堆 —— shader-visible 堆在 CPU 侧是只写的，
	// 驱动从里面读描述符是非法的（调试层会报 ERROR，清除会被丢弃）。
	ID3D12DescriptorHeap* _uavHeapGpu = nullptr;   // shader-visible
	ID3D12DescriptorHeap* _uavHeapCpu = nullptr;   // non-shader-visible
	uint32_t _uavStride = 0;

	Slot _slots[SLOT_COUNT]{};
	uint32_t _nextSlot = 0;
	ID3D12Fence* _fence = nullptr;
	HANDLE _fenceEvent = nullptr;
	uint64_t _fenceValue = 0;

	uint32_t _renderWidth = 0;
	uint32_t _renderHeight = 0;
	uint32_t _outputWidth = 0;
	uint32_t _outputHeight = 0;
	DXGI_FORMAT _colorFormat = DXGI_FORMAT_UNKNOWN;
	SrSettings _settings;

	uint64_t _skippedFrames = 0;
	bool _needsReset = true;
	bool _zeroTexturesCleared = false;
	bool _disabled = false;
	uint64_t _evaluateCount = 0;
	uint64_t _failureCount = 0;
	const char* _lastError = "";
};

}  // namespace DXL
