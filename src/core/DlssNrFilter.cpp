#include "DlssNrFilter.h"
#include "GpuEventScope.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <algorithm>

#include "../common/Log.h"
#include "FreezeWatchdog.h"
#include "CommandListTracker.h"
#include "NgxParameterBag.h"
#include "D3D12Validation.h"
#include "NrToneMapping.h"
#include "NrColorFormat.h"

namespace DXL {

namespace {

// NGX 里没有 feature 18 的枚举名，只能强转
constexpr NVSDK_NGX_Feature FEATURE_DLSSNR = static_cast<NVSDK_NGX_Feature>(18);
// 签名 snippet 认的 application id
constexpr unsigned long long DLSSNR_APPLICATION_ID = 0x0876232Cull;

/* ---------------- 参数键 ---------------- */
// 这些字符串就是接口本身，别改名也别"整理"

constexpr char PARAM_WIDTH[] = "DLSSNR.Width";
constexpr char PARAM_HEIGHT[] = "DLSSNR.Height";
// 这里以前还有 8 个键：InputWidth / InputHeight / OutputWidth / OutputHeight /
// Output.Width / Output.Height / Upscaling / Scale。**它们全是我们当初猜的名字，
// DLL 里根本没有。**（把 nvngx_dlssnr.dll 的字符串表扫一遍，DLSSNR.* 一共 61 个
// 名字，这 8 个一个都不在里面 —— 见 scripts/scan-dlssnr-keys.py。）
// 设了等于往参数容器里塞垃圾，DLL 永远不会去问。已删。
//
// 尺寸真正靠的是 DLSSNR.Width / Height，加上每个资源自己的
// <资源名>SubrectBaseX/BaseY/Width/Height 一族。
constexpr char PARAM_SCALING_RATIO[] = "DLSSNR.ScalingRatio";
constexpr char PARAM_PRESET[] = "DLSSNR.Hint.Render.Preset";
constexpr char PARAM_COLOR[] = "DLSSNR.Color";
constexpr char PARAM_OUTPUT[] = "DLSSNR.Output";
constexpr char PARAM_MVEC[] = "DLSSNR.MVec";
constexpr char PARAM_DEPTH[] = "DLSSNR.Depth";
constexpr char PARAM_MVEC_SCALE_X[] = "DLSSNR.MVecScaleX";
constexpr char PARAM_MVEC_SCALE_Y[] = "DLSSNR.MVecScaleY";
constexpr char PARAM_DEPTH_INVERTED[] = "DLSSNR.DepthInverted";
constexpr char PARAM_ENABLED[] = "DLSSNR.Enabled";
constexpr char PARAM_RESET[] = "DLSSNR.Reset";
constexpr char PARAM_STYLE[] = "DLSSNR.Style";
constexpr char PARAM_INTENSITY[] = "DLSSNR.Intensity";
constexpr char PARAM_LOCAL_TONE[] = "DLSSNR.LocalToneStrength";
constexpr char PARAM_LOCAL_STRUCTURE[] = "DLSSNR.LocalStructureStrength";
constexpr char PARAM_SKIN_STRUCTURE[] = "DLSSNR.SkinStructureStrength";
constexpr char PARAM_AUTO_MASK[] = "DLSSNR.UseAutoMask";
constexpr char PARAM_UI_CORRECTION[] = "DLSSNR.UICorrection";
constexpr char PARAM_INDICATOR_INVERT_X[] = "DLSS.Indicator.Invert.X.Axis";
constexpr char PARAM_INDICATOR_INVERT_Y[] = "DLSS.Indicator.Invert.Y.Axis";

// 每个资源都有一组子矩形参数。全图处理时也得设，DLL 不会自己推。
//
// **这些名字是从 DLL 自己嘴里问出来的，不是猜的。** 原来写的是
// `DLSSNR.Color.Subrect.Base.X` 这种带点的形式 —— 全部 16 个键**一个都没被读到过**，
// DLL 要的是不带点的 `DLSSNR.ColorSubrectBaseX`。后果：颜色/矢量/深度/输出的处理范围
// 一直是 DLL 的默认值。在 backbuffer 上（整张图、尺寸恰好等于默认）看不出问题，
// 一挪到游戏的 evaluate 点就暴露了：画面一片灰 + 严重拖影。
//
// 找出来的办法是给参数容器加了"记录它问了但我们没有的键"（NgxParameterBag::Miss）。
// 没有公开头文件的 DLL 就该这么摸 —— 让它自己报，别试名字。
struct SubrectKeys {
	const char* baseX;
	const char* baseY;
	const char* width;
	const char* height;
};

constexpr SubrectKeys SUBRECTS[]{
	{ "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
	  "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight" },
	{ "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
	  "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight" },
	{ "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
	  "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight" },
	{ "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
	  "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight" },
};

/* ---------------- snippet 的导出签名 ---------------- */

using SnippetInitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(
	unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
	const NVSDK_NGX_Parameter*);
using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
	ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
	NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
	ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
	const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

/* ---------------- 调用方身份伪装 ---------------- */

// 进程内只允许一份 DLSSNR，所以这几个全局量够用。用原子是因为 snippet 可能在
// 别的线程上调 GetModuleFileNameW。
//
// 要伪装的是**我们自己的模块**，不是 snippet 的：snippet 检查的是"谁在调我"，
// 它拿调用方（也就是 core DLL）的模块句柄去问文件名，要求答案是 nvngx.dll。
// 一开始错按 snippet 自己的句柄来判断，结果伪装从不触发，Init_Ext 返回
// 0xBAD00002（PlatformError）。
std::atomic<HMODULE> g_spoofedCallerModule{ nullptr };
std::atomic<GetModuleFileNameWFn> g_originalGetModuleFileNameW{ nullptr };
// 诊断：伪装命中过几次、总共被问过几次。Init_Ext 失败时这两个数字能直接区分
// "DLL 根本没问"（IAT 补丁没生效）和"问了但还是不满意"（别的原因）。
std::atomic<uint32_t> g_spoofHits{ 0 };
std::atomic<uint32_t> g_spoofCalls{ 0 };

// 问到我们这个模块时回答 nvngx.dll，其他模块的查询原样转发。
DWORD WINAPI SpoofedGetModuleFileNameW(
	HMODULE module, LPWSTR filename, DWORD size) noexcept {
	g_spoofCalls.fetch_add(1, std::memory_order_relaxed);
	if (module && module == g_spoofedCallerModule.load(std::memory_order_acquire)) {
		g_spoofHits.fetch_add(1, std::memory_order_relaxed);
		constexpr wchar_t AUTHORIZED[] = L"nvngx.dll";
		constexpr DWORD AUTHORIZED_LENGTH = ARRAYSIZE(AUTHORIZED) - 1;
		if (!filename || !size) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		// 严格照 Win32 契约来：缓冲区不够时要截断、补 0、置
		// ERROR_INSUFFICIENT_BUFFER 并返回 size
		if (size <= AUTHORIZED_LENGTH) {
			if (size > 1) {
				memcpy(filename, AUTHORIZED, (size - 1) * sizeof(wchar_t));
			}
			filename[size - 1] = L'\0';
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return size;
		}
		memcpy(filename, AUTHORIZED, sizeof(AUTHORIZED));
		return AUTHORIZED_LENGTH;
	}

	const GetModuleFileNameWFn original =
		g_originalGetModuleFileNameW.load(std::memory_order_acquire);
	if (original) return original(module, filename, size);
	SetLastError(ERROR_INVALID_FUNCTION);
	return 0;
}

// 在模块的导入表里找某个函数的 IAT 槽位。改 IAT 只影响这一个模块的调用，
// 比 inline hook 安全得多 —— 进程里其他人调 GetModuleFileNameW 不受影响。
void** FindImportSlot(HMODULE module, const char* functionName) noexcept {
	if (!module || !functionName) return nullptr;
	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
	const auto* nt =
		reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return nullptr;
	}

	const IMAGE_DATA_DIRECTORY& directory =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
	if (!directory.VirtualAddress || !directory.Size ||
		directory.VirtualAddress >= imageSize || directory.Size > imageSize ||
		directory.VirtualAddress > imageSize - directory.Size) {
		return nullptr;
	}

	auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress);
	const auto* descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress + directory.Size);
	for (; descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
		if (descriptor->Name >= imageSize) continue;
		const char* library = reinterpret_cast<const char*>(base + descriptor->Name);
		// GetModuleFileNameW 可能从 kernel32 导入，也可能走 api-set 转发
		if (_stricmp(library, "KERNEL32.dll") != 0 &&
			_stricmp(library, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
			_stricmp(library, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
			continue;
		}
		if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;

		auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->OriginalFirstThunk);
		auto* addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
			base + descriptor->FirstThunk);
		for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
			if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
			const auto nameRva = static_cast<DWORD>(nameThunk->u1.AddressOfData);
			if (nameRva >= imageSize) return nullptr;
			const auto* import =
				reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameRva);
			if (strcmp(reinterpret_cast<const char*>(import->Name),
				functionName) == 0) {
				return reinterpret_cast<void**>(&addressThunk->u1.Function);
			}
		}
	}
	return nullptr;
}

bool WriteImportSlot(void** slot, void* value, void** previous) noexcept {
	DWORD oldProtect = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
		return false;
	}
	if (previous) *previous = *slot;
	*slot = value;
	VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
	return true;
}

/* ---------------- 资源工具（和 DLSS SR 那边同构） ---------------- */

ID3D12Resource* CreateTexture(
	ID3D12Device* device,
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT format,
	bool allowUav,
	D3D12_RESOURCE_STATES initialState,
	const wchar_t* debugName) noexcept {
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = allowUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
		: D3D12_RESOURCE_FLAG_NONE;

	ID3D12Resource* resource = nullptr;
	const HRESULT hr = device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
		IID_PPV_ARGS(&resource));
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"NR 纹理 %s (%ux%u fmt=%u) 创建失败: 0x%08X",
			debugName, width, height, (unsigned)format, hr);
		return nullptr;
	}
	resource->SetName(debugName);
	return resource;
}

// D3D12 里**只读状态可以叠加**：一个资源可以同时处于
// NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE，两种读都合法。
// 这些位就是只读的那一批。
constexpr D3D12_RESOURCE_STATES READ_ONLY_STATES =
	D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
	D3D12_RESOURCE_STATE_INDEX_BUFFER |
	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
	D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
	D3D12_RESOURCE_STATE_COPY_SOURCE |
	D3D12_RESOURCE_STATE_DEPTH_READ |
	D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

// **两个只读状态里一个包含另一个时，这条 barrier 既不必要也有害。**
//
// 这是实测抓出来的崩溃真因。真实游戏（RE Engine）在 evaluate 点上把资源放在
// **组合读状态**里：颜色和矢量是 0xC0（= 非像素着色器读 | 像素着色器读），
// 深度是 0xE0（再加上深度读）。而我们无脑往 0x40（非像素着色器读）转：
//
//   1. **完全没必要** —— 0xC0 已经包含 0x40，计算着色器当 SRV 读它本来就合法。
//   2. **有害** —— 游戏把它放在组合读状态，恰恰是因为**多条命令列表在同时读它**。
//      只读状态允许并发读，但 transition 要求独占。我们在自己这条列表上把它
//      转走再转回，另外那些列表还按 0xC0 在读 —— 未定义行为。
//      表现就是 DEVICE_HUNG：没有页错误、没有哪条列表执行到一半、
//      调试层也抓不到（真实游戏里根本没有调试层）。
//
// 判断做成**对称**的：正向（0xC0 -> 0x40）要跳过，那么还原（0x40 -> 0xC0）
// 也必须跳过 —— 因为资源其实一直在 0xC0，还原时报 before=0x40 就是错的 before，
// 比原来那条多余的 barrier 更危险。这条对称性是这个函数唯一的正确性要求。
bool ReadStatesOverlap(
	D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) noexcept {
	if (!a || !b) return false;                       // COMMON(0) 不参与
	if ((a & ~READ_ONLY_STATES) || (b & ~READ_ONLY_STATES)) return false;
	return (a & b) == a || (a & b) == b;              // 一个包含另一个
}

void Barrier(
	ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* resource,
	D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after) noexcept {
	if (before == after) return;
	if (ReadStatesOverlap(before, after)) {
		static uint32_t told = 0;
		if (told++ < 2) {
			D5_LOG_INFO(L"barrier 跳过：0x%X 已经包含 0x%X（只读状态可叠加）——"
				L"多余的 transition 会和其它命令列表的并发读打架",
				(unsigned)before, (unsigned)after);
		}
		return;
	}
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	commandList->ResourceBarrier(1, &barrier);
}

template <typename T>
void SafeRelease(T*& ptr) noexcept {
	if (ptr) {
		ptr->Release();
		ptr = nullptr;
	}
}

