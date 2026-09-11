// OSD 面板的 CPU 光栅化。
//
// **单独一个头文件是为了能独立测出来。** 这段是纯 CPU + GDI，画得好不好看只能用
// 眼睛判断，而它跑在被注入的游戏进程里 —— 那个环境下截图很不可靠（flip 模型的
// swapchain，DWM 不一定把内容给 PrintWindow）。抽出来之后
// tests\test-osd-raster.cpp 可以直接调它并存成 BMP。
//
// 输出是 BGRA、顶向下、行距固定 maxWidth*4。面板不透明，所以 alpha 一律 255，
// 调用方不需要做混合（见 Osd.h 顶部对"为什么不混合"的说明）。

#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace DXL {

// 面板尺寸上限。够放两行中文；再大就该做真正的浮层了（那是另一件事）。
// **定义在这里、Osd.h 引用它** —— 两个文件各写一份数字，改一处忘一处是必然的。
constexpr uint32_t OSD_MAX_WIDTH = 1024;
constexpr uint32_t OSD_MAX_HEIGHT = 96;

struct OsdPanelStyle {
	COLORREF background = RGB(0x1c, 0x1c, 0x1c);
	COLORREF edge = RGB(0x76, 0xb9, 0x00);   // NVIDIA 绿
	COLORREF text = RGB(0xf2, 0xf2, 0xf2);
	int fontHeight = -22;                    // 负数 = 按字符高度而不是单元格高度
	int padX = 18;
	int padY = 10;
	int edgeWidth = 4;
	const wchar_t* face = L"Microsoft YaHei UI";
};

// 返回 false = 一个像素都没画出来（调用方应当放弃这次提示）。
inline bool RasterizeOsdPanel(
	const std::wstring& text,
	uint8_t* bgra,
	uint32_t maxWidth,
	uint32_t maxHeight,
	uint32_t& outWidth,
	uint32_t& outHeight,
	const OsdPanelStyle& style = {},
	// 非空时回填"文字有没有完整放进面板"。传空表示不关心。
	bool* didFit = nullptr) noexcept {
	outWidth = outHeight = 0;
	if (!bgra || text.empty() || maxWidth < 64 || maxHeight < 16) return false;

	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = (LONG)maxWidth;
	// 负高度 = 顶向下。**必须顶向下**：D3D12 的纹理行序是从上到下，
	// 默认的底向上 DIB 抄过去会得到一张上下翻转的面板。
	info.bmiHeader.biHeight = -(LONG)maxHeight;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	HDC dc = CreateCompatibleDC(nullptr);
	if (!dc) return false;
	void* bits = nullptr;
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!bitmap || !bits) {
		if (bitmap) DeleteObject(bitmap);
		DeleteDC(dc);
		return false;
	}
	HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

	// 中文必须能显示（提示语是中文的），所以点名雅黑；装不上时 GDI 自己回退，
	// 不会失败，只是字形不同。
	HFONT font = CreateFontW(style.fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
		FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, style.face);
	HGDIOBJ oldFont = SelectObject(dc, font);

	// **先把文字区宽度定死，其余尺寸从它推出来。**
	//
	// 第一版是反过来的：量一次自然宽度 -> 算面板宽度 -> 绘制时用
	// `面板宽度 - padX` 反推文字区。那一步的舍入（再加上往下取偶）把最后一个字
	// 挤了出去 —— 而 DT_WORDBREAK 遇到放不下的词是**整个换到下一行**，那一行又在
	// 矩形外面被裁掉，所以症状是"字凭空少了"：「DLSSNR 已开」变成「DLSSNR 已」。
	// 现在量和画用的是**同一个** textWidth，中间不再有任何换算，没有漂移的余地。
	const LONG maxTextWidth =
		(LONG)maxWidth - style.padX * 2 - style.edgeWidth;
	RECT natural{ 0, 0, maxTextWidth, 0 };
	DrawTextW(dc, text.c_str(), (int)text.size(), &natural,
		DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
	// +2 给字形右侧溢出留一点（斜体/连笔的墨可以超出 advance width）
	const LONG textWidth = (std::min)(natural.right + 2, maxTextWidth);

	// 用最终的文字区宽度再量一次高度 —— 换行位置可能和自然宽度那次不同
	RECT fitted{ 0, 0, textWidth, 0 };
	DrawTextW(dc, text.c_str(), (int)text.size(), &fitted,
		DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

	// CopyTextureRegion 对某些格式的宽高有对齐要求，取偶数 —— **往上取**。
	// 往下取会再削掉 1 px，那正是上面说的那个坑的另一半。
	uint32_t width = (std::min)(
		uint32_t(textWidth + style.padX * 2 + style.edgeWidth), maxWidth);
	uint32_t height = (std::min)(
		uint32_t(fitted.bottom + style.padY * 2), maxHeight);
	width = (std::min)((width + 1u) & ~1u, maxWidth);
	height = (std::min)((height + 1u) & ~1u, maxHeight);

	RECT panel{ 0, 0, (LONG)width, (LONG)height };
	HBRUSH background = CreateSolidBrush(style.background);
	FillRect(dc, &panel, background);
	DeleteObject(background);

	// 左侧那道绿条：一眼认出是本工具的提示，而不是游戏自己的 UI
	RECT edge{ 0, 0, style.edgeWidth, (LONG)height };
	HBRUSH edgeBrush = CreateSolidBrush(style.edge);
	FillRect(dc, &edge, edgeBrush);
	DeleteObject(edgeBrush);

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, style.text);
	// **用 textWidth，不用 width - padX。** 见上面对"字凭空少了"的说明。
	RECT textRect{ style.edgeWidth + style.padX, style.padY,
		style.edgeWidth + style.padX + textWidth,
		style.padY + fitted.bottom };
	DrawTextW(dc, text.c_str(), (int)text.size(), &textRect,
		DT_WORDBREAK | DT_NOPREFIX);

	// DIB 的 32bpp BI_RGB 就是 BGRX，alpha 通道 GDI 不填 —— 我们自己补 255。
	const uint8_t* source = static_cast<const uint8_t*>(bits);
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* srcRow = source + size_t(y) * maxWidth * 4;
		uint8_t* dstRow = bgra + size_t(y) * maxWidth * 4;
		for (uint32_t x = 0; x < width; ++x) {
			dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
			dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
			dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
			dstRow[x * 4 + 3] = 0xFF;
		}
	}

	SelectObject(dc, oldFont);
	DeleteObject(font);
	SelectObject(dc, oldBitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);

	outWidth = width;
	outHeight = height;
	// 放不下就说出来（调用方可以缩字号或截断），别默默少画几个字 ——
	// 那种失败在尺寸、返回值、D3D 调试层上全都看不出来。
	if (didFit) {
		*didFit = uint32_t(fitted.bottom + style.padY * 2) <= maxHeight &&
			natural.right + 2 <= maxTextWidth;
	}
	return width > 0 && height > 0;
}

}  // namespace DXL
