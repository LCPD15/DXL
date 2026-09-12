#pragma once
// NVIDIA Smooth Motion（驱动级 AI 插帧）开关 —— 通过 NVAPI 改 per-app profile。
//
// Smooth Motion 是 NVIDIA App 里的"AI 插帧"（驱动级帧生成），和游戏内 FG 不同：
// 是否可用取决于驱动、游戏及当前图形 API；DRS 设置存在不代表游戏兼容。
//
// 它的开关是**驱动 per-app profile 设置**，游戏启动时读取，运行中改了必须重启游戏
// 才生效 —— 所以 UI 上勾选后只改 profile 并提示"重启游戏生效"，不尝试实时切换。
//
// 实现：动态加载 nvapi64.dll → nvapi_QueryInterface 取函数指针 → DRS 会话改设置。
// 全部 header-only（inline 函数 + 结构体），不改构建脚本。
//
// 反查来源（settingId / 函数 offset）：
//   · Smooth Motion - Enable       = 0xB0D384C0（0/1，per application）
//   · Smooth Motion - Enable APIs  = 0xB0CC0875（bitfield：1=DX12 2=DX11 4=Vulkan）
//   · NVAPI 函数 offset 见下面常量（来自开源 nvapi 封装 / NVIDIA Profile Inspector）

#include <windows.h>
#include <cwchar>
#include <initializer_list>

