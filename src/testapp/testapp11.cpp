// D3D11 测试夹具 —— DXL 的 DX11 路径验收目标。
//
// 和 testapp.cpp（D3D12 版）同一套思路，但刻意**不做**这些：
//   · 深度缓冲 —— D3D11 路径不接深度探测（旁听在 D3D11 上无输入，
//     DLSS 只存在于 DX12/Vulkan，D3D11 游戏没有原生 DLSS 可旁听）
//   · 假的 NGX lookup —— 同上，D3D11 游戏不会解析 NGX 函数
//   · 引用泄漏/重建夹具 —— 那些是 D3D12 代理（真超分）的事，
//     D3D11 路径不建代理（代理靠 D3D12 命令队列）
//
// 做的：窗口 / 设备 / immediate context / swapchain / 每帧动画清屏 /
// Present(1, 0)。渲染内容是逐帧变化的动画清屏色 + 一个移动的色块 ——
// 够验证"滤镜真的在处理画面"（每帧内容都不同，静止画面验证不了时域累积）。
//
// 用法：DXL-testapp11.exe [--debuglayer] [--seconds N] [--width W] [--height H]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>   // IDXGISwapChain3::GetCurrentBackBufferIndex（FLIP 轮转用；
                       // 这套 SDK 里 IDXGISwapChain3 声明在 dxgi1_4.h，不在 1_3）
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr UINT FRAME_COUNT = 2;
constexpr wchar_t CLASS_NAME[] = L"D5Q.TestApp11";

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain1* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv[FRAME_COUNT]{};
ID3D11Texture2D* g_pattern = nullptr;   // 动画纹理：每帧 UpdateSubresource 后 CopyResource 进 backbuffer
// CPU 侧动画缓冲。unique_ptr 而不是 vector：这是 C 子集风格的测试目标，
// 每帧整块覆盖，只需要"一块连续内存 + 析构时释放"。
std::unique_ptr<unsigned int[]> g_patternPixels;

HWND g_window = nullptr;
UINT g_width = 1280;
UINT g_height = 720;
bool g_useDebugLayer = false;
bool g_readback = false;         // 诊断：每 N 帧把 backbuffer 回读到 CPU 打像素真值
int g_seconds = 0;               // 0 = 一直跑；>0 = 到点自动退出（自动验收用）
UINT g_syncInterval = 1;
UINT g_frame = 0;
bool g_pendingResize = false;

// ---- 命令行 ----
void ParseArgs(int argc, wchar_t** argv) {
	for (int i = 1; i < argc; ++i) {
		const wchar_t* arg = argv[i];
		if (wcscmp(arg, L"--debuglayer") == 0) g_useDebugLayer = true;
		else if (wcscmp(arg, L"--readback") == 0) g_readback = true;
		else if (wcscmp(arg, L"--seconds") == 0 && i + 1 < argc) g_seconds = _wtoi(argv[++i]);
		else if (wcscmp(arg, L"--width") == 0 && i + 1 < argc) g_width = (UINT)_wtoi(argv[++i]);
		else if (wcscmp(arg, L"--height") == 0 && i + 1 < argc) g_height = (UINT)_wtoi(argv[++i]);
		else if (wcscmp(arg, L"--sync") == 0 && i + 1 < argc) g_syncInterval = (UINT)_wtoi(argv[++i]);
	}
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
	if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
	// 和真实游戏一致：窗口尺寸变了置标志，主循环里重建 buffer。
	// D3D11 路径的 ResizeBuffers 补丁（如果做了）靠这条路生效。
	if (msg == WM_SIZE && w != SIZE_MINIMIZED) { g_pendingResize = true; return 0; }
	return DefWindowProcW(hwnd, msg, w, l);
}

bool Fail(const char* what, HRESULT hr) {
	printf("%s failed: 0x%08X\n", what, hr);
	return false;
}

bool InitWindow(HINSTANCE instance) {
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = CLASS_NAME;
	RegisterClassExW(&wc);

	RECT rect{ 0, 0, (LONG)g_width, (LONG)g_height };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	g_window = CreateWindowExW(0, CLASS_NAME, L"DXL D3D11 TestApp",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		rect.right - rect.left, rect.bottom - rect.top,
		nullptr, nullptr, instance, nullptr);
	if (!g_window) return false;
	// 客户区才是交换链尺寸
	RECT client{};
	GetClientRect(g_window, &client);
	g_width = client.right - client.left;
	g_height = client.bottom - client.top;
	ShowWindow(g_window, SW_SHOW);
	return true;
}

