#include "DlssSrUpscaler.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <cmath>

#include "../common/Log.h"
#include "FreezeWatchdog.h"
#include "NgxSession.h"

namespace DXL {

float QualityMultiplier(SrQuality quality) noexcept {
	switch (quality) {
	case SrQuality::UltraPerformance: return 1.0f / 3.0f;
	case SrQuality::Performance:      return 0.500f;
	case SrQuality::Balanced:         return 0.580f;
	case SrQuality::Quality:          return 2.0f / 3.0f;
	case SrQuality::Custom:           break;
	}
	return 1.0f;
}

namespace {

// NGX 只接受有限的颜色格式。这里挑一个和 backbuffer 兼容、且能建 UAV 的。
// backbuffer 常见的 *_SRGB 变体不能直接建 UAV，所以统一落到非 sRGB 的等价格式。
DXGI_FORMAT PickColorFormat(DXGI_FORMAT backBufferFormat) noexcept {
	switch (backBufferFormat) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

// D3D12 的 CopyResource 允许两侧格式**同属一个 typeless 家族**，不要求完全相同。
// 这一点很关键：很多游戏的 backbuffer 是 *_UNORM_SRGB，而 sRGB 格式建不了 UAV
// （NGX 的输出要 UAV），所以我们的中间纹理必须用非 sRGB 变体，靠家族内拷贝对接。
//
// 注意这是**按位拷贝，不做色彩空间转换**。也就是说 sRGB 编码的数值会被 DLSS
// 当作线性值处理。进出都是同一套编码，所以往返是自洽的，只是 DLSS 内部的假设
// 和实际编码不符，属于画质层面的偏差，不是正确性问题。
bool FormatsCopyCompatible(DXGI_FORMAT a, DXGI_FORMAT b) noexcept {
	if (a == b) return true;
	auto family = [](DXGI_FORMAT format) noexcept {
		switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		default:
			return format;
		}
	};
	return family(a) == family(b);
}

// NGX 会按档位挑模型，所以档位要和实际的渲染倍率对得上。Custom 倍率没有对应的
// 档位，就挑比例最接近的那个，让 NGX 至少用到合适的模型。
NVSDK_NGX_PerfQuality_Value PickPerfQuality(
	const SrSettings& settings, uint32_t renderWidth, uint32_t outputWidth) noexcept {
	if (settings.mode == SrMode::Dlaa) {
		return NVSDK_NGX_PerfQuality_Value_DLAA;
	}
	switch (settings.quality) {
	case SrQuality::UltraPerformance:
		return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	case SrQuality::Performance:
		return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	case SrQuality::Balanced:
		return NVSDK_NGX_PerfQuality_Value_Balanced;
	case SrQuality::Quality:
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	case SrQuality::Custom:
		break;
	}
	const float ratio = outputWidth ? float(renderWidth) / float(outputWidth) : 1.0f;
	const struct { float ratio; NVSDK_NGX_PerfQuality_Value value; } presets[]{
		{ 1.0f / 3.0f, NVSDK_NGX_PerfQuality_Value_UltraPerformance },
		{ 0.500f,      NVSDK_NGX_PerfQuality_Value_MaxPerf },
		{ 0.580f,      NVSDK_NGX_PerfQuality_Value_Balanced },
		{ 2.0f / 3.0f, NVSDK_NGX_PerfQuality_Value_MaxQuality },
	};
	NVSDK_NGX_PerfQuality_Value best = presets[0].value;
	float bestDistance = 1e9f;
	for (const auto& preset : presets) {
		const float distance = fabsf(preset.ratio - ratio);
		if (distance < bestDistance) {
			bestDistance = distance;
			best = preset.value;
		}
	}
	return best;
}

ID3D12Resource* CreateTexture(
	ID3D12Device* device,
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT format,
	bool allowUav,
	D3D12_RESOURCE_STATES initialState,
	const wchar_t* debugName) noexcept {
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = allowUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
		: D3D12_RESOURCE_FLAG_NONE;

	ID3D12Resource* resource = nullptr;
	const HRESULT hr = device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
		IID_PPV_ARGS(&resource));
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"CreateCommittedResource(%s %ux%u fmt=%u) failed: 0x%08X",
			debugName, width, height, (unsigned)format, hr);
		return nullptr;
	}
	resource->SetName(debugName);
	return resource;
}

