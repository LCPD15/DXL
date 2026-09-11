// CPU 画好的一块 BGRA 面板 -> backbuffer。
//
// 从 Osd 里抽出来的，因为浮层（Overlay）要用同一条路。**这条路的全部意义是"不读背景"**：
// 不读背景就不需要在 backbuffer 上建 UAV，而 `R10G10B10A2_UNORM`（实测真游戏就是这个
// 格式）的 typed UAV store 在 D3D12 里是**可选**支持，不保险。
// 于是：CPU 上用 GDI 画不透明面板 -> 转成 backbuffer 的像素格式 -> 一次
// CopyTextureRegion。不需要 shader、不需要根签名、不需要描述符堆。
//
// 代价是面板不透明。这恰好和 NVIDIA / Steam 的浮层长得一样，不算损失。
//
// **格式转换那段是这个文件里最容易写错的部分**（10 位铺开、half 编码），
// 所以只能有一份 —— Osd 和 Overlay 共用它。

#pragma once

#include <d3d12.h>
#include <cstdint>

namespace DXL {

class GpuPanel {
public:
	// 支持的 backbuffer 格式。**不支持就整个功能关掉，别猜着写** ——
	// 写错格式的后果是画面上一块彩色噪声。
	static uint32_t BytesPerPixelFor(DXGI_FORMAT format) noexcept;
	static bool FormatSupported(DXGI_FORMAT format) noexcept {
		return BytesPerPixelFor(format) != 0;
	}

	// BGRA（GDI DIB 顺序）-> backbuffer 格式的逐像素转换。
	// **全工程只有这一份**（10 位铺开、half 编码最容易写错）——
	// D3D11 后端（GpuPanel11）不自己写，调这里的。
	// srcRowPitch/dstRowPitch 都是字节；src 行距按调用方的 GDI 行距传。
	static void ConvertPixels(const uint8_t* bgra, uint32_t srcRowPitch,
		uint8_t* dst, uint32_t dstRowPitch,
		uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept;

	bool Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat,
		uint32_t maxWidth, uint32_t maxHeight, const wchar_t* debugName) noexcept;
	void Release() noexcept;
	bool IsReady() const noexcept { return _upload != nullptr; }

	// bgra：顶向下，行距固定 maxWidth*4（和 GDI 的 DIB 一致）。
	// width/height 是这次实际要贴的尺寸，必须 <= max。
	bool Upload(const uint8_t* bgra, uint32_t width, uint32_t height) noexcept;

	// 往 list 上录一次拷贝。调用方负责把 target 转成 COPY_DEST。
	void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
		uint32_t destX, uint32_t destY) noexcept;

	uint32_t Width() const noexcept { return _width; }
	uint32_t Height() const noexcept { return _height; }

private:
	ID3D12Resource* _upload = nullptr;
	DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
	uint32_t _bytesPerPixel = 0;
	uint32_t _rowPitch = 0;      // 已按 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT 对齐
	uint32_t _maxWidth = 0;
	uint32_t _maxHeight = 0;
	uint32_t _width = 0;
	uint32_t _height = 0;
};

}  // namespace DXL