bool InitDevice() {
	UINT flags = 0;
	if (g_useDebugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;

	// 和真实 D3D11 游戏一致的设备创建：feature level 11_0 起步，交给驱动挑。
	// 不指定 adapter（默认适配器）—— 测试"注入方从 swapchain 的 GetDevice 抓
	// ID3D11Device"这条路不依赖任何特定适配器。
	const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		flags, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
	if (FAILED(hr)) return Fail("D3D11CreateDevice", hr);

	// debug 层没装时 HARWARE+DEBUG 会失败，退回无 debug 再试一次
	if (g_useDebugLayer && !g_device) {
		printf("警告：带 debug 层创建失败，退回无 debug 层\n");
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			0, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
		if (FAILED(hr)) return Fail("D3D11CreateDevice(retry)", hr);
	}

	IDXGIDevice* dxgiDevice = nullptr;
	if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
		// 留着以后要枚举适配器用；现在只是确认能拿到
		dxgiDevice->Release();
	}
	return true;
}

// factory 必须从**设备自己的**适配器链拿（IDXGIDevice→GetAdapter→GetParent），
// 不能 CreateDXGIFactory1 造一个独立的。
//
// **D3D11 FLIP 模型的经典坑**：独立 factory 造的 swapchain 在部分驱动上
// CreateSwapChainForHwnd 能成功，之后 GetBuffer 返回 DXGI_ERROR_INVALID_CALL
// （实测正是这个症状：链建出来了、buffer 拿不到）。D3D11 的 FLIP 模型
// 要求 factory 和设备同属一个适配器 —— 标准做法就是这条链。
static IDXGIFactory2* FactoryFromDevice() {
	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* adapter = nullptr;
	IDXGIFactory2* factory = nullptr;
	if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
		SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
		SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
		// 成功
	}
	if (adapter) adapter->Release();
	if (dxgiDevice) dxgiDevice->Release();
	return factory;
}

bool InitSwapChain() {
	// 从设备自己的适配器链拿 factory —— 见 FactoryFromDevice 里的说明
	// （独立 factory 造的链 GetBuffer 会 INVALID_CALL，实测踩过）。
	IDXGIFactory2* factory = FactoryFromDevice();
	if (!factory) return Fail("FactoryFromDevice", E_FAIL);
	HRESULT hr = S_OK;

	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.BufferCount = FRAME_COUNT;
	desc.Width = g_width;
	desc.Height = g_height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	// FLIP_DISCARD：现代 D3D11 游戏的标准选择（BITBLT 是老游戏的）。
	// 两条路注入方都得能挂 —— 先测 FLIP。
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.SampleDesc.Count = 1;

	// **第一个参数必须是 ID3D11Device，不是 immediate context。**
	// 第一版传了 g_context —— DXGI 直接 DXGI_ERROR_INVALID_CALL（0x887A0001），
	// pDevice 要的是设备（D3D12 上是队列，D3D11 上是设备，不是 context）。
	hr = factory->CreateSwapChainForHwnd(g_device, g_window, &desc, nullptr, nullptr,
		&g_swapChain);
	factory->Release();
	if (FAILED(hr)) return Fail("CreateSwapChainForHwnd", hr);

	// 建出来之后立刻把它自己报的 desc 打出来 —— "建链成功但 GetBuffer 拒绝"
	// 这种怪事，第一现场就在这里（尺寸/格式/模型到底长什么样）。
	DXGI_SWAP_CHAIN_DESC1 actual{};
	if (SUCCEEDED(g_swapChain->GetDesc1(&actual))) {
		printf("swapchain: %ux%u fmt=%u effect=%u buffers=%u\n",
			actual.Width, actual.Height, (unsigned)actual.Format,
			(unsigned)actual.SwapEffect, actual.BufferCount);
	} else {
		printf("警告：GetDesc1 失败 —— 链是坏的\n");
		return false;
	}
	// 隔离用：建完立刻试一次 GetBuffer，把"建链"和"拿 buffer"两步的失败分开
	{
		ID3D11Texture2D* probe = nullptr;
		const HRESULT gb = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&probe));
		printf("建链后立刻 GetBuffer(0) = 0x%08X\n", (unsigned)gb);
		if (probe) probe->Release();
		if (FAILED(gb)) return false;
	}
	return true;
}

