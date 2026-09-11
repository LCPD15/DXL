// DXL asynchronous YOLO/ADE segmentation. See SemanticMask.h for channel contract.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include <mutex>
#include "SemanticMask.h"
#include <windows.h>
#include <d3d12.h>

#include "SemGroups.h"

namespace DXL {

// SEM_GROUP_COUNT 在 SemGroups.h（#81 起 18 = COCO 12 + ADE 场景 6），
// UI（OverlayRaster）和这边共用 —— 两处各写一个 12 会漂移。

class SegMaskFilter {
public:
	// selfModule：core dll 自己 —— nvinfer/plan 都在它旁边找（便携模式）。
	bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
		HMODULE selfModule) noexcept;
	// 停线程 + 释放 TRT/显存/窗口。退出清理里必须调
	//（TRT 上下文和 NGX feature 一样，拆卸时机错了就是退出崩溃）。
	void TeardownForExit() noexcept;
	void RetainForExit() noexcept { _retainForExit.store(true); _active.store(false); }
	bool IsReady() const noexcept { return _trtReady.load(std::memory_order_acquire); }
	const wchar_t* LastError() const noexcept { return _lastError; }

	// present 线程每帧调用。source = NR 看到的那张颜色
	//（代理生效时是代理 buffer，和 NR 输入同源 —— mask 才和画面对齐）。
	// idleState = source 在调用时刻的静止态：D3D12 路径是 PRESENT（present
	// 线程此刻在 RunFilters 里，NR 已跑完）；#87 D3D11 桥接传 COMMON
	//（跨 API 共享纹理从 COMMON 起步，见 DlssNrFilter11::Execute）。
	// barrier 往返成对：idleState → COPY_SOURCE → idleState。
	void RequestFrame(ID3D12Resource* source,
		D3D12_RESOURCE_STATES idleState = D3D12_RESOURCE_STATE_PRESENT) noexcept;

	// debug 小窗贴哪个窗口的左下角（present 线程随时可调，只存指针）
	void SetGameWindow(HWND hwnd) noexcept { _gameHwnd = hwnd; }

	// Copies a coherent pair without blocking the render thread on inference.
	bool CopyLatest(SemanticMaskSnapshot& snapshot) noexcept;
	uint64_t MaskVersion() const noexcept { return _publishedVersion.load(); }
	uint32_t MaskWidth() const noexcept { return _publishedWidth.load(); }
	uint32_t MaskHeight() const noexcept { return _publishedHeight.load(); }
	float LastInferenceMs() const noexcept { return _lastInferenceMs.load(); }
	float PersonPercent() const noexcept { return _personPercent.load(); }
	void SetActive(bool on) noexcept { _active.store(on); }
	// Record on the exact NR list. Caller must notify real submission/reset.
	// Evaluate callers supply encoded pre-NR colour (bounded display-like range).
	bool RecordFrame(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
		D3D12_RESOURCE_STATES state, bool flipY = false) noexcept;
	void NotifySubmitted(ID3D12CommandQueue* queue, UINT count,
		ID3D12CommandList* const* lists) noexcept;
	void NotifyReset(ID3D12GraphicsCommandList* list) noexcept;

	// debug 小窗开关（线程安全的"想要"，worker 按它建/拆窗）
	void SetDebugView(bool on) noexcept { _debugWant.store(on, std::memory_order_release); }
	bool DebugViewOn() const noexcept { return _debugWant.load(std::memory_order_acquire); }
	// 组开关位掩码（present 每帧写，worker 读，#79）：bit 0..11 = 整合组。
	// **worker 按它过滤 NMS**（全类解码的 proto 合成是按实例数的，
	// 12 组全开 + 繁忙场景会到几十个实例、10ms+；过滤后典型 1~3 组）。
	// 开关变化 ≤33ms（下一帧 worker）生效 —— 无感。
	void SetSemanticGroups(uint32_t enabledMask) noexcept {
		_groupEnabled.store(enabledMask & SEM_OBJECT_GROUP_MASK, std::memory_order_release);
	}

private:
	friend struct SegMaskTestAccess;
	void WorkerLoop() noexcept;
	bool EnsureTrt() noexcept;          // 只在 worker 线程调
	bool EnsureAdeEngine() noexcept;    // #81 第二引擎（可选，缺 plan 降级）
	void ReleaseTrt() noexcept;         // 同上（worker 退出路径；连第二引擎一起释放）
	// rgba：帧的 RGBA8（w*h*4 行主序，已解格式）；返回 false = 这一帧放弃
	bool LetterboxAndInfer(const uint8_t* rgba, uint32_t w, uint32_t h) noexcept;
	// _hInput 已就绪时的那一跳：H2D -> enqueueV3 -> D2H -> 计时 -> 解码发布
	bool RunInference() noexcept;
	bool DecodeAndPublish(const float* det, const float* proto) noexcept;
	void UpdateDebugWindow() noexcept;  // worker 线程（窗口 owner）
	void Fail(const wchar_t* msg) noexcept;
	// readback 行解包成 RGBA8（pitch != w*bpp；只支持 backbuffer 常见格式）
	void UnpackFrameToRgba(const uint8_t* src, uint32_t pitch,
		uint32_t w, uint32_t h, DXGI_FORMAT format, uint8_t* rgba) noexcept;
	static float HalfToFloat(uint16_t h) noexcept;