namespace DXL {
namespace NvSmoothMotion {

/* ---------------- NVAPI 函数 offset（nvapi_QueryInterface 的参数） ---------------- */
constexpr unsigned int kApiInitialize = 0x0150E828;
constexpr unsigned int kApiDrsCreateSession = 0x0694D52E;
constexpr unsigned int kApiDrsDestroySession = 0xDAD9CFF8;
constexpr unsigned int kApiDrsLoadSettings = 0x375DBD6B;
constexpr unsigned int kApiDrsSaveSettings = 0xFCBC7E14;
constexpr unsigned int kApiDrsSetSetting = 0x8A2CF5F5;         // 新驱动
constexpr unsigned int kApiDrsSetSettingFallback = 0x577DD202; // 旧驱动
constexpr unsigned int kApiDrsGetSetting = 0xEA99498D;         // 新驱动
constexpr unsigned int kApiDrsGetSettingFallback = 0x73BF8338; // 旧驱动
constexpr unsigned int kApiDrsFindApplicationByName = 0xEEE566B2;
constexpr unsigned int kApiDrsFindProfileByName = 0x7E4A9A0B;
constexpr unsigned int kApiDrsCreateProfile = 0xCC176068;
constexpr unsigned int kApiDrsCreateApplication = 0x4347A9DE;
constexpr unsigned int kApiDrsDeleteProfileSetting = 0xE4A26362;
constexpr unsigned int kApiSysGetDriverAndBranchVersion = 0x2926AAAD;
constexpr unsigned int kApiEnumPhysicalGpus = 0xE5AC921F;
constexpr unsigned int kApiGpuGetFullName = 0xCEEE8E9F;
constexpr unsigned int kApiUnload = 0xD22BDD7E;

/* ---------------- Smooth Motion 设置 ID 与取值 ---------------- */
constexpr unsigned int kSmEnableId = 0xB0D384C0;      // 0=关 1=开
constexpr unsigned int kSmEnableApisId = 0xB0CC0875;  // API 限制；不得强写 7 绕过驱动预设

/* ---------------- NVAPI 常量 ---------------- */
constexpr unsigned int kDrsDwordType = 0;
constexpr unsigned int kDrsCurrentProfileLocation = 0;
constexpr int kOk = 0;  // NVAPI_OK
constexpr int kSettingNotFound = -160;
constexpr int kProfileNotFound = -163;
constexpr int kExecutableNotFound = -166;
constexpr int kNvidiaDeviceNotFound = -6;
constexpr int kInvalidArgument = -5;
constexpr int kUnavailable = -3;

/* ---------------- 函数指针类型（NVAPI 用 __cdecl） ---------------- */
using QueryInterfaceFn = void* (__cdecl*)(unsigned int);
using InitializeFn = int(__cdecl*)();
using UnloadFn = int(__cdecl*)();
using DrsCreateSessionFn = int(__cdecl*)(void** session);
using DrsDestroySessionFn = int(__cdecl*)(void* session);
using DrsLoadSettingsFn = int(__cdecl*)(void* session);
using DrsSaveSettingsFn = int(__cdecl*)(void* session);
// 新驱动（R570+）把 SetSetting/GetSetting 的签名改了：SetSetting 多了两个 uint 参数
// （都传 0），GetSetting 多了个 uint* 输出参数。x64 __cdecl 下多传参数是安全的（旧
// 3/4 参数实现会忽略多余参数），所以统一用新 5 参数签名。
using DrsSetSettingFn = int(__cdecl*)(void* session, void* profile, void* setting,
	unsigned int x, unsigned int y);
using DrsGetSettingFn = int(__cdecl*)(void* session, void* profile,
	unsigned int settingId, void* setting, unsigned int* x);
using DrsFindApplicationByNameFn = int(__cdecl*)(void* session,
	const wchar_t* appName, void** profile, void* app);
using DrsFindProfileByNameFn = int(__cdecl*)(void* session,
	const wchar_t* profileName, void** profile);
using DrsCreateProfileFn = int(__cdecl*)(void* session, void* profileInfo,
	void** profile);
using DrsCreateApplicationFn = int(__cdecl*)(void* session, void* profile,
	void* app);
using DrsDeleteProfileSettingFn = int(__cdecl*)(void* session, void* profile,
	unsigned int settingId);
using SysGetDriverAndBranchVersionFn = int(__cdecl*)(
	unsigned int* driverVersion, char branchString[64]);
using EnumPhysicalGpusFn = int(__cdecl*)(
	void* gpuHandles[64], unsigned int* gpuCount);
using GpuGetFullNameFn = int(__cdecl*)(void* gpuHandle, char name[64]);

/* ---------------- 结构体（字段顺序/大小必须和 NVAPI 对齐） ---------------- */

// 设置值的三态 union（DWORD / binary / unicode string）。Smooth Motion 是 DWORD
// 设置，只用 u32Value（union 起始 4 字节）；binary（4+4096）撑起 union 大小，保证和
// 官方 NVDRS_SETTING_V1 的 sizeof 完全一致 —— version 字段依赖 sizeof 做 ABI 校验，
// 大小不对 NVAPI 会直接拒绝 SetSetting。
union DrsSettingValue {
	unsigned int u32Value;
	struct { unsigned int valueLength; unsigned char valueData[4096]; } binary;
	wchar_t wszValue[2048];
};

// NVDRS_SETTING_V1。官方字段顺序：version → settingName → settingId → settingType →
// settingLocation → isCurrentPredefined → isPredefinedValid → predefinedValue →
// currentValue。之前漏了 settingName（导致所有字段偏移错 4096 字节）且 predefined/
// current 顺序反了，version 校验不过、SetSetting 失败。
struct DrsSetting {
	unsigned int version;             // MAKE_VERSION(DrsSetting, 1)
	wchar_t settingName[2048];        // String name of setting
	unsigned int settingId;
	unsigned int settingType;         // kDrsDwordType = 0
	unsigned int settingLocation;     // kDrsCurrentProfileLocation = 0
	unsigned int isCurrentPredefined;
	unsigned int isPredefinedValid;
	DrsSettingValue predefined;       // u32PredefinedValue
	DrsSettingValue current;          // u32CurrentValue
};

// NVDRS_APPLICATION_V4。NvAPI_UnicodeString = NvU16[2048]，Windows 下 wchar_t 也是
// 2 字节，所以用 wchar_t[2048] 等价。FindApplicationByName 会填这个结构体，字段顺序
// 和大小必须和 NVAPI 完全一致，否则越界写崩。
struct DrsApplicationV4 {
	unsigned int version;             // MAKE_VERSION(DrsApplicationV4, 4)
	unsigned int isPredefined;
	wchar_t appName[2048];
	wchar_t userFriendlyName[2048];
	wchar_t launcher[2048];
	wchar_t fileInFolder[2048];
	unsigned int flags;               // isMetro:1 isCommandLine:1 reserved:30
	wchar_t commandLine[2048];
};

// NVDRS_PROFILE_V1。NVDRS_GPU_SUPPORT 是一个 32 位 bitfield（geforce/quadro/nvs/
// reserved），用单个 NvU32 等价。CreateProfile 会读 profileName，其余字段填 0。
struct DrsProfileV1 {
	unsigned int version;             // MAKE_VERSION(DrsProfileV1, 1)
	wchar_t profileName[2048];
	unsigned int gpuSupport;
	unsigned int isPredefined;
	unsigned int numOfApps;           // 只读
	unsigned int numOfSettings;       // 只读
};

// NVAPI 的结构体版本字段 = sizeof(type) | (ver << 16)
#define NVSM_MAKE_VERSION(typeName, ver) (sizeof(typeName) | ((ver) << 16))

// 开关操作的返回码（细分失败原因，UI 据此显示不同提示）。
enum class SmResult {
	Ok = 0,
	NvApiUnavailable = 1,   // nvapi64 加载 / 初始化失败（非 N 卡或驱动问题）
	ProfileUnavailable = 2, // 找不到游戏 profile，且无法创建（需 NVIDIA App 添加）
	WriteFailed = 3,        // SetSetting 或 SaveSettings 失败
	InvalidTarget = 4,
	ReadFailed = 5,
};

/* ---------------- 内部：加载 + 取函数指针 ---------------- */

struct NvApi {
	InitializeFn initialize = nullptr;
	UnloadFn unload = nullptr;
	DrsCreateSessionFn createSession = nullptr;
	DrsDestroySessionFn destroySession = nullptr;
	DrsLoadSettingsFn loadSettings = nullptr;
	DrsSaveSettingsFn saveSettings = nullptr;
	DrsSetSettingFn setSetting = nullptr;
	DrsGetSettingFn getSetting = nullptr;
	DrsFindApplicationByNameFn findApplicationByName = nullptr;
	DrsFindProfileByNameFn findProfileByName = nullptr;
	DrsCreateProfileFn createProfile = nullptr;
	DrsCreateApplicationFn createApplication = nullptr;
	DrsDeleteProfileSettingFn deleteProfileSetting = nullptr;
	SysGetDriverAndBranchVersionFn getDriverAndBranchVersion = nullptr;
	EnumPhysicalGpusFn enumPhysicalGpus = nullptr;
	GpuGetFullNameFn gpuGetFullName = nullptr;
	bool loaded = false;
	bool initialized = false;
	HMODULE module = nullptr;
	bool moduleMissing = false;

