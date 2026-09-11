#include "NgxSession.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <filesystem>

#include "../common/Log.h"

namespace DXL {

namespace {

// 用自己的 project ID 而不是抄别人的。NGX 用它做 CMS/白名单映射，
// 借用别人的 ID 是这次调查里在 Magpie 侧看到的做法，不适合一个新项目。
constexpr char PROJECT_ID[] = "b7e4f2a1-6c39-4d58-9a2e-1f0c7d38b45a";
constexpr char ENGINE_VERSION[] = "DXL-0.2.0";

// NGX 要一个可写目录放它自己的日志和缓存
std::filesystem::path NgxDataDir() noexcept {
	std::error_code ec;
	const std::filesystem::path dir = Log::LogDir().parent_path() / L"ngx";
	std::filesystem::create_directories(dir, ec);
	return dir;
}

// 放 nvngx_dlss.dll / nvngx_dlssg.dll / nvngx_dlssnr.dll 的目录，在 core DLL
// 旁边的 ngx\。注入进别人的进程时，NGX 默认只会在**宿主 exe** 的目录和驱动商店
// 里找这些 DLL —— 游戏目录里显然没有，所以必须把我们自己的目录显式加进搜索表。
std::filesystem::path NgxRuntimeDir(HMODULE selfModule) noexcept {
	wchar_t path[MAX_PATH]{};
	if (!GetModuleFileNameW(selfModule, path, MAX_PATH)) return {};
	std::error_code ec;
	const std::filesystem::path dir =
		std::filesystem::path(path).parent_path() / L"ngx";
	return std::filesystem::exists(dir / L"nvngx_dlss.dll", ec) ? dir
		: std::filesystem::path{};
}

// 把 NGX 自己的内部消息接进我们的日志。这次调查里证明过：这是唯一能看到闭源
// DLL 一侧怎么说的通道，而且默认是关着的。
void NVSDK_CONV NgxLogCallback(
	const char* message,
	NVSDK_NGX_Logging_Level level,
	NVSDK_NGX_Feature sourceComponent) {
	if (!message) return;
	// NGX 的消息自带换行，去掉避免日志里出现空行
	char buffer[1024]{};
	size_t length = 0;
	while (message[length] && length < sizeof(buffer) - 1) ++length;
	while (length && (message[length - 1] == '\n' || message[length - 1] == '\r')) {
		--length;
	}
	if (!length) return;
	memcpy(buffer, message, length);
	D5_LOG_INFO(L"NGX[%hs feature=%u]: %hs",
		level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? "verbose" : "on",
		(unsigned)sourceComponent, buffer);
}

// 游戏是不是**已经有一个活着的 NGX 会话**。
//
// 这是个必须拦住的组合：`NVSDK_NGX_D3D12_Init*` 是每进程每设备一次的，游戏已经在跑
// DLSS 时我们再用自己的 project ID init 一次，会把它的会话弄坏 —— 它的上采样从此失效，
// 3D 场景渲进一个再也不会被解析出来的目标，UI 是之后画的所以看起来完好，症状就是
// "画面糊成一片、UI 正常"。**而且不可逆，关掉我们的开关也救不回来，只能重启游戏。**
//
// 所以这里宁可拒绝也不能试。判据是 DLSS 的 **snippet** 在不在：
//   · `nvngx_dlss.dll` / `sl.dlss.dll` 只有在游戏真的创建了 DLSS feature 之后才会被
//     加载，是"DLSS 正在用"的强信号；
//   · 不能看 `sl.interposer.dll` —— Streamline 的 interposer 无论游戏开不开 DLSS 都会
//     加载，按它判断必然误报（README 里记过这一条）。
//
// 时机：这个检查跑在我们第一次要 init NGX 的时候（第一帧 present），那时游戏的画质
// 设置早就应用完了，所以信号是可靠的。
}  // namespace

bool GameAlreadyOwnsNgx(std::string* detail) noexcept {
	// **我们自己的会话不算。**
	//
	// 判据是"进程里有没有 DLSS 的 snippet"，而我们自己的 SR 一旦创建 feature 就会把
	// `nvngx_dlss.dll` 加载进来 —— 之后再问这个函数就会把自己认成"游戏在跑 DLSS"。
	// 实测踩过：代理第一次建好了，游戏改一次分辨率触发 SetupScaler 重来，这时误判
	// 生效、拒绝重建代理，真超分就莫名降级成了 DLAA。
	// 我们的会话已经起来了就说明这一局已经定了，没有什么要再拦的。
	if (NgxSession::Get().IsInitialized()) return false;

	static const wchar_t* const LIVE_DLSS_MARKERS[]{
		L"nvngx_dlss.dll",    // NGX 的 DLSS SR snippet，创建 feature 时才加载
		L"sl.dlss.dll",       // Streamline 的 DLSS 插件，开了才加载
		L"sl.dlss_d.dll",     // Ray Reconstruction
	};
	for (const wchar_t* name : LIVE_DLSS_MARKERS) {
		if (!GetModuleHandleW(name)) continue;
		char narrow[64]{};
		WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1,
			nullptr, nullptr);
		if (detail) *detail = narrow;
		return true;
	}
	return false;
}

