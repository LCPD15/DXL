// DlssNrFilter11 的实现。头文件上有这条桥接架构的完整设计说明。

#include "DlssNrFilter11.h"

#include <d3d12.h>
#include <dxgi1_2.h>   // IDXGIResource1（共享句柄）
#include <cstring>

#include "HookTeardown.h"
#include "../common/Log.h"

namespace DXL {

namespace {

// A D3D10-created device can expose D3D11 interfaces while its immediate
// context still uses D3D10 behavior. In that mode CopyResource can silently
// do nothing and Context4::Signal fails. Enter our private D3D11 state for
// bridge commands, then restore the exact game state on every return path.
class ScopedBridgeContext final {
public:
	ScopedBridgeContext(ID3D11DeviceContext1* context, ID3DDeviceContextState* state) noexcept
		: _context(context) {
		if (_context && state) _context->SwapDeviceContextState(state, &_previous);
	}
	~ScopedBridgeContext() {
		if (_previous) {
			_context->SwapDeviceContextState(_previous, nullptr);
			_previous->Release();
		}
	}
	bool Ready() const noexcept { return _previous != nullptr; }
	ScopedBridgeContext(const ScopedBridgeContext&) = delete;
	ScopedBridgeContext& operator=(const ScopedBridgeContext&) = delete;
private:
	ID3D11DeviceContext1* _context = nullptr;
	ID3DDeviceContextState* _previous = nullptr;
};

// 私有 D3D12 设备建在**游戏的适配器**上（不是默认适配器）——
// 多 GPU 机器上建错卡的话共享纹理打开会失败或巨慢。
IDXGIAdapter* AdapterOfD3D11(ID3D11Device* device11) noexcept {
	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* adapter = nullptr;
	if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
		SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
		// GetAdapter 成功；句柄在这里还回
	}
	if (dxgiDevice) dxgiDevice->Release();
	return adapter;   // 失败返回 nullptr（调用方退回默认适配器）
}

bool IsSupportedFormat(DXGI_FORMAT format) noexcept {
	// sRGB 变体一并放行：这条路径只做逐位拷贝（SRGB 只影响采样时的解释，
	// 不影响数据位），共享纹理用同格式建就行。古墓丽影 DX11 的 backbuffer
	// 就是 R8G8B8A8_UNORM_SRGB（fmt 29）——第一版只认了非 sRGB 四种，
	// 每帧刷"D3D11 桥接不支持 backbuffer 格式 29"，滤镜整个不工作。
	return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
		format == DXGI_FORMAT_B8G8R8A8_UNORM ||
		format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
		format == DXGI_FORMAT_R10G10B10A2_UNORM ||
		format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// D3D11 侧创建可共享纹理（MISC SHARED_NTHANDLE）。
//
// 组合要求（第三/四轮实测踩出来的，全是 E_INVALIDARG 0x80070057）：
//   1. **必须带 bind flag**（SHADER_RESOURCE 就行）—— bind 留 0 直接 INVALIDARG；
//   2. **SHARED_NTHANDLE 必须和 SHARED 组合** —— MSDN："Use this flag
//      in combination with the D3D11_RESOURCE_MISC_SHARED flag"，
//      单用 NTHANDLE 一样 INVALIDARG（第四轮：加了 bind flag 还是不行，
//      这一条是组合的硬性要求）。
// 多出的 SRV 能力无副作用（D3D12 侧只 CopyResource，
// NGX 的 SRV/UAV 都在滤镜自己的私有纹理上）。
ID3D11Texture2D* CreateShared11(
	ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
	const wchar_t* name) noexcept {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags =
		D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	ID3D11Texture2D* texture = nullptr;
	const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
	if (FAILED(hr) || !texture) {
		D5_LOG_ERROR(L"共享纹理创建失败 %s (%ux%u fmt=%u): 0x%08X",
			name, width, height, (unsigned)format, hr);
		return nullptr;
	}
	texture->SetPrivateData(
		WKPDID_D3DDebugObjectNameW,
		(UINT)(wcslen(name) * sizeof(wchar_t)), name);
	return texture;
}

// 拿 D3D11 纹理的 NT 共享句柄（IDXGIResource1::CreateSharedHandle）。
HANDLE SharedHandleOf(ID3D11Texture2D* texture) noexcept {
	HANDLE handle = nullptr;
	IDXGIResource1* resource = nullptr;
	if (SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&resource))) && resource) {
		// 句柄只在我们自己的进程里用；权限全给
		resource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			nullptr, &handle);
		resource->Release();
	}
	return handle;
}

}  // namespace