// DLSSNR 要的颜色格式。
//
// **不用写死的白名单。** 上一版就是一张手写清单，结果鬼武者的场景颜色是
// R11G11B10_FLOAT（fmt=26）—— 不在清单里，evaluate 点那条路一帧都没跑起来，
// 而界面上只说"还没处理到任何一帧"，完全看不出是格式被拒了。手写清单的问题不是
// "漏了一个"，而是**它凭什么知道**：能不能用取决于设备，那就该问设备。
//
// 两件事：
//   1. RGBA8/BGRA8 typeless 和 sRGB 折回同族的 UNORM typed 工作格式。
//      游戏资源保持原格式；typeless/sRGB 本身不能直接当 UAV 视图。
//      D3D12 允许在同一 typeless 家族内 CopyResource，所以对接得上。
//   2. 问设备这个格式能不能做 typed UAV 写入 + 能不能当 2D 纹理采样。
//
// 注意我们的工作格式**必须和游戏的颜色同族**：两边都要 CopyResource
// （游戏颜色 → _colorIn，_output → 游戏颜色），换族就非法了。所以这里不能"挑一个
// 好用的格式"，只能回答"游戏这个格式我们做不做得了"。
DXGI_FORMAT PickColorFormat(ID3D12Device* device, DXGI_FORMAT format) noexcept {
	const DXGI_FORMAT candidate = NrColorViewFormat(format);
	if (candidate == DXGI_FORMAT_UNKNOWN) return DXGI_FORMAT_UNKNOWN;

	// 老白名单。只在**问不出来**的时候用（CheckFeatureSupport 本身失败），
	// 不用它来否决设备的回答 —— 那等于又回到猜。
	const bool onLegacyList =
		candidate == DXGI_FORMAT_R8G8B8A8_UNORM ||
		candidate == DXGI_FORMAT_B8G8R8A8_UNORM ||
		candidate == DXGI_FORMAT_R10G10B10A2_UNORM ||
		candidate == DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (!device) return onLegacyList ? candidate : DXGI_FORMAT_UNKNOWN;

	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
	support.Format = candidate;
	if (FAILED(device->CheckFeatureSupport(
		D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
		return onLegacyList ? candidate : DXGI_FORMAT_UNKNOWN;
	}

	const bool canStore =
		(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
	const bool canSample =
		(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0 &&
		(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
	if (canStore && canSample) return candidate;

	// 被拒也要说清楚是**哪一项**不行，否则下次又是一句"格式不支持"，
	// 谁也不知道该怎么办
	D5_LOG_WARN(L"DLSSNR 做不了颜色格式 %u（折算后 %u）：typed UAV 写入=%hs "
		L"2D 采样=%hs（Support1=0x%X Support2=0x%X）",
		(unsigned)format, (unsigned)candidate,
		canStore ? "行" : "**不行**", canSample ? "行" : "**不行**",
		(unsigned)support.Support1, (unsigned)support.Support2);
	return DXGI_FORMAT_UNKNOWN;
}

/* ---------------- 像素解码（诊断用） ---------------- */

// 无符号浮点解码。R11G11B10_FLOAT 的三个通道都是"没有符号位的小浮点"：
// R/G 是 5 位指数 + 6 位尾数，B 是 5 位指数 + 5 位尾数，偏移都是 15。
float DecodeSmallFloat(uint32_t bits, uint32_t mantissaBits) noexcept {
	const uint32_t mantissaMask = (1u << mantissaBits) - 1u;
	const uint32_t exponent = (bits >> mantissaBits) & 0x1Fu;
	const uint32_t mantissa = bits & mantissaMask;
	const float scale = float(1u << mantissaBits);
	if (exponent == 0) {
		// 非规格化数
		return mantissa == 0 ? 0.0f
			: (float(mantissa) / scale) * ldexpf(1.0f, -14);
	}
	if (exponent == 0x1Fu) {
		// Inf / NaN。返回一个显眼的大数，日志里一看就知道不对
		return mantissa ? -1.0f : 1e30f;
	}
	return (1.0f + float(mantissa) / scale) * ldexpf(1.0f, int(exponent) - 15);
}

// 把一个像素解成 RGB。认不出的格式返回 false，调用方去打原始字节。
bool DecodePixel(
	DXGI_FORMAT format, const uint8_t* bytes, float rgb[3]) noexcept {
	switch (format) {
	case DXGI_FORMAT_R11G11B10_FLOAT: {
		uint32_t packed = 0;
		memcpy(&packed, bytes, sizeof(packed));
		rgb[0] = DecodeSmallFloat(packed & 0x7FFu, 6);
		rgb[1] = DecodeSmallFloat((packed >> 11) & 0x7FFu, 6);
		rgb[2] = DecodeSmallFloat((packed >> 22) & 0x3FFu, 5);
		return true;
	}
	case DXGI_FORMAT_R16G16B16A16_FLOAT: {
		uint16_t half[4]{};
		memcpy(half, bytes, sizeof(half));
		for (int i = 0; i < 3; ++i) {
			// 半精度：1 位符号 + 5 位指数 + 10 位尾数
			const uint32_t h = half[i];
			const float magnitude = DecodeSmallFloat(h & 0x7FFFu, 10);
			rgb[i] = (h & 0x8000u) ? -magnitude : magnitude;
		}
		return true;
	}
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM: {
		const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
		rgb[0] = float(bytes[bgra ? 2 : 0]) / 255.0f;
		rgb[1] = float(bytes[1]) / 255.0f;
		rgb[2] = float(bytes[bgra ? 0 : 2]) / 255.0f;
		return true;
	}
	default:
		return false;
	}
}

uint32_t BytesPerPixel(DXGI_FORMAT format) noexcept {
	switch (format) {
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return 4;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return 8;
	default:
		return 0;
	}
}

}  // namespace

DlssNrFilter::~DlssNrFilter() {
	if (_queue && _fence) {
		const uint64_t target = ++_fenceValue;
		if (SUCCEEDED(_queue->Signal(_fence, target)) &&
			_fence->GetCompletedValue() < target && _fenceEvent) {
			_fence->SetEventOnCompletion(target, _fenceEvent);
			WaitForSingleObject(_fenceEvent, 2000);
		}
	}
	DestroyFeature();
	ReleaseTextures();
	// 查询堆和回读缓冲也归我们，别漏 —— 漏了在游戏里就是每次重建泄一份显存
	SafeRelease(_timingHeap);
	SafeRelease(_timingReadback);
	for (Slot& slot : _slots) {
		SafeRelease(slot.commandList);
		SafeRelease(slot.allocator);
	}
	SafeRelease(_fence);
	if (_fenceEvent) CloseHandle(_fenceEvent);
	UnloadSnippet();
	SafeRelease(_queue);
}

void DlssNrFilter::Fail(const char* what) noexcept {
	++_failureCount;
	_lastError = what;
	if (_failureCount >= 8) {
		_disabled = true;
		D5_LOG_ERROR(L"DLSSNR 连续失败，已停用（%hs）", what);
	}
}

/* ============================ snippet ============================ */

bool DlssNrFilter::LoadSnippet(HMODULE selfModule) noexcept {
	if (_snippet) return true;

	// DXL keeps models beside its core DLL in ngx/, independent of each game installation.
	wchar_t hostPath[MAX_PATH]{};
	if (!GetModuleFileNameW(selfModule, hostPath, MAX_PATH)) {
		_lastError = "无法定位 DXL core 路径";
		return false;
	}
	const std::filesystem::path directory =
		std::filesystem::path(hostPath).parent_path();
	std::error_code ec;
	std::filesystem::path dllPath = directory / L"ngx" / L"nvngx_dlssnr.dll";
    if (!std::filesystem::exists(dllPath, ec)) dllPath = directory / L"nvngx_dlssnr.dll";
    ec.clear();
	if (!std::filesystem::exists(dllPath, ec)) {
		D5_LOG_WARN(L"找不到 %s —— DLSS5 需要它，公开 SDK 里没有，"
			L"请检查 DXL 工具目录 ngx\\nvngx_dlssnr.dll 是否完整", dllPath.c_str());
		_lastError = "nvngx_dlssnr.dll 缺失";
		return false;
	}

	// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR：让它的依赖也从 ngx\ 里找
	_snippet = LoadLibraryExW(dllPath.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!_snippet) {
		D5_LOG_ERROR(L"加载 nvngx_dlssnr.dll 失败: %lu", GetLastError());
		_lastError = "nvngx_dlssnr.dll 加载失败";
		return false;
	}

	_initExt = (void*)GetProcAddress(_snippet, "NVSDK_NGX_D3D12_Init_Ext");
	_createFeature = (void*)GetProcAddress(_snippet, "NVSDK_NGX_D3D12_CreateFeature");
	_evaluateFeature =
		(void*)GetProcAddress(_snippet, "NVSDK_NGX_D3D12_EvaluateFeature");
	_releaseFeature =
		(void*)GetProcAddress(_snippet, "NVSDK_NGX_D3D12_ReleaseFeature");
	_shutdown = (void*)GetProcAddress(_snippet, "NVSDK_NGX_D3D12_Shutdown1");
	if (!_initExt || !_createFeature || !_evaluateFeature || !_releaseFeature ||
		!_shutdown) {
		D5_LOG_ERROR(L"nvngx_dlssnr.dll 的导出不完整（init=%p create=%p eval=%p "
			L"release=%p shutdown=%p）", _initExt, _createFeature,
			_evaluateFeature, _releaseFeature, _shutdown);
		_lastError = "nvngx_dlssnr.dll 导出不完整";
		UnloadSnippet();
		return false;
	}

	// 要伪装的模块 = 装这个 hook 函数的模块，也就是我们自己。从函数地址反查而不是
	// 直接用传进来的 selfModule，这样即使调用方传错也不会静默失效。
	HMODULE hookModule = nullptr;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&SpoofedGetModuleFileNameW), &hookModule) ||
		!hookModule) {
		D5_LOG_ERROR(L"反查 hook 所在模块失败: %lu", GetLastError());
		_lastError = "无法确定要伪装的模块";
		UnloadSnippet();
		return false;
	}

	// 进程内只允许一份，抢不到就说明已经有人装了
	HMODULE expected = nullptr;
	if (!g_spoofedCallerModule.compare_exchange_strong(
		expected, hookModule, std::memory_order_acq_rel)) {
		_lastError = "DLSSNR 的调用方伪装已被另一个实例占用";
		UnloadSnippet();
		return false;
	}

	_getModuleFileNameSlot = FindImportSlot(_snippet, "GetModuleFileNameW");
	if (!_getModuleFileNameSlot) {
		D5_LOG_ERROR(L"nvngx_dlssnr.dll 的导入表里没有 GetModuleFileNameW");
		_lastError = "找不到 GetModuleFileNameW 的 IAT 槽位";
		UnloadSnippet();
		return false;
	}
	void* previous = nullptr;
	if (!WriteImportSlot(_getModuleFileNameSlot,
		reinterpret_cast<void*>(&SpoofedGetModuleFileNameW), &previous) ||
		!previous) {
		D5_LOG_ERROR(L"改写 GetModuleFileNameW 的 IAT 槽位失败");
		_lastError = "IAT 改写失败";
		_getModuleFileNameSlot = nullptr;
		UnloadSnippet();
		return false;
	}
	g_originalGetModuleFileNameW.store(
		reinterpret_cast<GetModuleFileNameWFn>(previous),
		std::memory_order_release);

	// snippet 自己的 Init，不走 NGX core
	const std::wstring directoryString = directory.wstring();
	const auto initExt = reinterpret_cast<SnippetInitExtFn>(_initExt);
	const NVSDK_NGX_Result result = initExt(
		DLSSNR_APPLICATION_ID, directoryString.c_str(), _device,
		NVSDK_NGX_Version_API, nullptr);
	D5_LOG_INFO(L"DLSSNR Init_Ext 返回 0x%08X（身份查询 %u 次，其中命中伪装 %u 次）",
		(unsigned)result, g_spoofCalls.load(std::memory_order_relaxed),
		g_spoofHits.load(std::memory_order_relaxed));
	if (NVSDK_NGX_FAILED(result)) {
		_lastError = "DLSSNR Init_Ext 失败";
		UnloadSnippet();
		return false;
	}
	_snippetInitialized = true;
	D5_LOG_INFO(L"DLSSNR snippet 就绪: %s", dllPath.c_str());
	return true;
}

void DlssNrFilter::UnloadSnippet() noexcept {
	if (_snippetInitialized && _shutdown) {
		reinterpret_cast<ShutdownFn>(_shutdown)(_device);
		_snippetInitialized = false;
	}
	// 先撤 IAT 补丁再卸模块，顺序反了会让 snippet 在卸载路径上调到我们的函数
	if (_getModuleFileNameSlot) {
		const GetModuleFileNameWFn original =
			g_originalGetModuleFileNameW.load(std::memory_order_acquire);
		if (original) {
			WriteImportSlot(_getModuleFileNameSlot,
				reinterpret_cast<void*>(original), nullptr);
		}
		_getModuleFileNameSlot = nullptr;
	}
	g_originalGetModuleFileNameW.store(nullptr, std::memory_order_release);
	g_spoofedCallerModule.store(nullptr, std::memory_order_release);
	if (_snippet) {
		FreeLibrary(_snippet);
		_snippet = nullptr;
	}
	_initExt = _createFeature = _evaluateFeature = nullptr;
	_releaseFeature = _shutdown = nullptr;
}

/* ============================ 初始化 ============================ */

bool DlssNrFilter::Initialize(
	ID3D12Device* device, ID3D12CommandQueue* queue, HMODULE selfModule) noexcept {
	_device = device;
	_queue = queue;
	if (_queue) _queue->AddRef();
	if (!_device || !_queue) {
		_lastError = "没有 device 或 command queue";
		return false;
	}
	if (!CreateCommandObjects()) return false;
	// 色调往返只有 evaluate 点那条路要用。**建不出来不算致命** —— backbuffer 那条路
	// 完全不需要它，不该因为它把整个 DLSS5 一起废掉。
	if (!_passes.Initialize(_device)) {
		D5_LOG_WARN(L"色调往返初始化失败（%hs）—— "
			L"「在游戏的 DLSS 之前处理」将不可用，backbuffer 那条路不受影响",
			_passes.LastError());
	}
	if (!LoadSnippet(selfModule)) return false;
	D5_LOG_INFO(L"DlssNrFilter 已初始化 (device=%p queue=%p)", device, queue);
	return true;
}

bool DlssNrFilter::CreateCommandObjects() noexcept {
	for (Slot& slot : _slots) {
		HRESULT hr = _device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
		if (SUCCEEDED(hr)) {
			hr = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				slot.allocator, nullptr, IID_PPV_ARGS(&slot.commandList));
		}
		if (FAILED(hr)) {
			D5_LOG_ERROR(L"NR 命令对象创建失败: 0x%08X", hr);
			return false;
		}
		slot.commandList->Close();
		// **命令列表必须起名。** 设备挂掉时 DRED 的面包屑只认名字，无名的一律打成
		// "(无名)" —— 上一次真崩溃拿到的就是"三条无名列表卡住了"，
		// 我连"这是不是我们的"都答不上来。ReShade 起了名，所以它的一眼就认得出。
		slot.commandList->SetName(L"D5Q.NR.List");
		slot.allocator->SetName(L"D5Q.NR.Allocator");
	}
	HRESULT hr = _device->CreateFence(
		0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"NR CreateFence 失败: 0x%08X", hr);
		return false;
	}
	_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	return _fenceEvent != nullptr;
}

// **绝对不能用 INFINITE**：这个函数跑在游戏的 present 线程上。游戏重新配置
// 渲染管线（比如在设置里切换 DLSS）时会刷新/重建队列，我们的 fence 可能永远不再
// signal —— 那就把游戏的 present 线程永久堵死，表现为"画面冻住、进程活着、不崩溃"。
// 实测在鬼武者上遇到过这个现象。
//
// 超时就放弃并彻底停用自己：宁可少一个滤镜，也不能挂住别人的游戏。
// 规则和取舍完全同 DlssSrUpscaler::TryClaimSlot，那边有详细说明：
// 跑在游戏的 present 线程上，所以等不到就跳过这一帧，绝不阻塞、绝不自我停用。
bool DlssNrFilter::TryClaimSlot(Slot& slot) noexcept {
	if (!_fence) return false;
	auto completed = _fence->GetCompletedValue();
	// UINT64_MAX means device removal, not completion of every pending slot.
	if (completed == UINT64_MAX) return false;
	if (!slot.fenceValue || completed >= slot.fenceValue) return true;
	if (_fenceEvent) {
		// A previous timed-out registration can signal this shared event later.
		// Clear an old notification, then still verify the fence after every wake:
		// ResetEvent alone cannot stop an older registration firing during Wait.
		ResetEvent(_fenceEvent);
		if (SUCCEEDED(_fence->SetEventOnCompletion(slot.fenceValue, _fenceEvent))) {
			const auto start = std::chrono::steady_clock::now();
			for (;;) {
				completed = _fence->GetCompletedValue();
				if (completed == UINT64_MAX) return false;
				if (completed >= slot.fenceValue) return true;
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - start).count();
				if (elapsed >= SLOT_WAIT_BUDGET_MS) break;
				const auto wait = WaitForSingleObject(_fenceEvent, DWORD(SLOT_WAIT_BUDGET_MS - elapsed));
				if (wait != WAIT_OBJECT_0) {
					completed = _fence->GetCompletedValue();
					if (completed != UINT64_MAX && completed >= slot.fenceValue) return true;
					break;
				}
			}
		}
	}

	const uint64_t skipped = ++_skippedFrames;
	if (skipped == 1 || skipped % 300 == 0) {
		const HRESULT removed = _device ? _device->GetDeviceRemovedReason() : S_OK;
		D5_LOG_WARN(L"DLSS5 跳过第 %llu 帧：命令槽还没完成（目标 %llu，已完成 %llu，"
			L"GetDeviceRemovedReason=0x%08X）。不阻塞 present 线程，下一帧再试。",
			(unsigned long long)skipped, (unsigned long long)slot.fenceValue,
			(unsigned long long)_fence->GetCompletedValue(), removed);
	}
	return false;
}

// 等 GPU 把我们提交过的活全部跑完。见头文件里那段说明。
//
// 刻意**不复用析构里那段** —— 那里可以接受超时后照样往下走（进程都要退了），
// 这里不行：超时就意味着接下来要释放正在被 GPU 读的资源。所以超时要**放弃重建**，
// 而不是硬着头皮释放。返回值靠调用方检查 _feature 是否还在来间接体现。
void DlssNrFilter::WaitForOwnWorkIdle() noexcept {
	_pendingRebuildBlocked = true;
	if (!_fence || !_fenceEvent) return;
	// Wait only for our last submission, not unrelated game work queued later.
	// External Evaluate work is protected separately by EvaluateGpuGate.
	const auto target = _fenceValue;
	auto done = _fence->GetCompletedValue();
	if (done == UINT64_MAX) return;
	if (done >= target) { _pendingRebuildBlocked = false; return; }
	ResetEvent(_fenceEvent);
	if (FAILED(_fence->SetEventOnCompletion(target, _fenceEvent))) return;
	const auto start = GetTickCount64();
	do {
		const auto elapsed = GetTickCount64() - start;
		if (elapsed >= 100) return;
		if (WaitForSingleObject(_fenceEvent, DWORD(100 - elapsed)) == WAIT_FAILED) return;
		done = _fence->GetCompletedValue();
		if (done == UINT64_MAX) return;
	} while (done < target);
	_pendingRebuildBlocked = false;
}

bool DlssNrFilter::SetPresentQueue(ID3D12CommandQueue* queue) noexcept {
	if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
	if (queue == _queue) return true;
	ID3D12Device* device = nullptr;
	if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return false;
	const bool sameDevice = device == _device;
	device->Release();
	if (!sameDevice) return false;
	WaitForOwnWorkIdle();
	if (_pendingRebuildBlocked) return false;
	queue->AddRef(); SafeRelease(_queue); _queue = queue;
	_timingFrequency = 0;
	_needsReset = true;
	return true;
}

void DlssNrFilter::ReleaseTextures() noexcept {
	_optical.Destroy();
	_usedOptical = false;
	// 回读缓冲跟着工作格式走（footprint 用 _colorFormat），格式变了必须重建
	SafeRelease(_dumpBefore);
	SafeRelease(_dumpAfter);
	SafeRelease(_dumpFilterIn);
	SafeRelease(_dumpFilterOut);
	SafeRelease(_dumpWritten);
	_dumpRecordedAt = 0;
	SafeRelease(_exposureReadback);
	_exposureFormat = 0;
	_gameExposure = 0.0f;
	SafeRelease(_controlMask);
    SafeRelease(_controlMaskLow);
    _controlMaskLowReady = false;
	for (auto*& upload : _controlUploads) SafeRelease(upload);
	_controlMaskFilled = false; _maskVersion = UINT64_MAX;
	SafeRelease(_colorIn);
	SafeRelease(_decoded);
	SafeRelease(_output);
	SafeRelease(_layerPing);
	SafeRelease(_zeroDepth);
	SafeRelease(_zeroMotion);
	SafeRelease(_motionUpscaled);
	SafeRelease(_depthUpscaled);
	SafeRelease(_fullOut);
	SafeRelease(_downsampled);
	SafeRelease(_ratio);
	SafeRelease(_uavHeapGpu);
	SafeRelease(_uavHeapCpu);
	_zeroTexturesCleared = false;
}

bool DlssNrFilter::CreateTextures(DXGI_FORMAT colorFormat, bool atEvaluate) noexcept {
	ReleaseTextures();
    if (_settings.controlMask || _settings.semanticMask) {
        _controlMask = CreateTexture(_device, _width, _height, DXGI_FORMAT_R8G8B8A8_UNORM,
            true, D3D12_RESOURCE_STATE_COPY_DEST, L"DXL.NR.ControlMask");
        if (!_controlMask) return false;
        if (_settings.semanticMask && std::max(_width,_height)>640 && _passes.CanResample()) {
            const float scale=640.0f/float(std::max(_width,_height));
            const uint32_t w=std::max(1u,uint32_t(_width*scale+.5f));
            const uint32_t h=std::max(1u,uint32_t(_height*scale+.5f));
            _controlMaskLow=CreateTexture(_device,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,
                false,D3D12_RESOURCE_STATE_COPY_DEST,L"DXL.NR.SemanticMaskLow");
            if (!_controlMaskLow) return false;
        }
    }

	// _colorIn 现在要能当 UAV：evaluate 点那条路是用计算着色器把游戏的 HDR 颜色
	// **压**进它，不是拷进它。backbuffer 那条路仍然只拷，多一个 flag 不影响。
	_colorIn = CreateTexture(_device, _width, _height, colorFormat, true,
		D3D12_RESOURCE_STATE_COPY_DEST, L"D5Q.NR.ColorIn");
	// 解码目标。只有 evaluate 点用，但建出来的成本很低，不值得为它加一条分支。
	_decoded = CreateTexture(_device, _width, _height, colorFormat, true,
		D3D12_RESOURCE_STATE_COPY_SOURCE, L"D5Q.NR.Decoded");
	// 降分辨率那条路多要两张：全分辨率结果 + 低分辨率比值图。
	// 只在真的降了才建 —— 4K 下一张全分辨率纹理不是小数目。
	if (_scaled) {
		_fullOut = CreateTexture(_device, _fullWidth, _fullHeight, colorFormat,
			true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"D5Q.NR.FullOut");
		// **比值图必须是浮点，不能跟着 colorFormat。**
		//
		// 比值范围是 [0.5, 2.0]，而游戏的 backbuffer 是 UNORM
		// （实测：真游戏 fmt=24 R10G10B10A2_UNORM，夹具 fmt=28 R8G8B8A8_UNORM）——
		// UNORM 存不了 >1 的值，**所有"变亮"的比值会被硬钳到 1.0**，只剩变暗的一半，
		// 而且量化到 1/1024（R10）的台阶上。对一个局部改动只有百分之几的滤镜来说，
		// 这等于把效果整个抹掉。
		//
		// 这张图只在降分辨率时存在，所以症状精确地表现为"处理分辨率一旦不是 100%
		// 就完全没效果"，而且 95% 和 40% 一样坏 —— 和比例无关，是格式的硬钳。
		// R16F 四通道在低分辨率上很便宜（4K 的 60% 也就 ~24 MB）。
		_ratio = CreateTexture(_device, _width, _height,
			DXGI_FORMAT_R16G16B16A16_FLOAT, true,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"D5Q.NR.Ratio");
        if (atEvaluate) {
            _downsampled = CreateTexture(_device, _width, _height, colorFormat, true,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"D5Q.NR.LowHDR");
        }
        if (!_fullOut || !_ratio || (atEvaluate && !_downsampled)) {
			D5_LOG_ERROR(L"降分辨率那条路的纹理建不出来（全 %ux%u / 比值 %ux%u）",
				_fullWidth, _fullHeight, _width, _height);
			ReleaseTextures();
			return false;
		}
	}
	_output = CreateTexture(_device, _width, _height, colorFormat, true,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"D5Q.NR.Output");
	if (_settings.trueLayers > 1) {
		_layerPing = CreateTexture(_device, _width, _height, colorFormat, true,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"D5Q.NR.LayerPing");
	}
	// 深度这一路已经实测过 DLSSNR 不看，但契约要完整，还是给一张清零的。
	// 矢量是它真正会用的，等里程碑 5 接上原生矢量。
	_zeroDepth = CreateTexture(_device, _width, _height, DXGI_FORMAT_R32_FLOAT,
		true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"D5Q.NR.ZeroDepth");
	_zeroMotion = CreateTexture(_device, _width, _height, DXGI_FORMAT_R16G16_FLOAT,
		true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"D5Q.NR.ZeroMotion");
	// 游戏矢量重采样到工作分辨率之后放这里。格式跟零矢量一致 —— 游戏的矢量实测
	// 也是 R16G16_FLOAT（fmt=34）。
	_motionUpscaled = CreateTexture(_device, _width, _height,
		DXGI_FORMAT_R16G16_FLOAT, true,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"D5Q.NR.MotionUp");
	// 深度重采样目标。用 R32_FLOAT（不是游戏那个 typeless 深度格式）——
	// 我们只需要"能采样能写"的单通道浮点，不需要它还能当深度缓冲用。
	_depthUpscaled = CreateTexture(_device, _width, _height,
		DXGI_FORMAT_R32_FLOAT, true,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"D5Q.NR.DepthUp");
	if (!_colorIn || !_output || !_zeroDepth || !_zeroMotion || !_decoded ||
		(_settings.trueLayers > 1 && !_layerPing)) {
		ReleaseTextures();
		_lastError = "NR 纹理创建失败";
		return false;
	}

	// 清零要两个堆：GPU 句柄来自 shader-visible，CPU 句柄必须来自
	// non-shader-visible（后者在 CPU 侧只写，驱动读它是非法的）
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	HRESULT hr = _device->CreateDescriptorHeap(
		&heapDesc, IID_PPV_ARGS(&_uavHeapGpu));
	if (SUCCEEDED(hr)) {
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		hr = _device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_uavHeapCpu));
	}
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"NR 描述符堆创建失败: 0x%08X", hr);
		ReleaseTextures();
		_lastError = "NR 描述符堆创建失败";
		return false;
	}
	_uavStride = _device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_CPU_DESCRIPTOR_HANDLE gpuHeapCpu =
		_uavHeapGpu->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE cpuHeapCpu =
		_uavHeapCpu->GetCPUDescriptorHandleForHeapStart();
	ID3D12Resource* const zeroTextures[]{ _zeroDepth, _zeroMotion };
	for (ID3D12Resource* texture : zeroTextures) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = texture->GetDesc().Format;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		_device->CreateUnorderedAccessView(texture, nullptr, &uav, gpuHeapCpu);
		_device->CreateUnorderedAccessView(texture, nullptr, &uav, cpuHeapCpu);
		gpuHeapCpu.ptr += _uavStride;
		cpuHeapCpu.ptr += _uavStride;
	}

	_colorFormat = colorFormat;
	return true;
}