	~NvApi() {
		if (initialized && unload) unload();
		if (module) FreeLibrary(module);
	}
};

inline bool LoadNvApi(NvApi& api) noexcept {
	if (api.loaded) return true;
	HMODULE mod = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!mod) {
		// A failed load may also mean a broken dependency. Only a genuinely
		// absent system DLL is a harmless 'not installed' result for cleanup.
		wchar_t path[MAX_PATH]{};
		const UINT length = GetSystemDirectoryW(path, MAX_PATH);
		if (length && length + 13 < MAX_PATH) {
			wcscat_s(path, L"\\nvapi64.dll");
			if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
				const DWORD error = GetLastError();
				api.moduleMissing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
			}
		}
		return false;
	}
	api.module = mod;
	auto query = reinterpret_cast<QueryInterfaceFn>(
		GetProcAddress(mod, "nvapi_QueryInterface"));
	if (!query) return false;
	api.initialize = reinterpret_cast<InitializeFn>(query(kApiInitialize));
	api.unload = reinterpret_cast<UnloadFn>(query(kApiUnload));
	api.createSession = reinterpret_cast<DrsCreateSessionFn>(query(kApiDrsCreateSession));
	api.destroySession = reinterpret_cast<DrsDestroySessionFn>(query(kApiDrsDestroySession));
	api.loadSettings = reinterpret_cast<DrsLoadSettingsFn>(query(kApiDrsLoadSettings));
	api.saveSettings = reinterpret_cast<DrsSaveSettingsFn>(query(kApiDrsSaveSettings));
	// SetSetting / GetSetting 的 interface ID 在较新驱动里迁移过（0x8A2CF5F5/0xEA99498D），
	// 旧驱动用 0x577DD202/0x73BF8338。先试新 ID，空则回退旧 ID。
	{
		void* p = query(kApiDrsSetSetting);
		if (!p) p = query(kApiDrsSetSettingFallback);
		api.setSetting = reinterpret_cast<DrsSetSettingFn>(p);
	}
	{
		void* p = query(kApiDrsGetSetting);
		if (!p) p = query(kApiDrsGetSettingFallback);
		api.getSetting = reinterpret_cast<DrsGetSettingFn>(p);
	}
	api.findApplicationByName = reinterpret_cast<DrsFindApplicationByNameFn>(
		query(kApiDrsFindApplicationByName));
	api.findProfileByName = reinterpret_cast<DrsFindProfileByNameFn>(
		query(kApiDrsFindProfileByName));
	api.createProfile = reinterpret_cast<DrsCreateProfileFn>(
		query(kApiDrsCreateProfile));
	api.createApplication = reinterpret_cast<DrsCreateApplicationFn>(
		query(kApiDrsCreateApplication));
	api.deleteProfileSetting = reinterpret_cast<DrsDeleteProfileSettingFn>(
		query(kApiDrsDeleteProfileSetting));
	api.getDriverAndBranchVersion = reinterpret_cast<SysGetDriverAndBranchVersionFn>(
		query(kApiSysGetDriverAndBranchVersion));
	api.enumPhysicalGpus = reinterpret_cast<EnumPhysicalGpusFn>(
		query(kApiEnumPhysicalGpus));
	api.gpuGetFullName = reinterpret_cast<GpuGetFullNameFn>(
		query(kApiGpuGetFullName));
	api.loaded = true;
	return true;
}