DlssNrFilter11::~DlssNrFilter11() {
	TeardownForExit();
}

void DlssNrFilter11::Fail(const char* what) noexcept {
	// 8 次停用（和 DlssNrFilter::Fail 同一条路）：一次失败可能只是尺寸切换
	// 半路上的暂态，连续失败才是真坏了。不停的话每帧刷一条 error
	//（第三轮实测：90ms 刷了 16 条共享纹理创建失败）。
	if (++_failureCount >= 8) {
		_disabled = true;
		D5_LOG_ERROR(L"D3D11 桥接连续失败，已停用（%hs）", what);
	} else {
		D5_LOG_WARN(L"D3D11 桥接失败（第 %u 次）：%hs", _failureCount, what);
	}
	_lastError = what;
}

bool DlssNrFilter11::TeardownForExit() noexcept {
	// 先滤镜（等它自己的 GPU 活 + 释放 NGX feature —— 挂在私有设备的
	// NGX 会话上），再放共享资源。顺序反了的话滤镜会在已释放的设备上跑。
	if (!WaitForSharedWorkIdle() || !_filter12.TeardownForExit()) {
		D5_LOG_WARN(L"D3D11 bridge exit: GPU work incomplete; retaining bridge resources");
		return false;
	}
	ReleaseShared();
	if (_context4) { _context4->Release(); _context4 = nullptr; }
	if (_state11) { _state11->Release(); _state11 = nullptr; }
	if (_context1) { _context1->Release(); _context1 = nullptr; }
	if (_context) { _context->Release(); _context = nullptr; }
	if (_queue12) { _queue12->Release(); _queue12 = nullptr; }
	if (_device12) { _device12->Release(); _device12 = nullptr; }
	// device11 不放：引用是 core 的 g_state.device11 持有的
	return true;
}

bool DlssNrFilter11::WaitForSharedWorkIdle() noexcept {
	if (!_lastSharedWork) return !_bridgeSyncFailed;
	if ((_device11 && FAILED(_device11->GetDeviceRemovedReason())) ||
		(_device12 && FAILED(_device12->GetDeviceRemovedReason()))) return true;
	if (_bridgeSyncFailed || !_fence11to12) return false;
	ScopedBridgeContext contextScope(_context1, _state11);
	if (!contextScope.Ready()) return false;
	// A last Present is not guaranteed on resize/exit. Submit the immediate
	// context's buffered copy-out and terminal Signal before the bounded wait.
	// This is only a retirement path; steady-state frames do not gain a flush.
	if (_context) _context->Flush();
	const auto done = _fence11to12->GetCompletedValue();
	if (done == UINT64_MAX || done >= _lastSharedWork) return true;
	if (!_retireEvent) _retireEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!_retireEvent) return false;
	ResetEvent(_retireEvent);
	if (FAILED(_fence11to12->SetEventOnCompletion(_lastSharedWork, _retireEvent))) return false;
	WaitForSingleObject(_retireEvent, 100);
	const auto completed = _fence11to12->GetCompletedValue();
	return completed == UINT64_MAX || completed >= _lastSharedWork;
}

void DlssNrFilter11::ReleaseShared() noexcept {
	if (_retireEvent) { CloseHandle(_retireEvent); _retireEvent = nullptr; }
	_lastSharedWork = 0;
	_bridgeSyncFailed = false;
	if (_fence11to12_11) { _fence11to12_11->Release(); _fence11to12_11 = nullptr; }
	if (_fence11to12) { _fence11to12->Release(); _fence11to12 = nullptr; }
	if (_fence12to11_11) { _fence12to11_11->Release(); _fence12to11_11 = nullptr; }
	if (_fence12to11) { _fence12to11->Release(); _fence12to11 = nullptr; }
	if (_color12) { _color12->Release(); _color12 = nullptr; }
	if (_color11) { _color11->Release(); _color11 = nullptr; }
	if (_out12) { _out12->Release(); _out12 = nullptr; }
	if (_out11) { _out11->Release(); _out11 = nullptr; }
	_width = _height = 0;
	_format = DXGI_FORMAT_UNKNOWN;
}

