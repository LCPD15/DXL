// 最小 D3D12 测试目标，专供验证 core 的 hook 用。
//
// 为什么需要它：注入真实游戏有反作弊风险，而且不可复现。这个程序把 hook 需要的
// 每条路径都走一遍：CreateSwapChainForHwnd（core 靠它抓 command queue）、
// Present、以及一个真实的 D3D12 设备和队列。
//
//   DXL-testapp.exe [--delay <秒>] [--debug] [--novsync] [--format <名>]
//                            [--resize <秒>]
//
// --delay   在创建 swapchain 之前等一会儿，这样可以先注入再创建 —— 用来验证
//           queue 抓取路径（注入晚于 swapchain 创建时抓不到 queue，是已知限制）。
// --debug   开 D3D12 调试层并每帧把 info queue 里的消息打到控制台。注入方改了
//           backbuffer 的资源状态，状态迁移写错了只有调试层会说话，肉眼看不出来。
// --novsync 不等垂直同步，这样帧时间反映真实开销。
// --resize  跑到第 N 秒时调一次 ResizeBuffers，模拟玩家在游戏里改分辨率。
// --recreate-swapchain <秒>
//           跑到第 N 秒时销毁 swapchain 并在同一个 HWND 上重建，模拟玩家在游戏里改
//           画质设置。**重建必须成功** —— 失败说明注入方多持了一份 swapchain 引用。
// --fake-ngx-lookup <秒>
//           跑到第 N 秒时像游戏那样 GetProcAddress 解析 NGX 的 EvaluateFeature，
//           验证注入方的旁听链条接上了没有（拿回的指针必须属于注入方的模块）。
// --fake-game-dlss
//           启动时加载 ngx 目录里的 nvngx_dlss.dll，冒充"游戏自己已经在跑 DLSS"。
//           注入方必须因此**拒绝**初始化它自己的 NGX 会话（否则会把游戏画面弄坏且
//           不可逆）。注意这只验证"检测逻辑"，不验证 NGX 冲突本身 —— 后者没法在不
//           真的弄坏什么的前提下复现。
// --stop-presenting <秒>
//           跑到第 N 秒后**不再调 Present**（但继续抽消息，窗口不会被判无响应）。
//           这是给注入侧的卡顿看门狗准备的夹具：它必须判定成"不是我们"。
//           注入方的真超分需要在游戏建渲染资源之前在场；迟到注入时它靠这条路
//           补救，所以必须能测到。
// --format  backbuffer 格式：unorm（默认）/ bgra / rgb10a2 / rgba16f。
//           后两个是真实游戏开 HDR 时的常见格式，注入方必须都能处理。
//           注意**没有 sRGB 选项**：翻转模型的 swapchain 不接受 *_UNORM_SRGB，
//           而 D3D12 只能用翻转模型，所以 D3D12 的 backbuffer 不可能是 sRGB
//           （实测 CreateSwapChainForHwnd 返回 DXGI_ERROR_INVALID_CALL）。

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

#ifndef DXL_TEST_FRAME_COUNT
#define DXL_TEST_FRAME_COUNT 2
#endif
constexpr UINT FRAME_COUNT = DXL_TEST_FRAME_COUNT;

