#pragma once

// 真超分需要的"代理 backbuffer"。
//
// 问题：注入式工具改不了游戏内部的渲染分辨率。游戏问 swapchain 要 backbuffer，
// 就按 backbuffer 的尺寸渲染。所以要让游戏渲染得更小，唯一的办法是**给它一组
// 更小的假 backbuffer**，同时把真 swapchain 留在目标分辨率上：
//
//   真 swapchain   2560x1440  <- DLSS 输出写这里，然后真正 Present
//   代理 buffer     1707x960  <- 游戏以为这就是 backbuffer，在这上面渲染
//
// 具体做法是给 IDXGISwapChain 的 vtable 多打几个补丁：
//   GetBuffer   -> 返回代理纹理而不是真 backbuffer
//   GetDesc(1)  -> 报告代理尺寸，这样游戏的视口/裁剪/后处理都按小尺寸来
//   ResizeBuffers -> 真 swapchain 用目标尺寸，代理重建成缩放后的尺寸
//
// 代价（必须知道）：GetDesc 在撒谎。绝大多数游戏只是拿它当"我的渲染分辨率"，
// 没有问题；但确实存在会因此困惑的游戏。所以 DLAA 模式（不代理、不撒谎）是
// 更安全的默认值，真超分是可选项。

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace DXL {

class SwapChainScaler {
public:
	~SwapChainScaler();

	SwapChainScaler(const SwapChainScaler&) = delete;
	SwapChainScaler& operator=(const SwapChainScaler&) = delete;
	SwapChainScaler() = default;

	// targetWidth/Height 是真 swapchain 的尺寸（也就是游戏要求的那个尺寸）。
	// multiplier 是渲染倍率，代理尺寸 = target * multiplier。
	bool Setup(
		ID3D12Device* device,
		uint32_t bufferCount,
		DXGI_FORMAT format,
		uint32_t targetWidth,
		uint32_t targetHeight,
		float multiplier) noexcept;

	void Teardown() noexcept;

	bool IsActive() const noexcept { return _bufferCount != 0; }

	// 交给游戏的代理纹理。索引越界或没启用时返回 nullptr。
	ID3D12Resource* ProxyBuffer(uint32_t index) const noexcept {
		return index < _bufferCount ? _buffers[index] : nullptr;
	}

	uint32_t ProxyWidth() const noexcept { return _proxyWidth; }
	uint32_t ProxyHeight() const noexcept { return _proxyHeight; }
	uint32_t TargetWidth() const noexcept { return _targetWidth; }
	uint32_t TargetHeight() const noexcept { return _targetHeight; }
	uint32_t BufferCount() const noexcept { return _bufferCount; }

private:
	static constexpr uint32_t MAX_BUFFERS = 8;

	ID3D12Resource* _buffers[MAX_BUFFERS]{};
	uint32_t _bufferCount = 0;
	uint32_t _proxyWidth = 0;
	uint32_t _proxyHeight = 0;
	uint32_t _targetWidth = 0;
	uint32_t _targetHeight = 0;
};

}  // namespace DXL