void Barrier(
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* resource,
	D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after) noexcept {
	if (before == after) return;
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	commandList->ResourceBarrier(1, &barrier);
}

template <typename T>
void SafeRelease(T*& ptr) noexcept {
	if (ptr) {
		ptr->Release();
		ptr = nullptr;
	}
}

}  // namespace

DlssSrUpscaler::~DlssSrUpscaler() {
	// 等 GPU 把我们提交的活干完再拆资源，否则驱动会踩到已释放的内存
	if (_queue && _fence) {
		const uint64_t target = ++_fenceValue;
		if (SUCCEEDED(_queue->Signal(_fence, target)) &&
			_fence->GetCompletedValue() < target && _fenceEvent) {
			_fence->SetEventOnCompletion(target, _fenceEvent);
			WaitForSingleObject(_fenceEvent, 2000);
		}
	}
	DestroyFeature();
	ReleaseTextures();
	for (Slot& slot : _slots) {
		SafeRelease(slot.commandList);
		SafeRelease(slot.allocator);
	}
	SafeRelease(_fence);
	if (_fenceEvent) CloseHandle(_fenceEvent);
}

void DlssSrUpscaler::Fail(const char* what) noexcept {
	++_failureCount;
	_lastError = what;
	// 连续失败就彻底停掉，别每帧刷日志也别把游戏拖崩
	if (_failureCount >= 8) {
		_disabled = true;
		D5_LOG_ERROR(L"DLSS SR disabled after repeated failures (%hs)", what);
	}
}

bool DlssSrUpscaler::Initialize(
	ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
	_device = device;
	_queue = queue;
	if (!_device || !_queue) {
		_lastError = "no device or command queue";
		return false;
	}
	if (!CreateCommandObjects()) return false;
	D5_LOG_INFO(L"DlssSrUpscaler initialised (device=%p queue=%p)", device, queue);
	return true;
}

bool DlssSrUpscaler::CreateCommandObjects() noexcept {
	for (Slot& slot : _slots) {
		HRESULT hr = _device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
		if (SUCCEEDED(hr)) {
			hr = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				slot.allocator, nullptr, IID_PPV_ARGS(&slot.commandList));
		}
		if (FAILED(hr)) {
			D5_LOG_ERROR(L"create command objects failed: 0x%08X", hr);
			return false;
		}
		slot.commandList->Close();
		// 起名，理由同 DlssNrFilter：DRED 的面包屑只认名字
		slot.commandList->SetName(L"D5Q.SR.List");
		slot.allocator->SetName(L"D5Q.SR.Allocator");
	}
	HRESULT hr = _device->CreateFence(
		0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"CreateFence failed: 0x%08X", hr);
		return false;
	}
	_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	return _fenceEvent != nullptr;
}

// 取一个命令槽。**这个函数跑在游戏的 present 线程上，所以它几乎不等。**
//
// 为什么这条规则是硬的：我们把命令列表提交到**游戏自己的** command queue，然后等
// 一个由那个队列 signal 的 fence。D3D12 队列是严格按提交顺序执行的，所以"等我们的
// fence"实际上等于"等这个队列里排在我们前面的所有活"——包括游戏的活。游戏重新配置
// 渲染管线时（在设置里切 DLSS 就是），它自己会做跨队列/跨线程的同步；只要它的某个
// 环节在等 present 线程，而 present 线程在等我们的 fence，环就闭合了 —— 死锁。
// 表现正是"画面冻住、进程活着、不崩溃"。
//
// 所以：等不到就**跳过这一帧**，绝不阻塞。
//   · 跳过是无害的：3 个槽意味着只有 GPU 落后 3 帧以上才会跳，而跳一帧只是这一帧
//     不做滤镜（代理生效时是显示上一帧的结果），下一帧自然恢复。
//   · 也**不停用自己**：停用是不可逆的，而队列拥堵是暂时的。上一版超时即停用，
//     结果玩家改一次设置就永久失去效果。
//
// 留一个很小的等待预算（几毫秒）只是为了吸收正常的帧间抖动，不至于一有波动就掉帧；
// 这个量级人眼不可能察觉，也不可能把死锁变成"卡住"。
bool DlssSrUpscaler::TryClaimSlot(Slot& slot) noexcept {
	if (!slot.fenceValue || _fence->GetCompletedValue() >= slot.fenceValue) {
		return true;
	}
	if (_fenceEvent &&
		SUCCEEDED(_fence->SetEventOnCompletion(slot.fenceValue, _fenceEvent)) &&
		WaitForSingleObject(_fenceEvent, SLOT_WAIT_BUDGET_MS) == WAIT_OBJECT_0) {
		return true;
	}

	const uint64_t skipped = ++_skippedFrames;
	// 只在开始跳和跳得离谱时说话，别每帧刷日志
	if (skipped == 1 || skipped % 300 == 0) {
		const HRESULT removed = _device ? _device->GetDeviceRemovedReason() : S_OK;
		D5_LOG_WARN(L"DLSS SR 跳过第 %llu 帧：命令槽还没完成（目标 %llu，已完成 %llu，"
			L"GetDeviceRemovedReason=0x%08X）。不阻塞 present 线程，下一帧再试。",
			(unsigned long long)skipped, (unsigned long long)slot.fenceValue,
			(unsigned long long)_fence->GetCompletedValue(), removed);
	}
	return false;
}