HWND g_window = nullptr;
IDXGIFactory4* g_factory = nullptr;
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
IDXGISwapChain3* g_swapChain = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12Resource* g_renderTargets[FRAME_COUNT]{};
ID3D12CommandAllocator* g_allocator = nullptr;
ID3D12GraphicsCommandList* g_commandList = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceValue = 0;
ID3D12InfoQueue* g_infoQueue = nullptr;
UINT g_rtvSize = 0;
UINT g_frameIndex = 0;
UINT g_width = 0;
UINT g_height = 0;
bool g_useDebugLayer = false;
UINT g_syncInterval = 1;
int g_resizeAfterSeconds = 0;
int g_stopPresentingAfterSeconds = 0;
int g_recreateAfterSeconds = 0;
bool g_fakeGameDlss = false;
// #84 夹具：冒充"游戏每帧把光标钉死在画面中心"（原神那类 mouselock）。
bool g_pinCursor = false;
int g_fakeNgxLookupAfterSeconds = 0;
bool g_pendingResize = false;
// 两张深度缓冲，专门用来测注入方的深度探测启发式：
//   scene  —— 尺寸等于渲染分辨率，每帧绑定多次，应该被选中
//   shadow —— 正方形、边长 2 的幂，每帧只绑一次，应该被排除（影子贴图的典型特征）
ID3D12Resource* g_sceneDepth = nullptr;
ID3D12Resource* g_shadowDepth = nullptr;
ID3D12DescriptorHeap* g_dsvHeap = nullptr;
UINT g_dsvSize = 0;
constexpr UINT SHADOW_DEPTH_SIZE = 1024;
// 真实游戏在 HDR 下常用 R16G16B16A16_FLOAT 或 R10G10B10A2_UNORM，注入方对这几种
// 都要能处理，所以做成可选。
DXGI_FORMAT g_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
	if (msg == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	// 真实游戏都会这么做：窗口尺寸变了就重建 swapchain 的 buffer。
	// 注入方的真超分在迟到注入时靠这条路补救 —— 它会临时改一下我们的窗口尺寸，
	// 就是为了等这个 ResizeBuffers。所以测试目标必须照做，否则测不到那条路径。
	// 只置标志，真正的重建放到主循环里做，避免在消息处理里重入渲染。
	if (msg == WM_SIZE && w != SIZE_MINIMIZED) {
		g_pendingResize = true;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, w, l);
}

bool Fail(const char* what, HRESULT hr) {
	printf("%s failed: 0x%08X\n", what, hr);
	return false;
}

bool AcquireBuffers();
bool CreateDepthBuffers();
void ReleaseDepthBuffers();
bool ResizeSwapChain();
bool RecreateSwapChain();
void FakeNgxLookup();
void WaitForGpu();

// 把调试层攒下的消息打出来。注入方每帧改 backbuffer 的状态，状态写错了
// 只有这里会报，画面上往往看不出来。
void DrainDebugMessages() {
	if (!g_infoQueue) return;
	const UINT64 count = g_infoQueue->GetNumStoredMessages();
	for (UINT64 i = 0; i < count; ++i) {
		SIZE_T length = 0;
		if (FAILED(g_infoQueue->GetMessage(i, nullptr, &length)) || !length) continue;
		auto* message = static_cast<D3D12_MESSAGE*>(malloc(length));
		if (!message) continue;
		if (SUCCEEDED(g_infoQueue->GetMessage(i, message, &length))) {
			const char* severity =
				message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
				message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" :
				message->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "WARNING" :
				message->Severity == D3D12_MESSAGE_SEVERITY_INFO ? "info" : "message";
			printf("[D3D12 %s] %s\n", severity, message->pDescription);
		}
		free(message);
	}
	g_infoQueue->ClearStoredMessages();
}

bool InitDevice() {
	UINT flags = 0;
	if (g_useDebugLayer) {
		ID3D12Debug* debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
			debug->EnableDebugLayer();
			flags |= DXGI_CREATE_FACTORY_DEBUG;
			debug->Release();
			printf("D3D12 调试层已开启\n");
		} else {
			printf("警告：拿不到 D3D12 调试接口（没装 Graphics Tools？）\n");
		}
	}
	HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&g_factory));
	if (FAILED(hr)) return Fail("CreateDXGIFactory2", hr);

	hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
	if (FAILED(hr)) return Fail("D3D12CreateDevice", hr);

	if (g_useDebugLayer &&
		SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&g_infoQueue)))) {
		// 只在这里打印，不要断到调试器里 —— 我们是在控制台跑的
		g_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
		g_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		// info/message 量太大且没用，过滤掉
		D3D12_MESSAGE_SEVERITY denied[]{
			D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
		// 这条是性能提示不是错误：注入方给的代理 backbuffer 没有 optimized clear
		// value（真 backbuffer 也没有），而我们每帧都用任意颜色 Clear，于是每次
		// 都提示一遍。留着会把真正的 ERROR 埋在几千行噪音里。
		D3D12_MESSAGE_ID deniedIds[]{
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE };
		D3D12_INFO_QUEUE_FILTER filter{};
		filter.DenyList.NumSeverities = _countof(denied);
		filter.DenyList.pSeverityList = denied;
		filter.DenyList.NumIDs = _countof(deniedIds);
		filter.DenyList.pIDList = deniedIds;
		g_infoQueue->PushStorageFilter(&filter);
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue));
	if (FAILED(hr)) return Fail("CreateCommandQueue", hr);
	return true;
}

