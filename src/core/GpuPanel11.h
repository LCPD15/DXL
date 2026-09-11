// GpuPanel 的 D3D11 后端 —— 同一条"CPU 画好的 BGRA 面板 -> backbuffer"路。
//
// 存在的原因：OSD / 游戏内浮层在 D3D11 游戏上也要显示（古墓丽影 DX11 那局，
// DLSSNR 桥接跑通了但提示和浮窗全都没有 —— 之前设计里"D3D11 没有浮层"是
// 阶段性放弃，不是终局）。架构和 D3D12 版对称：
//
//   D3D12 版：上传堆 Map          -> ConvertPixels -> CopyTextureRegion（命令列表）
//   D3D11 版：CPU 缓冲 -> ConvertPixels -> UpdateSubresource 进 DEFAULT 纹理
//             -> CopySubresourceRegion 进 backbuffer
//
// **没有 DYNAMIC 中转**（第一版就是这么写的，实测 E_INVALIDARG 0x80070057：
// DYNAMIC 纹理 BindFlags 不能留 0。而"必须带 bind flag"这条 D3D11 规矩
// 在 DlssNrFilter11 的共享纹理上已经踩过一次）。UpdateSubresource 直写
// DEFAULT 是官方路径，也是 ReShade 的 imgui D3D11 后端（update_texture）
// 用的同一条路 —— D3D12 上传堆的等价物。
//
// 两版共用 GpuPanel::ConvertPixels（10 位铺开 / half 编码全工程只此一份）。
// 面板不透明（和 D3D12 版一样）：不读背景就不需要 SRV/blend。

#pragma once

#include <d3d11.h>
#include <dxgiformat.h>
#include <cstdint>

#include "GpuPanel.h"   // FormatSupported / ConvertPixels（转换全工程只此一份）

namespace DXL {

class GpuPanel11 {
public:
	bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
		DXGI_FORMAT backbufferFormat,
		uint32_t maxWidth, uint32_t maxHeight, const wchar_t* debugName) noexcept;
	void Release() noexcept;
	bool IsReady() const noexcept { return _texture != nullptr; }

	// bgra：顶向下，行距固定 maxWidth*4（和 GDI 的 DIB 一致，同 GpuPanel）。
	bool Upload(const uint8_t* bgra, uint32_t width, uint32_t height) noexcept;

	// 在 immediate context 上录一次拷贝（游戏线程串行，无需 fence）。
	void CopyTo(ID3D11DeviceContext* context, ID3D11Resource* target,
		uint32_t destX, uint32_t destY) noexcept;

	uint32_t Width() const noexcept { return _width; }
	uint32_t Height() const noexcept { return _height; }

private:
	// 转换缓冲的行距（字节）：maxWidth × 转换后每像素字节数
	uint32_t ConvertedPitch() const noexcept { return _maxWidth * _bytesPerPixel; }

	ID3D11Texture2D* _texture = nullptr;   // DEFAULT（UpdateSubresource 写入 + CopySubresourceRegion 的源）
	ID3D11DeviceContext* _context = nullptr;   // immediate（引用自持，Release 时放）
	uint8_t* _converted = nullptr;   // 格式转换的落点（CPU 侧，UpdateSubresource 整体上传）
	DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
	uint32_t _bytesPerPixel = 0;
	uint32_t _maxWidth = 0;
	uint32_t _maxHeight = 0;
	uint32_t _width = 0;
	uint32_t _height = 0;
};

}  // namespace DXL