void DlssSrUpscaler::ReleaseTextures() noexcept {
	SafeRelease(_colorIn);
	SafeRelease(_output);
	SafeRelease(_zeroDepth);
	SafeRelease(_zeroMotion);
	SafeRelease(_uavHeapGpu);
	SafeRelease(_uavHeapCpu);
	_zeroTexturesCleared = false;
}

bool DlssSrUpscaler::CreateTextures(DXGI_FORMAT colorFormat) noexcept {
	ReleaseTextures();

	// 输入是 render 尺寸；输出是 output 尺寸且必须支持 UAV（NGX 用 UAV 写）。
	// _colorIn 总是建：到底需不需要它取决于每帧传进来的源（是否同一块资源、
	// 格式是否就是我们的工作格式），Prepare 时判断不了，而它只占一张渲染
	// 分辨率的纹理。
	_colorIn = CreateTexture(_device, _renderWidth, _renderHeight, colorFormat,
		false, D3D12_RESOURCE_STATE_COPY_DEST, L"D5Q.SR.ColorIn");
	if (!_colorIn) {
		ReleaseTextures();
		_lastError = "texture creation failed";
		return false;
	}
	_output = CreateTexture(_device, _outputWidth, _outputHeight, colorFormat,
		true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"D5Q.SR.Output");

	// 里程碑 3 会换成真深度/真矢量；在那之前用清零的纹理，让契约完整。
	// 这个会话里已经实测过：零矢量意味着 DLSS 认为画面完全静止，所以现在
	// 不要指望画质提升，验收标准是"evaluate 成功且画面不崩"。
	// 建在 UNORDERED_ACCESS：清零要用 UAV，清完一次性转成着色器资源状态常驻
	_zeroDepth = CreateTexture(_device, _renderWidth, _renderHeight,
		DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		L"D5Q.SR.ZeroDepth");
	_zeroMotion = CreateTexture(_device, _renderWidth, _renderHeight,
		DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		L"D5Q.SR.ZeroMotion");

	if (!_output || !_zeroDepth || !_zeroMotion) {
		ReleaseTextures();
		_lastError = "texture creation failed";
		return false;
	}

	// 清零用的 UAV。D3D12 资源的初始内容未定义，不清就是给 NGX 喂垃圾。
	// 两个堆都要：见头文件里对 ClearUnorderedAccessViewFloat 两个句柄的说明。
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HRESULT hr = _device->CreateDescriptorHeap(
		&heapDesc, IID_PPV_ARGS(&_uavHeapGpu));
	if (SUCCEEDED(hr)) {
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_uavHeapCpu));
	}
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"CreateDescriptorHeap failed: 0x%08X", hr);
		ReleaseTextures();
		_lastError = "UAV heap creation failed";
		return false;
	}
	_uavStride = _device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// 同一批描述符写进两个堆
	D3D12_CPU_DESCRIPTOR_HANDLE gpuHeapCpu =
		_uavHeapGpu->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHeapCpu =
		_uavHeapCpu->GetCPUDescriptorHandleForHeapStart();
	ID3D12Resource* const zeroTextures[]{ _zeroDepth, _zeroMotion };
	for (ID3D12Resource* texture : zeroTextures) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = texture->GetDesc().Format;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		_device->CreateUnorderedAccessView(texture, nullptr, &uav, gpuHeapCpu);
		_device->CreateUnorderedAccessView(texture, nullptr, &uav, cpuHeapCpu);
		gpuHeapCpu.ptr += _uavStride;
		cpuHeapCpu.ptr += _uavStride;
	}

	_colorFormat = colorFormat;
	return true;
}