inline int InitializeNvApi(NvApi& api) noexcept {
	if (api.initialized) return kOk;
	const int result = api.initialize ? api.initialize() : kUnavailable;
	api.initialized = (result == kOk);
	return result;
}

// 找当前游戏 exe 的 per-app profile。先按完整路径、再按文件名回退。只读查找，
// 找不到返回 nullptr（不创建）。
inline void* FindGameProfile(NvApi& api, void* session,
	const wchar_t* exePath, int* outError = nullptr) noexcept {
	if (outError) *outError = kInvalidArgument;
	if (!exePath || !*exePath || !api.findApplicationByName) return nullptr;
	DrsApplicationV4 app{};
	app.version = NVSM_MAKE_VERSION(DrsApplicationV4, 4);
	void* profile = nullptr;

	// 完整路径
	int result = api.findApplicationByName(session, exePath, &profile, &app);
	if (outError) *outError = result;
	if (result == kOk) {
		return profile;
	}
	if (result != kExecutableNotFound && result != kProfileNotFound) return nullptr;
	// 文件名回退
	const wchar_t* leaf = wcsrchr(exePath, L'\\');
	leaf = leaf ? leaf + 1 : exePath;
	profile = nullptr;
	app = {};
	app.version = NVSM_MAKE_VERSION(DrsApplicationV4, 4);
	result = api.findApplicationByName(session, leaf, &profile, &app);
	if (outError) *outError = result;
	if (result == kOk) {
		return profile;
	}
	return nullptr;
}