bool DlssNrFilter11::CreateSharedTextures(
	uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept {
	// A private NR fence does not cover the later D3D11 copy-out. Retire both
	// halves before replacing shared images/fences during a resolution change.
	if (!WaitForSharedWorkIdle() || !_filter12.WaitForOwnGpuIdle()) return false;
	ReleaseShared();
	if (!IsSupportedFormat(format)) {
		D5_LOG_WARN(L"D3D11 桥接不支持 backbuffer 格式 %u (%ux%u) —— 不建",
			(unsigned)format, width, height);
		return false;
	}

	/* ---- 共享纹理：D3D11 建、D3D12 开 ---- */
	_color11 = CreateShared11(_device11, width, height, format, L"D5Q.Bridge.Color11");
	_out11 = CreateShared11(_device11, width, height, format, L"D5Q.Bridge.Out11");
	if (!_color11 || !_out11) {
		_lastError = "共享纹理创建失败";
		ReleaseShared();
		return false;
	}
	HANDLE colorHandle = SharedHandleOf(_color11);
	HANDLE outHandle = SharedHandleOf(_out11);
	if (!colorHandle || !outHandle) {
		_lastError = "共享纹理句柄创建失败";
		if (colorHandle) CloseHandle(colorHandle);
		if (outHandle) CloseHandle(outHandle);
		ReleaseShared();
		return false;
	}
	// OpenSharedHandle 是 3 参（NTHandle, riid, ppvObj）—— IID_PPV_ARGS
	// 展开就是 (riid, &obj) 两参，前面再塞一个 __uuidof 就是 4 参，过不了编译
	const HRESULT hrColor = _device12->OpenSharedHandle(
		colorHandle, IID_PPV_ARGS(&_color12));
	const HRESULT hrOut = _device12->OpenSharedHandle(
		outHandle, IID_PPV_ARGS(&_out12));
	CloseHandle(colorHandle);
	CloseHandle(outHandle);
	if (FAILED(hrColor) || FAILED(hrOut) || !_color12 || !_out12) {
		D5_LOG_ERROR(L"D3D12 打开共享纹理失败 color=0x%08X out=0x%08X",
			hrColor, hrOut);
		_lastError = "D3D12 打开共享纹理失败";
		ReleaseShared();
		return false;
	}

	/* ---- 共享 fence：D3D12 建（SHARED flag）、句柄交 D3D11 开 ---- */
	// 两个方向各一条：11→12（D3D11 的拷贝完成）和 12→11（滤镜的活完成）。
	// **跨两个设备的 GPU 时间线只有 fence 能对齐** —— 两个设备各自有各自的
	// 队列域，implicit ordering 不跨设备，这是共享 fence 存在的全部理由。
	ID3D12Fence* fence = nullptr;
	if (FAILED(_device12->CreateFence(
			0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) || !fence) {
		_lastError = "共享 fence（11to12）创建失败";
		ReleaseShared();
		return false;
	}
	_fence11to12 = fence;
	fence = nullptr;
	if (FAILED(_device12->CreateFence(
			0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) || !fence) {
		_lastError = "共享 fence（12to11）创建失败";
		ReleaseShared();
		return false;
	}
	_fence12to11 = fence;

	// 句柄：D3D12 侧 CreateSharedHandle（要 ID3D12Device 上这个方法），
	// D3D11 侧 ID3D11Device5::OpenSharedFence 打开成 ID3D11Fence。
	ID3D11Device5* device5 = nullptr;
	if (FAILED(_device11->QueryInterface(IID_PPV_ARGS(&device5))) || !device5) {
		_lastError = "拿不到 ID3D11Device5（OpenSharedFence）";
		ReleaseShared();
		return false;
	}
	HANDLE fenceHandleA = nullptr;
	HANDLE fenceHandleB = nullptr;
	bool ok = SUCCEEDED(_device12->CreateSharedHandle(
			_fence11to12, nullptr, GENERIC_ALL, nullptr, &fenceHandleA)) &&
		SUCCEEDED(_device12->CreateSharedHandle(
			_fence12to11, nullptr, GENERIC_ALL, nullptr, &fenceHandleB)) &&
		fenceHandleA && fenceHandleB &&
		SUCCEEDED(device5->OpenSharedFence(
			fenceHandleA, IID_PPV_ARGS(&_fence11to12_11))) &&
		SUCCEEDED(device5->OpenSharedFence(
			fenceHandleB, IID_PPV_ARGS(&_fence12to11_11))) &&
		_fence11to12_11 && _fence12to11_11;
	device5->Release();
	if (fenceHandleA) CloseHandle(fenceHandleA);
	if (fenceHandleB) CloseHandle(fenceHandleB);
	if (!ok) {
		_lastError = "D3D11 打开共享 fence 失败";
		ReleaseShared();
		return false;
	}

	// immediate context 的 D3D11.4 接口（Signal/Wait 在 context4 上）
	if (!_context4) {
		if (FAILED(_context->QueryInterface(IID_PPV_ARGS(&_context4))) ||
			!_context4) {
			_lastError = "拿不到 ID3D11DeviceContext4（fence Signal/Wait）";
			ReleaseShared();
			return false;
		}
	}

	_width = width;
	_height = height;
	_format = format;
	D5_LOG_INFO(L"D3D11 桥接共享资源就绪：%ux%u fmt=%u（颜色/输出双纹理 + 双向 fence）",
		width, height, (unsigned)format);
	return true;
}

bool DlssNrFilter11::Initialize(ID3D11Device* device11, HMODULE selfModule) noexcept {
	_device11 = device11;
	if (!_device11) {
		_lastError = "没有 ID3D11Device";
		return false;
	}
	_device11->GetImmediateContext(&_context);
	if (!_context) {
		_lastError = "GetImmediateContext 失败";
		return false;
	}
	// GetImmediateContext already guarantees an immediate context. Do not call
	// other D3D11 context methods until our D3D11 API state has been activated.
	if (FAILED(_context->QueryInterface(IID_PPV_ARGS(&_context1)))) {
		_lastError = "D3D11 immediate context state interface unavailable";
		return false;
	}
	ID3D11Device1* device1 = nullptr;
	if (FAILED(_device11->QueryInterface(IID_PPV_ARGS(&device1)))) {
		_lastError = "D3D11 device context state interface unavailable";
		return false;
	}
	const D3D_FEATURE_LEVEL level = _device11->GetFeatureLevel() > D3D_FEATURE_LEVEL_11_1
		? D3D_FEATURE_LEVEL_11_1 : _device11->GetFeatureLevel();
	const UINT stateFlags = _device11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED
		? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
	const HRESULT stateHr = device1->CreateDeviceContextState(stateFlags, &level, 1,
		D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &_state11);
	device1->Release();
	if (FAILED(stateHr) || !_state11) {
		D5_LOG_ERROR(L"D3D11 bridge context state creation failed: hr=0x%08X level=0x%X flags=0x%X",
			stateHr, unsigned(level), _device11->GetCreationFlags());
		_lastError = "D3D11 bridge context state creation failed";
		return false;
	}
	D5_LOG_INFO(L"D3D11 bridge API state ready: level=0x%X flags=0x%X; scoped D3D11 commands and native state restore",
		unsigned(level), _device11->GetCreationFlags());

	// 私有 D3D12 设备：建在游戏的适配器上。
	// 为什么不是 D3D11On12：桥接要的是"一个能跑 NGX 的独立 D3D12 设备"——
	// dlss5-dx11-bridge 验证过的路径就是这个（游戏镜像到私有 D3D12 会话）。
	IDXGIAdapter* adapter = AdapterOfD3D11(_device11);
	const HRESULT hr = D3D12CreateDevice(
		adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_device12));
	if (adapter) adapter->Release();
	if (FAILED(hr) || !_device12) {
		_lastError = "私有 D3D12 设备创建失败";
		return false;
	}
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(_device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_queue12))) ||
		!_queue12) {
		_lastError = "私有 DIRECT 队列创建失败";
		return false;
	}

	// 滤镜走 D3D12 版整套（snippet 加载 / IAT 伪装 / feature / evaluate）。
	if (!_filter12.Initialize(_device12, _queue12, selfModule)) {
		_lastError = _filter12.LastError();
		return false;
	}
	_initialized = true;
	D5_LOG_INFO(L"D3D11 桥接就绪（私有 D3D12 设备 %p + 队列 %p）",
		(void*)_device12, (void*)_queue12);
	return true;
}