void DlssSrUpscaler::DestroyFeature() noexcept {
	if (_feature) {
		NVSDK_NGX_D3D12_ReleaseFeature(_feature);
		_feature = nullptr;
	}
	if (_parameters) {
		NVSDK_NGX_D3D12_DestroyParameters(_parameters);
		_parameters = nullptr;
	}
}

bool DlssSrUpscaler::CreateFeature() noexcept {
	DestroyFeature();

	NVSDK_NGX_Result result =
		NVSDK_NGX_D3D12_AllocateParameters(&_parameters);
	if (NVSDK_NGX_FAILED(result)) {
		D5_LOG_ERROR(L"AllocateParameters failed: 0x%08X", (unsigned)result);
		_lastError = "NGX AllocateParameters failed";
		return false;
	}

	// feature 的创建必须录在一条命令列表里并提交，NGX 在里面做初始化工作
	Slot& slot = _slots[_nextSlot % SLOT_COUNT];
	D5_STAGE_D(SrClaimSlot, slot.fenceValue);
	if (!TryClaimSlot(slot)) return false;
	slot.allocator->Reset();
	slot.commandList->Reset(slot.allocator, nullptr);

	NVSDK_NGX_DLSS_Create_Params params{};
	params.Feature.InWidth = _renderWidth;
	params.Feature.InHeight = _renderHeight;
	params.Feature.InTargetWidth = _outputWidth;
	params.Feature.InTargetHeight = _outputHeight;
	params.Feature.InPerfQualityValue =
		PickPerfQuality(_settings, _renderWidth, _outputWidth);
	// 不设 MVLowRes：我们的矢量就是 render 尺寸。
	// 不设 IsHDR：目前只处理 UNORM backbuffer，HDR 留到后面。
	params.InFeatureCreateFlags =
		NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
		NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
	if (_settings.sharpness > 0.0f) {
		params.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
	}

	result = NGX_D3D12_CREATE_DLSS_EXT(
		slot.commandList, 1, 1, &_feature, _parameters, &params);

	slot.commandList->Close();
	if (NVSDK_NGX_SUCCEED(result)) {
		ID3D12CommandList* lists[]{ slot.commandList };
		D5_STAGE(SrSubmit);
	_queue->ExecuteCommandLists(1, lists);
		slot.fenceValue = ++_fenceValue;
		_queue->Signal(_fence, slot.fenceValue);
		++_nextSlot;
	} else {
		D5_LOG_ERROR(L"NGX_D3D12_CREATE_DLSS_EXT failed: 0x%08X", (unsigned)result);
		_lastError = "NGX DLSS feature creation failed";
		DestroyFeature();
		return false;
	}

	_needsReset = true;
	D5_LOG_INFO(L"DLSS SR feature created: %ux%u -> %ux%u mode=%s sharpness=%.2f",
		_renderWidth, _renderHeight, _outputWidth, _outputHeight,
		_settings.mode == SrMode::Dlaa ? L"DLAA" : L"upscale",
		_settings.sharpness);
	return true;
}