bool InitSwapChain() {
	RECT client{};
	GetClientRect(g_window, &client);

	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.BufferCount = FRAME_COUNT;
	desc.Width = client.right - client.left;
	desc.Height = client.bottom - client.top;
	g_width = desc.Width;
	g_height = desc.Height;
	desc.Format = g_backBufferFormat;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.SampleDesc.Count = 1;

	// core 就是 hook 这个调用来抓 command queue 的
	IDXGISwapChain1* swapChain1 = nullptr;
	HRESULT hr = g_factory->CreateSwapChainForHwnd(
		g_queue, g_window, &desc, nullptr, nullptr, &swapChain1);
	if (FAILED(hr)) return Fail("CreateSwapChainForHwnd", hr);
	hr = swapChain1->QueryInterface(IID_PPV_ARGS(&g_swapChain));
	swapChain1->Release();
	if (FAILED(hr)) return Fail("QueryInterface(IDXGISwapChain3)", hr);

	// 真实游戏的做法：渲染尺寸取自 swapchain 自己报的 desc，而不是我们请求的值。
	// 注入方的真超分就是靠改这里的返回值让游戏渲染得更小，所以测试目标必须照做，
	// 否则测不到那条路径。
	DXGI_SWAP_CHAIN_DESC1 actual{};
	if (SUCCEEDED(g_swapChain->GetDesc1(&actual))) {
		if (actual.Width != g_width || actual.Height != g_height) {
			printf("swapchain 报告的尺寸是 %ux%u（我们请求的是 %ux%u）—— 按它的来\n",
				actual.Width, actual.Height, g_width, g_height);
		}
		g_width = actual.Width;
		g_height = actual.Height;
	}

	g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.NumDescriptors = FRAME_COUNT;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_rtvHeap));
	if (FAILED(hr)) return Fail("CreateDescriptorHeap", hr);
	g_rtvSize = g_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	if (!AcquireBuffers()) return false;
	if (!CreateDepthBuffers()) return false;

	hr = g_device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator));
	if (FAILED(hr)) return Fail("CreateCommandAllocator", hr);
	hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		g_allocator, nullptr, IID_PPV_ARGS(&g_commandList));
	if (FAILED(hr)) return Fail("CreateCommandList", hr);
	g_commandList->Close();

	hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
	if (FAILED(hr)) return Fail("CreateFence", hr);
	g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	return g_fenceEvent != nullptr;
}

// 夹具：**销毁 swapchain 再在同一个 HWND 上建一个新的**。
//
// 这是真实游戏改画质设置时的动作（鬼武者改 DLSS 就是这么干的），也是最容易被注入方
// 搞坏的一步：DXGI 不允许同一个 HWND 上同时存在两个 swapchain，所以只要还有**任何人**
// 持着旧对象的引用，这里的 CreateSwapChainForHwnd 就会返回
// DXGI_ERROR_INVALID_CALL。注入方多持一份引用（比如为了"记住当前 swapchain"而
// AddRef），游戏就会永远建不出新链、反复重试、画面冻住。
//
// 所以这个夹具的判据很硬：**重建必须成功**。失败就说明注入方漏了引用。
bool RecreateSwapChain() {
	printf("[夹具] 销毁 swapchain 并在同一个 HWND 上重建（模拟游戏改画质设置）\n");
	WaitForGpu();

	// 先放掉我们自己持有的一切 backbuffer 引用 —— 这是游戏侧的义务
	for (UINT i = 0; i < FRAME_COUNT; ++i) {
		if (g_renderTargets[i]) {
			g_renderTargets[i]->Release();
			g_renderTargets[i] = nullptr;
		}
	}
	if (g_swapChain) {
		g_swapChain->Release();
		g_swapChain = nullptr;
	}

	RECT client{};
	GetClientRect(g_window, &client);
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.BufferCount = FRAME_COUNT;
	desc.Width = client.right - client.left;
	desc.Height = client.bottom - client.top;
	desc.Format = g_backBufferFormat;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.SampleDesc.Count = 1;

	IDXGISwapChain1* swapChain1 = nullptr;
	const HRESULT hr = g_factory->CreateSwapChainForHwnd(
		g_queue, g_window, &desc, nullptr, nullptr, &swapChain1);
	if (FAILED(hr)) {
		printf("[夹具] **重建失败: 0x%08X**%s\n", (unsigned)hr,
			hr == DXGI_ERROR_INVALID_CALL
				? "（INVALID_CALL —— 这个 HWND 上还有没被销毁的 swapchain，"
				  "说明有人还持着引用，注入方漏了）"
				: "");
		return false;
	}
	if (FAILED(swapChain1->QueryInterface(IID_PPV_ARGS(&g_swapChain)))) {
		swapChain1->Release();
		return Fail("QueryInterface(IDXGISwapChain3) after recreate", E_FAIL);
	}
	swapChain1->Release();

	DXGI_SWAP_CHAIN_DESC1 actual{};
	if (SUCCEEDED(g_swapChain->GetDesc1(&actual))) {
		g_width = actual.Width;
		g_height = actual.Height;
	}
	g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
	if (!AcquireBuffers()) return false;
	if (!CreateDepthBuffers()) return false;
	printf("[夹具] 重建成功，新尺寸 %ux%u\n", g_width, g_height);
	return true;
}