NgxSession& NgxSession::Get() noexcept {
	static NgxSession instance;
	return instance;
}

bool NgxSession::Initialize(
	ID3D12Device* device, HMODULE selfModule, bool allowCoexist) noexcept {
	if (_initialized) return true;
	if (!device) {
		_reason = "no D3D12 device";
		return false;
	}

	// 先看游戏是不是已经在跑 DLSS。是的话**直接拒绝**，别去试 —— 试一次就把它的
	// 画面弄坏了，而且不可逆。详见 GameAlreadyOwnsNgx 上面那段。
	std::string marker;
	if (GameAlreadyOwnsNgx(&marker)) {
		_blockedByGameNgx = true;
		if (!allowCoexist) {
			D5_LOG_ERROR(L"游戏已经在跑自己的 DLSS（进程里有 %hs）—— **拒绝**初始化我们的"
				L"NGX 会话。NGX 每进程每设备只能 init 一次，硬上会把游戏的 DLSS 弄坏"
				L"（画面糊成一片、UI 正常），而且只能重启游戏才能恢复。"
				L"请在游戏里关掉 DLSS 再用本工具的超分，或者只开 DLSS5（它不需要 NGX "
				L"core，可以和游戏原生 DLSS 共存）。", marker.c_str());
			_reason = "游戏自己在跑 DLSS，本工具的超分无法与它共存"
				"（请在游戏里关掉 DLSS，或只开 DLSS5）";
			return false;
		}
		D5_LOG_WARN(L"游戏已经在跑自己的 DLSS（进程里有 %hs），但 diagAllowNgxCoexist "
			L"打开了，仍然继续 init —— **预期会把游戏画面弄坏，且不可逆**。",
			marker.c_str());
	}

	_selfModule = selfModule;

	const std::filesystem::path dataDir = NgxDataDir();
	const std::wstring dataDirString = dataDir.wstring();
	const std::wstring runtimeDirString = NgxRuntimeDir(_selfModule).wstring();

	const wchar_t* searchPaths[2]{};
	unsigned searchPathCount = 0;
	if (!runtimeDirString.empty()) {
		searchPaths[searchPathCount++] = runtimeDirString.c_str();
	} else {
		D5_LOG_WARN(L"no ngx\\nvngx_dlss.dll next to the core DLL; relying on the "
			L"driver store copy, which may not exist");
	}
	searchPaths[searchPathCount++] = dataDirString.c_str();

	NVSDK_NGX_FeatureCommonInfo featureInfo{};
	featureInfo.PathListInfo.Path = searchPaths;
	featureInfo.PathListInfo.Length = searchPathCount;
	featureInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
	featureInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	featureInfo.LoggingInfo.DisableOtherLoggingSinks = false;

	const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(
		PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_VERSION,
		dataDirString.c_str(), device, &featureInfo,
		NVSDK_NGX_Version_API);
	if (NVSDK_NGX_FAILED(result)) {
		D5_LOG_ERROR(L"NVSDK_NGX_D3D12_Init_with_ProjectID failed: 0x%08X",
			(unsigned)result);
		_reason = "NGX init failed (driver too old, or no RTX GPU)";
		return false;
	}

	_device = device;
	_initialized = true;
	D5_LOG_INFO(L"NGX initialised, runtime dir = %s, data dir = %s",
		runtimeDirString.empty() ? L"(driver store)" : runtimeDirString.c_str(),
		dataDirString.c_str());
	QueryCapabilities();
	return true;
}

void NgxSession::QueryCapabilities() noexcept {
	NVSDK_NGX_Parameter* capabilities = nullptr;
	if (NVSDK_NGX_FAILED(
		NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities)) || !capabilities) {
		_reason = "GetCapabilityParameters failed";
		return;
	}

	int available = 0;
	if (NVSDK_NGX_FAILED(capabilities->Get(
		NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
		// 驱动会在这个键里给出拒绝原因
		int reasonCode = 0;
		capabilities->Get(
			NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &reasonCode);
		D5_LOG_WARN(L"DLSS SR unavailable (available=%d initResult=0x%08X)",
			available, (unsigned)reasonCode);
		_reason = "DLSS SR not supported by this GPU/driver";
	} else {
		_srAvailable = true;
		_reason = "";
		int major = 0, minor = 0;
		capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &major);
		D5_LOG_INFO(L"DLSS SR available (needsUpdatedDriver=%d)", major);
		(void)minor;
	}
	NVSDK_NGX_D3D12_DestroyParameters(capabilities);
}

void NgxSession::Shutdown() noexcept {
	if (!_initialized) return;
	NVSDK_NGX_D3D12_Shutdown1(_device);
	_initialized = false;
	_srAvailable = false;
	_device = nullptr;
	D5_LOG_INFO(L"NGX shut down");
}

}  // namespace DXL