// FLIP 模型的"当前 backbuffer"索引 —— Present 之后轮转（0↔1）。
// **必须每帧问它**：GetBuffer(0) 永远是 0 号物理 buffer，FLIP 轮转后
// 0 号可能正是正在显示的 front buffer——写它会被驱动丢弃，屏幕上是
// 从没被写过的另一张 buffer 的初始垃圾值（纯深蓝画面事故的根因，
// 回读+当前索引+截屏三证对质钉死：buffer 0 里每帧都是渐变，
// 屏幕却恒定纯色）。D3D12 是 GetCurrentBackBufferIndex，D3D11 的
// FLIP 链同样有（QI IDXGISwapChain3，这套 SDK 里声明在 dxgi1_4.h）。
UINT CurrentBackBufferIndex() {
	IDXGISwapChain3* sc3 = nullptr;
	UINT idx = 0;
	if (SUCCEEDED(g_swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
		idx = sc3->GetCurrentBackBufferIndex();
		sc3->Release();
	}
	return idx;
}

// **FLIP 系模型：每帧只持"当前"buffer 的 RTV，Present 之后按新索引重取。**
// 早期版本注释里"GetBuffer(0) 是 FLIP 标准写法"是错的——那是从 BITBLT
// 时代带过来的惯性；FLIP 必须跟踪轮转（见 CurrentBackBufferIndex）。
bool AcquireBuffers() {
	if (g_rtv[0]) { g_rtv[0]->Release(); g_rtv[0] = nullptr; }
	ID3D11Texture2D* buffer = nullptr;
	HRESULT hr = g_swapChain->GetBuffer(CurrentBackBufferIndex(), IID_PPV_ARGS(&buffer));
	if (FAILED(hr)) {
		printf("GetBuffer(cur) failed: 0x%08X\n", (unsigned)hr);
		return false;
	}
	hr = g_device->CreateRenderTargetView(buffer, nullptr, &g_rtv[0]);
	buffer->Release();
	if (FAILED(hr)) return Fail("CreateRenderTargetView", hr);
	return true;
}

// Present 之后重取 RTV：FLIP 模型下"当前 backbuffer"轮转，
// 继续用旧 RTV 画的就是上一帧的画面（画面会撕裂/冻结）。
// 真实游戏里这一步跟着 Present 走，注入方如果在 Present 里改了状态，
// 这条也验证它没把重取搞坏。
void RefetchAfterPresent() {
	ID3D11Texture2D* buffer = nullptr;
	HRESULT hr = g_swapChain->GetBuffer(CurrentBackBufferIndex(), IID_PPV_ARGS(&buffer));
	if (FAILED(hr)) {
		printf("Present 后 GetBuffer(0) failed: 0x%08X —— 继续用旧 RTV\n", (unsigned)hr);
		return;
	}
	ID3D11RenderTargetView* rtv = nullptr;
	hr = g_device->CreateRenderTargetView(buffer, nullptr, &rtv);
	buffer->Release();
	if (FAILED(hr) || !rtv) {
		printf("Present 后 CreateRenderTargetView failed —— 继续用旧 RTV\n");
		return;
	}
	if (g_rtv[0]) g_rtv[0]->Release();
	g_rtv[0] = rtv;
}

// 动画内容：DEFAULT 纹理，CPU 数据用 UpdateSubresource 上传，再 CopyResource
// 进 backbuffer。
//
// **源纹理不能是 DYNAMIC**：CopyResource 的源/目标都必须是 DEFAULT 或
// IMMUTABLE（MSDN 的硬性要求）。第一版用 DYNAMIC + Map —— Map 写入成功
// 只是 CPU 侧，随后的 CopyResource 在 release 驱动下静默不拷（对照实验
// 钉死：不注入也是一片清屏色，渐变/圆块从来没上过屏）。
// 渐变 + 移动圆块逐帧不同 —— 时序累积需要逐帧不同的输入。
bool CreatePattern() {
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = g_width;
	desc.Height = g_height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;   // CopyResource 的源必须是 DEFAULT
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &g_pattern);
	if (FAILED(hr)) return Fail("CreateTexture2D(pattern)", hr);
	// CPU 侧上传缓冲：每帧 UpdateSubresource 之前在这里画
	g_patternPixels.reset(new unsigned int[size_t(g_width) * g_height]);
	return true;
}