// 夹具：像游戏那样去解析 NGX 的入口，验证注入方的"旁听"链条真的接上了。
//
// 为什么这样测：真实游戏（鬼武者实测）解析 NGX 全靠 LoadLibrary + GetProcAddress，
// 没有任何静态导入。注入方就是靠 IAT 补丁 GetProcAddress 来把 EvaluateFeature 换成
// 自己的包装的。这里复现的正是那个动作。
//
// 判据很硬：**拿回来的函数指针必须属于注入方的模块**。属于 _nvngx.dll 就说明没接上。
void FakeNgxLookup() {
	// **对多个 snippet 各问一次**，这一点是判据的核心。
	//
	// 每个 NGX snippet（NGX core / dlss / dlssg / dlssd）都各自导出一份
	// `NVSDK_NGX_D3D12_EvaluateFeature`，真实游戏里 Streamline 会逐个解析（鬼武者实测
	// 出现过七个不同的真地址）。注入方如果把它们全存进同一个变量、后写覆盖前写，
	// 就会把"给 A 的包装"转发到 B 的实现上 —— 换了个神经网络处理这一帧，画面被撕成
	// 条带错位。实测踩过。
	//
	// 所以判据是两条：拿回的指针必须属于注入方；而且**不同 snippet 必须拿到不同的
	// 包装**。第二条才是那个 bug 的照妖镜。
	static const wchar_t* const MODULES[]{
		L"_nvngx.dll", L"nvngx_dlss.dll", L"nvngx_dlssg.dll",
	};
	void* seen[ARRAYSIZE(MODULES)]{};
	int found = 0;
	int wrapped = 0;
	bool duplicate = false;

	for (const wchar_t* name : MODULES) {
		const HMODULE h = GetModuleHandleW(name);
		if (!h) continue;
		const FARPROC fn = GetProcAddress(h, "NVSDK_NGX_D3D12_EvaluateFeature");
		if (!fn) continue;

		HMODULE owner = nullptr;
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(fn), &owner);
		wchar_t path[MAX_PATH]{};
		if (owner) GetModuleFileNameW(owner, path, MAX_PATH);
		const wchar_t* leaf = wcsrchr(path, L'\\');
		leaf = leaf ? leaf + 1 : path;
		const bool isOurs = leaf[0] && wcsstr(leaf, L"DXL-core") != nullptr;

		for (int i = 0; i < found; ++i) {
			if (seen[i] == (void*)fn) duplicate = true;
		}
		seen[found++] = (void*)fn;
		if (isOurs) ++wrapped;
		printf("[夹具] %-18ls -> %p (%ls)%s\n", name, (void*)fn,
			leaf[0] ? leaf : L"未知", isOurs ? "  <- 已包装" : "");
	}

	if (!found) {
		printf("[夹具] 进程里没有任何导出 EvaluateFeature 的 NGX 模块 —— "
			"需要注入方开着 DLSS SR 才能测旁听\n");
		return;
	}
	printf("[夹具] 查到 %d 个 snippet，其中 %d 个被包装；重复的包装：%s\n",
		found, wrapped, duplicate ? "**有（这就是那个 bug）**" : "没有");
	printf("[夹具] 旁听是否接上：%s\n",
		(wrapped > 0 && !duplicate) ? "**是**" : (wrapped ? "接上了但转发会错" : "否"));
}

