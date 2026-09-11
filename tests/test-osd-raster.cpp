#include <filesystem>
// OSD 面板长什么样 —— 存成 BMP 让人用眼睛看。
//
// 为什么需要这个：面板画在被注入的游戏进程里，那个环境下截图很不可靠
// （flip 模型的 swapchain，DWM 不一定把内容交给 PrintWindow；实测抓到的是壁纸）。
// 而"面板画得对不对"是纯 CPU 的事，完全可以在这里定死。
//
// 编译运行：OS temporary directory: osd-*.bmp

#include <cstdio>
#include <string>
#include <vector>
#include "../src/core/OsdRaster.h"

using DXL::OsdPanelStyle;

namespace {

// 32bpp BMP。顶向下的 DIB 数据写进 BMP 要么翻过来、要么用负高度 —— 用负高度，
// 少一次拷贝，而且所有看图软件都认。
bool WriteBmp(const char* path, const uint8_t* bgra,
	uint32_t stride, uint32_t width, uint32_t height) {
	FILE* file = nullptr;
	if (fopen_s(&file, path, "wb") != 0 || !file) return false;

	const uint32_t pixelBytes = width * height * 4;
	const uint32_t headerBytes = 14 + 40;
	uint8_t header[54]{};
	header[0] = 'B'; header[1] = 'M';
	*reinterpret_cast<uint32_t*>(header + 2) = headerBytes + pixelBytes;
	*reinterpret_cast<uint32_t*>(header + 10) = headerBytes;
	*reinterpret_cast<uint32_t*>(header + 14) = 40;
	*reinterpret_cast<int32_t*>(header + 18) = (int32_t)width;
	*reinterpret_cast<int32_t*>(header + 22) = -(int32_t)height;   // 顶向下
	*reinterpret_cast<uint16_t*>(header + 26) = 1;
	*reinterpret_cast<uint16_t*>(header + 28) = 32;
	*reinterpret_cast<uint32_t*>(header + 34) = pixelBytes;
	fwrite(header, 1, sizeof(header), file);

	for (uint32_t y = 0; y < height; ++y) {
		fwrite(bgra + size_t(y) * stride, 1, size_t(width) * 4, file);
	}
	fclose(file);
	return true;
}

struct Case {
	const char* file;
	const wchar_t* text;
};

}  // namespace

int wmain() {
    const auto output = std::filesystem::temp_directory_path() / (L"dxl-raster-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(output);
    SetCurrentDirectoryW(output.c_str());
	SetConsoleOutputCP(CP_UTF8);

	const Case cases[] = {
		{ "osd-injected.bmp",
		  L"DLSS5 Quick 已注入 · 3840x2160 · DLSSNR 已开" },
		{ "osd-off.bmp",
		  L"DLSS5 Quick：效果已关（hook 还在，按一下又能开）" },
		{ "osd-debug.bmp", L"Debug 视图：运动矢量" },
		// 超长文本：要能自动换行并且不越界（面板上限 1024x96）
		{ "osd-long.bmp",
		  L"处理分辨率已改为 60%（像素数 36%）—— DLSSNR 现在跑在更少的像素上，"
		  L"开销大约降到原来的四成，但高于低分辨率奈奎斯特频率的细节搬不回来。" },
	};

	std::vector<uint8_t> pixels(
		size_t(DXL::OSD_MAX_WIDTH) * DXL::OSD_MAX_HEIGHT * 4, 0);
	int failures = 0;
	for (const Case& item : cases) {
		uint32_t width = 0;
		uint32_t height = 0;
		bool fits = false;
		const bool ok = DXL::RasterizeOsdPanel(item.text, pixels.data(),
			DXL::OSD_MAX_WIDTH, DXL::OSD_MAX_HEIGHT, width, height,
			OsdPanelStyle{}, &fits);
		if (!ok) {
			printf("[失败] %s：一个像素都没画出来\n", item.file);
			++failures;
			continue;
		}
		// 面板不该退化成一条线，也不该顶满上限（顶满 = 文字被截断了）
		if (width < 80 || height < 24) {
			printf("[可疑] %s：面板只有 %ux%u，太小了\n", item.file, width, height);
			++failures;
		}
		// **"文字放下了吗"由光栅化自己回答。**
		//
		// 第一版这里扫的是"右侧留白里有没有墨"，结果一点用都没有 —— 故意退回到
		// 有 bug 的宽度计算，它照样报通过。原因是 DT_WORDBREAK 遇到放不下的词会
		// 把整个词换到下一行，那一行在矩形外面被裁掉，右边留白里**根本不会有墨**，
		// 字是凭空少的。所以判据必须来自"量出来的尺寸 vs 面板能给的尺寸"，
		// 也就是 RasterizeOsdPanel 回填的 didFit。
		if (!fits) {
			printf("[失败] %s：文字放不进面板（%ux%u）—— 会少画几个字\n",
				item.file, width, height);
			++failures;
		}
		if (!WriteBmp(item.file, pixels.data(),
				DXL::OSD_MAX_WIDTH * 4, width, height)) {
			printf("[失败] %s：写文件失败\n", item.file);
			++failures;
			continue;
		}
		printf("%-38s %ux%u\n", item.file, width, height);
	}

	printf("\n%s —— 用看图软件打开 osd-*.bmp 确认字能看清、"
		"左边那道绿条在、文字没被切掉。\n",
		failures ? "有问题" : "四个都画出来了");
	return failures ? 1 : 0;
}
