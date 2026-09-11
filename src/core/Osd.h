// 游戏内提示文字（OSD）。注入成功、功能开关这类事件在画面上方正中显示几秒。
//
// 贴图那条路在 `GpuPanel` 里（CPU 画不透明面板 -> 转 backbuffer 格式 -> 一次
// CopyTextureRegion，不读背景所以不需要 UAV）。这个类只管"什么时候显示什么字"。
// 面板的绘制在 `OsdRaster.h`（抽出去是为了能独立测出来）。

#pragma once

#include <d3d12.h>
#include <d3d11.h>
#include <cstdint>
#include <string>

#include "GpuPanel.h"
#include "GpuPanel11.h"
#include "OsdRaster.h"

namespace DXL {

class Osd {
public:
	// 尺寸上限在 OsdRaster.h 里定义（光栅化和上传都要用同一个数字）
	static constexpr uint32_t MAX_WIDTH = OSD_MAX_WIDTH;
	static constexpr uint32_t MAX_HEIGHT = OSD_MAX_HEIGHT;

	bool Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat) noexcept;
	// D3D11 后端（古墓丽影 DX11 那类）。哪个先 Initialize 哪个生效。
	bool Initialize11(ID3D11Device* device, ID3D11DeviceContext* context,
		DXGI_FORMAT backbufferFormat) noexcept;
	void Release() noexcept;

	// 显示一条消息。seconds 秒后自动消失。同一条消息重复调用只是重新计时。
	void Show(const std::wstring& text, double seconds = 3.0) noexcept;

	// 现在还该画吗。false 时调用方连命令列表都不用开。
	bool Visible() const noexcept;

	// 把面板贴到 backbuffer 上。调用方负责先把 target 转成 COPY_DEST。
	void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
		uint32_t targetWidth, uint32_t targetHeight) noexcept;

	// D3D11 版：immediate context 串行、无 barrier，拷贝即时执行。
	void Record11(ID3D11DeviceContext* context, ID3D11Resource* target,
		uint32_t targetWidth, uint32_t targetHeight) noexcept;

private:
	GpuPanel _panel;
	GpuPanel11 _panel11;   // D3D11 后端的贴图层（Initialize11 时启用）
	bool _use11 = false;
	std::wstring _text;
	// 到期时间。用 GetTickCount64 —— 不需要高精度，但要不受系统时间调整影响。
	uint64_t _expireTick = 0;
	bool _uploaded = false;
	uint8_t* _pixels = nullptr;   // GDI 画出来的 BGRA，堆上（1024*96*4 放不进栈）
};

}  // namespace DXL