void WaitForGpu() {
	const UINT64 target = ++g_fenceValue;
	g_queue->Signal(g_fence, target);
	if (g_fence->GetCompletedValue() < target) {
		g_fence->SetEventOnCompletion(target, g_fenceEvent);
		WaitForSingleObject(g_fenceEvent, INFINITE);
	}
}

// 取 backbuffer 并建 RTV。注入方的真超分就是在这一步把代理纹理交给我们的，
// 所以 resize 之后必须重新走一遍。
bool AcquireBuffers() {
	D3D12_CPU_DESCRIPTOR_HANDLE handle =
		g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < FRAME_COUNT; ++i) {
		const HRESULT hr = g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
		if (FAILED(hr)) return Fail("GetBuffer", hr);
		g_device->CreateRenderTargetView(g_renderTargets[i], nullptr, handle);
		handle.ptr += g_rtvSize;
	}
	return true;
}

// 模拟玩家在游戏里改分辨率。真实游戏就是这么做的：等 GPU 空闲、放掉所有
// backbuffer 引用、ResizeBuffers、再重新取 buffer 和 desc。

// 建两张深度缓冲和它们的 DSV。不需要 shader —— 只要 OMSetRenderTargets +
// ClearDepthStencilView 就足以让注入方观察到它们。
bool CreateDepthBuffers() {
	ReleaseDepthBuffers();

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.NumDescriptors = 2;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_dsvHeap));
	if (FAILED(hr)) return Fail("CreateDescriptorHeap(DSV)", hr);
	g_dsvSize = g_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clear{};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1.0f;

	D3D12_CPU_DESCRIPTOR_HANDLE dsv =
		g_dsvHeap->GetCPUDescriptorHandleForHeapStart();

	desc.Width = g_width;
	desc.Height = g_height;
	hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&g_sceneDepth));
	if (FAILED(hr)) return Fail("CreateCommittedResource(sceneDepth)", hr);
	g_sceneDepth->SetName(L"TestApp.SceneDepth");
	g_device->CreateDepthStencilView(g_sceneDepth, nullptr, dsv);

	dsv.ptr += g_dsvSize;
	desc.Width = SHADOW_DEPTH_SIZE;
	desc.Height = SHADOW_DEPTH_SIZE;
	hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&g_shadowDepth));
	if (FAILED(hr)) return Fail("CreateCommittedResource(shadowDepth)", hr);
	g_shadowDepth->SetName(L"TestApp.ShadowDepth");
	g_device->CreateDepthStencilView(g_shadowDepth, nullptr, dsv);

	printf("深度缓冲已建：scene %ux%u, shadow %ux%u\n", g_width, g_height,
		SHADOW_DEPTH_SIZE, SHADOW_DEPTH_SIZE);
	return true;
}

void ReleaseDepthBuffers() {
	if (g_sceneDepth) { g_sceneDepth->Release(); g_sceneDepth = nullptr; }
	if (g_shadowDepth) { g_shadowDepth->Release(); g_shadowDepth = nullptr; }
	if (g_dsvHeap) { g_dsvHeap->Release(); g_dsvHeap = nullptr; }
}

bool ResizeSwapChain() {
	WaitForGpu();
	ReleaseDepthBuffers();
	for (UINT i = 0; i < FRAME_COUNT; ++i) {
		if (g_renderTargets[i]) {
			g_renderTargets[i]->Release();
			g_renderTargets[i] = nullptr;
		}
	}

	// 传 0 表示"按窗口客户区来"，这是游戏改分辨率时的常见写法之一
	const HRESULT hr = g_swapChain->ResizeBuffers(
		FRAME_COUNT, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) return Fail("ResizeBuffers", hr);

	// 尺寸重新问一遍 —— 注入方可能在这里报一个更小的代理尺寸
	DXGI_SWAP_CHAIN_DESC1 actual{};
	if (SUCCEEDED(g_swapChain->GetDesc1(&actual))) {
		printf("ResizeBuffers 之后 swapchain 报告 %ux%u（之前是 %ux%u）\n",
			actual.Width, actual.Height, g_width, g_height);
		g_width = actual.Width;
		g_height = actual.Height;
	}
	if (!AcquireBuffers()) return false;
	// 深度缓冲的尺寸跟着渲染分辨率走，所以 resize 之后要重建。
	// 真超分生效时这里建出来的就是代理尺寸 —— 正是注入方需要的那个尺寸。
	if (!CreateDepthBuffers()) return false;
	g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
	return true;
}


