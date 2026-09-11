#pragma once
// NVIDIA Smooth Motion（驱动级 AI 插帧）开关 —— 通过 NVAPI 改 per-app profile。
//
// Smooth Motion 是 NVIDIA App 里的"AI 插帧"（驱动级帧生成），和游戏内 FG 不同：
// 它不改游戏的 swapchain / 不碰 NVGX API / 不碰游戏渲染管线，所以能和本 MOD 共存
// （实测鬼武者开 Smooth Motion 时 swapchain BufferCount 仍=3，不触发我们的 FG 检测）。
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
constexpr unsigned int kApiSysGetDriverAndBranchVersion = 0x2926AAAD;
constexpr unsigned int kApiEnumPhysicalGpus = 0xE5AC921F;
constexpr unsigned int kApiGpuGetFullName = 0xCEEE8E9F;
constexpr unsigned int kApiUnload = 0xD22BDD7E;

/* ---------------- Smooth Motion 设置 ID 与取值 ---------------- */
constexpr unsigned int kSmEnableId = 0xB0D384C0;      // 0=关 1=开
constexpr unsigned int kSmEnableApisId = 0xB0CC0875;  // bitfield，默认 7（DX11+DX12+Vulkan）

/* ---------------- NVAPI 常量 ---------------- */
constexpr unsigned int kDrsDwordType = 0;
constexpr unsigned int kDrsCurrentProfileLocation = 0;
constexpr int kOk = 0;  // NVAPI_OK

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
	SysGetDriverAndBranchVersionFn getDriverAndBranchVersion = nullptr;
	EnumPhysicalGpusFn enumPhysicalGpus = nullptr;
	GpuGetFullNameFn gpuGetFullName = nullptr;
	bool loaded = false;

	~NvApi() { if (loaded && unload) unload(); }
};

inline bool LoadNvApi(NvApi& api) noexcept {
	if (api.loaded) return true;
	HMODULE mod = LoadLibraryW(L"nvapi64.dll");
	if (!mod) return false;
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
	api.getDriverAndBranchVersion = reinterpret_cast<SysGetDriverAndBranchVersionFn>(
		query(kApiSysGetDriverAndBranchVersion));
	api.enumPhysicalGpus = reinterpret_cast<EnumPhysicalGpusFn>(
		query(kApiEnumPhysicalGpus));
	api.gpuGetFullName = reinterpret_cast<GpuGetFullNameFn>(
		query(kApiGpuGetFullName));
	api.loaded = true;
	return true;
}

// 找当前游戏 exe 的 per-app profile。先按完整路径、再按文件名回退。只读查找，
// 找不到返回 nullptr（不创建）。
inline void* FindGameProfile(NvApi& api, void* session,
	const wchar_t* exePath) noexcept {
	DrsApplicationV4 app{};
	app.version = NVSM_MAKE_VERSION(DrsApplicationV4, 4);
	void* profile = nullptr;

	// 完整路径
	if (api.findApplicationByName(session, exePath, &profile, &app) == kOk) {
		return profile;
	}
	// 文件名回退
	const wchar_t* leaf = wcsrchr(exePath, L'\\');
	leaf = leaf ? leaf + 1 : exePath;
	profile = nullptr;
	if (api.findApplicationByName(session, leaf, &profile, &app) == kOk) {
		return profile;
	}
	return nullptr;
}

// 查找当前游戏 exe 的 per-app profile，找不到就自动创建（建一个以游戏 exe 名命名的
// profile + 把 exe 挂进去）。返回 nullptr 表示既找不到也建不了。
inline void* FindOrCreateGameProfile(NvApi& api, void* session,
	const wchar_t* exePath) noexcept {
	void* profile = FindGameProfile(api, session, exePath);
	if (profile) return profile;
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
	if (!api.initialize || api.initialize() != kOk) return false;
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

// 读第一块物理 GPU 的完整名称（用于诊断显卡型号；Smooth Motion 官方仅 RTX 50 系
// 开放，40 系需特殊手段，30 系及以下驱动不提供该 setting）。成功返回 true。
inline bool ReadGpuName(char name[64]) noexcept {
	if (name) name[0] = 0;
	NvApi api;
	if (!LoadNvApi(api)) return false;
	if (!api.initialize || api.initialize() != kOk) return false;
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
inline int GetSmoothMotion(int* outError = nullptr) noexcept {
	if (outError) *outError = 0;
	NvApi api;
	if (!LoadNvApi(api)) return -1;
	if (!api.initialize || api.initialize() != kOk) return -1;

	void* session = nullptr;
	if (!api.createSession || api.createSession(&session) != kOk) return -1;
	if (api.loadSettings && api.loadSettings(session) != kOk) {
		api.destroySession(session);
		return -1;
	}

	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
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

// 开/关 Smooth Motion。返回 SmResult：Ok=成功（需重启游戏生效），否则细分失败原因。
// outError（可选）回填最后一次 NVAPI 调用的返回码，便于日志定位。
inline SmResult SetSmoothMotion(bool enable, int* outError = nullptr) noexcept {
	if (outError) *outError = 0;
	NvApi api;
	if (!LoadNvApi(api)) return SmResult::NvApiUnavailable;
	if (!api.initialize || api.initialize() != kOk) return SmResult::NvApiUnavailable;

	void* session = nullptr;
	if (!api.createSession || api.createSession(&session) != kOk)
		return SmResult::NvApiUnavailable;
	if (api.loadSettings && api.loadSettings(session) != kOk) {
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

	// 2) 允许的 API（开的时候顺手设成全开，避免某些游戏漏了 API 位导致不生效）
	if (ok && enable) {
		DrsSetting apis{};
		apis.version = NVSM_MAKE_VERSION(DrsSetting, 1);
		apis.settingId = kSmEnableApisId;
		apis.settingType = kDrsDwordType;
		apis.settingLocation = kDrsCurrentProfileLocation;
		apis.current.u32Value = 0x7u;   // DX11 | DX12 | Vulkan
		apis.predefined.u32Value = 0x7u;
		api.setSetting(session, profile, &apis, 0, 0);
	}

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