	// ---- D3D12 读回（present 线程录，worker 消费）----
	HMODULE _selfModule = nullptr;
	ID3D12Device* _device = nullptr;
	ID3D12CommandQueue* _queue = nullptr;
	ID3D12CommandAllocator* _copyAlloc = nullptr;
	ID3D12GraphicsCommandList* _copyList = nullptr;   // 建好即 Close（空），每帧 Reset 重录
	ID3D12Fence* _fence = nullptr;
	HANDLE _fenceEvent = nullptr;
	uint64_t _fenceValue = 0;               // 只在 present 线程碰
	static constexpr uint32_t RB_SLOTS = 1; // One allocator, one readback owner until GPU + inference finish.
	ID3D12Resource* _readback[RB_SLOTS] = {};
	std::atomic<uint64_t> _copyFenceValue[RB_SLOTS]{}; // 每槽自己的 signal 值（worker 拿它等）
	std::atomic<bool> _copyInFlight[RB_SLOTS] = {};
	uint32_t _rbPitch = 0;
	DXGI_FORMAT _frameFormat = DXGI_FORMAT_UNKNOWN;
	uint32_t _frameWidth = 0, _frameHeight = 0;
	uint64_t _capturedMs = 0;
    bool _frameFlipY = false; // Protected by the single in-flight capture slot.
	uint32_t _inferenceGroups = 1;
	std::atomic<bool> _active{true};
	std::mutex _captureMutex;
	ID3D12GraphicsCommandList* _pendingList = nullptr;
	std::atomic<bool> _frameWanted{ false };  // worker：我想要下一帧
	std::atomic<HWND> _gameHwnd{nullptr};

	// ---- 线程 / 生命周期 ----
	std::thread _worker;
	std::atomic<bool> _stop{ false };
	std::atomic<bool> _retainForExit{ false };
	std::atomic<bool> _trtReady{ false };
	std::atomic<bool> _trtFailed{ false };
	wchar_t _lastError[256] = L"";

	// ---- TRT / CUDA（全部只在 worker 线程碰；头文件不引 TRT 头，cpp 里转型）----
	HMODULE _nvinfer = nullptr;     // selected Lean/Full DLL; released after its last TRT object
	bool _leanRuntime = false;     // fixed by the package, no fallback after a Lean failure
	HMODULE _nvcuda = nullptr;      // System32\nvcuda.dll（不 Free：驱动的东西）
	int _cuDevice = -1;
	bool _driverApiLoaded = false;  // ReleaseTrt 知道显存/上下文可释放
	void* _runtime = nullptr;       // nvinfer1::IRuntime*
	void* _engine = nullptr;        // nvinfer1::ICudaEngine*
	void* _context = nullptr;       // nvinfer1::IExecutionContext*
	void* _cuCtx = nullptr;         // CUcontext（主上下文 retain，退出释放）
	void* _cuStream = nullptr;      // CUstream
	void* _dInput = nullptr;        // 640*640*3 fp32（两个引擎共用同一 letterbox 输入）
	void* _dOut0 = nullptr;         // 116*8400 fp32
	void* _dOut1 = nullptr;         // 32*160*160 fp32
	std::vector<float> _hInput;     // letterbox 结果（H2D 源，640*640*3）
	std::vector<float> _hOut0, _hOut1;
	uint32_t _out0Vol = 0, _out1Vol = 0;   // 输出体积（元素数；shape 从 engine 问）
	uint32_t _anchors = 0;                  // 8400（= out0Vol/116，排布 #71）

	// ---- 第二引擎（#81）：yolo26s-sem ADE20K 场景解析 ----
	// 可选引擎：缺 plan = 功能降级（COCO 12 组照跑），不 Fail。
	// 输出 (1,640,640) uint8 逐像素 train-ID 类图（argmax 已烘进图）。
	void* _engine2 = nullptr;       // nvinfer1::ICudaEngine*
	void* _context2 = nullptr;      // nvinfer1::IExecutionContext*
	void* _dOut2 = nullptr;         // 640*640 uint8
	std::vector<uint8_t> _hOut2;    // D2H 目标（类图）
	bool _adeReady = false;         // EnsureAdeEngine 成功置位；ReleaseTrt 清零
	bool _adeFailed = false;        // 缺 plan/反序列化失败：本局不再试（同 _trtFailed 模式）

	// Publishing swaps the complete snapshot under a short lock. Render reads
	// use try_lock and retain their last still-fresh local snapshot on contention.
	std::mutex _publishMutex;
	SemanticMaskSnapshot _published;
	std::atomic<uint64_t> _publishedVersion{0};
	std::atomic<uint32_t> _publishedWidth{0}, _publishedHeight{0};
	std::vector<uint8_t> _workerTemp;   // 解码中转（coverage，不进发布环）
	std::vector<uint8_t> _workerGroup;  // #79 组 ID 中转（与 _workerTemp 同步写）
	std::vector<uint8_t> _rgbaFrame;    // 帧解码中转
	std::atomic<float> _lastInferenceMs{0.0f};      // worker 写，状态区读（撕裂无害：显示用）
	std::atomic<float> _personPercent{0.0f};

	// ---- debug 小窗（worker 线程：创建/绘制/销毁）----
	std::atomic<bool> _debugWant{ false };
	std::atomic<uint32_t> _groupEnabled{ 1 };   // 组开关位掩码（默认只开组 0=人物）
	HWND _debugHwnd = nullptr;
	HDC _debugDc = nullptr;
	HBITMAP _debugBmp = nullptr;
	void* _debugBits = nullptr;     // DIB 位（32bpp BGRA，w*h）
	uint32_t _debugW = 0, _debugH = 0;
};

}  // namespace DXL