// 查找当前游戏 exe 的 per-app profile，找不到就自动创建（建一个以游戏 exe 名命名的
// profile + 把 exe 挂进去）。返回 nullptr 表示既找不到也建不了。
inline void* FindOrCreateGameProfile(NvApi& api, void* session,
	const wchar_t* exePath) noexcept {
	int error = kOk;
	void* profile = FindGameProfile(api, session, exePath, &error);
	if (profile) return profile;
	if (error != kProfileNotFound && error != kExecutableNotFound) return nullptr;
	if (!api.createProfile || !api.createApplication) return nullptr;

	const wchar_t* leaf = wcsrchr(exePath, L'\\');
	leaf = leaf ? leaf + 1 : exePath;

	// profile 名 = exe 文件名去掉 .exe（NVIDIA App / Profile Inspector 都认这种
	// 用户自定义 profile）。
	wchar_t profileName[256]{};
	{
		size_t i = 0;
		while (leaf[i] && leaf[i] != L'.' && i + 1 < 256) {
			profileName[i] = leaf[i];
			++i;
		}
		profileName[i] = 0;
	}
	if (!profileName[0]) wcscpy_s(profileName, L"Game");

	DrsProfileV1 prof{};
	prof.version = NVSM_MAKE_VERSION(DrsProfileV1, 1);
	wcscpy_s(prof.profileName, 2048, profileName);
	prof.isPredefined = 0;
	if (api.createProfile(session, &prof, &profile) != kOk) return nullptr;

	// 把 exe 挂进新 profile
	DrsApplicationV4 newApp{};
	newApp.version = NVSM_MAKE_VERSION(DrsApplicationV4, 4);
	newApp.isPredefined = 0;
	wcscpy_s(newApp.appName, 2048, exePath);
	wcscpy_s(newApp.userFriendlyName, 2048, leaf);
	wcscpy_s(newApp.launcher, 2048, L"");
	wcscpy_s(newApp.fileInFolder, 2048, L"");
	if (api.createApplication(session, profile, &newApp) != kOk) return nullptr;
	return profile;
}

// 读驱动版本，用于诊断（Smooth Motion 需要较新驱动，老驱动会 SETTING_NOT_FOUND）。
// 成功返回 true；drvVersion 回填 NvAPI 原始版本号，branch 回填分支字符串（含版本，
// 如 "r571_86-xx"）。
inline bool ReadDriverVersion(unsigned int* drvVersion, char branch[64]) noexcept {
	if (drvVersion) *drvVersion = 0;
	if (branch) branch[0] = 0;
	NvApi api;
	if (!LoadNvApi(api)) return false;
	if (InitializeNvApi(api) != kOk) return false;
	if (!api.getDriverAndBranchVersion) return false;
	unsigned int v = 0;
	char b[64]{};
	if (api.getDriverAndBranchVersion(&v, b) != kOk) return false;
	if (drvVersion) *drvVersion = v;
	if (branch) {
		for (int i = 0; i < 63; ++i) { branch[i] = b[i]; if (!b[i]) break; }
		branch[63] = 0;
	}
	return true;
}

// 读第一块物理 GPU 的名称，仅用于诊断，不能代表游戏实际使用的显卡。
// Smooth Motion 已支持 RTX 40 / 50；是否兼容具体游戏仍应以 NVIDIA App 为准。
inline bool ReadGpuName(char name[64]) noexcept {
	if (name) name[0] = 0;
	NvApi api;
	if (!LoadNvApi(api)) return false;
	if (InitializeNvApi(api) != kOk) return false;
	if (!api.enumPhysicalGpus || !api.gpuGetFullName) return false;
	void* handles[64]{};
	unsigned int count = 0;
	if (api.enumPhysicalGpus(handles, &count) != kOk || count == 0) return false;
	char n[64]{};
	if (api.gpuGetFullName(handles[0], n) != kOk) return false;
	if (name) {
		for (int i = 0; i < 63; ++i) { name[i] = n[i]; if (!n[i]) break; }
		name[63] = 0;
	}
	return true;
}

// 读 Smooth Motion 当前开关。返回 -1=未知/失败，0=关，1=开。
// outError（可选）回填 NvAPI_DRS_GetSetting 的返回码，便于诊断 setting 是否存在。
inline int GetSmoothMotionForExe(const wchar_t* exePath, int* outError = nullptr) noexcept {
	if (outError) *outError = 0;
	NvApi api;
	if (!LoadNvApi(api)) return -1;
	if (InitializeNvApi(api) != kOk) return -1;

	void* session = nullptr;
	if (!api.createSession || !api.destroySession || !api.loadSettings ||
		api.createSession(&session) != kOk) return -1;
	if (api.loadSettings(session) != kOk) {
		api.destroySession(session);
		return -1;
	}

	void* profile = FindGameProfile(api, session, exePath);
	if (!profile) {
		api.destroySession(session);
		return -1;
	}

	DrsSetting s{};
	s.version = NVSM_MAKE_VERSION(DrsSetting, 1);
	s.settingId = kSmEnableId;
	s.settingType = kDrsDwordType;
	s.settingLocation = kDrsCurrentProfileLocation;
	int result = -1;
	if (api.getSetting) {
		unsigned int x = 0;
		int gr = api.getSetting(session, profile, kSmEnableId, &s, &x);
		if (outError) *outError = gr;
		if (gr == kOk) result = s.current.u32Value ? 1 : 0;
	}
	api.destroySession(session);
	return result;
}