bool DlssSrUpscaler::Prepare(
	uint32_t renderWidth,
	uint32_t renderHeight,
	uint32_t outputWidth,
	uint32_t outputHeight,
	DXGI_FORMAT colorFormat_,
	const SrSettings& settings) noexcept {
	if (_disabled || !settings.enabled) return false;
	if (!renderWidth || !renderHeight || !outputWidth || !outputHeight) {
		return false;
	}
	if (!NgxSession::Get().IsSuperSamplingAvailable()) {
		_lastError = "DLSS SR not available on this GPU/driver";
		return false;
	}

	const DXGI_FORMAT colorFormat = PickColorFormat(colorFormat_);
	if (colorFormat == DXGI_FORMAT_UNKNOWN) {
		// 只报一次，别每帧刷
		if (_colorFormat != colorFormat_) {
			D5_LOG_WARN(L"不支持的 backbuffer 格式 %u，跳过超分",
				(unsigned)colorFormat_);
			_colorFormat = colorFormat_;
		}
		_lastError = "unsupported backbuffer format";
		return false;
	}
	if (!FormatsCopyCompatible(colorFormat, colorFormat_)) {
		if (_colorFormat != colorFormat_) {
			D5_LOG_WARN(L"backbuffer 格式 %u 和我们能用的 %u 不同族，无法拷贝，跳过超分",
				(unsigned)colorFormat_, (unsigned)colorFormat);
			_colorFormat = colorFormat_;
		}
		_lastError = "backbuffer 格式无法拷贝";
		return false;
	}

	const bool geometryChanged =
		renderWidth != _renderWidth || renderHeight != _renderHeight ||
		outputWidth != _outputWidth || outputHeight != _outputHeight ||
		colorFormat != _colorFormat;
	// 锐化值是**每帧**传给 evaluate 的（InSharpness），不需要重建 feature；
	// 建 feature 时只用到 DoSharpening 这个开关。原来任何锐化改动都重建，
	// 拖一次滑条能刷出十几次 feature 创建（日志里看到的）。
	const bool sharpeningToggled =
		(settings.sharpness > 0.0f) != (_settings.sharpness > 0.0f);
	const bool settingsChanged =
		settings.mode != _settings.mode ||
		settings.quality != _settings.quality ||
		sharpeningToggled;

	if (_feature && !geometryChanged && !settingsChanged) {
		// 不重建，但每帧生效的参数（锐化）要更新，否则 evaluate 还在用旧值
		_settings = settings;
		return true;
	}

	_renderWidth = renderWidth;
	_renderHeight = renderHeight;
	_outputWidth = outputWidth;
	_outputHeight = outputHeight;
	_settings = settings;

	if (geometryChanged && !CreateTextures(colorFormat)) return false;
	return CreateFeature();
}