bool DlssNrFilter11::Execute(
	ID3D11Texture2D* backBuffer, const NrSettings& settings) noexcept {
	if (!_initialized || _disabled || !backBuffer) return false;
	ScopedBridgeContext contextScope(_context1, _state11);
	if (!contextScope.Ready()) {
		Fail("D3D11 bridge context state activation failed");
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	backBuffer->GetDesc(&desc);
	// 尺寸/格式变了重建共享纹理（fence 也重建——它们跟尺寸无关但一起干净）
	if (desc.Width != _width || desc.Height != _height || desc.Format != _format) {
		if (!CreateSharedTextures(desc.Width, desc.Height, desc.Format)) {
			return false;
		}
	}

	// 每帧的同步协议。v 是这一帧的 fence 值（两个方向共用一个计数）：
	//
	//   D3D11（immediate context）            D3D12（私有队列）
	//   ─────────────────────────            ─────────────────────────
	//   (1) Copy(backbuffer → 共享颜色)
	//   (2) Signal(f11to12, v)   ──GPU等──>  (3) queue.Wait(f11to12, v)
	//                                          (4) 滤镜：共享颜色 → evaluate → 共享输出
	//                                       (5) queue.Signal(f12to11, v)
	//   (6) Wait(f12to11, v)    <──GPU等──
	//   (7) Copy(共享输出 → backbuffer)
	//
	// 全部 GPU 侧（queue->Wait 是 GPU 等待，context4->Wait 也是），
	// CPU 一次都不等 —— present 线程不等的硬约束。
	// (3)(4)(5) 在同一条 D3D12 队列上天然串行；(6) 保证 (7) 读到的是
	// (4) 写完的东西 —— 两个设备的时间线只有 fence 能对齐。
	const uint64_t v = ++_fenceValue;

	// Odd/even values share the 11->12 fence in strictly increasing order:
	// 2v-1 after copy-in; 2v after copy-out. The even value is the resize/exit proof.
	_context->CopyResource(_color11, backBuffer);
	_lastSharedWork = 2 * v - 1;
	const HRESULT copyInSignal = _context4->Signal(_fence11to12_11, _lastSharedWork);
	if (FAILED(copyInSignal)) {
		if (_failureCount < 8) D5_LOG_WARN(L"D3D11 copy-in Signal: hr=0x%08X device11=0x%08X device12=0x%08X",
			copyInSignal, _device11->GetDeviceRemovedReason(), _device12->GetDeviceRemovedReason());
		_bridgeSyncFailed = true;
		Fail("D3D11 copy-in fence Signal failed");
		return false;
	}

	// (3) 队列等 D3D11 拷完（GPU 侧；CPU 立刻往下走）
	if (FAILED(_queue12->Wait(_fence11to12, 2 * v - 1))) {
		Fail("queue12->Wait(f11to12) 失败");
		return false;
	}

	// (4) 滤镜：Prepare（尺寸/设置变了内部重建 feature，没变秒回）+ Execute。
	// 共享纹理在 D3D12 侧的状态：跨 API 的共享资源从 COMMON 起步
	//（D3D12 的规则），滤镜内部用 barrier 迁移、出去之前还原 ——
	// GpuImage 的进出同状态约定（见 GpuImage.h）就是为这个。
	if (!_filter12.Prepare(_width, _height, _format, settings, NrMode::Present)) {
		Fail("滤镜 Prepare 失败（D3D11 桥接）");
		return false;
	}
	const GpuImage colorSrc{ _color12, D3D12_RESOURCE_STATE_COMMON };
	const GpuImage outDest{ _out12, D3D12_RESOURCE_STATE_COMMON };
	if (!_filter12.Execute(colorSrc, outDest)) {
		Fail("滤镜 Execute 失败（D3D11 桥接）");
		return false;
	}

    // The shared input is captured inside the underlying filter's own list,
    // before NR, and its submitted callback fences that exact GPU copy.

	// (5) 队列发信号（排在已入队的滤镜活后面 —— 同队列串行）
	if (FAILED(_queue12->Signal(_fence12to11, v))) {
		Fail("queue12->Signal(f12to11) 失败");
		return false;
	}

	// (6)(7) D3D11 等滤镜完成，再把结果拷回 backbuffer
	if (FAILED(_context4->Wait(_fence12to11_11, v))) {
		Fail("D3D11 copy-out fence Wait failed");
		return false;
	}
	_context->CopyResource(backBuffer, _out11);
	_lastSharedWork = 2 * v;
	if (FAILED(_context4->Signal(_fence11to12_11, _lastSharedWork))) {
		_bridgeSyncFailed = true;
		Fail("D3D11 copy-out fence Signal failed");
		return false;
	}
	++_evaluateCount;
	return true;
}

}  // namespace DXL