inline int GetSmoothMotion(int* outError = nullptr) noexcept {
	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	return GetSmoothMotionForExe(exePath, outError);
}

// A short DRS session keeps these changes separate from DXL's JSON settings.
// Only save after every requested operation succeeds. NVIDIA profiles can be
// shared by several executables; never remove the profile or unrelated settings.
inline int ReadSetting(NvApi& api, void* session, void* profile,
	unsigned int id, DrsSetting& setting) noexcept {
	setting = {};
	setting.version = NVSM_MAKE_VERSION(DrsSetting, 1);
	setting.settingId = id;
	unsigned int extra = 0;
	if (!api.getSetting) return kUnavailable;
	const int result = api.getSetting(session, profile, id, &setting, &extra);
	return result == kOk && setting.settingType != kDrsDwordType ? kInvalidArgument : result;
}

inline bool IsUserProfileOverride(const DrsSetting& setting) noexcept {
	return setting.settingLocation == kDrsCurrentProfileLocation && !setting.isCurrentPredefined;
}

inline SmResult CleanupEnabledInSession(NvApi& api, void* session,
	const wchar_t* exePath, int* outError) noexcept {
	int error = kOk;
	const auto fail = [&](SmResult result, int code) {
		if (outError) *outError = code;
		return result;
	};
	void* profile = FindGameProfile(api, session, exePath, &error);
	if (!profile) {
		if (error == kProfileNotFound || error == kExecutableNotFound) return SmResult::Ok;
		return fail(SmResult::ReadFailed, error == kOk ? kInvalidArgument : error);
	}
	DrsSetting enabled{};
	error = ReadSetting(api, session, profile, kSmEnableId, enabled);
	if (error == kSettingNotFound) return SmResult::Ok;
	if (error != kOk) return fail(SmResult::ReadFailed, error);
	if (!enabled.current.u32Value) return SmResult::Ok;
	if (!api.deleteProfileSetting || !api.setSetting || !api.saveSettings)
		return fail(SmResult::NvApiUnavailable, kUnavailable);

	DrsSetting apis{};
	const int apiRead = ReadSetting(api, session, profile, kSmEnableApisId, apis);
	if (apiRead != kOk && apiRead != kSettingNotFound)
		return fail(SmResult::ReadFailed, apiRead);
	bool changed = false;
	for (const DrsSetting* setting : { &enabled, &apis }) {
		if (setting == &apis && apiRead != kOk) continue;
		if (!IsUserProfileOverride(*setting)) continue;
		error = api.deleteProfileSetting(session, profile, setting->settingId);
		// Some drivers expose Smooth Motion through the newer Get/Set entry
		// points while the public Delete entry point cannot see those settings.
		// A delete miss is not evidence that the enabled value disappeared:
		// re-read below and explicitly write OFF through the working Set API.
		if (error == kSettingNotFound) continue;
		if (error != kOk) return fail(SmResult::WriteFailed, error);
		changed = true;
	}

	// Restoring the default can inherit an enabled global/predefined value.
	// Keep that global value intact and explicitly disable this game profile.
	error = ReadSetting(api, session, profile, kSmEnableId, enabled);
	if (error != kOk && error != kSettingNotFound) return fail(SmResult::ReadFailed, error);
	if (error == kOk && enabled.current.u32Value) {
		DrsSetting off{};
		off.version = NVSM_MAKE_VERSION(DrsSetting, 1);
		off.settingId = kSmEnableId;
		off.settingType = kDrsDwordType;
		off.settingLocation = kDrsCurrentProfileLocation;
		error = api.setSetting(session, profile, &off, 0, 0);
		if (error != kOk) return fail(SmResult::WriteFailed, error);
		changed = true;
		error = ReadSetting(api, session, profile, kSmEnableId, enabled);
		if (error != kOk || enabled.current.u32Value)
			return fail(SmResult::WriteFailed, error == kOk ? kInvalidArgument : error);
	}
	if (changed) {
		error = api.saveSettings(session);
		if (error != kOk) return fail(SmResult::WriteFailed, error);
	}
	return SmResult::Ok;
}