bool DlssSrUpscaler::Execute(
	const GpuImage& colorSource, const GpuImage& dest) noexcept {
	if (_disabled || !_feature || !colorSource.IsValid() || !dest.IsValid()) {
		return false;
	}
	// 两种情况必须先拷到中转纹理再喂给 NGX：
	//   · 源和目标是同一块资源（DLAA 直接在 backbuffer 上做）
	//   · 源的格式不是我们的工作格式（典型是 sRGB 的代理 backbuffer —— 游戏需要
	//     它是 sRGB 才能正确写入，而 NGX 这一侧要非 sRGB）
	const bool aliased = colorSource.resource == dest.resource;
	const bool viaColorIn =
		aliased || colorSource.resource->GetDesc().Format != _colorFormat;
	if (viaColorIn && !_colorIn) return false;

	Slot& slot = _slots[_nextSlot % SLOT_COUNT];
	++_nextSlot;
	D5_STAGE_D(SrClaimSlot, slot.fenceValue);
	if (!TryClaimSlot(slot)) return false;
	if (FAILED(slot.allocator->Reset()) ||
		FAILED(slot.commandList->Reset(slot.allocator, nullptr))) {
		Fail("command list reset failed");
		return false;
	}
	ID3D12GraphicsCommandList* commandList = slot.commandList;

	// 第一帧把零深度/零矢量清干净
	if (!_zeroTexturesCleared && _uavHeapGpu && _uavHeapCpu) {
		ID3D12DescriptorHeap* heaps[]{ _uavHeapGpu };
		commandList->SetDescriptorHeaps(1, heaps);
		const float zeros[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		// GPU 句柄取自刚绑定的 shader-visible 堆，CPU 句柄取自另一个
		// non-shader-visible 堆。两者混用会被调试层拒掉且清除静默失效。
		D3D12_GPU_DESCRIPTOR_HANDLE gpu =
			_uavHeapGpu->GetGPUDescriptorHandleForHeapStart();
		D3D12_CPU_DESCRIPTOR_HANDLE cpu =
			_uavHeapCpu->GetCPUDescriptorHandleForHeapStart();
		ID3D12Resource* const zeroTextures[]{ _zeroDepth, _zeroMotion };
		for (ID3D12Resource* texture : zeroTextures) {
			commandList->ClearUnorderedAccessViewFloat(gpu, cpu, texture, zeros, 0, nullptr);
			gpu.ptr += _uavStride;
			cpu.ptr += _uavStride;
		}
		// NGX 要读它们，转成着色器资源状态
		Barrier(commandList, _zeroDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Barrier(commandList, _zeroMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		_zeroTexturesCleared = true;
	}

	// 准备 NGX 的输入
	ID3D12Resource* ngxColor = nullptr;
	if (viaColorIn) {
		ngxColor = _colorIn;
		Barrier(commandList, colorSource.resource, colorSource.state,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		// 跨 sRGB 变体的拷贝是按位的，不做转换 —— 同族即可，见
		// FormatsCopyCompatible 上面的说明
		commandList->CopyResource(_colorIn, colorSource.resource);
		Barrier(commandList, _colorIn, D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	} else {
		// 源是我们自己的纹理（上一级滤镜的输出），NGX 可以直接读，
		// 省掉一次全分辨率拷贝
		ngxColor = colorSource.resource;
		Barrier(commandList, colorSource.resource, colorSource.state,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	// NGX evaluate
	NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
	eval.Feature.pInColor = ngxColor;
	eval.Feature.pInOutput = _output;
	eval.Feature.InSharpness = _settings.sharpness;
	eval.pInDepth = _zeroDepth;
	eval.pInMotionVectors = _zeroMotion;
	// 注入式改不了游戏的投影矩阵，所以没有 jitter。这是已知的质量上限来源。
	eval.InJitterOffsetX = 0.0f;
	eval.InJitterOffsetY = 0.0f;
	eval.InMVScaleX = 1.0f;
	eval.InMVScaleY = 1.0f;
	eval.InRenderSubrectDimensions = { _renderWidth, _renderHeight };
	eval.InReset = _needsReset ? 1 : 0;

	D5_STAGE(SrEvaluate);
	const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(
		commandList, _feature, _parameters, &eval);
	if (NVSDK_NGX_FAILED(result)) {
		commandList->Close();
		D5_LOG_ERROR(L"NGX_D3D12_EVALUATE_DLSS_EXT failed: 0x%08X", (unsigned)result);
		Fail("NGX evaluate failed");
		return false;
	}
	_needsReset = false;

	// output -> 目标。源和目标同一块时它此刻在 COPY_SOURCE（上面拷走过），
	// 否则还在调用方给的状态上。
	Barrier(commandList, _output,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Barrier(commandList, dest.resource,
		aliased ? D3D12_RESOURCE_STATE_COPY_SOURCE : dest.state,
		D3D12_RESOURCE_STATE_COPY_DEST);
	commandList->CopyResource(dest.resource, _output);

	// 还原状态：调用方约定进出状态一致，我们的资源回到初始状态
	Barrier(commandList, dest.resource,
		D3D12_RESOURCE_STATE_COPY_DEST, dest.state);
	Barrier(commandList, _output,
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (!aliased) {
		// 源还给它的主人（代理纹理还给游戏 / 交接纹理还给上一级滤镜）
		Barrier(commandList, colorSource.resource,
			viaColorIn ? D3D12_RESOURCE_STATE_COPY_SOURCE
				: D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			colorSource.state);
	}
	if (viaColorIn) {
		Barrier(commandList, _colorIn,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST);
	}

	if (FAILED(commandList->Close())) {
		Fail("command list close failed");
		return false;
	}

	// 提交到**游戏自己的队列**：这样我们的命令和游戏的渲染天然串行，
	// 不需要额外的跨队列同步。
	ID3D12CommandList* lists[]{ commandList };
	D5_STAGE(SrSubmit);
	_queue->ExecuteCommandLists(1, lists);
	slot.fenceValue = ++_fenceValue;
	_queue->Signal(_fence, slot.fenceValue);

	++_evaluateCount;
	return true;
}

}  // namespace DXL
