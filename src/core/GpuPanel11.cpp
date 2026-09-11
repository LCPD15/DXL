#include "GpuPanel11.h"

#include <cwchar>
#include <new>
#include "GpuPanel.h"
#include "../common/Log.h"

namespace DXL {

bool GpuPanel11::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
	DXGI_FORMAT backbufferFormat,
	uint32_t maxWidth, uint32_t maxHeight, const wchar_t* debugName) noexcept {
	Release();
	if (!device || !context || !GpuPanel::FormatSupported(backbufferFormat) ||
		!maxWidth || !maxHeight) {
		D5_LOG_WARN(L"面板(D3D11)：backbuffer 格式 %u 不在支持列表里，这个功能关掉。"
			L"（支持 RGBA8 / BGRA8 / RGB10A2 / RGBA16F）", (unsigned)backbufferFormat);
		return false;
	}
	_format = backbufferFormat;
	_bytesPerPixel = GpuPanel::BytesPerPixelFor(backbufferFormat);
	_maxWidth = maxWidth;
	_maxHeight = maxHeight;
	_context = context;
	_context->AddRef();   // 引用由我们持有，Release() 时放

	// 一张 DEFAULT 纹理：UpdateSubresource 写入 + CopySubresourceRegion 的源。
	// **不走 DYNAMIC 中转**（第一版实测 E_INVALIDARG：DYNAMIC 的 bind 不能留 0。
	// 这条 D3D11 规矩在 DlssNrFilter11 的共享纹理上已经踩过一次）。UpdateSubresource
	// 直写 DEFAULT 是官方路径，也是 ReShade 的 imgui D3D11 后端同一条路
	//（见 GpuPanel11.h 顶部的说明）。
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = maxWidth;
	desc.Height = maxHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = backbufferFormat;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	HRESULT hr = device->CreateTexture2D(&desc, nullptr, &_texture);
	if (FAILED(hr)) {
		D5_LOG_WARN(L"面板(D3D11)：纹理创建失败 0x%08X，这个功能关掉。", hr);
		Release();
		return false;
	}
	if (debugName) {
		// 调试层点名用；没开调试层就是无操作
		const UINT bytes = UINT(wcslen(debugName) * sizeof(wchar_t));
		_texture->SetPrivateData(WKPDID_D3DDebugObjectNameW, bytes, debugName);
	}
	// 格式转换的 CPU 落点：转换不能就地（GDI 的 BGRA 行距和目标行距不同，
	// RGBA16F 一行还是 8 字节）—— 先转进这块再 UpdateSubresource 整体上传
	_converted = new (std::nothrow) uint8_t[size_t(ConvertedPitch()) * maxHeight];
	if (!_converted) {
		D5_LOG_WARN(L"面板(D3D11)：转换缓冲分配失败，这个功能关掉。");
		Release();
		return false;
	}
	return true;
}

void GpuPanel11::Release() noexcept {
	if (_texture) { _texture->Release(); _texture = nullptr; }
	if (_context) { _context->Release(); _context = nullptr; }
	delete[] _converted;
	_converted = nullptr;
	_width = _height = 0;
	_bytesPerPixel = 0;
}

bool GpuPanel11::Upload(const uint8_t* bgra, uint32_t width, uint32_t height) noexcept {
	if (!_texture || !_context || !_converted || !bgra || !width || !height) return false;
	if (width > _maxWidth || height > _maxHeight) return false;

	// 1) CPU 侧转换：GDI 的 BGRA（行距 maxWidth*4）-> backbuffer 格式。
	//    逐像素转换共用 GpuPanel 的那一份（10 位铺开 / half 编码全工程只此一份）。
	const uint32_t dstPitch = ConvertedPitch();
	GpuPanel::ConvertPixels(bgra, _maxWidth * 4, _converted, dstPitch,
		width, height, _format);

	// 2) 整体上传进 DEFAULT 纹理。行距按**转换后**的字节数
	//   （UpdateSubresource 的 SysMemPitch 是源数据的行距）。
	_context->UpdateSubresource(_texture, 0, nullptr, _converted, dstPitch, 0);

	_width = width;
	_height = height;
	return true;
}

void GpuPanel11::CopyTo(ID3D11DeviceContext* context, ID3D11Resource* target,
	uint32_t destX, uint32_t destY) noexcept {
	// 用传入的 context（present hook 的调用线程上那个），不是构造时的 ——
	// 两个是同一个 immediate context，但别让调用方操心我们存了谁。
	if (!context || !target || !_texture || !_width || !_height) return;
	// 只拷实际内容那一块（纹理按 max 建的，右下留白）
	D3D11_BOX box{};
	box.left = 0;
	box.top = 0;
	box.front = 0;
	box.right = _width;
	box.bottom = _height;
	box.back = 1;
	context->CopySubresourceRegion(target, 0, destX, destY, 0,
		_texture, 0, &box);
}

}  // namespace DXL