void RenderFrame() {
	// 动画清屏色：HSL 随帧旋转 —— 每帧内容都不同，时序累积才有东西可累积
	const float hue = (g_frame % 360) * 3.14159265f / 180.0f;
	const float r = 0.5f + 0.5f * cosf(hue);
	const float g = 0.5f + 0.5f * cosf(hue + 2.094f);
	const float b = 0.5f + 0.5f * cosf(hue + 4.188f);
	const float clearColor[4]{ r, g, b, 1.0f };
	g_context->ClearRenderTargetView(g_rtv[0], clearColor);

	// 移动色块：动画纹理 + UpdateSubresource 上传 + CopyResource 进 backbuffer。
	// **源纹理是 DEFAULT**（见 CreatePattern 里的说明）—— DYNAMIC 源的
	// CopyResource 在 release 驱动下静默不拷（对照实验钉死过）。
	if (g_pattern && g_patternPixels) {
		// 画一个按圆轨迹移动的亮块 + 全幅渐变（X 方向线性、Y 方向二次）
		const float cx = g_width * (0.5f + 0.35f * cosf(g_frame * 0.02f));
		const float cy = g_height * (0.5f + 0.35f * sinf(g_frame * 0.03f));
		auto* pixel = g_patternPixels.get();
		for (UINT y = 0; y < g_height; ++y) {
			for (UINT x = 0; x < g_width; ++x) {
				const float fx = float(x) / g_width;
				const float fy = float(y) / g_height;
				const float dx = (float)x - cx;
				const float dy = (float)y - cy;
				const float dist = sqrtf(dx * dx + dy * dy);
				unsigned int R = unsigned(255 * fx);
				unsigned int G = unsigned(255 * fy * fy);
				unsigned int B = unsigned(255 * (1.0f - fx));
				if (dist < 64.0f) {
					// 圆块：纯白，边缘软过渡 —— 验证滤镜对高频边缘的处理
					const float t = 1.0f - dist / 64.0f;
					R = unsigned(255 * t + R * (1 - t));
					G = unsigned(255 * t + G * (1 - t));
					B = unsigned(255 * t + B * (1 - t));
				}
				pixel[size_t(y) * g_width + x] =
					0xFF000000u | (B << 16) | (G << 8) | R;
			}
		}
		// CPU 缓冲 → DEFAULT 纹理（UpdateSubresource 接受 DEFAULT），
		// 再拷进 backbuffer 0（FLIP 模型的当前 backbuffer 就是 0 号）
		g_context->UpdateSubresource(g_pattern, 0, nullptr,
			g_patternPixels.get(), g_width * sizeof(unsigned int), 0);
		ID3D11Texture2D* buffer = nullptr;
		// **写"当前"backbuffer**（轮转索引）——写死 0 号就是写 front buffer
		// （纯深蓝画面事故，见 CurrentBackBufferIndex 里的说明）。
		if (SUCCEEDED(g_swapChain->GetBuffer(CurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) {
			g_context->CopyResource(buffer, g_pattern);
			buffer->Release();
		}
		// 拷贝是整块覆盖，清屏色实际被盖掉 —— 但两步都留着：
		// 注入方的滤镜如果只截获了其中一条路径，画面上能看出来
	}
}

// 诊断回读：把 backbuffer 拷到 STAGING 纹理再 Map 到 CPU，打印四角+中心
// 像素真值。"画面没上屏"到底出在哪一段——CopyResource 没落、还是落了
// 只是屏幕没显示——回读值一锤定音（截图只能看后者）。
// 用 STAGING 拷 DEFAULT：CopyResource 的源/目标限制管的是 DYNAMIC，STAGING
// 作为目标是官方指定的回读路径，没有那条静默不拷的问题。
void ReadbackOnce() {
	// 当前索引一并打出来 —— "写的 buffer"和"Present 上屏的 buffer"是不是同一张，
	// 靠这个和屏幕截图对质。回读的也是"当前"那张（和写路径一致），
	// 读出来的就是下一帧要上屏的内容。
	const UINT curIdx = CurrentBackBufferIndex();
	ID3D11Texture2D* buffer = nullptr;
	if (FAILED(g_swapChain->GetBuffer(curIdx, IID_PPV_ARGS(&buffer)))) return;

	D3D11_TEXTURE2D_DESC desc{};
	buffer->GetDesc(&desc);
	D3D11_TEXTURE2D_DESC st{};
	st.Width = desc.Width; st.Height = desc.Height;
	st.MipLevels = 1; st.ArraySize = 1;
	st.Format = desc.Format;
	st.SampleDesc.Count = 1;
	st.Usage = D3D11_USAGE_STAGING;
	st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D* staging = nullptr;
	if (FAILED(g_device->CreateTexture2D(&st, nullptr, &staging))) {
		buffer->Release();
		return;
	}
	g_context->CopyResource(staging, buffer);
	buffer->Release();

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
		const auto* px = static_cast<const unsigned int*>(mapped.pData);
		const UINT w = desc.Width, h = desc.Height;
		auto at = [&](UINT x, UINT y) { return px[size_t(y) * (mapped.RowPitch / 4) + x]; };
		// 期望：渐变（左红右蓝、上亮下暗）+ 移动圆块。清屏色是 hue 旋转的，
		// 和渐变明显不同——四角真值能直接分辨"拷的是哪一版内容"。
		printf("rb f%u (cur=%u): TL=%08X TR=%08X BL=%08X BR=%08X C=%08X\n",
			g_frame, curIdx, at(4, 4), at(w - 5, 4), at(4, h - 5), at(w - 5, h - 5),
			at(w / 2, h / 2));
		g_context->Unmap(staging, 0);
	}
	staging->Release();
}

void Present() {
	// 真实游戏都会先解绑 RTV 再 Present —— 状态留在绑定时某些驱动会报错，
	// 注入方如果在 Present 里临时绑自己的 SRV/UAV，这条也验证它还原了状态
	ID3D11RenderTargetView* nullRtv[1]{ nullptr };
	g_context->OMSetRenderTargets(1, nullRtv, nullptr);
	g_swapChain->Present(g_syncInterval, 0);
	// FLIP 模型：Present 后重取当前 backbuffer 的 RTV（见 AcquireBuffers 里的说明）
	RefetchAfterPresent();
	++g_frame;
}

bool ResizeBuffers() {
	// 真实游戏改分辨率/窗口尺寸的动作。注入方的 ResizeBuffers 补丁
	// （如果做了）靠这条生效；没做也不该崩。
	g_context->OMSetRenderTargets(0, nullptr, nullptr);
	// **必须先释放对 swapchain buffer 的全部引用**（RTV 持着一层）——
	// 留着的话 ResizeBuffers 返回 DXGI_ERROR_INVALID_CALL（实测：
	// 启动时窗口尺寸一变就触发，正好钉死这条）。
	if (g_rtv[0]) { g_rtv[0]->Release(); g_rtv[0] = nullptr; }
	HRESULT hr = g_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) return Fail("ResizeBuffers", hr);
	DXGI_SWAP_CHAIN_DESC1 desc{};
	if (SUCCEEDED(g_swapChain->GetDesc1(&desc))) {
		g_width = desc.Width;
		g_height = desc.Height;
	}
	return AcquireBuffers();
}

}  // namespace