inline SmResult CleanupEnabledSmoothMotionForExe(const wchar_t* exePath,
	int* outError = nullptr) noexcept {
	if (outError) *outError = kOk;
	const wchar_t* ext = exePath ? wcsrchr(exePath, L'.') : nullptr;
	if (!ext || _wcsicmp(ext, L".exe") != 0) {
		if (outError) *outError = kInvalidArgument;
		return SmResult::InvalidTarget;
	}
	NvApi api;
	if (!LoadNvApi(api)) {
		// Missing NVAPI is normal on a computer without an NVIDIA driver.
		if (api.moduleMissing) return SmResult::Ok;
		if (outError) *outError = kUnavailable;
		return SmResult::NvApiUnavailable;
	}
	int error = InitializeNvApi(api);
	if (error == kNvidiaDeviceNotFound) return SmResult::Ok;
	if (error != kOk || !api.createSession || !api.destroySession || !api.loadSettings) {
		if (outError) *outError = error == kOk ? kUnavailable : error;
		return SmResult::NvApiUnavailable;
	}
	void* session = nullptr;
	error = api.createSession(&session);
	if (error != kOk || !session) {
		if (outError) *outError = error == kOk ? kUnavailable : error;
		return SmResult::ReadFailed;
	}
	SmResult result = SmResult::ReadFailed;
	error = api.loadSettings(session);
	if (error == kOk) result = CleanupEnabledInSession(api, session, exePath, outError);
	else if (outError) *outError = error;
	api.destroySession(session);
	return result;
}

// 开/关 Smooth Motion。返回 SmResult：Ok=成功（需重启游戏生效），否则细分失败原因。
// outError（可选）回填最后一次 NVAPI 调用的返回码，便于日志定位。
inline SmResult SetSmoothMotion(bool enable, int* outError = nullptr) noexcept {
	if (outError) *outError = 0;
	NvApi api;
	if (!LoadNvApi(api)) return SmResult::NvApiUnavailable;
	if (InitializeNvApi(api) != kOk) return SmResult::NvApiUnavailable;

	void* session = nullptr;
	if (!api.createSession || !api.destroySession || !api.loadSettings || !api.saveSettings ||
		api.createSession(&session) != kOk)
		return SmResult::NvApiUnavailable;
	if (api.loadSettings(session) != kOk) {
		api.destroySession(session);
		return SmResult::NvApiUnavailable;
	}

	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	// 找不到就自动创建（鬼武者等较新游戏 NVIDIA App 可能还没建 profile）。
	void* profile = FindOrCreateGameProfile(api, session, exePath);
	if (!profile) {
		api.destroySession(session);
		return SmResult::ProfileUnavailable;
	}

	bool ok = false;
	// 1) 开关本体
	DrsSetting s{};
	s.version = NVSM_MAKE_VERSION(DrsSetting, 1);
	s.settingId = kSmEnableId;
	s.settingType = kDrsDwordType;
	s.settingLocation = kDrsCurrentProfileLocation;
	s.current.u32Value = enable ? 1u : 0u;
	s.predefined.u32Value = enable ? 1u : 0u;
	int r = api.setSetting ? api.setSetting(session, profile, &s, 0, 0) : -1;
	if (outError) *outError = r;
	ok = (r == kOk);

	// Preserve the driver's API restrictions. Setting every API bit can enable
	// Smooth Motion in games/APIs that NVIDIA App intentionally excludes.

	if (ok && api.saveSettings) {
		r = api.saveSettings(session);
		if (outError) *outError = r;
		ok = (r == kOk);
	}
	api.destroySession(session);
	return ok ? SmResult::Ok : SmResult::WriteFailed;
}

}  // namespace NvSmoothMotion
}  // namespace DXL
