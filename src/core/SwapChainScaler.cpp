#include "SwapChainScaler.h"

#include "../common/Log.h"

namespace DXL {

namespace {

// 代理纹理要能被游戏当 render target 用，也要能被 NGX 当输入读，所以
// RENDER_TARGET 标志要给、DENY_SHADER_RESOURCE 不能给。
// 初始状态用 COMMON：D3D12_RESOURCE_STATE_PRESENT 和 COMMON 是同一个值（0），
// 游戏第一帧会写 StateBefore=PRESENT 的 barrier，正好对得上。
ID3D12Resource* CreateProxyBuffer(
	ID3D12Device* device,
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT format,
	uint32_t index) noexcept {
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
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	// 不给 optimized clear value：游戏会清成任意颜色，给了反而每次
	// ClearRenderTargetView 都会被调试层警告"清屏值不匹配"。真 swapchain 的
	// backbuffer 本来也没有这个值，保持一致。
	ID3D12Resource* resource = nullptr;
	const HRESULT hr = device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
		nullptr, IID_PPV_ARGS(&resource));
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"代理 backbuffer %u (%ux%u fmt=%u) 创建失败: 0x%08X",
			index, width, height, (unsigned)format, hr);
		return nullptr;
	}
	wchar_t name[64]{};
	_snwprintf_s(name, _TRUNCATE, L"D5Q.ProxyBackBuffer[%u]", index);
	resource->SetName(name);
	return resource;
}

}  // namespace

SwapChainScaler::~SwapChainScaler() {
	Teardown();
}

void SwapChainScaler::Teardown() noexcept {
	for (uint32_t i = 0; i < MAX_BUFFERS; ++i) {
		if (_buffers[i]) {
			_buffers[i]->Release();
			_buffers[i] = nullptr;
		}
	}
	_bufferCount = 0;
	_proxyWidth = _proxyHeight = 0;
	_targetWidth = _targetHeight = 0;
}

bool SwapChainScaler::Setup(
	ID3D12Device* device,
	uint32_t bufferCount,
	DXGI_FORMAT format,
	uint32_t targetWidth,
	uint32_t targetHeight,
	float multiplier) noexcept {
	Teardown();

	if (!device || !bufferCount || bufferCount > MAX_BUFFERS ||
		!targetWidth || !targetHeight) {
		D5_LOG_WARN(L"代理 backbuffer 参数不合法: count=%u %ux%u",
			bufferCount, targetWidth, targetHeight);
		return false;
	}
	// 太接近 1 就没意义了，还不如走 DLAA 少一层代理
	if (multiplier < 0.25f || multiplier > 0.95f) {
		D5_LOG_INFO(L"渲染倍率 %.2f 不在代理的有效区间 [0.25, 0.95]，不启用真超分",
			multiplier);
		return false;
	}

	// 对齐到偶数：奇数尺寸在很多后处理和 DLSS 的内部分块上都容易出问题
	const uint32_t proxyWidth =
		(uint32_t)(targetWidth * multiplier + 0.5f) & ~1u;
	const uint32_t proxyHeight =
		(uint32_t)(targetHeight * multiplier + 0.5f) & ~1u;
	if (proxyWidth < 64 || proxyHeight < 64) {
		D5_LOG_WARN(L"代理尺寸 %ux%u 太小，不启用", proxyWidth, proxyHeight);
		return false;
	}

	for (uint32_t i = 0; i < bufferCount; ++i) {
		_buffers[i] = CreateProxyBuffer(
			device, proxyWidth, proxyHeight, format, i);
		if (!_buffers[i]) {
			Teardown();
			return false;
		}
	}

	_bufferCount = bufferCount;
	_proxyWidth = proxyWidth;
	_proxyHeight = proxyHeight;
	_targetWidth = targetWidth;
	_targetHeight = targetHeight;
	D5_LOG_INFO(L"真超分已启用：游戏渲染 %ux%u -> 呈现 %ux%u（%u 个代理 buffer）",
		proxyWidth, proxyHeight, targetWidth, targetHeight, bufferCount);
	return true;
}

}  // namespace DXL