int main() {
	// CONSOLE 子系统 + main()：和 testapp.cpp（D3D12 版）一致 ——
	// printf 心跳直接进控制台，自动验收脚本靠它读。
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	ParseArgs(argc, argv);
	LocalFree(argv);

	if (!InitWindow(instance)) return 1;
	if (!InitDevice()) return 1;
	if (!InitSwapChain()) return 1;
	if (!AcquireBuffers()) return 1;
	if (!CreatePattern()) return 1;

	printf("D3D11 testapp running: %ux%u, sync=%u, seconds=%d\n",
		g_width, g_height, g_syncInterval, g_seconds);

	const ULONGLONG start = GetTickCount64();
	MSG msg{};
	while (true) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) return 0;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (g_pendingResize) {
			g_pendingResize = false;
			if (!ResizeBuffers()) return 1;
			printf("resized to %ux%u\n", g_width, g_height);
		}
		RenderFrame();
		if (g_readback && g_frame % 30 == 0) ReadbackOnce();
		Present();
		if (g_seconds > 0 && GetTickCount64() - start > ULONGLONG(g_seconds) * 1000) {
			printf("time up after %u frames\n", g_frame);
			// **真实游戏的退出路径：DestroyWindow → WM_DESTROY → PostQuitMessage
			// → 消息循环收 WM_QUIT 退出。** 不能从 main 直接 return——那样窗口
			// 没销毁、没有 WM_NCDESTROY，注入方的退出清理触发点（挂在那条
			// 消息上的）整段都验证不到（实测：直接 return 时日志里没有任何
			// "退出清理"条目）。
			DestroyWindow(g_window);
			g_window = nullptr;
			// 泵完剩下的消息直到 WM_QUIT（WM_DESTROY 里 Post 的）
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_QUIT) break;
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			break;
		}
		// 每秒打一次心跳 —— 自动验收脚本靠这行确认进程活着、
		// 注入方靠它对齐"注入后有没有输出"
		static ULONGLONG lastBeat = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - lastBeat >= 1000) {
			lastBeat = now;
			printf("frame %u\n", g_frame);
		}
	}

	if (g_pattern) g_pattern->Release();
	for (UINT i = 0; i < FRAME_COUNT; ++i) {
		if (g_rtv[i]) g_rtv[i]->Release();
	}
	if (g_swapChain) g_swapChain->Release();
	if (g_context) g_context->Release();
	if (g_device) g_device->Release();
	return 0;
}
