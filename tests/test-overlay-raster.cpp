#include <filesystem>
// 浮层长什么样 —— 存成 BMP 让人用眼睛看。
//
// 和 test-osd-raster.cpp 同一个理由：浮层画在被注入的游戏进程里，那个环境下截图
// 很不可靠（flip 模型的 swapchain，DWM 不一定把内容交给 PrintWindow，实测抓到壁纸）。
// 而"布局对不对、字有没有被切、控件画在哪"是纯 CPU 的事，可以在这里定死。
//
// 顺带查两件只能自动查的事：
//   1. 面板高度有没有顶到上限（顶满 = 底下的内容被裁掉了）
//   2. **命中测试和绘制用的是不是同一份矩形** —— 对每个可交互 item，
//      拿它自己矩形的中心点去 hit test，必须回到它自己。这条能抓住
//      "看到的和点到的不一致"，那是这类 UI 最经典也最难查的 bug。
//
// 编译运行：OS temporary directory: overlay-*.bmp

#include <cstdio>
#include <vector>
#include "../src/core/OverlayRaster.h"

namespace {

bool WriteBmp(const char* path, const uint8_t* bgra,
	uint32_t stride, uint32_t width, uint32_t height) {
	FILE* file = nullptr;
	if (fopen_s(&file, path, "wb") != 0 || !file) return false;
	const uint32_t pixelBytes = width * height * 4;
	uint8_t header[54]{};
	header[0] = 'B'; header[1] = 'M';
	*reinterpret_cast<uint32_t*>(header + 2) = 54 + pixelBytes;
	*reinterpret_cast<uint32_t*>(header + 10) = 54;
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

using namespace DXL;

OverlayState MakeState(bool warnNoMotion, int timingLevel) {
	OverlayState state;
	state.values[int(OverlaySetting::DebugDepth)] = 0.0f;
	state.values[int(OverlaySetting::DebugMotion)] = warnNoMotion ? 1.0f : 0.0f;
	state.values[int(OverlaySetting::Preset)] = 0.0f;
	state.values[int(OverlaySetting::Style)] = 2.0f;
	state.values[int(OverlaySetting::Intensity)] = 1.3f;
	state.values[int(OverlaySetting::LocalTone)] = 1.0f;
	state.values[int(OverlaySetting::LocalStructure)] = 1.15f;
	state.values[int(OverlaySetting::SkinStructure)] = -1.0f;
	state.values[int(OverlaySetting::AutoMask)] = 0.0f;
	state.values[int(OverlaySetting::UiCorrection)] = 0.0f;
	state.values[int(OverlaySetting::RenderScale)] = warnNoMotion ? 0.6f : 1.0f;

	state.timingMain = timingLevel == 0 ? L"3.42 ms"
		: timingLevel == 1 ? L"6.80 ms" : L"12.40 ms";
	state.timingSub = L"最差 5.1 ms · 占 60fps 预算 20%";
	state.timingLevel = timingLevel;

	if (warnNoMotion) {
		state.info.push_back({ L"当前用的是零矢量，DLSSNR 的时域累积没在工作 —— "
			L"要拿到原生矢量，请从工具启动游戏，或者打开「旁听」再启动游戏。", true });
	}
	state.info.push_back({ L"开销只跟像素数走，和强度无关：强度归零画面不变，"
		L"帧数照样掉。想省性能只能降「处理分辨率」。", false });
	state.info.push_back({ L"这里改的参数立即生效，但不会保存 —— "
		L"关掉游戏就回到工具里的设置。", false });
	return state;
}

struct Case { const char* file; bool warn; int level; };

}  // namespace

int wmain() {
    const auto output = std::filesystem::temp_directory_path() / (L"dxl-raster-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(output);
    SetCurrentDirectoryW(output.c_str());
	SetConsoleOutputCP(CP_UTF8);
	const Case cases[] = {
		{ "overlay-normal.bmp", false, 0 },
		{ "overlay-warn.bmp",   true,  2 },
	};

	std::vector<uint8_t> pixels(
		size_t(OVERLAY_WIDTH) * OVERLAY_MAX_HEIGHT * 4, 0);
	int failures = 0;

	for (const Case& item : cases) {
		OverlayState state = MakeState(item.warn, item.level);
		// 让第三个可交互项处于 hover，顺带把 hover 底色也画出来看看
		std::vector<OverlayItem> items;
		uint32_t width = 0, height = 0;
		LayoutOverlay(state, items, width, height);
		for (size_t i = 0; i < items.size(); ++i) {
			if (items[i].kind == OverlayItemKind::Slider) { state.hotIndex = (int)i; break; }
		}
		state.cursor = { 300, 300 };
		LayoutOverlay(state, items, width, height);

		if (!RasterizeOverlay(state, items, width, height, pixels.data())) {
			printf("[失败] %s：一个像素都没画出来\n", item.file);
			++failures;
			continue;
		}

		// 1. 顶到上限 = 底下被裁了
		if (height >= OVERLAY_MAX_HEIGHT) {
			printf("[失败] %s：面板高度顶到上限 %u —— 底部内容会被裁掉\n",
				item.file, OVERLAY_MAX_HEIGHT);
			++failures;
		}

		// 2. **命中测试 vs 绘制用的是同一份矩形吗。**
		//    拿每个可交互 item 自己矩形的中心去 hit test，必须回到自己。
		//    这条能抓住"看到的和点到的不一致"—— 这类 UI 最难查的 bug。
		for (size_t i = 0; i < items.size(); ++i) {
			const OverlayItem& it = items[i];
			const bool interactive =
				it.kind == OverlayItemKind::Toggle ||
				it.kind == OverlayItemKind::Slider ||
				it.kind == OverlayItemKind::Select ||
				it.kind == OverlayItemKind::Close;
			if (!interactive) continue;
			const RECT& area =
				it.kind == OverlayItemKind::Close ? it.control : it.row;
			POINT center{ (area.left + area.right) / 2,
				(area.top + area.bottom) / 2 };
			const int hit = OverlayHitTest(items, center, height);
			if (hit != (int)i) {
				printf("[失败] %s：item %zu（%ls）的中心点命中到了 %d —— "
					"命中测试和布局对不上\n", item.file, i,
					it.label ? it.label : L"(无名)", hit);
				++failures;
			}
		}

		// 3. 滑条取值映射：左端 = min，右端 = max
		for (const OverlayItem& it : items) {
			if (it.kind != OverlayItemKind::Slider) continue;
			const float atLeft = OverlaySliderValueAt(it, it.control.left);
			const float atRight = OverlaySliderValueAt(it, it.control.right);
			if (atLeft > it.minValue + 1e-3f || atRight < it.maxValue - 1e-3f) {
				printf("[失败] %s：滑条「%ls」端点映射不对（左 %.3f 右 %.3f，"
					"应为 %.3f..%.3f）\n", item.file, it.label,
					atLeft, atRight, it.minValue, it.maxValue);
				++failures;
			}
		}

		if (!WriteBmp(item.file, pixels.data(), OVERLAY_WIDTH * 4, width, height)) {
			printf("[失败] %s：写文件失败\n", item.file);
			++failures;
			continue;
		}
		printf("%-34s %ux%u，%zu 个 item\n", item.file, width, height, items.size());
	}

	// #82 滚动用例：小可视高度（600）逼出滚动 → 验证 maxScroll 回填、
	// 行平移、可见带命中、滚出带的行不可点、面板铺满可视高。
	{
		OverlayState state = MakeState(false, 0);
		std::vector<OverlayItem> items;
		uint32_t width = 0, height = 0;
		int maxScroll = 0;
		LayoutOverlay(state, items, width, height, 600, &maxScroll);
		if (maxScroll <= 0) {
			printf("[失败] 滚动用例：600 高装下全部内容，maxScroll=%d（应为正）\n",
				maxScroll);
			++failures;
		}
		if (height != 600) {
			printf("[失败] 滚动用例：面板高 %u（应铺满 600）\n", height);
			++failures;
		}
		// 滚到底：布局平移后最后一个 item 必须完整可见
		//（bottom <= 面板高；内容尾有 8px padding，bottom==height-8 也对）
		state.scroll = maxScroll;
		LayoutOverlay(state, items, width, height, 600, &maxScroll);
		const OverlayItem& last = items.back();
		if (last.kind != OverlayItemKind::Title &&
			(last.row.bottom > (LONG)height || last.row.top < 40)) {
			printf("[失败] 滚动用例：滚到底后末行 top=%ld bottom=%ld，面板高 %u"
				"（底下够不着/上端露空）\n", last.row.top, last.row.bottom, height);
			++failures;
		}
		// 可见带内中心点命中自己；滚出带（钻进标题带底下）的行不可点
		for (size_t i = 0; i < items.size(); ++i) {
			const OverlayItem& it = items[i];
			const bool interactive =
				it.kind == OverlayItemKind::Toggle ||
				it.kind == OverlayItemKind::Check ||
				it.kind == OverlayItemKind::Slider ||
				it.kind == OverlayItemKind::Select ||
				it.kind == OverlayItemKind::Close;
			if (!interactive) continue;
			// 中心取**可见那截**的：跨带行（top<40<bottom）的整行中心可能
			// 落在标题带里 —— 那里按设计不可点，命不中是正确行为不是 bug。
			RECT vis = it.kind == OverlayItemKind::Close ? it.control : it.row;
			if (it.kind != OverlayItemKind::Close) {
				if (vis.top < 40) vis.top = 40;                       // TITLE_H
				if (vis.bottom > (LONG)height) vis.bottom = (LONG)height;
			}
			if (vis.bottom - vis.top < 2) continue;   // 可见截不足 2px：没中心可点
			POINT center{ (vis.left + vis.right) / 2,
				(vis.top + vis.bottom) / 2 };
			const int hit = OverlayHitTest(items, center, height);
			if (hit != (int)i) {
				printf("[失败] 滚动用例：item %zu 可见中心(%ld,%ld)命中 %d —— "
					"平移后看到的和点到的对不上\n", i, center.x, center.y, hit);
				++failures;
			}
		}
		// 滚出带（row.bottom <= TITLE_H）的行：点它自己的中心必须 miss
		for (size_t i = 0; i < items.size(); ++i) {
			const OverlayItem& it = items[i];
			if (it.kind == OverlayItemKind::Close) continue;
			if (it.row.bottom > 40) continue;   // 40 ≈ TITLE_H，只测滚出带的
			POINT probe{ (it.row.left + it.row.right) / 2,
				(it.row.top + it.row.bottom) / 2 };
			if (probe.y < 0) continue;
			if (OverlayHitTest(items, probe, height) >= 0) {
				printf("[失败] 滚动用例：滚出带 item %zu 还能被点中 —— "
					"该不可见不可点\n", i);
				++failures;
			}
		}
		// 画一张滚到底的 BMP 人眼复核
		if (!RasterizeOverlay(state, items, width, height, pixels.data()) ||
			!WriteBmp("overlay-scrolled.bmp", pixels.data(),
				OVERLAY_WIDTH * 4, width, height)) {
			printf("[失败] 滚动用例：滚到底的 BMP 画不出来\n");
			++failures;
		} else {
			printf("%-34s %ux%u，滚到底（maxScroll=%d）\n",
				"overlay-scrolled.bmp", width, height, maxScroll);
		}
	}

	printf("\n%s —— 打开 overlay-*.bmp 看：耗时在最上、"
		"两个 debug 开关、参数、底部说明，右上角有 X。\n",
		failures ? "有问题" : "都画出来了");
	return failures ? 1 : 0;
}