/* ============================ feature ============================ */

void DlssNrFilter::SetCreateParameters(NVSDK_NGX_Parameter* p) noexcept {
	p->Set(PARAM_WIDTH, _width);
	p->Set(PARAM_HEIGHT, _height);
	// DLSSNR 是同分辨率滤镜，不放大。**只有 ScalingRatio 是真键** ——
	// 以前一起设的 Upscaling / Scale 在 DLL 的字符串表里根本不存在（见上面的说明）。
	p->Set(PARAM_SCALING_RATIO, 1.0f);
	p->Set(PARAM_PRESET, _settings.preset);
	// NGX 通用键也要设，DLL 内部两套都会读
	p->Set(NVSDK_NGX_Parameter_Width, _width);
	p->Set(NVSDK_NGX_Parameter_Height, _height);
	p->Set(NVSDK_NGX_Parameter_PerfQualityValue,
		NVSDK_NGX_PerfQuality_Value_MaxQuality);
	p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
	p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
	// **tuning 必须在 create 时设。** DLSSNR 在 feature 构建时把这些读一次，之后
	// evaluate 时再设是不生效的（OptiScaler 的 forwarder 明确在 create 里设全部 tuning，
	// 注释也写明 "the model reads these once, when it builds the feature"）。之前这里
	// 只设了 preset，style/强度/色调/结构/皮肤 全落在默认值上 → 效果弱。见 Prepare
	// 里 needsRebuild 的判断 —— 这些参数变了也要重建。
	p->Set(PARAM_ENABLED, 1);
	p->Set(PARAM_STYLE, _settings.style);
	p->Set(PARAM_INTENSITY, _settings.intensity);
	p->Set(PARAM_LOCAL_TONE, _settings.localTone);
	p->Set(PARAM_LOCAL_STRUCTURE, _settings.localStructure);
	p->Set(PARAM_SKIN_STRUCTURE, _settings.skinStructure);
	p->Set(PARAM_AUTO_MASK, _settings.autoMask ? 1 : 0);
	p->Set(PARAM_UI_CORRECTION, _settings.uiCorrection ? 1 : 0);
}

void DlssNrFilter::SetEvaluateParameters(
	ID3D12Resource* color,
	ID3D12Resource* motion,
	ID3D12Resource* depth,
	bool depthInverted,
	uint32_t subrectWidth,
	uint32_t subrectHeight,
	uint32_t guideSubrectWidth,
	uint32_t guideSubrectHeight,
	float mvScaleX,
	float mvScaleY,
	float jitterX,
	float jitterY,
	float preExposure,
	bool reset) noexcept {
	for (int layer = 0; layer < _settings.trueLayers; ++layer) {
		NVSDK_NGX_Parameter* p = LayerParameters(layer);
		p->Set(PARAM_COLOR, layer == 0 ? color : LayerOutput(layer - 1));
		p->Set(PARAM_OUTPUT, LayerOutput(layer));
        p->Set("DLSSNR.ControlMask", _controlMaskFilled ? _controlMask : static_cast<ID3D12Resource*>(nullptr));
        p->Set("DLSSNR.ControlMaskSubrectBaseX", 0u);
        p->Set("DLSSNR.ControlMaskSubrectBaseY", 0u);
        p->Set("DLSSNR.ControlMaskSubrectWidth", _width);
        p->Set("DLSSNR.ControlMaskSubrectHeight", _height);
		// 矢量和深度**都是真吃的**（见头文件）。零纹理只在拿不到真的时候兜底。
		p->Set(PARAM_MVEC, motion);
		p->Set(PARAM_DEPTH, depth ? depth : _zeroDepth);

		// 子矩形必须显式设，DLL 不会自己推。**不能想当然等于纹理尺寸** ——
		// 游戏可能给一张更大的资源、只让 DLSS 处理其中一块。
		//
		// **颜色/输出和矢量/深度是两套尺寸**（SR→NR 时颜色=输出分辨率、矢量/深度=
		// 渲染分辨率），所以这里按资源类型分开设，不能用同一个值灌全部 4 组键。
		// 之前用输出分辨率灌了 MVec/Depth 的子矩形，模型按 4K 采样 2228x1254 的矢量
		// → 时域累积错位 → 画面糊、效果弱。见 RunNrAtEvaluate 的 guideSubrect。
		// SUBRECTS 顺序：0=Color 1=Output 2=MVec 3=Depth。
		for (uint32_t i = 0; i < 4; ++i) {
			const SubrectKeys& keys = SUBRECTS[i];
			const bool isGuide = (i >= 2);
			const uint32_t w = isGuide ? guideSubrectWidth : subrectWidth;
			const uint32_t h = isGuide ? guideSubrectHeight : subrectHeight;
			p->Set(keys.baseX, 0u);
			p->Set(keys.baseY, 0u);
			p->Set(keys.width, w);
			p->Set(keys.height, h);
		}

		p->Set(PARAM_MVEC_SCALE_X, mvScaleX);
		p->Set(PARAM_MVEC_SCALE_Y, mvScaleY);
		// jitter：**实测确认 DLSSNR 不消费它。** 参数容器的"设了但从来没被读过"探针显示
		// `Jitter.Offset.X` / `Jitter.Offset.Y` 一次都没被问过（DLSSNR 自己命名空间下也没有
		// 对应的键被问）。留着这两行只是为了万一换个 preset 会读 —— 反正是 no-op，
		// 但**别再把 jitter 当成可能的画质变量去调**，它不是。
		p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitterX);
		p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterY);
		// 曝光/预曝光。**只设标量，不绑资源** —— 在还不知道 DLL 到底要不要的时候绑一张
		// 游戏的纹理，要多做一次 barrier、多一个出错面。标量设错最多是被忽略。
		// 键名用 NGX 通用的那套（真实存在的键），DLSSNR 自己的名字我们不知道，不编。
		// 装了之后看日志里"问了但我们没给"的清单，再决定要不要真的接资源。
		if (preExposure > 0.0f) {
			p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, preExposure);
			p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
		}
		p->Set(PARAM_DEPTH_INVERTED, depthInverted ? 1 : 0);
		p->Set(PARAM_INDICATOR_INVERT_X, 0);
		p->Set(PARAM_INDICATOR_INVERT_Y, 0);
		p->Set(PARAM_ENABLED, 1);
		p->Set(PARAM_RESET, reset ? 1 : 0);
		p->Set(PARAM_STYLE, _settings.style);
		p->Set(PARAM_INTENSITY, _settings.intensity);
		p->Set(PARAM_LOCAL_TONE, _settings.localTone);
		p->Set(PARAM_LOCAL_STRUCTURE, _settings.localStructure);
		p->Set(PARAM_SKIN_STRUCTURE, _settings.skinStructure);
		p->Set(PARAM_AUTO_MASK, _settings.autoMask ? 1 : 0);
		p->Set(PARAM_UI_CORRECTION, _settings.uiCorrection ? 1 : 0);
	}
}

void DlssNrFilter::CaptureSemanticInput(ID3D12GraphicsCommandList* list,
    ID3D12Resource* color) noexcept {
    if (_settings.semanticMask && _semantic.capture && _semantic.ctx)
        _semantic.capture(_semantic.ctx, list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            _mode == NrMode::AtEvaluate && _settings.semanticFlipY);
}