void RenderFrame(UINT frame) {
	g_allocator->Reset();
	g_commandList->Reset(g_allocator, nullptr);

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = g_renderTargets[g_frameIndex];
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	g_commandList->ResourceBarrier(1, &barrier);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv =
		g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += SIZE_T(g_frameIndex) * g_rtvSize;

	// 背景：缓慢变化的暗色，一眼能看出还在跑
	const float t = float(frame) * 0.02f;
	const float background[4]{
		0.10f + 0.06f * sinf(t),
		0.10f + 0.06f * sinf(t + 2.09f),
		0.14f + 0.06f * sinf(t + 4.19f),
		1.0f
	};
	g_commandList->ClearRenderTargetView(rtv, background, 0, nullptr);

	// 用带 rect 的 Clear 画图案，这样不用写 shader 和 PSO 也有真实内容。
	// 两件事要靠它验证：
	//   1. 细竖条 —— 高频细节。如果注入方的输出拷贝坏了，画面会变成纯色。
	//   2. 移动方块 —— 我们现在喂给 DLSS 的运动矢量是零，所以 DLSS 会以为画面
	//      静止，移动的方块必然拖影。看到拖影就证明 DLSS 真的在做时域累积。
	static constexpr UINT BAR_COUNT = 32;
	D3D12_RECT bars[BAR_COUNT]{};
	const LONG barStride = LONG(g_width / BAR_COUNT);
	const LONG barWidth = barStride > 3 ? barStride / 3 : 1;
	for (UINT i = 0; i < BAR_COUNT; ++i) {
		bars[i].left = LONG(i) * barStride;
		bars[i].right = bars[i].left + barWidth;
		bars[i].top = LONG(g_height) / 8;
		bars[i].bottom = LONG(g_height) * 3 / 8;
	}
	const float barColor[4]{ 0.95f, 0.95f, 0.95f, 1.0f };
	g_commandList->ClearRenderTargetView(rtv, barColor, BAR_COUNT, bars);

	const LONG blockSize = LONG(g_height) / 8;
	const float phase = 0.5f + 0.5f * sinf(float(frame) * 0.03f);
	const LONG blockLeft = LONG(phase * float(g_width - UINT(blockSize)));
	const LONG blockTop = LONG(g_height) * 5 / 8;
	const D3D12_RECT block{
		blockLeft, blockTop, blockLeft + blockSize, blockTop + blockSize };
	const float blockColor[4]{ 1.0f, 0.85f, 0.20f, 1.0f };
	g_commandList->ClearRenderTargetView(rtv, blockColor, 1, &block);

	// 让注入方的深度探测有东西可看。没有 shader 也能做到：绑定 + 清除就够了。
	// 影子贴图先来，只绑一次；场景深度绑三次（深度预通道 / 主通道 / 后处理），
	// 这正是真实引擎的模式，也是启发式据以区分两者的依据。
	if (g_dsvHeap && g_shadowDepth && g_sceneDepth) {
		D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv =
			g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
		D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = sceneDsv;
		shadowDsv.ptr += g_dsvSize;

		g_commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
		g_commandList->ClearDepthStencilView(
			shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		g_commandList->OMSetRenderTargets(1, &rtv, FALSE, &sceneDsv);
		g_commandList->ClearDepthStencilView(
			sceneDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		g_commandList->OMSetRenderTargets(1, &rtv, FALSE, &sceneDsv);
		g_commandList->OMSetRenderTargets(1, &rtv, FALSE, &sceneDsv);
	}

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	g_commandList->ResourceBarrier(1, &barrier);
	g_commandList->Close();

	ID3D12CommandList* lists[]{ g_commandList };
	g_queue->ExecuteCommandLists(1, lists);

	g_swapChain->Present(g_syncInterval, 0);
	WaitForGpu();
	DrainDebugMessages();
	g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
	// 输出重定向到文件时 CRT 默认全缓冲，而这个程序通常是被强制结束的 ——
	// 缓冲区里的内容会跟着一起丢掉。关掉缓冲，日志才靠得住。
	setvbuf(stdout, nullptr, _IONBF, 0);

	int delaySeconds = 0;
	for (int i = 1; i < argc; ++i) {
		if (_wcsicmp(argv[i], L"--delay") == 0 && i + 1 < argc) {
			delaySeconds = _wtoi(argv[++i]);
		} else if (_wcsicmp(argv[i], L"--debug") == 0) {
			g_useDebugLayer = true;
		} else if (_wcsicmp(argv[i], L"--novsync") == 0) {
			g_syncInterval = 0;
		} else if (_wcsicmp(argv[i], L"--resize") == 0 && i + 1 < argc) {
			g_resizeAfterSeconds = _wtoi(argv[++i]);
		} else if (_wcsicmp(argv[i], L"--stop-presenting") == 0 && i + 1 < argc) {
			g_stopPresentingAfterSeconds = _wtoi(argv[++i]);
		} else if (_wcsicmp(argv[i], L"--recreate-swapchain") == 0 && i + 1 < argc) {
			g_recreateAfterSeconds = _wtoi(argv[++i]);
		} else if (_wcsicmp(argv[i], L"--fake-game-dlss") == 0) {
			g_fakeGameDlss = true;
		} else if (_wcsicmp(argv[i], L"--pin-cursor") == 0) {
			g_pinCursor = true;
		} else if (_wcsicmp(argv[i], L"--fake-ngx-lookup") == 0 && i + 1 < argc) {
			g_fakeNgxLookupAfterSeconds = _wtoi(argv[++i]);
		} else if (_wcsicmp(argv[i], L"--format") == 0 && i + 1 < argc) {
			const wchar_t* name = argv[++i];
			if (_wcsicmp(name, L"bgra") == 0) {
				g_backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			} else if (_wcsicmp(name, L"rgb10a2") == 0) {
				g_backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
			} else if (_wcsicmp(name, L"rgba16f") == 0) {
				g_backBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
			} else if (_wcsicmp(name, L"unorm") != 0) {
				printf("未知的 --format %ls，用默认的 unorm\n", name);
			}
		}
	}

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = L"DXLTestApp";
	RegisterClassExW(&wc);
	g_window = CreateWindowExW(0, wc.lpszClassName,
		L"DXL D3D12 test target", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
		nullptr, nullptr, wc.hInstance, nullptr);
	if (!g_window) return 1;
	ShowWindow(g_window, SW_SHOW);

	// #84 夹具：光标钉线程。**用 GetProcAddress 解析出来的指针调 SetCursorPos**
	// —— 不走本进程的 IAT，注入方打在主 exe 上的指针锁定补丁拦不住它。
	// 这正是原神那局的调用路径（拽光标的调用不从被补丁的那个 IAT 发出），
	// 用来验证虚拟光标（raw 增量）那条兜底路。
	if (g_pinCursor) {
		CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
			const auto pin = reinterpret_cast<BOOL(WINAPI*)(int, int)>(
				GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCursorPos"));
			if (!pin) return 0;
			for (;;) {
				if (g_window) {
					RECT client{};
					if (GetClientRect(g_window, &client) &&
							client.right > client.left) {
						POINT center{ (client.right - client.left) / 2,
							(client.bottom - client.top) / 2 };
						ClientToScreen(g_window, &center);
						pin(center.x, center.y);
					}
				}
				Sleep(8);   // 比 60fps 每帧一次更凶
			}
		}, nullptr, 0, nullptr);
		printf("[夹具] 光标钉线程已启动（每 8ms 钉到客户区中心，GetProcAddress 路径）\n");
	}

	printf("test target pid=%lu\n", GetCurrentProcessId());

	// 夹具：冒充"游戏自己已经在跑 DLSS"。注入方必须因此拒绝初始化它自己的 NGX 会话。
	// 只加载 snippet，不真的建 NGX 会话 —— 我们要测的是注入方的**检测和拒绝**，
	// 而真正的 NGX 冲突没法在不弄坏什么的前提下复现。
	if (g_fakeGameDlss) {
		wchar_t self[MAX_PATH]{};
		GetModuleFileNameW(nullptr, self, MAX_PATH);
		wchar_t* slash = wcsrchr(self, L'\\');
		if (slash) *slash = L'\0';
		wchar_t dll[MAX_PATH]{};
		_snwprintf_s(dll, MAX_PATH, _TRUNCATE, L"%s\\ngx\\nvngx_dlss.dll", self);
		const HMODULE loaded = LoadLibraryW(dll);
		printf("[夹具] 冒充游戏的 DLSS：加载 %ls -> %s\n", dll,
			loaded ? "成功（注入方应当拒绝开自己的 NGX）" : "失败");
	}

	if (!InitDevice()) return 1;

	if (delaySeconds > 0) {
		printf("等待 %d 秒再创建 swapchain —— 现在注入即可验证 queue 抓取\n",
			delaySeconds);
		for (int i = delaySeconds; i > 0; --i) {
			printf("  %d...\n", i);
			// 保持窗口响应，不然会被系统判定无响应
			MSG msg{};
			for (int tick = 0; tick < 100; ++tick) {
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				Sleep(10);
			}
		}
	}

	if (!InitSwapChain()) return 1;
	printf("swapchain 已创建 (%ux%u, vsync=%u)，开始 present 循环。关窗口退出。\n",
		g_width, g_height, g_syncInterval);

	LARGE_INTEGER qpcFreq{}, lastReport{}, startTime{};
	QueryPerformanceFrequency(&qpcFreq);
	QueryPerformanceCounter(&lastReport);
	startTime = lastReport;

	MSG msg{};
	UINT frame = 0;
	UINT framesSinceReport = 0;
	bool stoppedPresenting = false;
	for (;;) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) return 0;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		// 夹具：到点就彻底停止 present，但保持消息循环活着。注入侧看到的现象和
		// "游戏自己卡死了"一样，用来验证看门狗不会把锅扣到自己头上。
		if (g_stopPresentingAfterSeconds > 0) {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const double elapsed =
				double(now.QuadPart - startTime.QuadPart) / double(qpcFreq.QuadPart);
			if (elapsed >= double(g_stopPresentingAfterSeconds)) {
				if (!stoppedPresenting) {
					stoppedPresenting = true;
					printf("[夹具] 第 %.0f 秒：停止调用 Present（消息循环继续）\n",
						elapsed);
				}
				Sleep(16);
				continue;
			}
		}

		RenderFrame(frame++);

		// 每两秒报一次真实帧率。开关注入方的超分时看这个数字的变化，
		// 比看画面更能说明开销。
		if (++framesSinceReport >= 120) {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const double seconds =
				double(now.QuadPart - lastReport.QuadPart) / double(qpcFreq.QuadPart);
			if (seconds > 0.0) {
				printf("frame %u @%ux%u: %.1f fps (%.2f ms)\n", frame,
					g_width, g_height,
					double(framesSinceReport) / seconds,
					seconds * 1000.0 / double(framesSinceReport));
			}
			lastReport = now;
			framesSinceReport = 0;
		}

		// 窗口尺寸变了（可能是注入方为了触发真超分而改的）
		if (g_pendingResize) {
			g_pendingResize = false;
			if (!ResizeSwapChain()) return 1;
		}

		// 到点了就像游戏那样解析一次 NGX 入口，验证旁听链条
		if (g_fakeNgxLookupAfterSeconds > 0) {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const double elapsed =
				double(now.QuadPart - startTime.QuadPart) / double(qpcFreq.QuadPart);
			if (elapsed >= double(g_fakeNgxLookupAfterSeconds)) {
				g_fakeNgxLookupAfterSeconds = 0;   // 只做一次
				FakeNgxLookup();
			}
		}

		// 到点了就销毁 swapchain 重建一次。注入方漏持引用的话这里会失败。
		if (g_recreateAfterSeconds > 0) {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const double elapsed =
				double(now.QuadPart - startTime.QuadPart) / double(qpcFreq.QuadPart);
			if (elapsed >= double(g_recreateAfterSeconds)) {
				g_recreateAfterSeconds = 0;   // 只做一次
				if (!RecreateSwapChain()) {
					printf("[夹具] 重建 swapchain 失败，退出（这就是要抓的 bug）\n");
					return 2;
				}
			}
		}

		// 到点了就改一次分辨率，模拟玩家在游戏设置里重新应用分辨率。
		// 迟到注入的真超分要靠这条路才能生效。
		if (g_resizeAfterSeconds > 0) {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const double elapsed =
				double(now.QuadPart - startTime.QuadPart) / double(qpcFreq.QuadPart);
			if (elapsed >= double(g_resizeAfterSeconds)) {
				printf("--- 第 %d 秒：调用 ResizeBuffers ---\n", g_resizeAfterSeconds);
				g_resizeAfterSeconds = 0;   // 只做一次
				if (!ResizeSwapChain()) return 1;
			}
		}
	}
}
