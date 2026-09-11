#include "Osd.h"

#include <windows.h>
#include <algorithm>
#include "../common/Log.h"

// GDI 用来在 CPU 上画面板和文字（DrawTextW 能画中文，省掉自己搓字形图集）。
// 就地声明依赖，别去改 build.cmd 的公共链接列表 —— 那会让每个目标都拖上 gdi32。
#pragma comment(lib, "gdi32.lib")

namespace DXL {

bool Osd::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat) noexcept {
	Release();
	_use11 = false;
	if (!_panel.Initialize(device, backbufferFormat, MAX_WIDTH, MAX_HEIGHT,
			L"D5Q.Osd.Upload")) {
		return false;
	}
	_pixels = new uint8_t[size_t(MAX_WIDTH) * MAX_HEIGHT * 4]();
	D5_LOG_INFO(L"游戏内提示已就绪（格式 %u，面板上限 %ux%u）",
		(unsigned)backbufferFormat, MAX_WIDTH, MAX_HEIGHT);
	return true;
}

bool Osd::Initialize11(ID3D11Device* device, ID3D11DeviceContext* context,
	DXGI_FORMAT backbufferFormat) noexcept {
	Release();
	_use11 = true;
	if (!_panel11.Initialize(device, context, backbufferFormat, MAX_WIDTH, MAX_HEIGHT,
			L"D5Q.Osd.Upload11")) {
		return false;
	}
	_pixels = new uint8_t[size_t(MAX_WIDTH) * MAX_HEIGHT * 4]();
	D5_LOG_INFO(L"游戏内提示已就绪（D3D11，格式 %u，面板上限 %ux%u）",
		(unsigned)backbufferFormat, MAX_WIDTH, MAX_HEIGHT);
	return true;
}

void Osd::Release() noexcept {
	_panel.Release();
	_panel11.Release();
	_use11 = false;
	delete[] _pixels;
	_pixels = nullptr;
	_uploaded = false;
	_expireTick = 0;
	_text.clear();
}

void Osd::Show(const std::wstring& text, double seconds) noexcept {
	// 就绪检查按后端选。**第一版这里只查 _panel.IsReady()**（D3D12 后端）——
	// D3D11 路径上 _panel 从没初始化，Show 静默早退：初始化日志全绿
	//（"提示已就绪（D3D11…）"），画面上什么都没有。testapp11 两轮像素分析
	//（OSD 位置扫不到面板色块）钉死的就是这条。
	if (!(_use11 ? _panel11.IsReady() : _panel.IsReady()) || text.empty()) return;
	const uint64_t ms = uint64_t((std::max)(seconds, 0.2) * 1000.0);
	_expireTick = GetTickCount64() + ms;
	if (text == _text && _uploaded) return;   // 同一句话就只是续命，不用重画
	_text = text;
	_uploaded = false;
}

bool Osd::Visible() const noexcept {
	// 就绪检查按后端选（_use11 只在 Initialize 时置，之后不再变）
	const bool ready = _use11 ? _panel11.IsReady() : _panel.IsReady();
	return ready && _expireTick != 0 && GetTickCount64() < _expireTick;
}

void Osd::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
	uint32_t targetWidth, uint32_t targetHeight) noexcept {
	if (!list || !target || !Visible()) return;

	if (!_uploaded) {
		uint32_t width = 0;
		uint32_t height = 0;
		// 真正的绘制在 OsdRaster.h 里 —— 抽出去是为了能独立测出来
		// （tests/test-osd-raster.cpp 把同一段代码的结果存成 BMP 让人用眼睛看）。
		if (!RasterizeOsdPanel(_text, _pixels, MAX_WIDTH, MAX_HEIGHT,
				width, height) ||
			!_panel.Upload(_pixels, width, height)) {
			// 画不出来就别反复试：把到期时间清掉，等下一条消息
			_expireTick = 0;
			return;
		}
		_uploaded = true;
	}
	if (!_panel.Width() || !_panel.Height()) return;
	if (_panel.Width() > targetWidth || _panel.Height() + 24 > targetHeight) return;

	// 上方正中。**距顶 24px 而不是贴顶** —— 很多游戏自己的 HUD 就贴在最上面
	// （任务提示、字幕），压在一起两边都看不清。
	_panel.Record(list, target, (targetWidth - _panel.Width()) / 2, 24);
}

// D3D11 版：与 Record 逐行对应（同 Overlay::Record11 里的说明——
// 贴的签名差太多，硬抽公共层会藏走样，对照着复制反而能看出两边是否同步）。
void Osd::Record11(ID3D11DeviceContext* context, ID3D11Resource* target,
	uint32_t targetWidth, uint32_t targetHeight) noexcept {
	if (!context || !target || !Visible()) return;

	if (!_uploaded) {
		uint32_t width = 0;
		uint32_t height = 0;
		if (!RasterizeOsdPanel(_text, _pixels, MAX_WIDTH, MAX_HEIGHT,
				width, height) ||
			!_panel11.Upload(_pixels, width, height)) {
			// 画不出来就别反复试：把到期时间清掉，等下一条消息
			_expireTick = 0;
			return;
		}
		_uploaded = true;
	}
	if (!_panel11.Width() || !_panel11.Height()) return;
	if (_panel11.Width() > targetWidth || _panel11.Height() + 24 > targetHeight) return;

	// 距顶 24px（同 Record 里的说明）。
	_panel11.CopyTo(context, target, (targetWidth - _panel11.Width()) / 2, 24);
}

}  // namespace DXL