bool DlssNrFilter::EnsureControlMaskFilled(ID3D12GraphicsCommandList* list,
    uint32_t uploadSlot) noexcept {
    if (!_controlMask || !list || uploadSlot >= 5) return false;
    const bool semantic = _settings.semanticMask;
    if (semantic && _semantic.Valid()) _semantic.snapshot(_semantic.ctx, _semanticSnapshot);
    const bool fresh = semantic && _semantic.Valid() && _semanticSnapshot.Valid(GetTickCount64()) &&
        _semanticSnapshot.sourceFlipY == (_mode == NrMode::AtEvaluate && _settings.semanticFlipY);
    // Hash only the controls that affect the active mask, including enabled bits.
    uint64_t hash = 14695981039346656037ull;
    auto add = [&](const auto& v) { const auto* p = reinterpret_cast<const uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(v); ++i) hash = (hash ^ p[i]) * 1099511628211ull; };
    add(semantic);
    if (semantic) { add(_settings.semanticBgIntensity); add(_settings.semanticEnabled);
        add(_settings.semanticFlipY); add(_settings.semanticFeather);
        for (float v : _settings.semanticIntensity) add(v); }
    else { add(_settings.controlMaskR); add(_settings.controlMaskG);
        add(_settings.controlMaskB); add(_settings.controlMaskA); }
    const uint64_t version = fresh ? _semanticSnapshot.version : 0;
    if (_controlMaskFilled && hash == _maskSettingsHash && version == _maskVersion && fresh == _maskWasFresh)
        return true;
    const bool gpuUpsample=semantic && _controlMaskLow;
    const auto uploadDesc=gpuUpsample ? _controlMaskLow->GetDesc() : _controlMask->GetDesc();
    const uint32_t maskWidth=uint32_t(uploadDesc.Width),maskHeight=uploadDesc.Height;
    const uint32_t pitch = (maskWidth * 4 + 255) & ~255u;
    auto*& upload = _controlUploads[uploadSlot];
    if (!upload) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = uint64_t(pitch) * maskHeight; desc.Height = 1; desc.DepthOrArraySize = 1;
        desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;
    }
    void* mapped = nullptr; D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, &mapped)) || !mapped) return false;
    auto* bytes = static_cast<uint8_t*>(mapped);
    if (semantic) {
        ComposeSemanticMask(bytes, pitch, maskWidth, maskHeight, fresh ? &_semanticSnapshot : nullptr,
            _settings.semanticBgIntensity, _settings.semanticIntensity, _settings.semanticEnabled, _settings.semanticFeather);
    } else {
        // Preserve existing checker-profile semantics: a slider denotes suppression.
        const uint8_t skip[]{MaskByte(1 - _settings.controlMaskR), MaskByte(1 - _settings.controlMaskG),
            MaskByte(1 - _settings.controlMaskB), MaskByte(1 - _settings.controlMaskA)};
        for (uint32_t y = 0; y < _height; ++y) for (uint32_t x = 0; x < _width; ++x)
            for (uint32_t c = 0; c < 4; ++c)
                bytes[size_t(y) * pitch + x * 4 + c] = ((x / 128 + y / 128) & 1) ? 255 : skip[c];
    }
    upload->Unmap(0, nullptr);
    auto* copyTarget=gpuUpsample ? _controlMaskLow : _controlMask;
    const bool copyReady=gpuUpsample ? _controlMaskLowReady : _controlMaskFilled;
    if (copyReady) Barrier(list, copyTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, maskWidth, maskHeight, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = copyTarget;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(list, copyTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (gpuUpsample) {
        _controlMaskLowReady=true;
        const auto before=_controlMaskFilled ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
        Barrier(list,_controlMask,before,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool drawn=_passes.RecordLinearResample(list,_controlMaskLow,_controlMask,_width,_height);
        Barrier(list,_controlMask,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            drawn ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : before);
        if (!drawn) return false;
    }
    if (!_controlMaskFilled || fresh != _maskWasFresh || hash != _maskSettingsHash)
        D5_LOG_INFO(L"DXL ControlMask: %ux%u semantic=%u fresh=%u bg=%.3f R=semantic GBA=background version=%llu",
            _width, _height, semantic, fresh, _settings.semanticBgIntensity, (unsigned long long)version);
    _controlMaskFilled = true; _maskWasFresh = fresh; _maskVersion = version; _maskSettingsHash = hash;
    return true;
}

NVSDK_NGX_Parameter* DlssNrFilter::LayerParameters(int layer) const noexcept {
	return layer == 0 ? _parameters : _extraParameters[layer - 1];
}

ID3D12Resource* DlssNrFilter::LayerOutput(int layer) const noexcept {
	// Choose parity up front: the last stage always writes the existing output.
	// Preserve _colorIn for residual reconstruction, even at reduced resolution.
	return ((_settings.trueLayers - 1 - layer) & 1) ? _layerPing : _output;
}

uint32_t DlssNrFilter::EvaluateLayers(ID3D12GraphicsCommandList* list) noexcept {
	static const char* labels[]{"NRFG/NR-layer-1", "NRFG/NR-layer-2", "NRFG/NR-layer-3", "NRFG/NR-layer-4", "NRFG/NR-layer-5"};
	for (int layer = 0; layer < _settings.trueLayers; ++layer) {
		auto* previous = layer ? LayerOutput(layer - 1) : nullptr;
		if (previous) Barrier(list, previous, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		NVSDK_NGX_Result result;
		{
			GpuEventScope event(list, labels[layer]);
			++_modelCallCount;
			result = reinterpret_cast<EvaluateFeatureFn>(_evaluateFeature)(list,
				layer == 0 ? _feature : _extraFeatures[layer - 1], LayerParameters(layer), nullptr);
		}
		if (previous) Barrier(list, previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (NVSDK_NGX_FAILED(result)) {
			D5_LOG_ERROR(L"NR layer %d/%d failed: result=0x%08X", layer + 1, _settings.trueLayers, unsigned(result));
			return uint32_t(result);
		}
	}
	return uint32_t(NVSDK_NGX_Result_Success);
}

void DlssNrFilter::DestroyFeature() noexcept {
	for (int i = 3; i >= 0; --i) {
		if (_extraFeatures[i] && _releaseFeature)
			reinterpret_cast<ReleaseFeatureFn>(_releaseFeature)(_extraFeatures[i]);
		_extraFeatures[i] = nullptr;
		delete _extraParameters[i]; _extraParameters[i] = nullptr;
	}
	if (_feature && _releaseFeature) {
		reinterpret_cast<ReleaseFeatureFn>(_releaseFeature)(_feature);
	}
	_feature = nullptr;
	// 参数容器是我们自己的对象（见 NgxParameterBag.h），直接删
	delete _parameterBag;
	_parameterBag = nullptr;
	_parameters = nullptr;
}

bool DlssNrFilter::WaitForOwnGpuIdle() noexcept {
	if (!_fence || !_fenceValue) return true;
	WaitForOwnWorkIdle();
	return !_pendingRebuildBlocked || (_device && FAILED(_device->GetDeviceRemovedReason()));
}

bool DlssNrFilter::TeardownForExit() noexcept {
	// Exit is not permission to free a feature still referenced by GPU work.
	// The tool's State has process lifetime so a false return really retains it.
	if (!WaitForOwnGpuIdle()) {
		D5_LOG_WARN(L"NR exit: private GPU work incomplete; retaining feature until process exit");
		return false;
	}
	DestroyFeature();
	return true;
}

bool DlssNrFilter::CreateFeature() noexcept {
	DestroyFeature();

	// 参数容器用我们自己的实现，**不碰 NGX core** —— 这是 DLSSNR 能和游戏原生
	// DLSS 共存的关键：借 core 的 AllocateParameters 会顺带初始化 core，
	// 把游戏自己的 NGX 会话弄坏。
	_parameterBag = new NgxParameterBag();
	_parameters = _parameterBag;
	// 换了新容器，"问了但没给"的清单也是新的，得重新倒一次
	_missesLogged = false;
	NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;

	Slot& slot = _slots[_nextSlot % SLOT_COUNT];
	if (!TryClaimSlot(slot)) return false;
	slot.allocator->Reset();
	slot.commandList->Reset(slot.allocator, nullptr);

	for (int layer = 0; layer < _settings.trueLayers; ++layer) {
		if (layer) _extraParameters[layer - 1] = new NgxParameterBag();
		auto* parameters = LayerParameters(layer);
		auto** feature = layer == 0 ? &_feature : &_extraFeatures[layer - 1];
		SetCreateParameters(parameters);
		result = reinterpret_cast<CreateFeatureFn>(_createFeature)(
			slot.commandList, FEATURE_DLSSNR, parameters, feature);
		if (NVSDK_NGX_FAILED(result) || !*feature) {
			if (NVSDK_NGX_SUCCEED(result)) result = NVSDK_NGX_Result_Fail;
			D5_LOG_ERROR(L"NR layer creation failed: layer=%d/%d result=0x%08X", layer + 1, _settings.trueLayers, unsigned(result));
			break;
		}
		if (_settings.trueLayers > 1) D5_LOG_INFO(L"NR temporal layer created: layer=%d/%d feature=%p parameters=%p output=%p",
			layer + 1, _settings.trueLayers, *feature, parameters, LayerOutput(layer));
	}
	slot.commandList->Close();

	if (NVSDK_NGX_FAILED(result) || !_feature) {
		D5_LOG_ERROR(L"DLSSNR CreateFeature 失败: 0x%08X", (unsigned)result);
		_lastError = "DLSSNR CreateFeature 失败";
		DestroyFeature();
		// CreateFeature 失败通常是环境问题（DLL 版本、驱动、白名单），重试没意义
		// A multi-layer allocation failure must allow lowering the layer count.
		// Prepare latches that failed configuration instead of retrying each frame.
		_disabled = _settings.trueLayers == 1;
		return false;
	}

	ID3D12CommandList* lists[]{ slot.commandList };
	_queue->ExecuteCommandLists(1, lists);
	slot.fenceValue = ++_fenceValue;
	_queue->Signal(_fence, slot.fenceValue);
	++_nextSlot;

	_needsReset = true;
	D5_LOG_INFO(L"DLSSNR feature 已创建: %ux%u preset=%d style=%d 强度=%.2f",
		_width, _height, _settings.preset, _settings.style, _settings.intensity);
	D5_LOG_INFO(L"NR layers ready: self=%.2f true=%d (independent temporal history per true layer)", _settings.selfLayers, _settings.trueLayers);
	return true;
}

bool DlssNrFilter::Prepare(
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT colorFormat_,
	const NrSettings& requestedSettings,
	NrMode mode, bool allowModeSwitch) noexcept {
	NrSettings settings = requestedSettings;
	settings.selfLayers = std::isfinite(settings.selfLayers) ? std::clamp(settings.selfLayers, 1.0f, 3.0f) : 1.0f;
	settings.trueLayers = std::clamp(settings.trueLayers, 1, 5);
    settings.semanticBgIntensity = MaskStrength(settings.semanticBgIntensity);
    settings.semanticFeather = std::isfinite(settings.semanticFeather) ? std::clamp(settings.semanticFeather, 0.0f, 8.0f) : 8.0f;
    settings.semanticEnabled &= (1u << SEM_GROUP_COUNT) - 1;
    for (auto& strength : settings.semanticIntensity) strength = MaskStrength(strength);
    settings.controlMaskR = MaskStrength(settings.controlMaskR);
    settings.controlMaskG = MaskStrength(settings.controlMaskG);
    settings.controlMaskB = MaskStrength(settings.controlMaskB);
    settings.controlMaskA = MaskStrength(settings.controlMaskA);
	if (_disabled || !settings.enabled || !width || !height) return false;

	// Legacy callers retain the mode lock. The automatic coordinator explicitly
	// opts in only after serializing CPU work and draining the external GPU fence.
	if (mode == NrMode::None) return false;
	if (_mode != NrMode::None && _mode != mode && !allowModeSwitch) {
		static bool complained = false;
		if (!complained) {
			complained = true;
			D5_LOG_WARN(L"DLSS5 的处理点这一局已经锁在「%s」了，"
				L"改设置不会切过去 —— **要换必须重启游戏**。"
				L"运行中热切换会在另一个线程还在用那些纹理时把它们释放掉，实测会让游戏崩。",
				_mode == NrMode::AtEvaluate ? L"游戏 DLSS 之前" : L"present");
		}
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::ModeLocked);
		return false;
	}
	if (!_snippet) {
		_lastError = "DLSSNR snippet 未加载";
		return false;
	}

	const DXGI_FORMAT colorFormat = PickColorFormat(_device, colorFormat_);
	if (colorFormat == DXGI_FORMAT_UNKNOWN) {
		if (_colorFormat != colorFormat_) {
			D5_LOG_WARN(L"DLSSNR 不支持的颜色格式 %u，跳过（上面一行有具体原因）",
				(unsigned)colorFormat_);
			_colorFormat = colorFormat_;
		}
		_lastError = "DLSSNR 不支持该颜色格式";
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::ColorFormat);
		return false;
	}
	const uint32_t typelessViewBit = colorFormat_ == DXGI_FORMAT_R8G8B8A8_TYPELESS ? 1u :
		colorFormat_ == DXGI_FORMAT_B8G8R8A8_TYPELESS ? 2u : 0u;
	if (typelessViewBit && !(_loggedTypelessColorViews & typelessViewBit)) {
		_loggedTypelessColorViews |= typelessViewBit;
		D5_LOG_INFO(L"DLSSNR color view: resource format=%u (TYPELESS) -> typed UNORM=%u; "
			L"original resource unchanged, SRV/work textures share the copy-compatible family",
			(unsigned)colorFormat_, (unsigned)colorFormat);
	}

    // Both routes retain full-sized output. Only the model's private color
    // input/output shrink; native depth/MV and their coordinate system stay intact.
	float scale = settings.renderScale;
	if (scale <= 0.0f || scale > 1.0f) scale = 1.0f;
	const bool wantScale = scale < 0.999f;
	if (wantScale && !_passes.CanScaledPath()) {
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_WARN(L"降分辨率跑 DLSSNR：比值放大的 PSO 没建出来，"
				L"**退回全分辨率**（更贵但画面是对的）");
		}
		scale = 1.0f;
	}
	const bool scaled = wantScale && _passes.CanScaledPath();
	// 对齐到 8：dispatch 是 8x8 一组，而且 NGX 对奇数尺寸一向不友好。
	// 下取整之后至少留 64 像素，避免极端滑块值算出 0。
	uint32_t nrWidth = width;
	uint32_t nrHeight = height;
	if (scaled) {
		nrWidth = uint32_t(float(width) * scale) & ~7u;
		nrHeight = uint32_t(float(height) * scale) & ~7u;
		if (nrWidth < 64) nrWidth = 64;
		if (nrHeight < 64) nrHeight = 64;
	}

	const bool modeChanged = _mode != mode;
	const bool geometryChanged = modeChanged ||
        settings.controlMask != _settings.controlMask || settings.semanticMask != _settings.semanticMask ||
		nrWidth != _width || nrHeight != _height ||
		width != _fullWidth || height != _fullHeight ||
		scaled != _scaled || colorFormat != _colorFormat ||
		settings.trueLayers != _settings.trueLayers || _layerSetupFailed;
	// **所有在 create 时被 DLSSNR 读走的参数，变了都要重建。**
	// 之前只把 preset 算作"建 feature 时才用"，把 style/强度/色调/结构/皮肤当成
	// "每帧设、不用重建" —— 但 DLSSNR 在 create 时读全部 tuning、evaluate 时再设
	// 不生效（OptiScaler 注释），所以那些参数改了画面没变、效果还弱。现在这些
	// 参数都进 create 了，变了就必须重建。
	const bool opticalChanged = settings.opticalQuality != _settings.opticalQuality;
	const bool needsRebuild =
		settings.preset != _settings.preset ||
		settings.style != _settings.style ||
		settings.intensity != _settings.intensity ||
		settings.localTone != _settings.localTone ||
		settings.localStructure != _settings.localStructure ||
		settings.skinStructure != _settings.skinStructure ||
		settings.autoMask != _settings.autoMask ||
		settings.uiCorrection != _settings.uiCorrection;
	if (_layerSetupFailed && mode == _failedLayerMode && colorFormat == _failedLayerFormat &&
		nrWidth == _width && nrHeight == _height && width == _fullWidth && height == _fullHeight &&
		settings.trueLayers == _settings.trueLayers && !needsRebuild) return false;

	if (_feature && !geometryChanged && !needsRebuild) {
		if (settings.selfLayers != _settings.selfLayers)
			D5_LOG_INFO(L"NR self layers: %.2f -> %.2f (composition only; no feature rebuild)", _settings.selfLayers, settings.selfLayers);
		if (opticalChanged) {
			WaitForOwnWorkIdle();
			if (_pendingRebuildBlocked) return false;
			_optical.Destroy();
			_needsReset = true;
		}
		// **参数变化要能从日志里看见。** 这条分支（不重建、每帧设进 evaluate）
		// 是色调/结构/skin 的静默路径 —— "工具改了游戏没变"时日志里完全看不到
		// 值走到哪层（用户实测，排查两轮查不出断点）。限流打一条：每次真的变了才打。
		static NrSettings lastLogged{};
		if (lastLogged.intensity != settings.intensity ||
			lastLogged.localTone != settings.localTone ||
			lastLogged.localStructure != settings.localStructure ||
			lastLogged.skinStructure != settings.skinStructure ||
			lastLogged.style != settings.style ||
			lastLogged.autoMask != settings.autoMask ||
			lastLogged.uiCorrection != settings.uiCorrection) {
			lastLogged = settings;
			D5_LOG_INFO(L"DLSSNR 参数已更新（不重建，下一帧 evaluate 生效）: "
				L"style=%d 强度=%.2f 色调=%.2f 结构=%.2f 皮肤=%.2f mask=%d ui=%d",
				settings.style, settings.intensity, settings.localTone,
				settings.localStructure, settings.skinStructure,
				settings.autoMask ? 1 : 0, settings.uiCorrection ? 1 : 0);
		}
		_settings = settings;
		if (!settings.opticalFlow) _optical.Suspend();
		return true;
	}

	// 等 fence 超时时要能原样退回去（那种情况下不许释放旧资源），所以先留一份。
	const NrSettings previousSettings = _settings;
	const uint32_t previousWidth = _width;
	const uint32_t previousHeight = _height;
	const uint32_t previousFullWidth = _fullWidth;
	const uint32_t previousFullHeight = _fullHeight;
	const bool previousScaled = _scaled;

	_width = nrWidth;
	_height = nrHeight;
	_fullWidth = width;
	_fullHeight = height;
	_scaled = scaled;
	_settings = settings;
	if (scaled) {
		D5_LOG_INFO(L"降分辨率跑 DLSSNR：画面 %ux%u，滤镜跑在 **%ux%u**"
			L"（%.0f%%，像素数 %.0f%%）—— 只把改动量放大回全分辨率，"
			L"细节仍然来自原生分辨率",
			width, height, nrWidth, nrHeight, scale * 100.0f,
			100.0f * float(nrWidth) * float(nrHeight) /
				(float(width) * float(height)));
	}

	// **先等 GPU 把我们的活跑完，再动纹理和 feature。** 见 WaitForOwnWorkIdle。
	WaitForOwnWorkIdle();
	if (_pendingRebuildBlocked) {
		// 等超时了。旧的纹理/feature 可能还在被 GPU 读，绝不能释放 ——
		// 保持现状（画面继续用旧设置），下一帧再试。
		_settings = previousSettings;
		_width = previousWidth;
		_height = previousHeight;
		_fullWidth = previousFullWidth;
		_fullHeight = previousFullHeight;
		_scaled = previousScaled;
		return false;
	}
	if (geometryChanged) {
		// 分辨率变了，之前累计的耗时统计就不能再混在一起报 —— 那个数字是用来
		// 决策"滑块该拉到哪"的，混了两个分辨率的样本就是在骗人。
		_timingSumMs = 0.0;
		_timingEmaMs = 0.0;
		_timingSamples = 0;
		_timingWorstMs = 0.0;
		for (bool& armed : _timingArmed) armed = false;
	}
	if (opticalChanged && !geometryChanged) _optical.Destroy();
	// Release histories before their textures; do not hold the old multi-layer
	// model allocations while allocating the replacement resolution/layer set.
	if (geometryChanged) DestroyFeature();
	const bool texturesReady = !geometryChanged || CreateTextures(colorFormat, mode == NrMode::AtEvaluate);
	if (!texturesReady || !CreateFeature()) {
		if (_settings.trueLayers > 1) {
			DestroyFeature(); ReleaseTextures();
			_layerSetupFailed = true;
			_failedLayerMode = mode; _failedLayerFormat = colorFormat;
			_lastError = "NR layers unavailable; lower True NR Layers or NR Render Scale";
			D5_LOG_WARN(L"NR layer setup stopped: true=%d size=%ux%u; waiting for different layers/scale/settings",
				_settings.trueLayers, _width, _height);
		}
		return false;
	}
	_layerSetupFailed = false;
	if (modeChanged) {
		_externalMotion = _externalDepth = nullptr;
		_externalMotionWidth = _externalMotionHeight = 0;
		_externalDepthWidth = _externalDepthHeight = 0;
		_autoWhitePoint = _lastSceneLuma = _lastEncodedLo = _lastEncodedHi = 0.0f;
		_dumpAtFrame = _dumpRecordedAt = _exposureReadFrame = 0;
		_gameExposure = 0.0f;
		D5_LOG_INFO(L"NR route prepared: %s -> %s size=%ux%u format=%u (GPU drained, history reset)",
			_mode == NrMode::Present ? L"Present" : _mode == NrMode::AtEvaluate ? L"Evaluate" : L"None",
			mode == NrMode::Present ? L"Present" : L"Evaluate", width, height, unsigned(colorFormat));
	}
	// 只有真的建成了才认领这条路。建不起来就别把路占住 ——
	// 否则另一条本来能跑的路也被锁在外面了。
	_mode = mode;
	return true;
}

/* ============================ 每帧 ============================ */

bool DlssNrFilter::CanResampleMotion() noexcept {
	if (!_externalMotionWidth || !_externalMotionHeight) return false;
	// **放行任何约定，只挡明显是垃圾的值。**
	//
	// MVecScale 的含义是：像素 = 矢量值 × MVecScale（在矢量纹理自己的分辨率上）。
	// 重采样不改值、只改"每个值代表几个工作分辨率像素"——所以换算到工作分辨率
	// 就是乘分辨率比（见 RecordMotionResample 调用处），这对 UV / NDC / 像素
	// 三种约定**全都对**，不需要知道游戏用哪种。
	//
	// 旧版只认 |MVecScale| == 矢量尺寸（UV 约定）：DS2 报 (-720, 408) =
	// 纹理尺寸的一半、带符号翻转（NDC 约定，Y 朝上），被整个拒绝掉回退零矢量
	// —— "拿得到深度、拿不到矢量"（用户实测）。同尺寸直通那条路不受影响
	//（原样传游戏自己的 MVecScale，本来就对）。
	//
	// 垃圾值判据：非零、有限（NaN 过不了任何比较）、小于纹理尺寸的 8 倍
	//（一个"值 1.0 挪 8 屏"的缩放不可能是真的）。
	const float tw = float(_externalMotionWidth);
	const float th = float(_externalMotionHeight);
	const float sx = fabsf(_externalMvScaleX);
	const float sy = fabsf(_externalMvScaleY);
	const bool plausible =
		sx > 0.0f && sy > 0.0f && sx < tw * 8.0f && sy < th * 8.0f;
	static bool reported = false;
	if (!reported) {
		reported = true;
		// 约定判别只为了日志说人话，放行与否不依赖它。
		const char* convention =
			fabsf(sx - tw) <= tw * 0.02f && fabsf(sy - th) <= th * 0.02f
				? "UV（值 1.0 = 整屏，鬼武者）"
			: fabsf(sx - tw * 0.5f) <= tw * 0.02f &&
				fabsf(sy - th * 0.5f) <= th * 0.02f
				? "NDC（值 1.0 = 半屏、带符号翻转，DS2）"
			: sx <= 1.5f && sy <= 1.5f
				? "像素（值就是像素数）"
			: "未识别（按分辨率比换算，赌它诚实）";
		D5_LOG_INFO(L"矢量重采样：MVecScale (%.1f, %.1f)，矢量纹理 %ux%u ——"
			L" 约定：%hs。重采样后按分辨率比换算 MVecScale（对哪种约定都对）",
			_externalMvScaleX, _externalMvScaleY,
			_externalMotionWidth, _externalMotionHeight, convention);
		if (!plausible) {
			D5_LOG_WARN(L"矢量重采样：MVecScale (%.1f, %.1f) 不在合理范围内"
				L"（非零、有限、小于纹理尺寸的 8 倍）—— 按垃圾值处理，"
				L"继续用零矢量（比错位鬼影好）。",
				_externalMvScaleX, _externalMvScaleY);
		}
	}
	return plausible;
}

float DlssNrFilter::EffectiveWhitePoint() const noexcept {
	// 手填优先；没填就用自动测出来的；都没有就用 4.0 保底
	// （比"用 1.0"安全：1.0 遇到 HDR 输入会把整张图顶到饱和区）。
	if (_settings.toneScale > 0.0f) return _settings.toneScale;
	// **游戏曝光优先。** 曝光纹理是游戏自己填的"当前曝光"，用它算白点才覆盖整个
	// 场景的高光（"画面正中 8x8 亮度/1.5"只代表局部，高光过曝）。白点 = preExposure
	// / exposure（对齐 OptiScaler ResolveWhitePoint 的 source=1 分支）。
	if (_gameExposure > 1e-6f) {
		const float wp = _gamePreExposure / _gameExposure;
		return ClampWhitePoint(wp);
	}
	// 场景亮度（自动白点）兜底：游戏曝光还没读到时的次优选择。
	if (_autoWhitePoint > 0.0f) return _autoWhitePoint;
	return 4.0f;
}

float DlssNrFilter::ClampWhitePoint(float wp) noexcept {
	// **自动白点必须夹住。** 它是从场景亮度推出来的，而场景亮度会归零：
	// 淡入淡出、过场黑屏、玩家对着阴影里的墙。亮度 0.01 推出来的白点是 0.0067，
	// 拿它去除 HDR 输入等于把整张图乘 150 —— 全白，比"一片灰"还糟，
	// 而且是在玩家从黑屏走出来的那一瞬间发生。上限同理，防止一帧过曝把白点顶飞。
	if (!(wp > WHITE_POINT_MIN)) return WHITE_POINT_MIN;
	if (wp > WHITE_POINT_MAX) return WHITE_POINT_MAX;
	return wp;
}

uint32_t DlssNrFilter::CurveFlags() const noexcept {
	// 编码和还原必须用同一个 —— 还原会在 shader 里重算 EncodeCurve。
	return _settings.pureGamma ? ComputePasses::FLAG_PURE_GAMMA : 0u;
}

float DlssNrFilter::DebugOutScale() const noexcept {
    if (_mode == NrMode::AtEvaluate && _evaluateLdr) return 1.0f;
	// **debug 图写进 tonemap 之前的缓冲，所以显示值（0..1）要放大到"游戏正在曝光的
	// 那个亮度区间"里，否则游戏的 tonemap 会把它压成一团。**
	//
	// 挑法：让显示值 0.5 落在实测的场景亮度上 —— 游戏的曝光本来就是按那个亮度调的，
	// 没测到亮度时按当前曲线的中灰目标反推；这不是绕过游戏 tonemap 的原始预览。
	if (_lastSceneLuma > 1e-3f) return _lastSceneLuma * 2.0f;
	return EffectiveWhitePoint() * 2.0f * NrSampleTargetLinear(_settings.pureGamma, _settings.toneGamma);
}

void DlssNrFilter::RecordZeroClears(ID3D12GraphicsCommandList* list) noexcept {
	if (_zeroTexturesCleared || !list || !_uavHeapGpu || !_uavHeapCpu) return;

	// 注意：SetDescriptorHeaps 会覆盖这条列表上已经绑好的描述符堆。所以这个函数
	// 只能录进**我们自己的**列表 —— 见 EnsureZeroTexturesCleared 上面的说明。
	ID3D12DescriptorHeap* heaps[]{ _uavHeapGpu };
	list->SetDescriptorHeaps(1, heaps);
	const float zeros[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
	D3D12_GPU_DESCRIPTOR_HANDLE gpu =
		_uavHeapGpu->GetGPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE cpu =
		_uavHeapCpu->GetCPUDescriptorHandleForHeapStart();
	ID3D12Resource* const zeroTextures[]{ _zeroDepth, _zeroMotion };
	for (ID3D12Resource* texture : zeroTextures) {
		list->ClearUnorderedAccessViewFloat(gpu, cpu, texture, zeros, 0, nullptr);
		gpu.ptr += _uavStride;
		cpu.ptr += _uavStride;
	}
	Barrier(list, _zeroDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Barrier(list, _zeroMotion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	_zeroTexturesCleared = true;
}

bool DlssNrFilter::EnsureZeroTexturesCleared() noexcept {
	if (_zeroTexturesCleared) return true;
	if (!_uavHeapGpu || !_uavHeapCpu || !_queue) return false;

	Slot& slot = _slots[_nextSlot % SLOT_COUNT];
	++_nextSlot;
	if (!TryClaimSlot(slot)) return false;
	if (FAILED(slot.allocator->Reset()) ||
		FAILED(slot.commandList->Reset(slot.allocator, nullptr))) {
		return false;
	}
	RecordZeroClears(slot.commandList);
	if (FAILED(slot.commandList->Close())) {
		_zeroTexturesCleared = false;
		return false;
	}
	// 和 evaluate 用的是同一个队列，所以清零一定排在它前面 —— 同队列内命令是串行的。
	ID3D12CommandList* lists[]{ slot.commandList };
	_queue->ExecuteCommandLists(1, lists);
	slot.fenceValue = ++_fenceValue;
	_queue->Signal(_fence, slot.fenceValue);
	return _zeroTexturesCleared;
}

bool DlssNrFilter::ExecuteOnList(
	ID3D12GraphicsCommandList* list, const NrEvaluateInput& input) noexcept {
	D3D12Validation::Scope nrPhase(L"NR ExecuteOnList");
	if (_disabled || !_feature || !list || !input.color ||
		!_colorIn || !_output) {
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::NrNotReady);
		return false;
	}
	// 路径锁：这一局归 present 的话，这里一步都不许做 —— 那些纹理/feature 是按
	// backbuffer 的尺寸建的，而且随时可能被 present 线程重建
	if (_mode != NrMode::AtEvaluate) {
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::ModeLocked);
		return false;
	}

	// **尺寸必须和 Prepare 出来的完全一致。** CopyResource 不缩放，尺寸不符只会把
	// 内容写进左上角 —— 那种画面是"左上角一小块在动、其余是残影"，不是报错。
	const D3D12_RESOURCE_DESC colorDesc = input.color->GetDesc();
	if (colorDesc.Width != _fullWidth || colorDesc.Height != _fullHeight) {
		static uint32_t complained = 0;
		if (complained++ < 4) {
			D5_LOG_WARN(L"DLSS5@evaluate：游戏颜色是 %llux%u，我们准备的是 %ux%u，"
				L"尺寸不符这一帧不处理", colorDesc.Width, colorDesc.Height,
				_width, _height);
		}
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::ColorSize);
		return false;
	}
	// CopyResource 要求两侧格式在同一个 typeless 家族里，否则直接非法
	if (PickColorFormat(_device, colorDesc.Format) != _colorFormat) {
		static uint32_t complained = 0;
		if (complained++ < 4) {
			D5_LOG_WARN(L"DLSS5@evaluate：游戏颜色格式 %u 和我们的工作格式 %u 不同族，"
				L"这一帧不处理", (unsigned)colorDesc.Format, (unsigned)_colorFormat);
		}
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::ColorFormat);
		return false;
	}
    ID3D12Resource* finalDecoded = _scaled ? _fullOut : _decoded;
    const auto finalIdle = _scaled ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                   : D3D12_RESOURCE_STATE_COPY_SOURCE;
	// 清零纹理要在**我们自己的**列表上清（SetDescriptorHeaps 会覆盖游戏绑的堆）
	if (!EnsureZeroTexturesCleared()) {
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::NrNotReady);
		return false;
	}
	// Feature creation and zero clears used our own queue. Do not consume them
	// on a potentially different game queue before that work completes.
	const uint64_t setupCompleted = _fence ? _fence->GetCompletedValue() : UINT64_MAX;
	if (setupCompleted == UINT64_MAX || setupCompleted < _fenceValue) {
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::NrNotReady);
		const uint64_t skipped = ++_setupWaitSkips;
		if (skipped <= 3 || skipped % 300 == 0) {
			D5_LOG_INFO(L"NR@evaluate setup pending: skipped=%llu completed=%llu required=%llu",
				skipped, setupCompleted, _fenceValue);
		}
		return false;
	}

	// **先把游戏在这条列表上绑过的东西拍下来，做完原样放回去。**
	//
	// 不还回去会怎样：我们的 dispatch 要 SetDescriptorHeaps 换成自己的堆，
	// 游戏后面的绘制拿着指向它原来那个堆的根描述符表句柄去采样 → 落到我们那个只有
	// 2 个描述符的堆外面 → GPU 越界 → 设备丢失 → **游戏闪退**。实测过。
	//
	// 记账没装上（TrackedCount 为 0）就**直接拒绝这一帧** —— 那时"还原"是个空动作，
	// 我们等于在裸着改游戏的状态。宁可这条路不工作，也不能崩别人的游戏。
	CommandListTracker& tracker = CommandListTracker::Get();
	if (!CommandListTracker::rootsReady.load() || tracker.TrackedCount() == 0) {
		static bool complained = false;
		if (!complained) {
			complained = true;
			D5_LOG_ERROR(L"DLSS5@evaluate：命令列表状态记账没装上（跟到 0 条列表）——"
				L"**拒绝执行**。没有它就没法把游戏的描述符堆还回去，会闪退。");
		}
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::StateUntracked);
		return false;
	}
	const bool ldr=input.colorHdrKnown && !input.colorIsHdr;
    if (_evaluateLdr != ldr) {
        D5_LOG_INFO(L"NR color contract: explicitLDR=%u known=%u HDR=%u; %s",
            ldr,input.colorHdrKnown,input.colorIsHdr,ldr?L"display input, no HDR encode":L"existing HDR encode");
        _evaluateLdr=ldr;
        _needsReset=true;
    }
    const CommandListState savedState = tracker.Snapshot(list);
	if (!savedState.RestorableComputeBindings()) {
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::StateUntracked);
		static uint64_t rejected = 0;
		const auto n = ++rejected;
		if (n <= 5 || n % 600 == 0) D5_LOG_WARN(
			L"NR ExecuteOnList rejected bindings: total=%llu list=%p heaps=%u computeRoot=%p resetObserved=%d frames=%llu",
			(unsigned long long)n, list, savedState.heapCount, savedState.computeRootSignature,
			savedState.resetObserved ? 1 : 0, (unsigned long long)_evaluateCount);
		return false;
	}
	CommandListTracker::IgnoreScope ignoreNrBindings;
	// **空跑档：记账、还原、走人，一条 GPU 命令都不下。**
	// 这一档还崩 = 问题不在我们录的任何命令上，而在"碰这条列表"本身或状态存/还上。
	if (_settings.dryRun) {
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_WARN(L"诊断：**空跑**（diagNrDryRun）—— 只做状态记账和还原，"
				L"不下任何 GPU 命令。这一档还崩说明 evaluate 点这条路本身走不通。");
		}
		D5_STAGE_D(NrEvalEnter, _evaluateCount + 1);
		CommandListTracker::Restore(list, savedState);
		D5_STAGE(NrEvalExit);
		++_evaluateCount;
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::None);
		return true;
	}
	// **这条路上的面包屑。** 它跑在游戏的渲染线程上，和 present 那条完全不同 ——
	// 上一次真崩溃时帧环里只有 present 的标记，于是"崩的时候我们在不在里面"
	// 这个最基本的问题都答不上来。detail 放 evaluate 帧号，方便和日志对齐。
	D5_STAGE_D(NrEvalEnter, _evaluateCount + 1);

	// 上一次录的回读缓冲够久了就报出来（不等 fence，见 ArmPixelDump 的说明）
	// EvaluateGpuGate guarantees the previous external list has completed here.
	TryReadExposure();
	ReportPixelDump();
	const uint64_t frameIndex = _evaluateCount + 1;
	// **自动白点要自己把探针开起来。**
	//
	// 这个探针本来是诊断用的，只有手填 diagNrDumpPixels 才会武装。但白点现在靠它
	// 测量 —— 没开诊断的普通用户会一直用 4.0 保底，而实测场景亮度是 30，
	// 信号顶在 Reinhard 的 0.94 处，对比度只剩 1.4%，滤镜等于白跑。
	// "功能只在开了诊断时才真正工作"是最难发现的一类 bug：两边都不报错。
	if (_settings.toneScale <= 0.0f && !_dumpAtFrame) {
		ArmPixelDump(frameIndex + AUTO_WHITE_POINT_FIRST_FRAME, /*quiet=*/true);
		// 打一行出来：这条路自己武装自己，没有日志的话"到底有没有开起来"
		// 只能靠读代码猜，而它一旦没开，症状就是"画面几乎没变化"——
		// 和别的十几种原因长得一模一样。
		D5_LOG_INFO(L"自动白点：探针已自行武装，第 %llu 帧取样"
			L"（在此之前用保底白点 %.2f）",
			(unsigned long long)_dumpAtFrame, EffectiveWhitePoint());
	}
	// **深度/矢量视图那两帧不能占用探针。**
	//
	// 那两档在下面早退（视图本身就是输入，不跑滤镜），于是"处理后"和"滤镜输出"两份
	// 拷贝永远录不下来，_dumpRecordedAt 一直是 0 —— 报告不会来，而 _dumpAtFrame
	// 已经过期，`frameIndex == _dumpAtFrame` 再也不会成立：**探针永久卡死**。
	// 实测就是这样：白点只测到了第一次，之后按 Alt+V 看了眼深度，重测再没发生过。
	// 往后挪一帧重试，代价是零。
	const bool debugEarlyOut =
		_settings.debugView == NrSettings::DebugView::Depth ||
		_settings.debugView == NrSettings::DebugView::Motion;
	if (debugEarlyOut && _dumpAtFrame && frameIndex >= _dumpAtFrame) {
		_dumpAtFrame = frameIndex + 1;
	}
	const bool dumpThisFrame =
		_dumpAtFrame && frameIndex == _dumpAtFrame && EnsureDumpBuffers();
	if (dumpThisFrame) {
		Barrier(list, input.color, input.colorState,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		RecordDumpCopy(list, input.color, _dumpBefore);
		Barrier(list, input.color, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	} else {
		Barrier(list, input.color, input.colorState,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	// 抄走处理**前**的像素（蹭 COPY_SOURCE 这一刻，不多下 barrier）

	// ---- debug 视图：直接把输入画出来，**不跑 DLSSNR、也不做色调往返** ----
	//
	// 这条路上的深度/矢量是**游戏的原分辨率原件**（不像 present 那条要先重采样），
	// 所以在这里看到的才是最接近"DLSSNR 真正吃到什么"的东西。
	// 深度/矢量这两种是"看原始输入"，滤镜跑不跑无所谓，所以在这里早退。
	// Encoded / Diff 不一样 —— 它们要看的东西**在滤镜跑完之后才存在**，
	// 所以那两种走下面的正常流程，只是最后写回的内容换掉。
	const bool debugInputOnly =
		_settings.debugView == NrSettings::DebugView::Depth ||
		_settings.debugView == NrSettings::DebugView::Motion;
	const bool opticalDebug = _settings.debugView == NrSettings::DebugView::Motion &&
		_settings.opticalFlow && (!input.motion || !_settings.useRealMotion);
	if (debugInputOnly && !opticalDebug && _passes.CanVisualize() && finalDecoded) {
		_optical.Suspend();
		const bool motion = _settings.debugView == NrSettings::DebugView::Motion;
		ID3D12Resource* const src = motion ? (input.motion && _settings.useRealMotion ? input.motion : _zeroMotion)
			: (input.depth && _settings.useRealDepth ? input.depth : _zeroDepth);
		// gain 只决定图长什么样（saturate 之前）。写回游戏 tonemap 之前的 HDR 缓冲
		// 所需的放大**不能并进 gain** —— 那样会把内容顶过 1.0 被 saturate 削平。
		// 见 ComputePasses.h 里 RecordVisualize 的说明。
		const float gain = _settings.debugGain > 0.0f ? _settings.debugGain
			: (motion ? 20.0f : 0.05f);
		bool drawn = false;
		if (src) {
			// Only transition the guide being sampled; preserve combined read masks
			// that may also be in use by a concurrent graphics list.
			const auto srcState = src == input.motion ? input.motionState :
				src == input.depth ? input.depthState : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			const bool guideBarrier = !(srcState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			if (guideBarrier) Barrier(list, src, srcState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Barrier(list, finalDecoded, finalIdle,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			drawn = _passes.RecordVisualize(list, src, finalDecoded,
				_fullWidth, _fullHeight, gain, motion, DebugOutScale());
			Barrier(list, finalDecoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			if (drawn) {
				Barrier(list, input.color,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_COPY_DEST);
				list->CopyResource(input.color, finalDecoded);
                Barrier(list, finalDecoded, D3D12_RESOURCE_STATE_COPY_SOURCE, finalIdle);
				Barrier(list, input.color, D3D12_RESOURCE_STATE_COPY_DEST,
					input.colorState);
			} else {
                Barrier(list, finalDecoded, D3D12_RESOURCE_STATE_COPY_SOURCE, finalIdle);
				Barrier(list, input.color,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					input.colorState);
			}
			// 无论画成没画成，摸过的状态都要还回去
			if (guideBarrier) Barrier(list, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, srcState);
		} else {
			Barrier(list, input.color,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				input.colorState);
		}
		CommandListTracker::Restore(list, savedState);
		D5_STAGE(NrEvalExit);
		if (drawn) {
			static NrSettings::DebugView announced = NrSettings::DebugView::Off;
			if (announced != _settings.debugView) {
				announced = _settings.debugView;
				D5_LOG_INFO(L"debug 视图（evaluate 点）已生效：画的是**%s**"
					L"（%ux%u 原分辨率，增益 %.2f）", motion ? L"运动矢量" : L"深度",
					_fullWidth, _fullHeight, gain);
			}
			++_evaluateCount;
		}
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::None);
		_needsReset = true;
		return drawn;
	}

	// **游戏曝光回读（算白点用）。** 曝光纹理是游戏自己填的"当前曝光"，用它算白点
	// 才覆盖整个场景的高光（"画面正中 8x8 亮度/1.5"只代表局部，高光过曝）。白点 =
	// preExposure / exposure。曝光纹理状态观察得到就按观察值还原，观察不到就假设
	// NON_PIXEL_SHADER_RESOURCE（NGX 契约）。
	if (input.exposure && input.exposureFormat && input.exposureStateKnown) {
		if (_exposureFormat != input.exposureFormat) {
			_exposureFormat = input.exposureFormat;
			_gameExposure = 0.0f;   // 格式变了，旧值作废
		}
		_gamePreExposure = input.preExposure > 0.0f ? input.preExposure : 1.0f;
		if (EnsureExposureReadback()) {
			// 曝光纹理在 evaluate 时是 NON_PIXEL_SHADER_RESOURCE（NGX 契约，游戏
			// 把它交给 DLSS 前已转好），直接信契约，不观察（ObservedState 在别的
			// 编译单元里，这里拿不到）。
			const D3D12_RESOURCE_STATES kExpState = input.exposureState;
			Barrier(list, input.exposure, kExpState,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			RecordExposureRead(list, input.exposure);
			Barrier(list, input.exposure, D3D12_RESOURCE_STATE_COPY_SOURCE,
				kExpState);
		}
	}

    ID3D12Resource* encodeSource = input.color;
    if (_scaled) {
        Barrier(list, _downsampled, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _passes.RecordBoxDownsample(list, input.color, _downsampled, _width, _height,
            float(_fullWidth) / float(_width));
        Barrier(list, _downsampled, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        encodeSource = _downsampled;
    }

	// **游戏颜色 -> 压进 0..1 -> _colorIn**，不是直接拷。
	//
	// 直接拷曾经是这里的做法，结果画面一片灰：evaluate 点的颜色是 tonemap 之前的
	// 线性 HDR（实测 3~6），而 DLSSNR 内部按 1.0 = 白 工作，输出整块钉在 0.9922。
	// 见 ToneRoundTrip.h。
	Barrier(list, _colorIn, D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	_dumpWhitePoint = ldr ? 1.0f : EffectiveWhitePoint();   // 诊断要核对的就是这个数
	D5_STAGE(NrEvalEncode);
	const bool encoded = ldr
        ? _passes.RecordLinearResample(list, encodeSource, _colorIn, _width, _height)
        : _passes.RecordEncode(list, encodeSource, _colorIn, _width, _height,
            _dumpWhitePoint, _settings.toneGamma, CurveFlags());
    if (!encoded) {
		// 压不了就整帧放弃，**别把没压过的 HDR 喂进去** —— 那就是一片灰
		Barrier(list, _colorIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_DEST);
		Barrier(list, input.color,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input.colorState);
		CommandListTracker::Restore(list, savedState);
		D5_STAGE(NrEvalExit);
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::ToneUnavailable);
		return false;
	}
	Barrier(list, _colorIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	// 矢量和深度搬到 NGX 读得到的状态。**用游戏的原资源，不用副本** —— 副本是给
	// present 路径用的，这里就在 evaluate 现场，直接读原件更准也更省两次拷贝。
	//
	// **关键：before 已「包含」NON_PIXEL 读位就不 transition。** 鬼武者进场景实测：
	// 矢量/深度是组合读状态（0xC0/0xE0 = NON_PIXEL|PIXEL 多列表并发读），transition
	// 0xC0→0x40 会去掉 PIXEL 位、破坏另一条列表的读 → DEVICE_HUNG（0x887A0006）。
	// DLSSNR 读 0xC0/0xE0 完全合法（已含 NON_PIXEL 位），根本不用动状态。只有不包含
	// NON_PIXEL 位（DEPTH_WRITE/RENDER_TARGET 等写状态）才需要真正 transition。
	const bool useNativeMotion = input.motion && _settings.useRealMotion;
	const bool motionNeedsBarrier = useNativeMotion &&
		(input.motionState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) !=
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const bool depthNeedsBarrier =
		input.depth && _settings.useRealDepth &&
		(input.depthState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) !=
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	if (motionNeedsBarrier) {
		Barrier(list, input.motion, input.motionState,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	if (depthNeedsBarrier) {
		Barrier(list, input.depth, input.depthState,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	ID3D12Resource* nrMotion = useNativeMotion ? input.motion : _zeroMotion;
	bool flowReset = false;
	bool flowActive = false;
	if (!useNativeMotion && _settings.opticalFlow) {
		uint64_t frequency=0; _queue->GetTimestampFrequency(&frequency);
		if (auto* flow=_optical.Record(_device,list,_passes,_colorIn,_settings.opticalQuality,
			(_needsReset && !_optical.Status().active) || input.reset,true,frequency)) {
			nrMotion=flow; flowActive=true; flowReset=_optical.Status().reset;
		}
	} else _optical.Suspend();
	const bool motionSourceChanged = _usedOptical != flowActive;
	_usedOptical = flowActive;
	if (opticalDebug) {
		Barrier(list,finalDecoded,finalIdle,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		const bool drawn=_passes.RecordVisualize(list,nrMotion,finalDecoded,_fullWidth,_fullHeight,
			_settings.debugGain>0?_settings.debugGain:0.025f,true,DebugOutScale());
		Barrier(list,finalDecoded,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
		if(drawn) {
			Barrier(list,input.color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyResource(input.color,finalDecoded);
			Barrier(list,input.color,D3D12_RESOURCE_STATE_COPY_DEST,input.colorState);
			++_evaluateCount;
		} else Barrier(list,input.color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,input.colorState);
		Barrier(list,finalDecoded,D3D12_RESOURCE_STATE_COPY_SOURCE,finalIdle);
		Barrier(list,_colorIn,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
		if(depthNeedsBarrier) Barrier(list,input.depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,input.depthState);
		CommandListTracker::Restore(list,savedState);
		_needsReset=true;
		return drawn;
	}
	CaptureSemanticInput(list, _colorIn);
	EnsureControlMaskFilled(list, 4);
	SetEvaluateParameters(_colorIn, nrMotion, _settings.useRealDepth ? input.depth : nullptr,
		// 游戏自己告诉过 NGX 的话就用它的；没读到才退回设置里的值
		input.hasDepthInverted ? input.depthInverted : _settings.depthInverted,
		_scaled ? _width : input.subrectWidth, _scaled ? _height : input.subrectHeight,
		input.guideSubrectWidth, input.guideSubrectHeight,
		flowActive ? 1.0f : input.mvScaleX, flowActive ? 1.0f : input.mvScaleY, input.jitterX, input.jitterY,
		input.preExposure,
		_needsReset || _settings.forceReset || input.reset || flowReset || motionSourceChanged);
	if (!useNativeMotion) {
		for (int layer = 0; layer < _settings.trueLayers; ++layer) {
			LayerParameters(layer)->Set(SUBRECTS[2].width,_width);
			LayerParameters(layer)->Set(SUBRECTS[2].height,_height);
		}
	}

	// DLSSNR 自己往这条列表里录命令。**它是这条路上最重的一步**（165MB 的网络），
	// 所以单独一颗面包屑 —— 卡在这里和卡在我们的 dispatch 上，要修的地方不一样。
	D5_STAGE(NrEvalNgx);
	// 诊断分半：跳过这一次调用，别的全照跑。见 NrSettings::skipNgx。
	if (_settings.skipNgx) {
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_WARN(L"诊断：**跳过 DLSSNR 的 EvaluateFeature**（diagNrSkipNgx）——"
				L"编码/barrier/写回/状态还原照跑，画面应当逐位不变。"
				L"这一档还崩说明问题不在那次重入 NGX 的调用上。");
		}
		// _output 里是上一帧的内容，直接拿去按比例还原会得到错的 ratio。
		// 拷一份编码结果过去，比值就恒等于 1 —— 真正的"什么都没做"。
		Barrier(list, _colorIn,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		Barrier(list, _output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_DEST);
		list->CopyResource(_output, _colorIn);
		Barrier(list, _output, D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Barrier(list, _colorIn, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
	if (!_settings.skipNgx) {
		GpuEventScope gpu(list, "NRFG/NR-model");
		result = static_cast<NVSDK_NGX_Result>(EvaluateLayers(list));
	}
	if (NVSDK_NGX_FAILED(result)) {
		// **失败也必须把状态原样还回去。** 这条列表是游戏的，它后面的命令按它自己
		// 记的状态走；我们中途改了不还，等于给它埋了个未定义行为。
		// 只有上面真的 transition 过（motionNeedsBarrier/depthNeedsBarrier）才还原；
		// 组合读状态（0xC0/0xE0）压根没动，还原会反向写错。
		if (depthNeedsBarrier) {
			Barrier(list, input.depth,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input.depthState);
		}
		if (motionNeedsBarrier) {
			Barrier(list, input.motion,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input.motionState);
		}
		Barrier(list, _colorIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST);
		Barrier(list, input.color,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input.colorState);
		// 失败路径同样要还状态 —— 我们已经 dispatch 过一次编码 pass 了
		CommandListTracker::Restore(list, savedState);
		D5_STAGE(NrEvalExit);
		D5_LOG_ERROR(L"DLSS5@evaluate EvaluateFeature 失败: 0x%08X",
			(unsigned)result);
		Fail("DLSS5@evaluate evaluate 失败");
		_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::EvaluateFailed);
		return false;
	}
	_needsReset = false;
	_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::None);

	// 压缩域的一对：喂进去的 vs 吐出来的。两者此刻都在 NON_PIXEL_SHADER_RESOURCE，
	// 而 CopyTextureRegion 要 COPY_SOURCE —— 所以这里要各下一对 barrier。
	// 只在被 arm 的那一帧做，平时零成本。
	if (dumpThisFrame && _dumpFilterIn && _dumpFilterOut) {
		Barrier(list, _colorIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		RecordDumpCopy(list, _colorIn, _dumpFilterIn);
		Barrier(list, _colorIn, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Barrier(list, _output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		RecordDumpCopy(list, _output, _dumpFilterOut);
		Barrier(list, _output, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	// Encoded / Diff：把要看的东西画进 _decoded，替代正常的还原结果。
	// **刻意放在滤镜跑完之后** —— Diff 要的就是"滤镜改了多少"，跳过滤镜就没得比。
	const bool debugAfterFilter =
		_settings.debugView == NrSettings::DebugView::Encoded ||
		_settings.debugView == NrSettings::DebugView::Diff;
	if (debugAfterFilter && _passes.CanVisualize()) {
		const bool diff = _settings.debugView == NrSettings::DebugView::Diff;
		// **写回倍数和增益必须分开，而且写回倍数只能在 saturate 之后乘。**
		//
		// 这些图写进的是游戏的**场景颜色缓冲**（evaluate 之前的线性 HDR），
		// 之后游戏还会做 DLSS 上采样 + tonemap。0..1 的图直接写进去，
		// 游戏当线性 HDR 看，tonemap 之后是一张死暗的图 —— 所以确实需要放大。
		//
		// 但之前是把这个倍数**并进 gain**（saturate 之前参与运算）：
		// 编码值 0.79 × 19.57 = 15.5，saturate 之后是 **1.0，整张图每个像素都一样**。
		// "压缩后的输入"从那天起就是一张纯色图，而它看起来正好像"输入一片灰"——
		// 我们当时正在怀疑的那个结论。**假证据比没有证据坏得多。**
		const float outScale = DebugOutScale();
		// **差值的默认增益从 50 降到 10。**
		//
		// 50 倍那会儿 0.005 的改动就画成纯白，一张"大面积饱和"的图看起来像
		// "滤镜在疯狂改动"，而实际只有 0.5%。诊断图的增益太大和太小一样糟：
		// 太小看不见，太大**把量级信息毁掉**，只剩"有/无"。
		const float gain = _settings.debugGain > 0.0f ? _settings.debugGain
			: (diff ? 10.0f : 1.0f);
		if (diff) {
			static bool told = false;
			if (!told) {
				told = true;
				D5_LOG_INFO(L"差值视图：增益 %.1f 倍（改动 %.4f 显示成中灰 0.5，"
					L"大面积纯白说明改动 >= %.4f）", gain, 0.5f / gain, 1.0f / gain);
			}
		}
		Barrier(list, _output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Barrier(list, finalDecoded, finalIdle,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		const bool ok = diff
			? _passes.RecordVisualizeDiff(list, _output, _colorIn, finalDecoded,
				_fullWidth, _fullHeight, gain, outScale)
			// Do not stretch the whole image using a centre 8x8 sample.
			// This is an input preview still subject to the game's tonemapper.
			: _passes.RecordVisualizeRaw(list, _colorIn, finalDecoded,
				_fullWidth, _fullHeight, gain, outScale);
		Barrier(list, finalDecoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		Barrier(list, _output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (ok) {
			static NrSettings::DebugView announced = NrSettings::DebugView::Off;
			if (announced != _settings.debugView) {
				announced = _settings.debugView;
				D5_LOG_INFO(L"debug 视图：%s（增益 %.1f，写出倍数 %.1f）；预览仍经过游戏后续调色",
					diff ? L"差值（滤镜改了多少，全黑=没改）"
						: L"Encoded 输入预览（无自动对比度拉伸）",
					gain, outScale);
			}
			// 跳过下面的还原，直接走写回
			Barrier(list, input.color,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyResource(input.color, finalDecoded);
                Barrier(list, finalDecoded, D3D12_RESOURCE_STATE_COPY_SOURCE, finalIdle);
			Barrier(list, input.color, D3D12_RESOURCE_STATE_COPY_DEST,
				input.colorState);
			Barrier(list, _colorIn,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_DEST);
			Barrier(list, input.motion,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				input.motionState);
			if (input.depth) {
				Barrier(list, input.depth,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					input.depthState);
			}
			CommandListTracker::Restore(list, savedState);
			D5_STAGE(NrEvalExit);
			_failureCount = 0;
			++_evaluateCount;
			_lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::None);
			return true;
		}
        Barrier(list, finalDecoded, D3D12_RESOURCE_STATE_COPY_SOURCE, finalIdle);
	}

	// **_output -> 还原回线性 HDR -> _decoded -> 拷回游戏的颜色缓冲。**
	// 中间必须多一张 _decoded：游戏的颜色资源不一定带 ALLOW_UNORDERED_ACCESS，
	// 建不出 UAV，没法直接解码进去。
	Barrier(list, _output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Barrier(list, _decoded, D3D12_RESOURCE_STATE_COPY_SOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	// Decode proxy and model into the same linear domain, then map only the
	// model residual back to the original. The original carries HDR headroom.
	// Keep the existing approximate-decode fallback if the main PSO is unavailable.
	D5_STAGE(NrEvalDecode);
	// **还原必须用和编码同一个白点、同一条曲线**（_dumpWhitePoint 就是这一帧编码时
	// 用的那个快照）。中间自动白点可能已经被更新过，现算就会错一档。
	if (ldr) {
        if (!_passes.RecordColourBlend(list, encodeSource, _output, _decoded, _width, _height,
            _scaled ? 1.0f : _settings.colourStrength, _scaled ? 1.0f : _settings.selfLayers))
            _passes.RecordLinearResample(list, _output, _decoded, _width, _height);
    } else if (!_passes.RecordDecodeRatio(list, encodeSource, _output, _decoded,
        _width, _height, _dumpWhitePoint, _settings.toneGamma, CurveFlags(),
        _scaled ? 1.0f : _settings.colourStrength, _scaled ? 1 : _settings.selfLayers)) {
        _passes.RecordDecode(list, _output, _decoded, _width, _height,
            _dumpWhitePoint, _settings.toneGamma, CurveFlags());
    }
	Barrier(list, _decoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (_scaled) {
        // Decode in the same HDR space as the 100% route, then transfer that
        // relative correction to the untouched full-resolution SR result.
        Barrier(list, _decoded, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _passes.RecordRatioMake(list, _decoded, _downsampled, _ratio, _width, _height);
        Barrier(list, _ratio, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _passes.RecordRatioApply(list, input.color, _ratio, _fullOut, _fullWidth, _fullHeight, _settings.colourStrength, _settings.selfLayers);
        Barrier(list, _ratio, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(list, _decoded, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, _fullOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
	// 抄走处理**后**的像素。抄的是 _decoded 而不是 _output —— 要看的是"最终写回游戏
	// 的是什么"，压缩域里的数字没有可比性。
	if (dumpThisFrame) {
		RecordDumpCopy(list, finalDecoded, _dumpAfter);
		_dumpRecordedAt = frameIndex;
	}
	D5_STAGE(NrEvalWriteBack);
	// 诊断分半：不写回。**这是这条路上唯一改动游戏状态的动作**，
	// 也是唯一有替代方案的（可以改成让 NGX 读我们的纹理）。见 NrSettings::skipWriteBack。
	if (_settings.skipWriteBack) {
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_WARN(L"诊断：**不写回游戏的颜色缓冲**（diagNrSkipWriteBack）——"
				L"读、编码、dispatch、状态还原全照跑，只是不动它的资源。"
				L"这一档还崩，说明连"
				L"「读游戏资源 + 下 barrier + 跑我们的 dispatch」都会出事。");
		}
		// 颜色只被读过（NON_PIXEL_SHADER_RESOURCE），原样还回去就行。
		Barrier(list, input.color,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input.colorState);
	} else {
	Barrier(list, input.color,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyResource(input.color, finalDecoded);

	// **写回之后立刻读一次游戏的颜色缓冲。**
	// 前面所有采样都在我们自己的纹理上 —— 它们证明不了"游戏真的用了这份数据"。
	// 差值图显示滤镜在大幅改动，而最终画面完全正常，这两件事只有一种解释方式，
	// 而这是唯一能直接验证它的地方。
	if (dumpThisFrame && _dumpWritten) {
		Barrier(list, input.color, D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		RecordDumpCopy(list, input.color, _dumpWritten);
		Barrier(list, input.color, D3D12_RESOURCE_STATE_COPY_SOURCE,
			input.colorState);
	} else {
		Barrier(list, input.color, D3D12_RESOURCE_STATE_COPY_DEST,
			input.colorState);
	}
	}
    Barrier(list, finalDecoded, D3D12_RESOURCE_STATE_COPY_SOURCE, finalIdle);
	Barrier(list, _output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Barrier(list, _colorIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_COPY_DEST);
	// 还原矢量/深度状态：只有上面真的 transition 过才还原（组合读 0xC0/0xE0 没动，
	// 反向还原会写错）。
	if (motionNeedsBarrier) {
		Barrier(list, input.motion,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input.motionState);
	}
	if (depthNeedsBarrier) {
		Barrier(list, input.depth,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input.depthState);
	}

	// **把游戏的渲染状态放回去。** 我们的 dispatch 和 NGX 内部的 dispatch 都覆盖过
	// 描述符堆 / root signature / PSO；不还回去游戏就会拿着失效的根表句柄去采样。
	CommandListTracker::Restore(list, savedState);
	D5_STAGE(NrEvalExit);

	// Fail() stops only a consecutive failure streak. Isolated transient
	// failures must not accumulate across otherwise successful NR frames.
	_failureCount = 0;
	++_evaluateCount;
	// 第一帧跑完就把"它问了什么我们没给"倒出来 —— 这时候 snippet 已经把它想读的键
	// 全问过一遍了
	LogParameterMisses();
	// 曝光回读的延迟 Map（每 30 帧读一次，更新白点）。
	return true;
}

/* ---------------- 游戏曝光回读 ---------------- */

// 确保曝光回读 buffer 存在（1 像素 readback）。
bool DlssNrFilter::EnsureExposureReadback() noexcept {
	if (_exposureReadback || !_device) return _exposureReadback != nullptr;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	// 行距对齐，够 1 像素。曝光纹理 1x1，读 1 个 float 就够。
	desc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
		&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&_exposureReadback)))) {
		return false;
	}
	_exposureReadback->SetName(L"D5Q.NR.ExposureReadback");
	return true;
}

// 把游戏曝光纹理（1x1）拷进 readback。曝光纹理必须已经在 COPY_SOURCE。
void DlssNrFilter::RecordExposureRead(
	ID3D12GraphicsCommandList* list, ID3D12Resource* exposure) noexcept {
	if (!list || !exposure || !_exposureReadback) return;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = exposure;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = _exposureReadback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Offset = 0;
	dst.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(_exposureFormat);
	dst.PlacedFootprint.Footprint.Width = 1;
	dst.PlacedFootprint.Footprint.Height = 1;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
	D3D12_BOX box{ 0, 0, 0, 1, 1, 1 };
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
}

// 延迟读回曝光值。曝光是每帧拷的，这里每 N 帧 Map 一次 —— 不严格 fence（接受偶尔
// 旧值，白点略偏不会崩），因为这条列表是游戏的、提交时机不归我们管。
void DlssNrFilter::TryReadExposure() noexcept {
	if (!_exposureReadback) return;
	if (_evaluateCount - _exposureReadFrame < 30) return;
	_exposureReadFrame = _evaluateCount;
	void* mapped = nullptr;
	D3D12_RANGE range{ 0, 16 };
	if (FAILED(_exposureReadback->Map(0, &range, &mapped)) || !mapped) return;
	const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
	float value = 0.0f;
	switch (_exposureFormat) {
	case DXGI_FORMAT_R32_FLOAT:
		value = *reinterpret_cast<const float*>(bytes);
		break;
	case DXGI_FORMAT_R16_FLOAT:
		value = DecodeSmallFloat(*reinterpret_cast<const uint16_t*>(bytes), 10);
		break;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16_FLOAT:
		// 取第一个分量（半精度）。
		value = DecodeSmallFloat(*reinterpret_cast<const uint16_t*>(bytes), 10);
		break;
	default:
		value = 0.0f;
		break;
	}
	D3D12_RANGE nothing{ 0, 0 };
	_exposureReadback->Unmap(0, &nothing);
	if (std::isfinite(value) && value > 1e-6f) {
		_gameExposure = value;
	}
}

/* ---------------- 像素回读诊断 ---------------- */

bool DlssNrFilter::EnsureDumpBuffers() noexcept {
	if (_dumpBefore && _dumpAfter) return true;
	const uint32_t stride = BytesPerPixel(_colorFormat);
	if (!stride || !_device) return false;

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	// 行距必须对齐到 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT，不是 宽*字节
	desc.Width = UINT64(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * DUMP_SIZE;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource** targets[]{
		&_dumpBefore, &_dumpAfter, &_dumpFilterIn, &_dumpFilterOut,
		&_dumpWritten };
	const wchar_t* names[]{
		L"D5Q.NR.Dump.Before", L"D5Q.NR.Dump.After",
		L"D5Q.NR.Dump.FilterIn", L"D5Q.NR.Dump.FilterOut",
		L"D5Q.NR.Dump.Written" };
	for (int i = 0; i < 5; ++i) {
		if (*targets[i]) continue;
		if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(targets[i])))) {
			D5_LOG_WARN(L"像素回读缓冲建不出来，诊断跳过");
			return false;
		}
		(*targets[i])->SetName(names[i]);
	}
	return true;
}

// source 必须**已经在 COPY_SOURCE 状态**。调用点刻意挑在已有的两次 transition
// 之后，这样不多下任何 barrier。
void DlssNrFilter::RecordDumpCopy(
	ID3D12GraphicsCommandList* list,
	ID3D12Resource* source,
	ID3D12Resource* destination) noexcept {
	if (!list || !source || !destination) return;

	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = source;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = destination;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Offset = 0;
	dst.PlacedFootprint.Footprint.Format = _colorFormat;
	dst.PlacedFootprint.Footprint.Width = DUMP_SIZE;
	dst.PlacedFootprint.Footprint.Height = DUMP_SIZE;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

	// 取画面正中那一小块 —— 边角常常是天空或纯色，看不出动态范围
	D3D12_BOX box{};
	box.left = _width / 2;
	box.top = _height / 2;
	box.right = box.left + DUMP_SIZE;
	box.bottom = box.top + DUMP_SIZE;
	box.back = 1;
	// **取样区必须按被抄那张纹理自己的尺寸算，不是按 _width/_height。**
	// 降分辨率之后这两个值是"滤镜跑的分辨率"，而 _dumpBefore 抄的是全分辨率的
	// backbuffer —— 用低分辨率的中心去抄全分辨率的图，抄到的不是画面中心，
	// 而且尺寸校验也会失效。诊断抄错位置比不抄更坏。
	const D3D12_RESOURCE_DESC srcDesc = source->GetDesc();
	const uint32_t srcWidth = uint32_t(srcDesc.Width);
	const uint32_t srcHeight = srcDesc.Height;
	box.left = srcWidth / 2;
	box.top = srcHeight / 2;
	box.right = box.left + DUMP_SIZE;
	box.bottom = box.top + DUMP_SIZE;
	if (box.right > srcWidth || box.bottom > srcHeight) return;

	list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
}

void DlssNrFilter::ReportPixelDump() noexcept {
	if (_dumpReported || !_dumpRecordedAt || !_dumpBefore || !_dumpAfter) return;
	if (!_dumpForce && _evaluateCount < _dumpRecordedAt + DUMP_SETTLE_FRAMES) {
		return;
	}
	_dumpReported = true;

	const uint32_t stride = BytesPerPixel(_colorFormat);
	struct Stat { float lo[3]; float hi[3]; double sum[3]; };
	auto measure = [&](ID3D12Resource* buffer, Stat& stat, bool& decoded) {
		for (int c = 0; c < 3; ++c) {
			stat.lo[c] = 1e30f;
			stat.hi[c] = -1e30f;
			stat.sum[c] = 0.0;
		}
		decoded = false;
		void* mapped = nullptr;
		D3D12_RANGE range{ 0, SIZE_T(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * DUMP_SIZE };
		if (FAILED(buffer->Map(0, &range, &mapped)) || !mapped) return;
		const auto* base = static_cast<const uint8_t*>(mapped);
		for (uint32_t y = 0; y < DUMP_SIZE; ++y) {
			const uint8_t* row = base + size_t(y) * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
			for (uint32_t x = 0; x < DUMP_SIZE; ++x) {
				float rgb[3]{};
				if (!DecodePixel(_colorFormat, row + size_t(x) * stride, rgb)) {
					buffer->Unmap(0, nullptr);
					return;
				}
				decoded = true;
				for (int c = 0; c < 3; ++c) {
					stat.lo[c] = rgb[c] < stat.lo[c] ? rgb[c] : stat.lo[c];
					stat.hi[c] = rgb[c] > stat.hi[c] ? rgb[c] : stat.hi[c];
					stat.sum[c] += rgb[c];
				}
			}
		}
		buffer->Unmap(0, nullptr);
	};

	Stat before{}, after{};
	bool okBefore = false, okAfter = false;
	measure(_dumpBefore, before, okBefore);
	measure(_dumpAfter, after, okAfter);
	if (!okBefore || !okAfter) {
		D5_LOG_WARN(L"像素回读：格式 %u 还没有解码器，只能看原始字节",
			(unsigned)_colorFormat);
		return;
	}

	// **抄到全黑就自动重试。**
	//
	// 第一次在真实游戏上量，处理前三个通道全是 0.0000 —— 不是"输入太亮"，是采样的那一帧
	// 本身就是黑的（第 300 帧 ≈ DLSS 起来后 5 秒，游戏还在淡入 / 过场）。
	// 一个必须靠人掐时间才有意义的探针是不合格的：它应该自己等到画面上有东西。
	const bool blank = before.hi[0] <= 0.0f && before.hi[1] <= 0.0f &&
		before.hi[2] <= 0.0f;
	if (blank && _dumpAttempts + 1 < MAX_DUMP_ATTEMPTS) {
		++_dumpAttempts;
		_dumpReported = false;
		_dumpRecordedAt = 0;
		_dumpAtFrame = _evaluateCount + DUMP_RETRY_FRAMES;
		D5_LOG_INFO(L"像素回读：这一帧的采样区整个是黑的（画面大概还在淡入），"
			L"第 %u 次重试改到第 %llu 帧",
			_dumpAttempts, (unsigned long long)_dumpAtFrame);
		return;
	}

	constexpr double COUNT = double(DUMP_SIZE) * DUMP_SIZE;

	// The fallback uses only a centre sample, not full-frame exposure metering.
	// Map neutral grey to encoded 0.5 using the ACTIVE transfer function.
	// Do not reuse Reinhard's x/(1+x)=0.6 target for Hybrid+sRGB.
	const float whitePointUsed = EffectiveWhitePoint();   // 更新前的值
	// **这一次要不要打详细诊断，必须在重新武装之前定下来。** 下面那个 re-arm 会把
	// _dumpQuiet 置起来（让后续的白点重测保持安静），先读后写，否则第一次的详细
	// 诊断会被自己刚设的标志挡掉 —— 那正好是最需要它的一次。
	const bool quiet = _dumpQuiet;
	// **滤镜到底动了多少 —— 这个数必须在常驻模式下也能看到。**
	//
	// 它是"DLSSNR 有没有效果"的唯一直接判据，而详细诊断只打第一次，
	// 那一次用的还是没校准的保底白点。只在那里打等于只在最没有代表性的时刻打。
	Stat fin{}, fout{};
	bool okFilter = false;
	double filterDelta = -1.0;
	if (_dumpFilterIn && _dumpFilterOut) {
		bool okIn = false, okOut = false;
		measure(_dumpFilterIn, fin, okIn);
		measure(_dumpFilterOut, fout, okOut);
		okFilter = okIn && okOut;
		if (okFilter) {
			filterDelta = 0.0;
			for (int c = 0; c < 3; ++c) {
				const double d = fabs(fout.sum[c] - fin.sum[c]) / COUNT;
				filterDelta = d > filterDelta ? d : filterDelta;
			}
			// 编码值的实际区间，给 debug 视图做对比度拉伸用。
			float lo = fin.lo[0], hi = fin.hi[0];
			for (int c = 1; c < 3; ++c) {
				lo = fin.lo[c] < lo ? fin.lo[c] : lo;
				hi = fin.hi[c] > hi ? fin.hi[c] : hi;
			}
			// 只有 8x8 的中心块，全画面的区间肯定更宽 —— 往两边各放 15%，
			// 否则拉伸会把画面上大部分区域推到 0 或 1。
			const float pad = (hi - lo) * 0.15f + 0.02f;
			_lastEncodedLo = lo - pad < 0.0f ? 0.0f : lo - pad;
			_lastEncodedHi = hi + pad > 1.0f ? 1.0f : hi + pad;
		}
	}
	{
		const double luma = 0.2126 * before.sum[0] / COUNT +
			0.7152 * before.sum[1] / COUNT + 0.0722 * before.sum[2] / COUNT;
		if (luma > 1e-3) {
			// Below the Hybrid knee, sRGB(0.214041)=0.5. The legacy
			// power encoder instead needs pow(0.5, gamma).
			const double target = NrSampleTargetLinear(_settings.pureGamma, _settings.toneGamma);
			const float suggested = ClampWhitePoint(float(luma / target));
			const bool manual = _settings.toneScale > 0.0f;
			_lastSceneLuma = float(luma);   // debug 视图的写出倍数要用
			if (!manual) {
				// 平滑而不是直接跳：场景亮度会随镜头变，白点每帧乱跳会让画面呼吸。
				// 第一次直接采纳（没有历史可平滑），之后每次只走 25%。
				_autoWhitePoint = ClampWhitePoint(_autoWhitePoint > 0.0f
					? _autoWhitePoint * 0.75f + suggested * 0.25f : suggested);
			}
			// 常驻模式下只留这一行，而且降到 info —— 它是"白点现在是多少"的记录，
			// 不是警告。手填白点时也照打：那是判断"手填的值离测出来的差多远"的唯一依据。
			if (quiet) {
				if (filterDelta >= 0.0) {
					D5_LOG_INFO(L"自动白点（中心采样，目标灰阶0.5）：亮度 %.3f -> 建议 %.2f，现在用 %.2f%s"
						L"；滤镜改动 %.5f（压缩域 %.4f -> %.4f）",
						luma, suggested, EffectiveWhitePoint(),
						manual ? L"（手填）" : L"",
						filterDelta, fin.sum[1] / COUNT, fout.sum[1] / COUNT);
				} else {
					D5_LOG_INFO(L"自动白点：亮度 %.3f -> 建议 %.2f，现在用 %.2f%s",
						luma, suggested, EffectiveWhitePoint(),
						manual ? L"（手填，想自动就把 nrToneScale 设成 0）" : L"");
				}
			} else {
				D5_LOG_WARN(L"自动白点：场景亮度 %.3f -> 建议白点 **%.2f**"
					L"（这一帧用的是 %.2f，%s）",
					luma, suggested, whitePointUsed,
					manual ? L"手填的，想自动就把 nrToneScale 设成 0"
						: L"自动，下一帧起生效");
			}
		}
	}

	// **重新武装，让白点能跟着场景走。**
	//
	// 这个探针原本是一次性的（诊断用完就完），但白点必须持续跟踪 ——
	// 从室内走到室外场景亮度会差一个数量级，定死的白点在另一个场景里就是错的。
	// 每隔一段时间重测一次，代价是一帧里两次 8x8 的拷贝，可以忽略。
	if (_settings.toneScale <= 0.0f && _dumpAtFrame) {
		_dumpReported = false;
		_dumpRecordedAt = 0;
		_dumpAttempts = 0;
		_dumpAtFrame = _evaluateCount + WHITE_POINT_REFRESH_FRAMES;
		// 详细诊断只打**第一次**。后面这些重测是为白点服务的，
		// 每 5 秒复读一遍十几行排障信息只会把真正的问题埋掉。
		_dumpQuiet = true;
	}
	// 常驻测白点的那一路到此为止，下面全是排障用的详细诊断。
	if (quiet) return;

	D5_LOG_WARN(L"===== 像素回读（画面正中 %ux%u，格式 %u）=====",
		DUMP_SIZE, DUMP_SIZE, (unsigned)_colorFormat);
	D5_LOG_WARN(L"  处理前 R[%.4f..%.4f 均 %.4f] G[%.4f..%.4f 均 %.4f] "
		L"B[%.4f..%.4f 均 %.4f]",
		before.lo[0], before.hi[0], before.sum[0] / COUNT,
		before.lo[1], before.hi[1], before.sum[1] / COUNT,
		before.lo[2], before.hi[2], before.sum[2] / COUNT);
	D5_LOG_WARN(L"  处理后 R[%.4f..%.4f 均 %.4f] G[%.4f..%.4f 均 %.4f] "
		L"B[%.4f..%.4f 均 %.4f]",
		after.lo[0], after.hi[0], after.sum[0] / COUNT,
		after.lo[1], after.hi[1], after.sum[1] / COUNT,
		after.lo[2], after.hi[2], after.sum[2] / COUNT);
	// 直接把结论写出来，别让人对着六组数字自己推
	const double spanBefore = double(before.hi[1]) - double(before.lo[1]);
	const double spanAfter = double(after.hi[1]) - double(after.lo[1]);
	if (spanBefore < 1e-6) {
		// 采样区本身就是纯色（测试目标只做 clear 就是这样）。这时候没资格对"动态范围
		// 塌没塌"下任何结论 —— 诊断不许说自己支持不了的话。
		D5_LOG_WARN(L"  判定：采样的这一小块本身就是纯色，看不出动态范围。"
			L"换个有细节的画面再量。");
	} else if (spanAfter < spanBefore * 0.1) {
		D5_LOG_WARN(L"  判定：输出的动态范围塌了（绿通道 %.4f -> %.4f）—— "
			L"滤镜基本输出了一个常数，不是「输入太亮/太暗」那么简单。",
			spanBefore, spanAfter);
	} else if (before.hi[1] > 4.0f) {
		D5_LOG_WARN(L"  判定：输入是**高动态范围**（绿通道最大 %.2f，远超 1.0）。"
			L"滤镜按 1.0=漫反射白 设计的话，必须先归一化再喂。", before.hi[1]);
	} else {
		D5_LOG_WARN(L"  判定：输入范围看着正常（绿通道最大 %.2f），"
			L"输出也还有动态范围 —— 灰不是量程问题，往别处查。", before.hi[1]);
	}
	if (_mode == NrMode::AtEvaluate) {
		D5_LOG_INFO(L"NR HDR resolve: linear residual anchored to original; curve=%s; whitePoint=%.4f; colour=%.3f",
			_settings.pureGamma ? L"clamp+power" : L"Hybrid+sRGB",
			_dumpWhitePoint, _settings.colourStrength);
	}

	// **滤镜在压缩域里到底动了多少。** 这是"几乎没效果"那个问题的直接判据 ——
	// before/after 那一对隔着压缩和还原，看不清；这一对是滤镜的输入和输出本身。
	// 数值在上面已经量过了（常驻那一行也要用），这里只负责把结论讲清楚。
	if (okFilter) {
		{
			const double maxDelta = filterDelta;
			// A nonlinear transform of the sample mean is NOT the mean of
			// transformed pixels. Report actual sampled ranges; do not label
			// a guessed Reinhard prediction as an encoding failure.
			if (_mode == NrMode::AtEvaluate) {
				D5_LOG_INFO(L"NR encoded sample 8x8: R[%.4f,%.4f] G[%.4f,%.4f] B[%.4f,%.4f]; source=%s; targetGrey=0.5",
					fin.lo[0], fin.hi[0], fin.lo[1], fin.hi[1], fin.lo[2], fin.hi[2],
					_settings.toneScale > 0 ? L"manual" : _gameExposure > 1e-6f ? L"game exposure" : L"centre sample");
			}
			D5_LOG_WARN(L"  %s 滤镜输入 R%.4f G%.4f B%.4f -> "
				L"输出 R%.4f G%.4f B%.4f",
				_scaled ? L"低分辨率" : L"压缩域",
				fin.sum[0] / COUNT, fin.sum[1] / COUNT, fin.sum[2] / COUNT,
				fout.sum[0] / COUNT, fout.sum[1] / COUNT, fout.sum[2] / COUNT);
			D5_LOG_WARN(L"  **%s %.5f**（0..1 域里的绝对值）—— %s",
				_scaled ? L"[A] DLSSNR 在低分辨率上改了"
					: L"滤镜实际改动",
				maxDelta,
				maxDelta < 0.002 ? L"基本等于没动。问题在滤镜自己，不在还原方式上。"
					: maxDelta < 0.02 ? L"很轻微，肉眼在最终画面上大概看不出。"
					: L"这个幅度应该看得见。");
			// **降分辨率时这两个数必须分开看。** [A] 是 DLSSNR 自己做了多少事，
			// [B] 是搬回全分辨率之后最终画面上还剩多少。
			//   A 小        → DLSSNR 在降采样图上本来就没什么可做的（这条路的固有上限）
			//   A 大 && B 小 → 比值搬运把它吃掉了（是我们的 bug，可修）
			// 两种情况在画面上长得一模一样，都是"几乎没效果"，只有这两个数能分开。
			if (_scaled) {
				double bDelta = 0.0;
				for (int c = 0; c < 3; ++c) {
					const double d =
						fabs(after.sum[c] / COUNT - before.sum[c] / COUNT);
					if (d > bDelta) bDelta = d;
				}
				D5_LOG_WARN(L"  **[B] 搬回全分辨率后最终改了 %.5f** —— %s",
					bDelta,
					maxDelta < 0.002
						? L"[A] 本身就接近零，先看 [A]：降采样之后 DLSSNR 没什么可做的。"
						: bDelta > maxDelta * 0.6
							? L"和 [A] 同量级，**比值搬运是忠实的**，"
							  L"损失不在还原环节。"
							: L"明显小于 [A]，**是比值搬运吃掉的**，这一环可修。");
				D5_LOG_WARN(L"  另外注意固有上限：比值图是从 %ux%u 双线性放大到 %ux%u 的，"
					L"**高于低分辨率奈奎斯特频率的细节在数学上就搬不回来**。"
					L"DLSSNR 的可见效果很大一部分正好在那个频段，"
					L"所以即使 [A]=[B]，画面上也会比 100% 弱 —— 这不是 bug，是这条路的代价。",
					_width, _height, _fullWidth, _fullHeight);
			}
		}
	}
	// **写回验证**：游戏的颜色缓冲在我们写完之后是什么样。
	if (_dumpWritten) {
		Stat w{};
		bool okW = false;
		measure(_dumpWritten, w, okW);
		if (okW) {
			const double dAfter = fabs(w.sum[1] / COUNT - after.sum[1] / COUNT);
			// **全零 = 读错了资源，不是写失败。** 一个真的被后续步骤覆盖的缓冲不会
			// 三个通道齐刷刷读出 0.0000；那是抄回读时抄了张还没被填过的纹理
			// （夹具上就是这样）。这两种结论的修法完全不同，别混成一句话。
			const bool allZero =
				w.sum[0] / COUNT < 1e-6 && w.sum[1] / COUNT < 1e-6 &&
				w.sum[2] / COUNT < 1e-6;
			D5_LOG_WARN(L"  写回验证：游戏颜色缓冲现在是 R%.4f G%.4f B%.4f，"
				L"我们打算写的是 R%.4f G%.4f B%.4f —— %s",
				w.sum[0] / COUNT, w.sum[1] / COUNT, w.sum[2] / COUNT,
				after.sum[0] / COUNT, after.sum[1] / COUNT, after.sum[2] / COUNT,
				dAfter < 0.01 ? L"**写进去了**"
					: allZero ? L"读出来整块全零 —— 这一条不算证据，"
						L"是回读抄到了一张空纹理（画面本身正常就忽略它）"
					: L"**没写进去！**游戏用的不是这块缓冲，或者被后续步骤覆盖了");
		}
	}
	D5_LOG_WARN(L"===== 像素回读结束 =====");
}

void DlssNrFilter::LogParameterMisses() noexcept {
	if (_missesLogged || !_parameterBag) return;
	_missesLogged = true;
	const uint32_t missing = _parameterBag->MissCount();
	if (!missing) {
		D5_LOG_INFO(L"DLSSNR：它问的键我们全给上了。");
	} else {
		D5_LOG_WARN(L"DLSSNR **问了但我们没给**的键，共 %u 个 —— "
			L"这就是这个 DLL 的真实接口里我们还不知道的部分：", missing);
		for (uint32_t i = 0; i < missing; ++i) {
			D5_LOG_WARN(L"    %hs", _parameterBag->Miss(i));
		}
	}

	// 另一半，而且是更早的警报：**设了但它从来没问过**的键。
	// 带点的那 16 个子矩形键名错了很久都没发现，因为"设了"看起来就像"生效了"。
	const uint32_t unread = _parameterBag->UnreadCount();
	if (!unread) {
		D5_LOG_INFO(L"DLSSNR：我们设的键它全都读过了，没有白设的。");
		return;
	}
	D5_LOG_WARN(L"DLSSNR **我们设了但它从来没问**的键，共 %u 个 —— "
		L"要么名字错了，要么这个 DLL 不认它。名字错的话是静默失效，务必核对：", unread);
	for (uint32_t i = 0; i < unread; ++i) {
		D5_LOG_WARN(L"    %hs", _parameterBag->Unread(i));
	}
}

// ---- GPU 耗时测量（只在 present 那条路上）----
//
// 这条路的全部代价就是性能：DLSSNR 按输出分辨率算，1920x1080 对 1286x724 是
// 2.2 倍像素。"贵多少"以前只有估算，现在给真数字。
//
// 为什么放在 present 路径而不是通用：那边本来就有命令槽 + fence，槽位轮到下一圈时
// 上一次的结果一定写完了 —— 读回来不需要任何额外同步。evaluate 那条路没有这个条件。
bool DlssNrFilter::EnsureTimingQueries() noexcept {
	if (_timingHeap && _timingReadback) return true;
	if (!_device || !_queue) return false;

	// 队列的时间戳频率。计算/复制队列的频率可能和图形队列不同，所以必须问队列本身，
	// 不能用一个全局值。
	if (!_timingFrequency && FAILED(_queue->GetTimestampFrequency(
			&_timingFrequency))) {
		return false;
	}
	D3D12_QUERY_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	heapDesc.Count = SLOT_COUNT * 2;
	if (!_timingHeap && FAILED(_device->CreateQueryHeap(
			&heapDesc, IID_PPV_ARGS(&_timingHeap)))) {
		return false;
	}
	_timingHeap->SetName(L"D5Q.NR.TimingQueries");

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = sizeof(uint64_t) * SLOT_COUNT * 2;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (!_timingReadback && FAILED(_device->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(&_timingReadback)))) {
		return false;
	}
	_timingReadback->SetName(L"D5Q.NR.TimingReadback");
	return true;
}

void DlssNrFilter::RecordTimingBegin(
	ID3D12GraphicsCommandList* list, uint32_t slotIndex) noexcept {
	if (!_timingHeap || slotIndex >= SLOT_COUNT) return;
	list->EndQuery(_timingHeap, D3D12_QUERY_TYPE_TIMESTAMP, slotIndex * 2);
}

void DlssNrFilter::RecordTimingEnd(
	ID3D12GraphicsCommandList* list, uint32_t slotIndex) noexcept {
	if (!_timingHeap || !_timingReadback || slotIndex >= SLOT_COUNT) return;
	list->EndQuery(_timingHeap, D3D12_QUERY_TYPE_TIMESTAMP, slotIndex * 2 + 1);
	// 两个查询一起解析到 readback 缓冲里。ResolveQueryData 要求目标在 COPY_DEST，
	// 我们建的时候就是这个状态，而且从不改它。
	list->ResolveQueryData(_timingHeap, D3D12_QUERY_TYPE_TIMESTAMP,
		slotIndex * 2, 2, _timingReadback,
		sizeof(uint64_t) * slotIndex * 2);
	_timingArmed[slotIndex] = true;
}

void DlssNrFilter::ReportTiming(uint32_t slotIndex) noexcept {
	if (!_timingReadback || !_timingFrequency || slotIndex >= SLOT_COUNT) return;
	// **只有这个槽位上一圈真的录过才能读** —— 第一圈缓冲里是垃圾。
	if (!_timingArmed[slotIndex]) return;

	// 调用方保证已经等过这个槽位的 fence，所以数据一定写完了。
	const size_t offset = sizeof(uint64_t) * slotIndex * 2;
	D3D12_RANGE range{ offset, offset + sizeof(uint64_t) * 2 };
	void* mapped = nullptr;
	if (FAILED(_timingReadback->Map(0, &range, &mapped)) || !mapped) return;
	uint64_t stamps[2]{};
	memcpy(stamps, static_cast<const uint8_t*>(mapped) + offset, sizeof(stamps));
	_timingReadback->Unmap(0, nullptr);
	if (stamps[1] <= stamps[0]) return;

	const double ms =
		double(stamps[1] - stamps[0]) * 1000.0 / double(_timingFrequency);
	_timingSumMs += ms;
	++_timingSamples;
	if (ms > _timingWorstMs) _timingWorstMs = ms;
	// 界面读数用 EMA：0.06 的系数在 60fps 下大约 0.3 秒跟上，
	// 既不跳字也不会把几分钟前的数字拖进来。第一个样本直接落地，
	// 否则读数会从 0 慢慢爬上来，看起来像"刚开始很便宜"。
	_timingEmaMs = _timingSamples == 1 ? ms : _timingEmaMs * 0.94 + ms * 0.06;
	// 每 120 个样本报一次：够平滑，又不至于让人等太久才看到第一个数字。
	if (_timingSamples % 120 == 0) {
		D5_LOG_INFO(L"DLSS5 GPU 耗时：平均 **%.2f ms**，最差 %.2f ms"
			L"（%ux%u，%u 个样本）—— 60fps 的预算是 16.7ms",
			_timingSumMs / _timingSamples, _timingWorstMs,
			_width, _height, _timingSamples);
	}
}

bool DlssNrFilter::Execute(
	const GpuImage& colorSource, const GpuImage& dest) noexcept {
	if (_disabled || !_feature || !colorSource.IsValid() || !dest.IsValid()) {
		return false;
	}
	// 路径锁，同 ExecuteOnList 里那条 —— 这一局归 evaluate 点的话 present 别插手
	if (_mode != NrMode::Present) return false;
	// 和 SR 同一套判断：源和目标是同一块资源，或者源的格式不是我们的工作格式
	// （sRGB 的代理 backbuffer），都得先拷到中转纹理再喂给 NGX
	const bool aliased = colorSource.resource == dest.resource;
	// 降分辨率时**必须**走 _colorIn —— 降采样的目标就是它。
	const bool viaColorIn = _scaled || aliased ||
		colorSource.resource->GetDesc().Format != _colorFormat;
	if (viaColorIn && !_colorIn) return false;
	if (_scaled && (!_fullOut || !_ratio)) return false;
	// 目标就是我们自己的输出纹理时不用拷贝，直接把状态交出去。
	// **降分辨率时"我们自己的输出"是 _fullOut**（见 Handoff）——
	// 这里认错的话会变成从 _fullOut 拷到 _fullOut 自己。
	const bool destIsOwnOutput =
		dest.resource == (_scaled && _fullOut ? _fullOut : _output);
	// CopyResource 要求两侧尺寸完全一致。不一致时它不会缩放，只会写进左上角 ——
	// 实测踩过：代理生效但 SR 没跑时，852x480 的 NR 输出被拷进 2560x1440 的
	// backbuffer，画面上就是左上角一小块在动、其余是残影。
	if (!destIsOwnOutput) {
		// 降分辨率时最终写回的是 _fullOut（全分辨率），所以按全分辨率比。
		const uint32_t outWidth = _scaled ? _fullWidth : _width;
		const uint32_t outHeight = _scaled ? _fullHeight : _height;
		const D3D12_RESOURCE_DESC destDesc = dest.resource->GetDesc();
		if (destDesc.Width != outWidth || destDesc.Height != outHeight) {
			Fail("NR 输出和目标尺寸不一致，无法拷贝（缺少缩放环节）");
			return false;
		}
	}

	Slot& slot = _slots[_nextSlot % SLOT_COUNT];
	++_nextSlot;
	D5_STAGE_D(NrClaimSlot, slot.fenceValue);
	if (!TryClaimSlot(slot)) return false;
	if (FAILED(slot.allocator->Reset()) ||
		FAILED(slot.commandList->Reset(slot.allocator, nullptr))) {
		Fail("NR 命令列表 Reset 失败");
		return false;
	}
	ID3D12GraphicsCommandList* commandList = slot.commandList;
	D5_STAGE(NrRecord);

	// 取槽位时已经等过这个槽的 fence，所以上一圈的时间戳一定写完了 —— 先读再录。
	const uint32_t timingSlot = uint32_t((_nextSlot - 1) % SLOT_COUNT);
	if (EnsureTimingQueries()) {
		ReportTiming(timingSlot);
		RecordTimingBegin(commandList, timingSlot);
	}

	RecordZeroClears(commandList);

	// **把游戏的真矢量重采样到工作分辨率。**
	//
	// 这条路（backbuffer）的颜色已经是 tonemap 之后的 display-referred 数据，
	// 不需要色调往返；缺的只有矢量 —— 游戏的矢量在它自己的渲染分辨率上
	// （开着 DLSS Quality 时 1286x724），和 backbuffer 的 1920x1080 不一致。
	// 以前的做法是尺寸不符就退回零矢量，等于白抄了一场。
	//
	// 数值不用换算：矢量在归一化 UV 空间，UV 与分辨率无关。重采样完把 MVecScale
	// 改成工作分辨率就对了。
	ID3D12Resource* motionForNgx = _zeroMotion;
	float mvScaleX = _settings.mvScale;
	float mvScaleY = _settings.mvScale;
	if (_externalMotion && _settings.useRealMotion) {
		if (_externalMotionWidth == _width && _externalMotionHeight == _height) {
			motionForNgx = _externalMotion;
			mvScaleX = _externalMvScaleX;
			mvScaleY = _externalMvScaleY;
		} else if (_motionUpscaled && _passes.CanResample() &&
			CanResampleMotion()) {
			// _externalMotion 不用下 barrier：它是旁听那边的副本，常驻
			// NON_PIXEL_SHADER_RESOURCE，正好是 SRV 要的状态。
			//
			// **跨队列的顺序也是安全的**：那张副本是游戏的命令列表写的，而我们提交到
			// _queue —— 那就是从游戏 present 抓来的**同一个队列**。同队列内命令串行，
			// 所以"游戏拷完矢量"一定排在"我们读它"之前。换个队列就要 fence 了。
			Barrier(commandList, _motionUpscaled,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (_passes.RecordMotionResample(commandList, _externalMotion,
				_motionUpscaled, _width, _height)) {
				motionForNgx = _motionUpscaled;
				// 重采样之后每个矢量值代表的屏幕距离变了 —— 按分辨率比把
				// MVecScale 换算到工作分辨率。**对 UV / NDC / 像素约定都对**
				// （它只是"值→像素"换算的重新表达）：UV 约定下等于 _width/_height
				// （和旧版写死的值一致，向后兼容）；NDC 约定（DS2）得到
				// ±半宽/±半高。旧版在这里写死 float(_width)，NDC 游戏的矢量
				// 缩放会错一倍还翻不了向 —— 那正是"拿不到矢量"的另一半。
				mvScaleX = _externalMvScaleX * float(_width) /
					float(_externalMotionWidth);
				mvScaleY = _externalMvScaleY * float(_height) /
					float(_externalMotionHeight);
			}
			Barrier(commandList, _motionUpscaled,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
	}

	// 深度同理。**深度不是 UV 空间的量，但深度值本身与分辨率无关**（都是 NDC/[0,1]
	// 里的同一个视锥），所以重采样到工作分辨率是几何正确的 —— 只是必须用最近邻，
	// 双线性会在轮廓上插出不存在的中间深度。
	ID3D12Resource* depthForNgx = nullptr;
	if (_externalDepth && _settings.useRealDepth) {
		if (_externalDepthWidth == _width && _externalDepthHeight == _height) {
			depthForNgx = _externalDepth;
		} else if (_depthUpscaled && _passes.CanResample()) {
			Barrier(commandList, _depthUpscaled,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (_passes.RecordDepthResample(commandList, _externalDepth,
				_depthUpscaled, _width, _height)) {
				depthForNgx = _depthUpscaled;
			}
			Barrier(commandList, _depthUpscaled,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		static bool reportedDepth = false;
		if (!reportedDepth) {
			reportedDepth = true;
			D5_LOG_INFO(L"深度重采样：%ux%u -> %ux%u（最近邻）%s",
				_externalDepthWidth, _externalDepthHeight, _width, _height,
				depthForNgx ? L"" : L" —— **失败，退回零深度**");
		}
	}

	bool debugDrawn = false;
	const bool opticalDebug = _settings.debugView == NrSettings::DebugView::Motion &&
		motionForNgx == _zeroMotion && _settings.opticalFlow;
	// ---- debug 视图：直接把输入画出来，**不跑 DLSSNR** ----
	//
	// 存在的理由是可证伪：开关真深度/真矢量前后画面变了，你得能确认那个变化真的来自
	// 这一路输入。所以这里刻意绕开滤镜 —— 看到的就是原始输入本身。
	if (!opticalDebug && _settings.debugView != NrSettings::DebugView::Off &&
		_passes.CanVisualize()) {
		_optical.Suspend();
		const bool motion = _settings.debugView == NrSettings::DebugView::Motion;
		ID3D12Resource* const src = motion ? motionForNgx : depthForNgx;
		const float gain = _settings.debugGain > 0.0f ? _settings.debugGain
			: (motion ? 20.0f : 0.05f);
		// _output 已经在 UNORDERED_ACCESS（建的时候就是），可视化直接往里写。
		// 写完之后**复用下面 NGX 那条完全一样的搬运路径**（_output -> dest），
		// 所以这里只置一个标志，不另写一套拷贝逻辑 —— 两套搬运迟早会漂移。
		if (src && _passes.RecordVisualize(commandList, src, _scaled ? _fullOut : _output,
			_fullWidth, _fullHeight, gain, motion)) {
			debugDrawn = true;
			// **成功也要留一条日志。** "调试层没报错"只能说明没做非法操作，
			// 不能说明这段真的跑了 —— 同一个坑这个项目已经栽过三次。
			static NrSettings::DebugView announced = NrSettings::DebugView::Off;
			if (announced != _settings.debugView) {
				announced = _settings.debugView;
				D5_LOG_INFO(L"debug 视图已生效：画的是**%s**（%ux%u，增益 %.2f）—— "
					L"这一路**不跑 DLSSNR**，所以你看到的就是原始输入本身",
					motion ? L"运动矢量" : L"深度", _width, _height, gain);
			}
		} else {
			static bool complainedDebug = false;
			if (!complainedDebug) {
				complainedDebug = true;
				D5_LOG_WARN(L"debug 视图：%s还没有可画的输入"
					L"（真%s没接上，或被 A/B 开关关掉了）",
					motion ? L"矢量" : L"深度", motion ? L"矢量" : L"深度");
			}
		}
	}

	// 像素回读也装在这条路上。**不只是为了在测试目标上验夹具** —— backbuffer 这条路
	// 画面是好的，evaluate 那条路是灰的，两边量一次就能直接对比"输入量程差多少"。
	ReportPixelDump();
	const uint64_t frameIndex = _evaluateCount + 1;
	const bool dumpThisFrame =
		_dumpAtFrame && frameIndex == _dumpAtFrame && EnsureDumpBuffers();

	// debug 视图开着时整段跳过 —— 画面就是那张假彩色图，不掺滤镜的效果。
	const bool colorPrepared = !debugDrawn;
	ID3D12Resource* ngxColor = nullptr;
	if (!debugDrawn && _scaled) {
		// **盒式降采样进 _colorIn**，不是拷。比例传源/目标（>1 表示在缩小），
		// 着色器按这个 footprint 取平均 —— 双线性只混 2x2，非整数比例会折叠出摩尔纹，
		// 而这张图正是要喂给 DLSSNR 的。
		ngxColor = _colorIn;
		Barrier(commandList, colorSource.resource, colorSource.state,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		if (dumpThisFrame) {
			// 抄"处理前"要在只读状态下临时转一次；这一路是我们自己的列表，安全。
			Barrier(commandList, colorSource.resource,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			RecordDumpCopy(commandList, colorSource.resource, _dumpBefore);
			Barrier(commandList, colorSource.resource,
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		Barrier(commandList, _colorIn, D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		_passes.RecordBoxDownsample(commandList, colorSource.resource, _colorIn,
			_width, _height,
			float(_fullWidth) / float(_width ? _width : 1));
		Barrier(commandList, _colorIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	} else if (!debugDrawn && viaColorIn) {
		ngxColor = _colorIn;
		Barrier(commandList, colorSource.resource, colorSource.state,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		commandList->CopyResource(_colorIn, colorSource.resource);
		if (dumpThisFrame) {
			RecordDumpCopy(commandList, colorSource.resource, _dumpBefore);
		}
		Barrier(commandList, _colorIn, D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	} else if (!debugDrawn) {
		ngxColor = colorSource.resource;
		Barrier(commandList, colorSource.resource, colorSource.state,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	if (!debugDrawn) {
		bool flowActive = false, flowReset = false;
		if (motionForNgx == _zeroMotion && _settings.opticalFlow) {
			uint64_t frequency=0; _queue->GetTimestampFrequency(&frequency);
			const bool complete = _fence->GetCompletedValue() >= _fenceValue;
			if (auto* flow=_optical.Record(_device,commandList,_passes,ngxColor,_settings.opticalQuality,
				_needsReset && !_optical.Status().active,complete,frequency)) {
				motionForNgx=flow; mvScaleX=mvScaleY=1.0f;
				flowActive=true; flowReset=_optical.Status().reset;
			}
		} else _optical.Suspend();
		const bool motionSourceChanged = _usedOptical != flowActive;
		_usedOptical = flowActive;
		if (opticalDebug) {
			debugDrawn=_passes.RecordVisualize(commandList,motionForNgx,_scaled ? _fullOut : _output,
				_fullWidth,_fullHeight,_settings.debugGain>0?_settings.debugGain:0.025f,true);
			_needsReset=true;
		}
		if (!debugDrawn) {
		CaptureSemanticInput(commandList, ngxColor);
		EnsureControlMaskFilled(commandList, uint32_t((_nextSlot - 1) % SLOT_COUNT));
		SetEvaluateParameters(ngxColor, motionForNgx,
			depthForNgx,                  // nullptr 时 SetEvaluateParameters 用零深度
			_settings.depthInverted,
			_width, _height,
			_width, _height,              // present 路径颜色/矢量/深度同分辨率
			mvScaleX, mvScaleY,
			// present 路径拿不到游戏的 jitter / 预曝光（那是 evaluate 点才有的东西）
			0.0f, 0.0f, 0.0f,
			_needsReset || _settings.forceReset || flowReset || motionSourceChanged ||
			(_settings.opticalFlow && motionForNgx == _zeroMotion));
		// snippet 是社区改过的闭源 DLL，它内部做什么我们不知道。万一它自己阻塞，
		// 这颗面包屑就是唯一能指认它的证据。
		D5_STAGE(NrEvaluate);
		const NVSDK_NGX_Result result = static_cast<NVSDK_NGX_Result>(EvaluateLayers(commandList));
		if (NVSDK_NGX_FAILED(result)) {
			commandList->Close();
			D5_LOG_ERROR(L"DLSSNR EvaluateFeature 失败: 0x%08X", (unsigned)result);
			Fail("DLSSNR evaluate 失败");
			return false;
		}
		_needsReset = false;
		}
	}

	// **降分辨率那条路的收尾：只把"改动量"放大回全分辨率。**
	//
	//   比值 = 低分输出 / 低分输入        （低频、平滑，放大几乎无损）
	//   全分辨率结果 = 全分辨率原图 x 双线性放大的比值
	//
	// 细节全部来自原生分辨率 —— 直接把低分结果插值放大会明显变软，
	// 那等于用 DLSSNR 换来一张更糊的图。
	ID3D12Resource* finalOut = debugDrawn && _scaled ? _fullOut : _output;
	if (!debugDrawn && !_scaled && (_settings.colourStrength < 1.0f || _settings.selfLayers > 1)) {
		Barrier(commandList, _output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Barrier(commandList, _decoded, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		_passes.RecordColourBlend(commandList, ngxColor, _output, _decoded,
			_width, _height, _settings.colourStrength, _settings.selfLayers);
		Barrier(commandList, _output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		finalOut = _decoded;
	}
	if (!debugDrawn && _scaled) {
		Barrier(commandList, _output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		// **把低分辨率那一对也抄下来。** 这是分辨"DLSSNR 自己没做事"和
		// "做了但被我们的比值搬运吃掉了"的唯一判据 —— 两者的修法完全不同，
		// 而画面上长得一模一样（都是"几乎没效果"）。
		if (dumpThisFrame && _dumpFilterIn && _dumpFilterOut) {
			Barrier(commandList, _colorIn,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			RecordDumpCopy(commandList, _colorIn, _dumpFilterIn);
			Barrier(commandList, _colorIn, D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Barrier(commandList, _output,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			RecordDumpCopy(commandList, _output, _dumpFilterOut);
			Barrier(commandList, _output, D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		_passes.RecordRatioMake(commandList, _output, _colorIn, _ratio,
			_width, _height);
		Barrier(commandList, _ratio, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		// 原图此刻还在 NON_PIXEL_SHADER_RESOURCE（降采样读过它），直接当 t0 用
		_passes.RecordRatioApply(commandList, colorSource.resource, _ratio,
			_fullOut, _fullWidth, _fullHeight, _settings.colourStrength, _settings.selfLayers);
		Barrier(commandList, _ratio,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		Barrier(commandList, _output,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		finalOut = _fullOut;
	}

	if (destIsOwnOutput) {
		if (finalOut == _decoded) {
			Barrier(commandList, _decoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Barrier(commandList, dest.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
			commandList->CopyResource(dest.resource, _decoded);
			Barrier(commandList, dest.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
		// 结果留在 _output 里给下一级滤镜。Handoff() 报的就是
		// UNORDERED_ACCESS，所以这里什么都不用做。
	} else {
		Barrier(commandList, finalOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		if (dumpThisFrame) {
			RecordDumpCopy(commandList, finalOut, _dumpAfter);
			_dumpRecordedAt = frameIndex;
		}
		// **同一张资源既当输入又当输出时（aliased，present 那条路的常态），
		// 它此刻的状态取决于我们是"拷"过它还是"采样"过它：**
		//   非降分辨率：上面用 CopyResource 抄进 _colorIn，停在 COPY_SOURCE
		//   降分辨率：  上面是盒式降采样**采样**它，停在 NON_PIXEL_SHADER_RESOURCE
		// 一开始这里写死了 COPY_SOURCE，于是降分辨率一开就每帧两条 barrier 报错
		// （双缓冲两张 backbuffer 各一条）—— 调试层点名的是"Unnamed"资源，
		// 那正是游戏的 backbuffer，不是我们自己的纹理（我们的都 SetName 过）。
		Barrier(commandList, dest.resource,
			aliased ? (colorPrepared ? (_scaled ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
					: D3D12_RESOURCE_STATE_COPY_SOURCE) : colorSource.state)
				: dest.state,
			D3D12_RESOURCE_STATE_COPY_DEST);
		commandList->CopyResource(dest.resource, finalOut);
		Barrier(commandList, dest.resource,
			D3D12_RESOURCE_STATE_COPY_DEST, dest.state);
		Barrier(commandList, finalOut, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (finalOut == _decoded) Barrier(commandList, _decoded,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	}

	// debug 视图那一路根本没碰过 colorSource / _colorIn，所以也不能"还原"它们 ——
	// 下一条 barrier 的 before 状态会对不上，调试层会直接点名。
	if (!aliased && colorPrepared) {
		// 降分辨率那一路是**采样**原图（不是拷），所以它停在 NON_PIXEL_SHADER_RESOURCE。
		// 这里报错的 before 状态会让调试层直接点名，别猜。
		Barrier(commandList, colorSource.resource,
			(viaColorIn && !_scaled) ? D3D12_RESOURCE_STATE_COPY_SOURCE
				: D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			colorSource.state);
	}
	if (viaColorIn && colorPrepared) {
		Barrier(commandList, _colorIn,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST);
	}

	RecordTimingEnd(commandList, timingSlot);
	if (FAILED(commandList->Close())) {
		Fail("NR 命令列表 Close 失败");
		return false;
	}

	D5_STAGE(NrSubmit);
	ID3D12CommandList* lists[]{ commandList };
	_queue->ExecuteCommandLists(1, lists);
    if (_semantic.submitted && _semantic.ctx) _semantic.submitted(_semantic.ctx, _queue, 1, lists);
	slot.fenceValue = ++_fenceValue;
	_queue->Signal(_fence, slot.fenceValue);

	if (!debugDrawn) _failureCount = 0;
	++_evaluateCount;
	LogParameterMisses();
	return true;
}

}  // namespace DXL
