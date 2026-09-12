#include "../common/DataPaths.h"
#include "InjectionCoordination.h"
// DXL UI 宿主。
//
// 职责刻意做到最小：开一个 Win32 窗口，塞一个 WebView2，把 web/ 目录映射成一个
// 虚拟主机，然后在 UI 和磁盘之间转发消息。
//
// 关键设计：**宿主不解析 JSON**。UI 把整个 settings 对象序列化后发过来，宿主原样
// 写进 settings.json；启动时原样读回去发给 UI。这样以后加设置项只改 web/app.js，
// C++ 一行都不用动。等 core DLL 就位后，再由 core 侧解析同一份 JSON。

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <cstring>
#include <cmath>
#include <regex>
#include "../common/NrParameterEdit.h"
#include <filesystem>
#include <mutex>
#include <algorithm>
#include <tlhelp32.h>
#include <thread>
#include <stdexcept>
#include "WebView2.h"
#include "GameSession.h"
#include "StartupReport.h"
#include "ExeIcon.h"
#include "GameScan.h"
#include "LauncherPreferences.h"
#include "WindowState.h"
#include "UiLanguage.h"
#include "Hotkey.h"
#include "ProfileCommandRouting.h"
#include "ProfileDeletion.h"
#include "../core/NvSmoothMotion.h"
#include "GameListCleanup.h"
#include "LibraryMetadata.h"
#include "../common/Log.h"
#include "../common/SemanticExtension.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t WINDOW_CLASS[] = L"DXLHost";
constexpr wchar_t WINDOW_TITLE[] = L"DXL — DLSS eXtended Loader";
constexpr wchar_t VIRTUAL_HOST[] = L"dxl.local";

// 状态轮询：2Hz 足够，UI 上的数字不需要更快，也别为此抢 CPU
constexpr UINT_PTR STATUS_TIMER_ID = 1;
constexpr UINT STATUS_TIMER_INTERVAL_MS = 500;

// 后台线程干完活之后用它通知 UI 线程。WebView2 是单线程 COM，绝对不能从别的线程
// 直接调 PostWebMessageAsJson。
constexpr UINT WM_APP_WORKER_DONE = WM_APP + 1;
// 后台扫描已安装游戏干完了。lParam = new std::string*（UI 线程负责删）。
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 2;
// 全局监控抓到一个列表里的游戏刚启动。wParam = pid。
constexpr UINT WM_APP_WATCH_HIT = WM_APP + 3;
// #36：后台扫 exe 判定 DX 版本干完了。lParam = new std::string*（UI 线程负责删）。
// 扫的是全文件（鬼武者 203MB，字符串在 150MB 处），UI 线程跑会卡住界面，
// 所以照 SCAN_DONE 的先例挪后台 —— icons 那条回复只回图标，DX 走这条补发。
constexpr UINT WM_APP_DX_DONE = WM_APP + 4;
constexpr UINT WM_APP_CLEANUP_DONE = WM_APP + 5;
constexpr UINT WM_APP_LIBRARY_DONE = WM_APP + 6;
constexpr UINT WM_APP_PROFILE_DELETE_DONE = WM_APP + 8;

// 全局快捷键的 id。注入/断开这一个是新加的核心交互 —— 玩家在游戏里按一下就行，
// 不用切出来选进程。
enum : int {
	HOTKEY_TOGGLE_INJECT = 1,
	HOTKEY_TOGGLE_ALL,
	HOTKEY_TOGGLE_DEBUG_VIEW,
};

HWND g_window = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
std::wstring g_elevationStartupNote;

DXL::StatusView g_status;
std::wstring g_attachedName;
std::wstring g_attachedPath;

// ---- 多目标注入表（一次可以看到不止一个被注入的进程） ----
//
// 以前只有一个 g_status / g_attachedName：监控看到第二个进程启动时
// `if (g_status.IsOpen()) return 0;` 直接把它丢了 —— 界面上永远只显示
// 第一个（经常是 blender 之类的常驻软件），游戏到底注入没注入根本看不出。
// 现在每个注入过的进程都进这张表；g_status/g_attachedName/g_attachedPath
// 仍是**当前查看目标**的数据（切目标 = 重新 Open 到那个 pid）。
//
// 只在 UI 线程碰（消息处理 + WM_TIMER），不需要锁。
struct TargetEntry {
	DWORD pid = 0;
	std::wstring name;      // exe 名（带后缀）
	std::wstring path;      // 完整路径
	bool launchedByTool = false;
};
std::vector<TargetEntry> g_targets;
DWORD g_currentPid = 0;     // 界面当前查看的目标；0 = 没有目标

TargetEntry* FindTarget(DWORD pid) {
	for (auto& t : g_targets) if (t.pid == pid) return &t;
	return nullptr;
}
TargetEntry* CurrentTarget() {
	return FindTarget(g_currentPid);
}
// 当前目标不在表里（刚注入还没记 / 目标退出被清）时退到表里第一个。
TargetEntry* EnsureCurrentTarget() {
	if (!CurrentTarget() && !g_targets.empty()) g_currentPid = g_targets.front().pid;
	return CurrentTarget();
}

// 注入成功后调用：把目标记进表（已在了就只更新字段），并设为当前查看的目标。
// g_attachedName/g_attachedPath/g_launchedByTool 在调用前已由各注入路径填好。
void SendStatusToUi();
void SendLog(std::wstring_view text);
void NotifyUiOfAttachedGame();
// 定义在下面（"从工具启动"的说明块那里），这里先用
extern bool g_launchedByTool;
void RegisterTarget(DWORD pid) {
	TargetEntry* existing = FindTarget(pid);
	if (existing) {
		existing->name = g_attachedName;
		existing->path = g_attachedPath;
		existing->launchedByTool = g_launchedByTool;
	} else {
		TargetEntry entry;
		entry.pid = pid;
		entry.name = g_attachedName;
		entry.path = g_attachedPath;
		entry.launchedByTool = g_launchedByTool;
		g_targets.push_back(std::move(entry));
	}
	g_currentPid = pid;
}

// 切换查看目标（界面下拉框）。g_status 重新 Open 到那个 pid；
// 打不开（core 没了/进程退了）不报错 —— WM_TIMER 的存活检查会把它清掉。
void SwitchTarget(DWORD pid) {
	TargetEntry* entry = FindTarget(pid);
	if (!entry) return;
	if (pid == g_currentPid && g_status.IsOpen() && g_status.Pid() == pid) {
		SendStatusToUi();   // 已经在看它，只刷新
		return;
	}
	g_currentPid = pid;
	g_attachedName = entry->name;
	g_attachedPath = entry->path;
	g_launchedByTool = entry->launchedByTool;
	g_status.Close();
	g_status.Open(pid);
	// 配置列表的"正在运行"高亮跟当前查看目标走
	NotifyUiOfAttachedGame();
	SendStatusToUi();
}
// UI 界面语言（0 中 / 2 英，同 core 的 overlayLang 编号）。UI 切语言时用
// setLang 消息报上来；宿主自己发的提示（注入成功那几条）按它选文案 ——
// 不然英文界面下这些提示还是中文，看起来就像"没提示"（用户实测）。
std::atomic<int> g_uiLang{ 0 };

// Each launch transfers its own result to the UI thread through PostMessage.
struct WorkerResult {
	std::wstring log;
	DWORD attachPid = 0;
	std::wstring attachName;
	std::wstring attachPath;
	std::wstring launchName;
};
// Pending launch workers can finish after the window closes. Keep their coordinator alive until process exit.
DXL::InjectionCoordination& g_injectionCoordination = *new DXL::InjectionCoordination;

// 总开关。**权威状态在 core**（它启动时按配置里的 masterEnabled 取初值，
// 默认关），这里只是"我们以为它是什么"。以前宿主自己记一个 bool 且初值是 true，
// 而 core 现在默认是关的 —— 两边一不一致，玩家按第一下 Del 就会反着来
// （UI 发 0，core 本来就是 0，画面毫无变化，看起来像快捷键坏了）。
bool ReadMasterEnabled();

void Attach(DWORD pid, bool earlyStartup = false);
void Detach();

// **从工具启动的这一局，不允许用热键取消注入。**
//
// 为什么要禁：从工具启动 = 我们赶在游戏创建 swapchain 之前装好了 hook，
// 这是拿到游戏原生深度/矢量的**唯一**方式。断开之后再注入就是迟到注入，
// 旁听链接不上，DLSSNR 只能吃零矢量，效果会静默变差 —— 而玩家看到的现象是
// "开关注入好像还能用"，于是拿它当效果开关使，最后得到一个坏掉的效果还不知道。
// 想开关效果请用总开关（Del），那条路不影响 hook。
bool g_launchedByTool = false;
void SendStatusToUi();
void SendLog(std::wstring_view text);

std::filesystem::path ExeDir() {
	wchar_t buffer[MAX_PATH]{};
	GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ConfigDir() {
	return DXL::DataPaths::ConfigRoot(nullptr);
}

std::filesystem::path SettingsPath() {
	return ConfigDir() / L"settings.json";
}

// 每游戏配置放这里。core 只认这个目录里的扁平文件 —— 见 SettingsReader::FindPath
// 里对"为什么不让 core 理解嵌套 profile"的说明。
std::filesystem::path ProfilesDir() {
	const std::filesystem::path dir = ConfigDir() / L"profiles";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	return dir;
}

// 文件名来自 UI，必须当作不可信输入处理：只允许"字母数字 . _ - 空格"，
// 否则可以用 ..\ 写到任意路径去。
bool IsSafeProfileFileName(std::string_view name) {
	if (name.empty() || name.size() > 128) return false;
	if (!name.ends_with(".json")) return false;
	for (const unsigned char ch : name) {
		const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' ||
			ch == ' ';
		if (!ok) return false;
	}
	// 连续的点会构成 ..，直接拒掉
	return name.find("..") == std::string_view::npos;
}

std::string ReadFileUtf8(const std::filesystem::path& path) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return {};
	LARGE_INTEGER size{};
	std::string result;
	if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (32 << 20)) {
		result.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(file, result.data(), (DWORD)result.size(), &read, nullptr)) {
			result.clear();
		} else {
			result.resize(read);
		}
	}
	CloseHandle(file);
	// 去掉 BOM
	if (result.size() >= 3 && (unsigned char)result[0] == 0xEF &&
		(unsigned char)result[1] == 0xBB && (unsigned char)result[2] == 0xBF) {
		result.erase(0, 3);
	}
	return result;
}

bool WriteFileUtf8(const std::filesystem::path& path, std::string_view text) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	const bool ok = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr) &&
		written == text.size();
	CloseHandle(file);
	return ok;
}

std::filesystem::path LauncherPreferencesPath() {
	return ConfigDir() / L"launcher.json";
}

bool IsElevated() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	TOKEN_ELEVATION info{};
	DWORD returned = 0;
	const bool elevated = GetTokenInformation(token, TokenElevation, &info,
		sizeof(info), &returned) && info.TokenIsElevated;
	CloseHandle(token);
	return elevated;
}

bool StartElevatedIfPreferred(std::wstring_view arguments) {
	bool attempted = false;
	int argc = 0;
	if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
		for (int i = 1; i < argc; ++i)
			if (_wcsicmp(argv[i], L"--dxl-elevation-attempted") == 0) attempted = true;
		LocalFree(argv);
	}
	const bool enabled = DXL::LauncherPreferences::ReadAdminLaunch(
		ReadFileUtf8(LauncherPreferencesPath()));
	if (!DXL::LauncherPreferences::ShouldElevate(enabled, IsElevated(), attempted))
		return false;
	wchar_t self[32768]{};
	if (!GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)))) return false;
	const std::wstring nextArguments = std::wstring(arguments) + L" --dxl-elevation-attempted";
	SHELLEXECUTEINFOW request{ sizeof(request) };
	request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
	request.lpVerb = L"runas";
	request.lpFile = self;
	request.lpParameters = nextArguments.c_str();
	request.nShow = SW_SHOWNORMAL;
	if (ShellExecuteExW(&request)) {
		if (request.hProcess) CloseHandle(request.hProcess);
		return true;
	}
	const DWORD error = GetLastError();
	g_elevationStartupNote = error == ERROR_CANCELLED
		? L"已取消管理员启动，DXL 本次继续以普通权限运行。可在侧栏取消默认管理员启动。"
		: L"管理员启动失败，DXL 本次继续以普通权限运行（错误 " +
			std::to_wstring(error) + L"）。";
	return false;
}

std::wstring Utf8ToWide(std::string_view text) {
	if (text.empty()) return {};
	const int needed = MultiByteToWideChar(
		CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
	std::wstring result((size_t)needed, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(),
		result.data(), needed);
	return result;
}

std::string WideToUtf8(std::wstring_view text) {
	if (text.empty()) return {};
	const int needed = WideCharToMultiByte(
		CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
	std::string result((size_t)needed, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
		result.data(), needed, nullptr, nullptr);
	return result;
}

void PostToUi(std::string_view json) {
	if (!g_webview) return;
	const HRESULT hr = g_webview->PostWebMessageAsJson(Utf8ToWide(json).c_str());
	if (FAILED(hr)) {
		// **PostWebMessageAsJson 对非法 JSON 返回 E_INVALIDARG 并静默丢弃消息。**
		// 这一条日志是"UI 上为什么一片空白"的唯一线索 —— 之前状态 JSON 被截断成非法
		// 的，整块运行状态和诊断全是"—"，而 UI 和宿主两边都没有任何报错。
		// 不能在这里再 PostToUi（会递归），只写文件。
		const std::string head(json.substr(0, 240));
		D5_LOG_ERROR(L"PostWebMessageAsJson 失败 0x%08X，消息已被丢弃"
			L"（长度 %zu）。开头：%hs", hr, json.size(), head.c_str());
	}
}

// 极小的取值器：只用来从 UI 发来的消息里抠出 "type" 和 "payload"，
// 不是通用 JSON 解析器。宿主不需要理解 payload 的内容。
std::string ExtractStringField(std::string_view json, std::string_view field) {
	const std::string needle = "\"" + std::string(field) + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string_view::npos) return {};
	pos = json.find(':', pos + needle.size());
	if (pos == std::string_view::npos) return {};
	pos = json.find('"', pos);
	if (pos == std::string_view::npos) return {};
	const size_t end = json.find('"', pos + 1);
	if (end == std::string_view::npos) return {};
	return std::string(json.substr(pos + 1, end - pos - 1));
}

// 同上，取数字字段。找不到 / 不是数字返回 defaultValue。
// noReload 这个标记就靠它（applyProfile 的单向同步，见那边的注释）。
double ExtractNumberField(std::string_view json, std::string_view field,
	double defaultValue = 0.0) {
	const std::string needle = "\"" + std::string(field) + "\":";
	size_t pos = json.find(needle);
	if (pos == std::string_view::npos) return defaultValue;
	pos += needle.size();
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
	const size_t end = json.find_first_of(",}", pos);
	if (end == std::string_view::npos) return defaultValue;
	try {
		return std::stod(std::string(json.substr(pos, end - pos)));
	} catch (...) {
		return defaultValue;
	}
}

// 取出 "payload": 之后那一整个值（对象/数组/字面量），做括号配平
std::string ExtractPayload(std::string_view json) {
	size_t pos = json.find("\"payload\"");
	if (pos == std::string_view::npos) return {};
	pos = json.find(':', pos + 9);
	if (pos == std::string_view::npos) return {};
	++pos;
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
	if (pos >= json.size()) return {};
	if (json[pos] != '{' && json[pos] != '[') {
		const size_t end = json.find_first_of(",}", pos);
		return std::string(json.substr(pos,
			(end == std::string_view::npos ? json.size() : end) - pos));
	}
	const char open = json[pos];
	const char close = open == '{' ? '}' : ']';
	int depth = 0;
	bool inString = false;
	bool escaped = false;
	for (size_t i = pos; i < json.size(); ++i) {
		const char c = json[i];
		if (escaped) { escaped = false; continue; }
		if (c == '\\') { escaped = true; continue; }
		if (c == '"') { inString = !inString; continue; }
		if (inString) continue;
		if (c == open) ++depth;
		else if (c == close && --depth == 0) {
			return std::string(json.substr(pos, i - pos + 1));
		}
	}
	return {};
}

void SendSettingsToUi() {
	std::string json = ReadFileUtf8(SettingsPath());
	if (json.empty()) json = "{}";
	PostToUi("{\"type\":\"settings\",\"payload\":" + json + "}");
}

void SendLauncherPreferencesToUi() {
	const bool enabled = DXL::LauncherPreferences::ReadAdminLaunch(
		ReadFileUtf8(LauncherPreferencesPath()));
	PostToUi(std::string("{\"type\":\"launcherPreferences\",\"payload\":{\"adminLaunch\":") +
		(enabled ? "true" : "false") + ",\"elevated\":" +
		(IsElevated() ? "true" : "false") + "}}");
}

// JSON 字符串转义。只处理必须处理的：进程名和窗口标题里什么都可能出现。
std::string JsonEscape(std::string_view text) {
	std::string out;
	out.reserve(text.size() + 8);
	for (const unsigned char ch : text) {
		switch (ch) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (ch < 0x20) {
				char buffer[8]{};
				_snprintf_s(buffer, _TRUNCATE, "\\u%04x", ch);
				out += buffer;
			} else {
				out += static_cast<char>(ch);
			}
		}
	}
	return out;
}

std::string JsonQuoted(std::wstring_view text) {
	return "\"" + JsonEscape(WideToUtf8(text)) + "\"";
}

void SendLog(std::wstring_view text) {
	// 同时写文件：UI 面板里的日志没法从外部读，出问题时排查不了
	D5_LOG_INFO(L"%.*s", (int)text.size(), text.data());
	PostToUi("{\"type\":\"log\",\"payload\":" + JsonQuoted(text) + "}");
}

std::filesystem::path CoreDllPath() {
	return ExeDir() / L"DXL-core.dll";
}

const char* FeatureStateName(uint32_t state) {
	using DXL::Ipc::FeatureState;
	switch (static_cast<FeatureState>(state)) {
	case FeatureState::Disabled:    return "disabled";
	case FeatureState::Standby:     return "standby";
	case FeatureState::Active:      return "active";
	case FeatureState::Failed:      return "failed";
	case FeatureState::Unavailable: break;
	}
	return "unavailable";
}

const char* ApiName(uint32_t api) {
	using DXL::Ipc::GraphicsApi;
	switch (static_cast<GraphicsApi>(api)) {
	case GraphicsApi::D3D9:   return "D3D9";
	case GraphicsApi::D3D11:  return "D3D11";
	case GraphicsApi::D3D12:  return "D3D12";
	case GraphicsApi::Vulkan: return "Vulkan";
	case GraphicsApi::OpenGL: return "OpenGL";
	case GraphicsApi::Unknown: break;
	}
	return "未识别";
}

/* ---------------- 状态 JSON ---------------- */
//
// **别再用固定大小的缓冲区拼这一段。**
//
// 上一版把十七组字段一次性 _snprintf_s 进 char[512]，而实际需要 833 字节。
// _TRUNCATE 于是把它剪断在 `"srSkippedFrames":0,"nrSk` 中间，后面又接上了
// `,"target":...}}` —— 拼出来是非法 JSON。PostWebMessageAsJson 拒收非法 JSON 并
// **静默丢弃整条消息**，结果 UI 上"运行状态"和"诊断"两大块全程都是"—"、左下角
// 一直写着"未注入"，而后台其实一切正常（core 在跑，旁听抄到了一万多帧）。
// 两边都不报错，纯靠数格式串的字节数才找出来。
//
// 这些小函数往 std::string 上追加，没有长度上限，这一类 bug 从此不会再出现。

void AddRaw(std::string& json, std::string_view key, std::string_view value) {
	json += ",\"";
	json += key;
	json += "\":";
	json += value;
}

void AddBool(std::string& json, std::string_view key, bool value) {
	AddRaw(json, key, value ? "true" : "false");
}

void AddNumber(std::string& json, std::string_view key, uint64_t value) {
	AddRaw(json, key, std::to_string(value));
}

// 浮点要限小数位：默认的 6 位会让 UI 上的数字抖得读不了
void AddFloat(std::string& json, std::string_view key, double value, int decimals) {
	char buffer[64]{};
	_snprintf_s(buffer, _TRUNCATE, "%.*f", decimals, value);
	AddRaw(json, key, buffer);
}

void AddText(std::string& json, std::string_view key, std::string_view value) {
	AddRaw(json, key, "\"" + JsonEscape(value) + "\"");
}

// 读 core 当前的总开关状态。读不到（没注入）就按"关"处理。
bool ReadMasterEnabled() {
	DXL::Ipc::Status status{};
	if (!g_status.IsOpen() || !g_status.Read(status)) return false;
	return status.masterEnabled != 0;
}

void SendStatusToUi() {
	// 运行时 DLL 是否就位：core 要靠 ngx\ 目录下这几个文件
	const std::filesystem::path ngxDir = ExeDir() / L"ngx";
	auto probe = [&](const wchar_t* name) {
		std::error_code ec;
		return std::filesystem::exists(ngxDir / name, ec);
	};

	// DLL 的文件版本（资源段里的 FileVersion）。**static 缓存**：这份 JSON
	// 每个状态定时器拍都发一次，而版本号在运行期不可能变 —— 每拍都去
	// GetFileVersionInfo 读一遍纯属浪费。
	static const std::string nrVersion = [] {
		const std::wstring path =
			(ExeDir() / L"ngx" / L"nvngx_dlssnr.dll").wstring();
		DWORD handle = 0;
		const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
		if (!size) return std::string("");   // 读不到（没有资源段/没这个文件）
		std::vector<BYTE> block(size);
		if (!GetFileVersionInfoW(path.c_str(), handle, size, block.data())) return std::string("");
		// 按资源段里声明的语言/代码页拼 FileVersion 的查询串
		void* translation = nullptr;
		UINT len = 0;
		if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
				&translation, &len) || len < 4)
			return std::string("");
		wchar_t query[64]{};
		_snwprintf_s(query, _TRUNCATE, L"\\StringFileInfo\\%04X%04X\\FileVersion",
			*(const uint16_t*)translation, *(const uint16_t*)((const BYTE*)translation + 2));
		const wchar_t* value = nullptr;
		if (VerQueryValueW(block.data(), query, (void**)&value, &len) && value && len)
			return WideToUtf8(value);
		return std::string("");
	}();

	std::string json = "{\"type\":\"status\",\"payload\":{";
	json += "\"dlssDll\":"; json += probe(L"nvngx_dlss.dll") ? "true" : "false";
	AddBool(json, "nrDll", probe(L"nvngx_dlssnr.dll"));
	AddBool(json, "fgDll", probe(L"nvngx_dlssg.dll"));
	// 只报 DLSSNR 的版本：现在只有这一项功能在用 NGX DLL（用户拍板），
	// 以后真加了用别的 DLL 的功能再把那几个的版本一起报。
	AddText(json, "nrDllVersion", nrVersion);

	DXL::Ipc::Status status{};
	if (g_status.IsOpen() && g_status.Read(status)) {
		AddBool(json, "attached", true);
		AddNumber(json, "pid", status.gamePid);
		AddText(json, "api", ApiName(status.api));
		AddBool(json, "hooked", status.hooked != 0);
		AddText(json, "srState", FeatureStateName(status.srState));
		AddText(json, "nrState", FeatureStateName(status.nrState));
		AddText(json, "fgState", FeatureStateName(status.fgState));
		AddBool(json, "nativeDepth", status.nativeDepth != 0);
		AddBool(json, "nativeMotion", status.nativeMotion != 0);
		// 这几个是判断"真超分/原生深度能不能生效"的依据。曾经漏在这里没发出去，
		// 结果 UI 把它们当 undefined，界面上永远显示"晚于 swapchain 创建"、
		// "深度候选 0" —— 明明后台是好的，界面在骗人。加字段时务必两边都加。
		AddBool(json, "injectedEarly", status.injectedEarly != 0);
		AddBool(json, "proxyActive", status.proxyActive != 0);
		// **这一条以前漏了。** core 早就在算了（"游戏无视我们谎报的尺寸"），
		// 但宿主没往 UI 发，于是界面上完全看不出真超分为什么画面是裁切的。
		AddBool(json, "proxySizeIgnored", status.proxySizeIgnored != 0);
		// 总开关的权威状态来自 core，见 IpcProtocol 里 masterEnabled 的说明
		AddBool(json, "masterEnabled", status.masterEnabled != 0);
		// 当前 DLSSNR 参数（v11）：浮层改的值带回 UI，两边才同步。
		AddFloat(json, "nrParamIntensity", status.nrParamIntensity, 3);
		AddFloat(json, "nrParamLocalTone", status.nrParamLocalTone, 3);
		AddFloat(json, "nrParamLocalStructure", status.nrParamLocalStructure, 3);
		AddFloat(json, "nrParamSkinStructure", status.nrParamSkinStructure, 3);
		AddNumber(json, "nrParamPreset", status.nrParamPreset);
		AddNumber(json, "nrParamStyle", status.nrParamStyle);
		AddBool(json, "nrParamAutoMask", status.nrParamAutoMask != 0);
		AddBool(json, "nrParamUiCorrection", status.nrParamUiCorrection != 0);
		AddFloat(json, "nrParamRenderScale", status.nrParamRenderScale, 3);
		AddFloat(json, "nrParamColourStrength", status.nrParamColourStrength, 3);
		AddFloat(json, "nrParamSelfLayers", status.nrParamSelfLayers, 3);
		AddNumber(json, "nrParamTrueLayers", status.nrParamTrueLayers);
		AddBool(json, "nrParamOpticalFlow", status.nrParamOpticalFlow != 0);
		AddNumber(json, "nrParamOpticalFlowQuality", status.nrParamOpticalFlowQuality);
		AddBool(json, "nrParamSemanticMask", status.nrParamSemanticMask != 0);
		AddNumber(json, "nrParamSemanticEnabled", status.nrParamSemanticEnabled);
		AddFloat(json, "nrParamSemanticBg", status.nrParamSemanticBg, 3);
		for (size_t group = 0; group < std::size(status.nrParamSemanticIntensity); ++group)
			AddFloat(json, "nrParamSemanticIntensity" + std::to_string(group), status.nrParamSemanticIntensity[group], 3);
		AddBool(json, "nrParamSemanticDebug", status.nrParamSemanticDebug != 0);
        AddBool(json, "nrParamSemanticFlipY", status.nrParamSemanticFlipY != 0);
        AddFloat(json, "nrParamSemanticFeather", status.nrParamSemanticFeather, 3);
		AddNumber(json, "nrRoute", status.nrRoute);
		AddNumber(json, "nrMotionSource", status.nrMotionSource);
		AddFloat(json, "nrOpticalFlowMs", status.nrOpticalFlowMs, 3);
		// 参数版本号：只在游戏内浮层改参数时变。UI 靠它忽略自己推送的回声
		// （见 IpcProtocol 里 nrParamVersion 的说明）。
		AddNumber(json, "nrParamVersion", status.nrParamVersion);
		AddNumber(json, "depthCandidates", status.depthCandidates);
		AddNumber(json, "renderWidth", status.renderWidth);
		AddNumber(json, "renderHeight", status.renderHeight);
		AddNumber(json, "outputWidth", status.outputWidth);
		AddNumber(json, "outputHeight", status.outputHeight);
		AddNumber(json, "presentCount", status.presentCount);
		AddNumber(json, "evaluateCount", status.evaluateCount);
		AddNumber(json, "evaluateFailures", status.evaluateFailures);
		AddNumber(json, "nrEvaluateCount", status.nrEvaluateCount);
		AddNumber(json, "nrEvaluateFailures", status.nrEvaluateFailures);
		AddFloat(json, "frameMs", status.frameMs, 2);
		AddFloat(json, "nrGpuMs", status.nrGpuMs, 2);
		AddFloat(json, "nrGpuMsWorst", status.nrGpuMsWorst, 2);
		AddBool(json, "gameDlssSeen", status.gameDlssSeen != 0);
		// 这一局是不是从工具启动的。**不是 core 报的，是 UI 自己记的** ——
		// core 无从知道谁启动了它。界面用它决定要不要禁掉"取消注入"。
		AddBool(json, "launchedByTool", g_launchedByTool);
		/* ---- 诊断与旁听 ---- */
		AddNumber(json, "stallCount", status.stallCount);
		AddNumber(json, "lastStallStage", status.lastStallStage);
		AddNumber(json, "srSkippedFrames", status.srSkippedFrames);
		AddNumber(json, "nrSkippedFrames", status.nrSkippedFrames);
		AddNumber(json, "eavesdropModules", status.eavesdropModules);
		AddNumber(json, "eavesdropLookups", status.eavesdropLookups);
		AddNumber(json, "eavesdropFrames", status.eavesdropFrames);
		AddNumber(json, "eavesdropCapturedFrames", status.eavesdropCapturedFrames);
		AddNumber(json, "eavesdropCapturedWidth", status.eavesdropCapturedWidth);
		AddNumber(json, "eavesdropCapturedHeight", status.eavesdropCapturedHeight);
		AddFloat(json, "eavesdropMvScaleX", status.eavesdropMvScaleX, 3);
		AddFloat(json, "eavesdropMvScaleY", status.eavesdropMvScaleY, 3);
		AddFloat(json, "eavesdropJitterX", status.eavesdropJitterX, 5);
		AddFloat(json, "eavesdropJitterY", status.eavesdropJitterY, 5);
		AddBool(json, "nrUsingRealMotion", status.nrUsingRealMotion != 0);
		AddNumber(json, "nrAtEvaluateFrames", status.nrAtEvaluateFrames);
		AddBool(json, "nrAtEvaluateWanted", status.nrAtEvaluateWanted != 0);
		AddNumber(json, "nrAtEvaluateBlocked", status.nrAtEvaluateBlocked);

		json += ",\"target\":" + JsonQuoted(g_attachedName);
		// message 是 core 写进共享内存的 char[256]。**不能直接当 C 字符串用** ——
		// 万一 core 把 256 字节填满没留 NUL，就会一路读到状态块外面去，
		// 拼出来的 JSON 也就跟着废了（正是上面那个空白 bug 的同一种死法）。
		AddText(json, "message",
			std::string_view(status.message,
				strnlen(status.message, sizeof(status.message))));
	} else if (g_status.IsOpen()) {
		// 映射还在但读不到：core 刚注入还没写第一次，或者游戏已经退出
		json += ",\"attached\":true,\"hooked\":false";
		json += ",\"target\":" + JsonQuoted(g_attachedName);
		json += ",\"message\":\"已加载，等待 core 发布状态\"";
	} else {
		json += ",\"attached\":false";
	}
	// 多目标表 + 当前查看的 pid：状态条的下拉框靠这两个字段。
	// 表里每个目标都带（pid/名字/是否从工具启动），UI 侧按 pid 切换。
	{
		json += ",\"currentPid\":";
		json += std::to_string(g_currentPid);
		json += ",\"targets\":[";
		bool firstTarget = true;
		for (const TargetEntry& t : g_targets) {
			if (!firstTarget) json += ",";
			firstTarget = false;
			json += "{\"pid\":";
			json += std::to_string(t.pid);
			json += ",\"name\":";
			json += JsonQuoted(t.name);
			json += ",\"launchedByTool\":";
			json += t.launchedByTool ? "true" : "false";
			json += "}";
		}
		json += "]";
	}
	json += "}}";
	PostToUi(json);
}

void SendTargetsToUi() {
	std::string json = "{\"type\":\"targets\",\"payload\":[";
	bool first = true;
	for (const DXL::TargetCandidate& candidate :
		DXL::EnumerateTargets()) {
		if (!first) json += ",";
		first = false;
		char head[96]{};
		_snprintf_s(head, _TRUNCATE,
			"{\"pid\":%lu,\"is64\":%s,\"ownDlss\":%s,\"exe\":",
			candidate.pid, candidate.is64Bit ? "true" : "false",
			DXL::LooksLikeGameHasOwnDlss(candidate.exePath)
				? "true" : "false");
		json += head;
		json += JsonQuoted(candidate.exeName);
		json += ",\"title\":" + JsonQuoted(candidate.windowTitle) + "}";
	}
	json += "]}";
	PostToUi(json);
}

// payload 里取一个整数字段。宿主只需要认 pid 这一个数字，不值得引入 JSON 库。
uint32_t ExtractUint(std::string_view json, std::string_view field) {
	const std::string needle = std::string("\"") + std::string(field) + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string_view::npos) return 0;
	pos = json.find(':', pos + needle.size());
	if (pos == std::string_view::npos) return 0;
	++pos;
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
	uint32_t value = 0;
	bool any = false;
	while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
		value = value * 10 + uint32_t(json[pos++] - '0');
		any = true;
	}
	return any ? value : 0;
}

void Attach(DWORD pid, bool earlyStartup) {
	if (!pid) return;
	// 手动注入进一个已经在跑的进程 = 迟到注入，肯定不是"从工具启动"的那一局。
	// 这里显式清掉，别依赖"只有 Detach 会清"——那种依赖迟早被新代码破坏。
	g_launchedByTool = false;

	// **谁在被接管，必须在这里记。**
	//
	// 以前只有"按 Alt+I 注入前台窗口"那条路记了名字，全局监控那条路没记 ——
	// 于是监控自动接上的那一局，UI 侧 `target` 是空的。后果不止是界面上不显示
	// 游戏名：配置列表的"正在运行"高亮不亮，而且 Del 存盘时找不到"正在跑的
	// 那份配置"，把总开关写到了当前选中的另一份配置上（实测就是这样）。
	g_attachedPath = DXL::ProcessImagePath(pid);
	g_attachedName = g_attachedPath.empty() ? std::wstring()
		: std::filesystem::path(g_attachedPath).filename().wstring();
	// **拿不到路径必须报出来。** 这条失败是静默的（NotifyUiOfAttachedGame 直接
	// return，status.target 也一直是空）—— 后果是 UI 的配置同步整场找不到
	// "正在运行的游戏"，参数全写进当前选中的那份配置（实测 88 条输入全进了
	// default）。常见原因：游戏以管理员运行而工具不是。
	if (g_attachedPath.empty()) {
		SendLog(L"⚠ 拿不到游戏进程的路径（pid " + std::to_wstring(pid) +
			L"）—— 配置同步会找不到目标，参数会写进当前选中的配置。" +
			L"常见原因：游戏以管理员身份运行而本工具不是。");
	}

	// 已经注入过的进程再点一次不要重复注入：共享内存能打开就说明 core 在里面
	if (g_status.Open(pid)) {
		// **不再无条件把总开关打开。**
		//
		// 以前这里发 SetEnabled=1，理由是"断开时发过 0，不恢复就静默失效"。
		// 现在总开关默认是关的、并且由玩家用 Del 决定，硬发 1 等于替玩家
		// 做了决定 —— 他明明关着，重新接一下就自己开了。
		// Detach 那边发 0 的问题改成在那里说明白（它会提示按 Del 再开）。
		SendLog(ReadMasterEnabled()
			? L"该进程已经注入过，直接接上（总开关：开）。"
			: L"该进程已经注入过，直接接上（总开关：关，按已设置的效果快捷键打开效果）。");
		RegisterTarget(pid);
		SendStatusToUi();
		return;
	}

    // A monitor can reach explicit Early mode before any game window exists.
    // Use the same no-probe marker as tool-owned startup; keep it alive until
    // initialization acknowledges it. The external game's threads keep running.
    std::unique_ptr<DXL::EarlyInjectMarker> marker;
    HANDLE ready = nullptr;
    if (earlyStartup) {
        marker = std::make_unique<DXL::EarlyInjectMarker>(pid);
        wchar_t readyName[128]{};
        _snwprintf_s(readyName, _TRUNCATE, L"%s.%lu", DXL::Ipc::READY_EVENT_BASE, pid);
        ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
        if (!marker->IsValid() || !ready) {
            if (ready) CloseHandle(ready);
            SendLog(g_uiLang.load() == 2 ? L"Early injection canceled: startup synchronization is unavailable."
                : L"尽早注入已取消：无法建立启动同步标记。");
            return;
        }
    }
	std::wstring error;
	if (!DXL::InjectCore(pid, CoreDllPath(), error)) {
        if (ready) CloseHandle(ready);
		SendLog(DXL::StartupReport(pid, L"load-unconfirmed"));
		SendLog(L"注入失败：" + error);
		SendStatusToUi();
		return;
	}

    if (ready) {
        const DWORD wait = WaitForSingleObject(ready, 10000);
        CloseHandle(ready);
        if (wait != WAIT_OBJECT_0)
            D5_LOG_WARN(L"External early injection: startup acknowledgement timed out for pid=%lu", pid);
    }
	// core 在自己的线程里初始化，共享内存不是立刻就有的
	for (int attempt = 0; attempt < 40 && !g_status.Open(pid); ++attempt) {
		Sleep(50);
	}
	SendLog(DXL::StartupReport(pid, g_status.IsOpen() ? L"status-connected" : L"status-timeout"));
	if (g_status.IsOpen()) {
		// 总开关的初值由 core 从配置里读（默认关），这里只是把结果说给用户 ——
		// 不说的话玩家注入成功却看不到任何变化，只会以为工具坏了。
		// **文案跟着 UI 语言走**（g_uiLang，UI 用 setLang 报上来）—— 英文界面下
		// 发中文等于没提示（用户实测"英文下没任何提示"）。
		const bool en = g_uiLang.load() == 2;
		SendLog(ReadMasterEnabled()
			? (en ? L"Injection OK, connected to pid " + std::to_wstring(pid)
				: L"注入成功，已连接到 pid " + std::to_wstring(pid))
			: (en ? L"Injection OK, connected to pid " + std::to_wstring(pid) +
				L". **Effects are OFF by default — switch to the game window "
				L"and press your configured effects shortcut to enable.** The choice is saved, next "
				L"launch follows it automatically."
				: L"注入成功，已连接到 pid " + std::to_wstring(pid) +
				L"。**效果默认是关的 —— 切到游戏窗口按已设置的效果快捷键打开**，"
				L"按过之后这个选择会存进配置，下次自动照做。"));
	} else {
		SendLog(g_uiLang.load() == 2
			? L"Core DLL found in the target, but initialization was not confirmed within 2 seconds. See [InjectDiag] and [StartupDiag] in the DXL log."
			: L"已确认目标中存在核心 DLL，但 2 秒内未确认初始化完成。请查看 DXL 日志中的 [InjectDiag] 和 [StartupDiag]。");
	}
	// 注入成功（或至少 DLL 进去了）就进多目标表 —— 状态块晚一点也能看
	RegisterTarget(pid);
	SendStatusToUi();
}

/* ---------------- 全局快捷键 ---------------- */

// 把 "Alt + Shift + D" 这种 UI 里显示的组合解析成 RegisterHotKey 要的形式。
// 这是唯一需要在宿主里解析的"格式"，而且它是我们自己在 app.js 里生成的，
// 不是通用输入。

bool RegisterOneHotkey(HWND window, int id, std::wstring_view combo,
	const wchar_t* label) {
	UnregisterHotKey(window, id);
	if (combo.empty()) return false;
	UINT modifiers = 0, virtualKey = 0;
	if (!DXL::ParseHotkey(combo, modifiers, virtualKey)) {
		SendLog(std::wstring(L"快捷键无法识别（支持单键或组合键）：") +
			std::wstring(label) + L" = " + std::wstring(combo));
		return false;
	}
	// MOD_NOREPEAT：按住不放不要连发
	if (!RegisterHotKey(window, id, modifiers | MOD_NOREPEAT, virtualKey)) {
		SendLog(std::wstring(L"快捷键注册失败（可能被别的程序占用）：") +
			std::wstring(label) + L" = " + std::wstring(combo));
        return false;
	} else {
		// 成功也记一条：不然"没报错"和"根本没调用"分不开
		D5_LOG_INFO(L"快捷键已注册: %s = %.*s", label,
			(int)combo.size(), combo.data());
        return true;
	}
}

struct CleanupRequest {
    std::vector<std::pair<std::string, std::wstring>> entries;
    HWND notify = nullptr;
};
DWORD WINAPI CleanupWorker(LPVOID parameter) {
    std::unique_ptr<CleanupRequest> request(static_cast<CleanupRequest*>(parameter));
    std::string response = "{\"type\":\"invalidGames\",\"payload\":{\"missing\":[";
    unsigned unknown = 0;
    bool first = true;
    for (const auto& [id, path] : request->entries) {
        const auto state = DXL::InspectGameExecutable(path);
        if (state == DXL::GamePathState::Unknown) { ++unknown; continue; }
        if (state != DXL::GamePathState::Missing) continue;
        if (!first) response += ",";
        first = false;
        response += "{\"id\":" + JsonQuoted(Utf8ToWide(id)) +
            ",\"exePath\":" + JsonQuoted(path) + "}";
    }
    response += "],\"unknown\":" + std::to_string(unknown) + "}}";
    auto* result = new std::string(std::move(response));
    if (!PostMessageW(request->notify, WM_APP_CLEANUP_DONE, 0,
        reinterpret_cast<LPARAM>(result))) delete result;
    return 0;
}

/* ---------------- 从工具启动游戏 ---------------- */

// Early launches hold the primary thread until hooks are ready. Late mode
// waits for a stable game window, including platform-created replacement PIDs.
struct LaunchRequest {
	std::wstring exePath;
	std::wstring args;
	std::wstring exeName;
	std::filesystem::path coreDll;
	// Explicit late-load compatibility mode; default startup captures NGX early.
	bool lateInject = false;
	HWND notify = nullptr;
};

DWORD WINAPI LaunchWorker(LPVOID parameter) {
	std::unique_ptr<LaunchRequest> request(static_cast<LaunchRequest*>(parameter));

	// 真正的逻辑在 GameSession.h 的 LaunchAndInject 里 —— 那样命令行注入器能跑
	// 同一份代码，"跳过启动壳"这件事才验证得了（只能用真实进程验证）。
	const DXL::LaunchOutcome outcome = DXL::LaunchAndInject(
		request->exePath, request->args, request->coreDll, request->lateInject);

	WorkerResult result;
	result.launchName = request->exeName;
	// note 在前：它是"这一局旁听能不能接上"的前提，比"注入到了哪个 pid"更该先看见。
	result.log = outcome.note.empty()
		? outcome.message
		: outcome.note + L"；" + outcome.message;
	result.attachPid = outcome.pid;
	if (outcome.pid) {
		result.attachName = request->exeName;
		result.attachPath = outcome.exePath;
	}
    auto posted = std::make_unique<WorkerResult>(std::move(result));
    if (PostMessageW(request->notify, WM_APP_WORKER_DONE, 0, reinterpret_cast<LPARAM>(posted.get()))) posted.release();
    else g_injectionCoordination.EndLaunch(request->exeName);
	return 0;
}

// 注入成功后告诉 UI 是哪个游戏，UI 据此自动把它加进配置文件列表。
void NotifyUiOfAttachedGame() {
	if (g_attachedName.empty()) return;
	PostToUi("{\"type\":\"attached\",\"payload\":{\"exe\":" +
		JsonQuoted(g_attachedName) + ",\"path\":" +
		JsonQuoted(g_attachedPath) + "}}");
}

// 快捷键注入：目标就是当前前台窗口所属的进程。玩家在游戏里按一下即可，
// 不需要切出来在列表里找。
void AttachForegroundWindow() {
	const HWND foreground = GetForegroundWindow();
	if (!foreground) {
		SendLog(L"拿不到前台窗口");
		return;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(foreground, &pid);
	if (!pid || pid == GetCurrentProcessId()) {
		SendLog(L"前台窗口是本程序自己 —— 请切到游戏窗口再按快捷键。");
		return;
	}

	// 名字/路径由 Attach 统一记（监控那条路也要用），这里只拿路径判一件事
	const std::wstring imagePath = DXL::ProcessImagePath(pid);
	if (DXL::LooksLikeGameHasOwnDlss(imagePath)) {
		SendLog(L"检测到游戏自带 DLSS / Streamline。DXL 会优先捕获原生 Evaluate；若延迟加载未捕获，建议下次通过部署代理尽早加载。");
	}
	Attach(pid);
	NotifyUiOfAttachedGame();
}

void Detach() {
	if (g_status.IsOpen()) {
		// 尽力而为：core 目前不支持真正卸载 hook，这里只是断开 UI 的连接
		DXL::SendCommand(
			g_status.Pid(), DXL::Ipc::CommandId::SetEnabled, 0);
		SendLog(L"已停用并断开 pid " + std::to_wstring(g_status.Pid()) +
			L"（hook 会留在进程里直到它退出；重新接上后按已设置的效果快捷键再打开效果）");
	}
	g_status.Close();
	g_attachedName.clear();
	g_launchedByTool = false;
	// 断开当前查看目标，但**不把它从多目标表里删**——core 还在它进程里跑，
	// 状态块随时能重新 Open。列表上它还在，只是状态显示"未连接"。
	SendStatusToUi();
}

/* ---------------- 扫描已安装的游戏 / 全局监控 ---------------- */

struct LibraryRequest {
    HWND notify = nullptr;
    std::filesystem::path cache;
    std::vector<std::pair<std::string, std::wstring>> items;
    std::wstring pickedCover;
};

DWORD WINAPI LibraryWorker(LPVOID parameter) {
    std::unique_ptr<LibraryRequest> request(static_cast<LibraryRequest*>(parameter));
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string json;
    try {
        namespace metadata = DXL::LibraryMetadata;
        if (!request->pickedCover.empty()) {
            const auto& id = request->items.front().first;
            const auto cover = metadata::CacheImage(request->pickedCover, request->cache, Utf8ToWide(id));
            json = cover.empty()
                ? "{\"type\":\"log\",\"payload\":" + JsonQuoted(L"无法读取或保存封面，请使用有效的 PNG/JPEG/WebP 图片（最大 32 MiB）。") + "}"
                : "{\"type\":\"gameCover\",\"payload\":{\"id\":" + JsonQuoted(Utf8ToWide(id)) +
                    ",\"exePath\":" + JsonQuoted(request->items.front().second) +
                    ",\"coverUrl\":" + JsonQuoted(cover) + "}}";
        } else {
            const auto libraries = DXL::SteamLibraries();
            const auto installs = metadata::Collect(libraries);
            json = "{\"type\":\"libraryMetadata\",\"payload\":{";
            bool first = true;
            for (const auto& [id, exe] : request->items) {
                const auto* install = metadata::Match(exe, installs);
                if (!install) continue;
                if (!first) json += ',';
                first = false;
                json += JsonQuoted(Utf8ToWide(id)) + ":{\"exePath\":" + JsonQuoted(exe) +
                    ",\"source\":" + JsonQuoted(install->source);
                if (!install->steamAppId.empty()) {
                    json += ",\"steamAppId\":" + JsonQuoted(install->steamAppId);
                    const auto local = metadata::FindSteamCover(libraries, install->steamAppId);
                    const auto cover = local.empty() ? std::wstring() :
                        metadata::CacheImage(local, request->cache, L"Steam:" + install->steamAppId);
                    if (!cover.empty()) json += ",\"coverUrl\":" + JsonQuoted(cover);
                }
                json += '}';
            }
            json += "}}";
        }
    } catch (...) {
        json = "{\"type\":\"libraryMetadata\",\"payload\":{}}";
    }
    if (SUCCEEDED(com)) CoUninitialize();
    auto result = std::make_unique<std::string>(std::move(json));
    if (PostMessageW(request->notify, WM_APP_LIBRARY_DONE, 0,
            reinterpret_cast<LPARAM>(result.get()))) result.release();
    return 0;
}

void StartLibraryWorker(std::unique_ptr<LibraryRequest> request) {
    request->notify = g_window;
    request->cache = ExeDir() / L"web/covercache";
    HANDLE worker = CreateThread(nullptr, 0, LibraryWorker, request.get(), 0, nullptr);
    if (worker) { request.release(); CloseHandle(worker); }
    else SendLog(L"无法启动封面读取线程，请稍后重试。");
}

struct ProfileDeleteRequest {
    HWND notify = nullptr;
    std::string profileId;
    std::wstring exePath;
    double sequence = 0;
};
DXL::ProfileDeletion::SavedRequest g_profileDeletionSave;
HANDLE g_profileDeletionThread = nullptr;
std::mutex g_profileDeletionMutex;
std::unique_ptr<std::string> g_profileDeletionReply;

std::string ProfileDeletionReply(const ProfileDeleteRequest& request, bool ok,
    const char* reason, int error = 0) {
    return "{\"type\":\"profileDeletionReady\",\"payload\":{\"profileId\":" +
        JsonQuoted(Utf8ToWide(request.profileId)) + ",\"requestId\":" + std::to_string(request.sequence) +
        ",\"ok\":" + (ok ? "true" : "false") + ",\"reason\":\"" + reason +
        "\",\"error\":" + std::to_string(error) + "}}";
}

void SendProfileDeletionReply(std::string_view reply) {
    const std::wstring profile = Utf8ToWide(DXL::LaunchArguments::ReadField(reply, "profileId"));
    const std::string reason = DXL::LaunchArguments::ReadField(reply, "reason");
    const int error = static_cast<int>(ExtractNumberField(reply, "error", 0));
    if (reason == "ok") {
        SendLog(g_uiLang.load() == 2
            ? L"Smooth Motion check before profile deletion completed: " + profile
            : L"删除配置前的 Smooth Motion 检查已完成：" + profile);
    } else {
        SendLog((g_uiLang.load() == 2
            ? L"Profile kept: Smooth Motion cleanup failed for "
            : L"配置已保留：Smooth Motion 清理失败，配置 ") + profile +
            L"; reason=" + Utf8ToWide(reason) + L"; error=" + std::to_wstring(error));
    }
    PostToUi(reply);
}

DWORD WINAPI ProfileDeleteWorker(LPVOID parameter) {
    std::unique_ptr<ProfileDeleteRequest> request(static_cast<ProfileDeleteRequest*>(parameter));
    bool ok = false;
    int error = 0;
    const char* reason = "driverError";
    try {
        const auto result = DXL::NvSmoothMotion::CleanupEnabledSmoothMotionForExe(request->exePath.c_str(), &error);
        ok = result == DXL::NvSmoothMotion::SmResult::Ok;
        switch (result) {
        case DXL::NvSmoothMotion::SmResult::Ok: reason = "ok"; break;
        case DXL::NvSmoothMotion::SmResult::NvApiUnavailable: reason = "nvapiUnavailable"; break;
        case DXL::NvSmoothMotion::SmResult::ProfileUnavailable: reason = "profileUnavailable"; break;
        case DXL::NvSmoothMotion::SmResult::InvalidTarget: reason = "invalidTarget"; break;
        default: break;
        }
    } catch (...) { reason = "driverError"; }
    {
        std::lock_guard lock(g_profileDeletionMutex);
        g_profileDeletionReply = std::make_unique<std::string>(ProfileDeletionReply(*request, ok, reason, error));
    }
    PostMessageW(request->notify, WM_APP_PROFILE_DELETE_DONE, 0, 0);
    return 0;
}

void StartProfileDeletion(std::string_view json) {
    auto request = std::make_unique<ProfileDeleteRequest>();
    request->notify = g_window;
    request->profileId = DXL::LaunchArguments::ReadField(json, "profileId");
    request->sequence = ExtractNumberField(json, "requestId", 0);
    std::string exePath;
    if (!std::isfinite(request->sequence) || std::floor(request->sequence) != request->sequence ||
        request->sequence > 9007199254740991.0) request->sequence = 0;
    if (!g_profileDeletionSave.Take(request->sequence, request->profileId, exePath)) {
        SendProfileDeletionReply(ProfileDeletionReply(*request, false, "settingsNotSaved"));
        return;
    }
    if (g_profileDeletionThread) {
        SendProfileDeletionReply(ProfileDeletionReply(*request, false, "busy"));
        return;
    }
    // Empty legacy profiles have no application driver setting to clean up.
    if (exePath.empty()) {
        SendProfileDeletionReply(ProfileDeletionReply(*request, true, "ok"));
        return;
    }
    request->exePath = Utf8ToWide(exePath);
    // Never accept an arbitrary path from the operation message. The driver
    // helper additionally validates this path before finding an application.
    const std::filesystem::path target(request->exePath);
    if (!target.is_absolute() || _wcsicmp(target.extension().c_str(), L".exe") != 0) {
        SendProfileDeletionReply(ProfileDeletionReply(*request, false, "invalidTarget"));
        return;
    }
    g_profileDeletionThread = CreateThread(nullptr, 0, ProfileDeleteWorker, request.get(), 0, nullptr);
    if (g_profileDeletionThread) request.release();
    else SendProfileDeletionReply(ProfileDeletionReply(*request, false, "workerUnavailable", static_cast<int>(GetLastError())));
}

// 扫描要遍历每个游戏目录找 exe，慢到必须放后台。结果用 PostMessage 交回 UI 线程 ——
// WebView2 是单线程 COM，别的线程碰它就是未定义行为。
DWORD WINAPI ScanWorker(LPVOID) {
	const std::vector<DXL::DetectedGame> games =
		DXL::ScanInstalledGames();
	std::string json = "{\"type\":\"scannedGames\",\"payload\":{\"games\":[";
	bool first = true;
	for (const DXL::DetectedGame& game : games) {
		if (!first) json += ",";
		first = false;
		json += "{\"name\":" + JsonQuoted(game.name);
		json += ",\"exePath\":" + JsonQuoted(game.exePath);
		json += ",\"exe\":" +
			JsonQuoted(std::filesystem::path(game.exePath).filename().wstring());
		json += ",\"source\":" + JsonQuoted(game.source);
		if (!game.steamAppId.empty()) json += ",\"steamAppId\":" + JsonQuoted(game.steamAppId);
		json += "}";
	}
	json += "]}}";
	PostMessageW(g_window, WM_APP_SCAN_DONE, 0,
		(LPARAM)new std::string(std::move(json)));
	return 0;
}

// External Automatic/Compatibility waits for a settled window. Explicit Early
// samples loader stability without blocking other games. A tool-owned launch
// reserves its process names so this watcher cannot bypass its startup policy.
std::mutex g_watchMutex;
std::vector<std::wstring> g_watchNames;   // Lowercase executable names.
std::vector<std::wstring> g_watchLateNames;
std::atomic<bool> g_watchEnabled{false};
HANDLE g_watchThread = nullptr;

DWORD WINAPI WatchWorker(LPVOID) {
    while (g_watchEnabled.load()) {
        std::vector<std::wstring> names, lateNames;
        { std::lock_guard guard(g_watchMutex); names = g_watchNames; lateNames = g_watchLateNames; }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            std::vector<uint32_t> alive;
            PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry)) do {
                alive.push_back(entry.th32ProcessID);
                std::wstring leaf = entry.szExeFile;
                std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
                if (std::find(names.begin(), names.end(), leaf) == names.end()) continue;
                if (!g_injectionCoordination.NeedsWatch(entry.th32ProcessID, leaf)) continue;
                const bool late = std::find(lateNames.begin(), lateNames.end(), leaf) != lateNames.end();
                if (!late && !g_injectionCoordination.ObserveLoader(entry.th32ProcessID,
                    DXL::ModuleCount(entry.th32ProcessID), GetTickCount64())) continue;
                if (g_injectionCoordination.QueueWatch(entry.th32ProcessID, leaf, late,
                    late && DXL::HasVisibleWindow(entry.th32ProcessID), GetTickCount64())) {
                    if (!PostMessageW(g_window, WM_APP_WATCH_HIT, entry.th32ProcessID, 0))
                        g_injectionCoordination.CancelWatch(entry.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
            g_injectionCoordination.Prune(alive);
        }
        for (int i = 0; i < 4 && g_watchEnabled.load(); ++i) Sleep(50);
    }
    return 0;
}

void SetWatchEnabled(bool enabled) {
	if (enabled == (g_watchEnabled != false)) return;
	g_watchEnabled = enabled;
	if (enabled) {
		g_watchThread = CreateThread(nullptr, 0, WatchWorker, nullptr, 0, nullptr);
        SendLog(g_uiLang.load() == 2
            ? L"Monitoring enabled: Automatic mode waits for a stable game window when launched outside DXL. Launch from DXL to prioritize native DLSS capture."
            : L"全局监控已开：外部启动时，自动模式等待游戏窗口稳定后加载。需要优先捕获原生 DLSS 时，请从工具启动。");
	} else {
		if (g_watchThread) {
			// Join before enabling again; otherwise two scanners can survive a quick toggle.
			WaitForSingleObject(g_watchThread, INFINITE);
			CloseHandle(g_watchThread);
			g_watchThread = nullptr;
		}
		SendLog(L"全局监控已关。");
	}
}



// NVIDIA App 把 NVIDIA Sans 以 woff2 放在自己的安装目录里，文件名带内容哈希
// （每个版本都不一样），所以只能按前缀找。**这个字体是 NVIDIA 专有的，
// 绝不能塞进我们的安装包**；这里做的是"用户机器上已经有的那份，拷一份到
// 我们的 web/fonts/ 下让 WebView2 能同源加载"。没装 NVIDIA App 就直接返回，
// style.css 里的 @font-face 会自然失败并回退到 Segoe UI。
void BorrowNvidiaSans() {
	const std::filesystem::path dest = ExeDir() / L"web" / L"fonts";
	struct Want { const wchar_t* prefix; const wchar_t* leaf; };
	static constexpr Want WANTED[] = {
		{ L"NVIDIASans_W_Rg", L"nvidia-sans-rg.woff2" },
		{ L"NVIDIASans_W_Md", L"nvidia-sans-md.woff2" },
		{ L"NVIDIASans_W_Bd", L"nvidia-sans-bd.woff2" },
	};

	std::error_code ec;
	// 三个都已经在了就不用再找 —— 每次启动都去扫一遍安装目录没有意义。
	bool allPresent = true;
	for (const Want& w : WANTED) {
		if (!std::filesystem::exists(dest / w.leaf, ec)) { allPresent = false; break; }
	}
	if (allPresent) return;

	// 可能的安装位置。ProgramFiles 用环境变量拿，别写死盘符。
	std::vector<std::filesystem::path> roots;
	for (const wchar_t* var : { L"ProgramFiles", L"ProgramFiles(x86)" }) {
		wchar_t buffer[MAX_PATH]{};
		if (GetEnvironmentVariableW(var, buffer, MAX_PATH)) {
			const std::filesystem::path base =
				std::filesystem::path(buffer) / L"NVIDIA Corporation" / L"NVIDIA app";
			roots.push_back(base / L"www" / L"assets" / L"hashed");
			roots.push_back(base / L"osc");
		}
	}

	bool copiedAny = false;
	for (const std::filesystem::path& root : roots) {
		if (!std::filesystem::is_directory(root, ec)) continue;
		for (const Want& w : WANTED) {
			if (std::filesystem::exists(dest / w.leaf, ec)) continue;
			// 前缀匹配 + .woff2 后缀。目录里同名还有 .woff / .ttf，只要 woff2。
			const std::wstring pattern =
				(root / (std::wstring(w.prefix) + L"*.woff2")).wstring();
			WIN32_FIND_DATAW found{};
			HANDLE handle = FindFirstFileW(pattern.c_str(), &found);
			if (handle == INVALID_HANDLE_VALUE) continue;
			FindClose(handle);
			std::filesystem::create_directories(dest, ec);
			if (CopyFileW((root / found.cFileName).c_str(),
					(dest / w.leaf).c_str(), FALSE)) {
				copiedAny = true;
			}
		}
	}
	if (copiedAny) {
		OutputDebugStringW(L"[DXL] 从 NVIDIA App 取到了 NVIDIA Sans\n");
	}
}

// #36：判定游戏的 DX 版本，给配置列表显示用。
//
// 扫 exe 文件字节里的 "d3d12.dll" / "d3d11.dll"（ASCII + UTF-16、不分大小写）。
// **只看导入表是不够的**：图形 DLL 几乎全是游戏自己 LoadLibrary + GetProcAddress
// 动态解析的（实测见 IatPatch.h 顶部的说明），静态导入表里查不到 —— 但那个
// 字符串仍然躺在文件里。找不到就不显示，宁可少一行也不猜错。
//
// **必须读全文件、不能截断**：鬼武者 demo 实测 203MB 的 exe，字符串在 150MB
// 偏移处（打包大资源的引擎会把 .rdata 挤到很后面），128MB 截断一个都扫不到。
// 相应地这东西**只能在后台线程跑**（requestIcons 那条路就是这么调它的）。
//
// 注意这是个启发式：两个都引用的引擎（带 D3D11 回退的不少）显示 DX11/12。
static std::string DetectDxVersion(const std::wstring& exePath) {
	if (exePath.empty()) return {};
	HANDLE file = CreateFileW(exePath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return {};
	LARGE_INTEGER size{};
	std::vector<BYTE> bytes;
	// 上限的比较必须用 64 位字面量：`2 << 30` 是 **int 运算**，2^31 溢出成负数，
	// `size < 负数` 永远为假 → **每个文件都走不到读那一步**，全部返回空
	// （实测症状：[DX] 后台判定回来 0 项、23ms 就返回——GB 级文件根本没被读）。
	if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
		size.QuadPart < (2ll << 30)) {   // 超 2GB 的当不是游戏 exe，放弃
		bytes.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr)) read = 0;
		bytes.resize(read);
	}
	CloseHandle(file);
	if (bytes.empty()) return {};

	// 不分大小写地找 ASCII 模式。stride 1 = ASCII 字符串，2 = UTF-16LE。
	const auto contains = [&](const char* pattern, size_t stride) {
		const size_t len = strlen(pattern);
		if (!len || bytes.size() < len * stride) return false;
		for (size_t i = 0; i + (len - 1) * stride < bytes.size(); ++i) {
			bool ok = true;
			for (size_t j = 0; j < len; ++j) {
				if (tolower(bytes[i + j * stride]) != tolower((BYTE)pattern[j])) {
					ok = false;
					break;
				}
			}
			if (ok) return true;
		}
		return false;
	};
	const bool d3d12 = contains("d3d12.dll", 1) || contains("d3d12.dll", 2);
	const bool d3d11 = contains("d3d11.dll", 1) || contains("d3d11.dll", 2);
	std::string dx;
	if (d3d12 && d3d11) dx = "DX11/12";
	else if (d3d12) dx = "DX12";
	else if (d3d11) dx = "DX11";

	// 64/32 位：PE 头的 Machine 字段。只需要文件头，不用扫全文件。
	std::string arch;
	if (bytes.size() >= sizeof(IMAGE_DOS_HEADER)) {
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
		if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
			dos->e_lfanew > 0 && size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= bytes.size()) {
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
			if (nt->Signature == IMAGE_NT_SIGNATURE) {
				if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) arch = "64 位";
				else if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) arch = "32 位";
				else if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64) arch = "ARM64";
			}
		}
	}
	return dx + "\t" + arch;   // 两个值一起回去，调用方拆
}

// #36：DX 版本判定的后台线程。图标那条消息先回（UI 线程），这里只补 DX ——
// 全文件扫描一个 200MB 的 exe 要几百毫秒，放 UI 线程会卡住整个界面。
struct DxScanRequest {
	std::vector<std::pair<std::string, std::wstring>> items;   // id -> exe 路径
};

DWORD WINAPI DxScanWorker(LPVOID param) {
	std::unique_ptr<DxScanRequest> request(
		reinterpret_cast<DxScanRequest*>(param));
	// 每项 { dx: "...", arch: "..." }：DetectDxVersion 返回 "dx\tarch"，
	// 两个值都可能缺（识别不出就没有那个键）。
	std::string json = "{\"type\":\"dxVersions\",\"payload\":{";
	bool first = true;
	for (const auto& [id, exe] : request->items) {
		const std::string both = DetectDxVersion(exe);
		if (both.empty()) continue;
		const size_t tab = both.find('\t');
		const std::string dx = tab == std::string::npos ? both : both.substr(0, tab);
		const std::string arch = tab == std::string::npos ? std::string()
			: both.substr(tab + 1);
		if (dx.empty() && arch.empty()) continue;
		std::string item = "{";
		if (!dx.empty()) item += "\"dx\":" + JsonQuoted(Utf8ToWide(dx));
		if (!arch.empty()) item += std::string(item.size() > 1 ? "," : "") +
			"\"arch\":" + JsonQuoted(Utf8ToWide(arch));
		item += "}";
		if (!first) json += ",";
		first = false;
		json += JsonQuoted(Utf8ToWide(id)) + ":" + item;
	}
	json += "}}";
	PostMessageW(g_window, WM_APP_DX_DONE, 0, (LPARAM)new std::string(std::move(json)));
	return 0;
}

// Download metadata is independent from game profiles and stays editable for releases.
std::wstring SemanticDownloadUrl() {
    const auto path = ExeDir() / L"config" / L"extensions.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::filesystem::create_directories(path.parent_path(), ec);
        WriteFileUtf8(path, "{\n  \"semanticMaskDownloadUrl\": \"\"\n}\n");
    }
    const auto url = Utf8ToWide(ExtractStringField(ReadFileUtf8(path), "semanticMaskDownloadUrl"));
    return url.starts_with(L"https://") || url.starts_with(L"http://") ? url : L"";
}
void SendExtensionsToUi() {
    const bool installed = DXL::SemanticExtensionInstalled(ExeDir());
    PostToUi("{\"type\":\"extensions\",\"payload\":{\"installed\":" +
        std::string(installed ? "true" : "false") + ",\"downloadUrl\":" + JsonQuoted(SemanticDownloadUrl()) +
        ",\"folder\":" + JsonQuoted(DXL::SemanticExtensionFolder(ExeDir()).wstring()) + "}}");
}

#include "UpdateClient.h"

void HandleUiMessage(std::string_view json) {
	const std::string type = ExtractStringField(json, "type");
	if (type == "prepareProfileDeletion") {
        StartProfileDeletion(json);
    } else if (type == "checkUpdates") {
        if (!g_updateChecked) { g_updateChecked=true; StartUpdateAction("check"); }
    } else if (type == "downloadUpdate") { StartUpdateAction("download", ExtractStringField(json,"version"));
    } else if (type == "installUpdate") { StartUpdateAction("install", ExtractStringField(json,"version"));
    } else if (type == "uiReady") {
		SendSettingsToUi();
		SendLauncherPreferencesToUi();
        SendExtensionsToUi();
		if (!g_elevationStartupNote.empty()) {
			SendLog(g_elevationStartupNote);
			g_elevationStartupNote.clear();
		}
		SendStatusToUi();
	} else if (type == "editNrParameter") {
        const auto key = ExtractStringField(json, "key");
        const auto file = ExtractStringField(json, "file");
        const auto requestId = ExtractNumberField(json, "requestId", 0);
        const double value = ExtractNumberField(json, "value", -1);
        uint32_t packed = 0;
        bool ok = IsSafeProfileFileName(file) && file != "default.json" &&
            DXL::EncodeNrEdit(key, value, packed);
        bool live = false;
        if (ok) {
            const auto expected = Utf8ToWide(file.substr(0, file.size()-5));
            for (const auto& target : g_targets) {
                if (_wcsicmp(target.name.c_str(), expected.c_str()) != 0) continue;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
                DWORD code = 0;
                const bool alive = process && GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
                if (process) CloseHandle(process);
                if (!alive) continue;
                live = true;
                // The core serializes this edit with in-game edits and owns the
                // durable params layer. Never rewrite that layer underneath it.
                ok = DXL::SendCommand(target.pid, DXL::Ipc::CommandId::EditNrParameter, packed) && ok;
            }
            if (!live) {
                const auto path = ProfilesDir() / (expected + L".params.json");
                auto text = ReadFileUtf8(path);
                if (text.empty() && !std::filesystem::exists(path)) text = "{}";
                const auto open = text.find('{'), close = text.find_last_of('}');
                if (open == std::string::npos || close == std::string::npos) ok = false;
                else {
                    const std::regex item("\\\"" + key + "\\\"\\s*:\\s*[^,}\\r\\n]+");
                    const auto literal = DXL::NrEditSpecs[packed >> 24].boolean ? std::string(value != 0 ? "true" : "false") : std::to_string(value);
                    const auto entry = "\"" + key + "\": " + literal;
                    if (std::regex_search(text, item)) text = std::regex_replace(text, item, entry);
                    else {
                        const bool empty = text.find_first_not_of(" \t\r\n", open+1) == close;
                        text.insert(open+1, "\n" + entry + (empty ? "" : ","));
                    }
                    const auto temporary = path.wstring() + L".tmp";
                    ok = WriteFileUtf8(temporary, text) && MoveFileExW(temporary.c_str(), path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                }
            }
        }
        PostToUi("{\"type\":\"nrParameterEdited\",\"payload\":{\"file\":" + JsonQuoted(Utf8ToWide(file)) +
            ",\"key\":" + JsonQuoted(Utf8ToWide(key)) + ",\"value\":" + std::to_string(value) +
            ",\"requestId\":" + std::to_string(requestId) +
            ",\"ok\":" + (ok ? "true" : "false") + "}}");
        if (!ok) SendLog(g_uiLang.load() == 2 ? L"NR parameter was not saved. Restart the game with the updated core and try again."
            : L"NR 参数未保存，请使用更新后的核心重启游戏后重试。");
        if (live) SendStatusToUi();
} else if (type == "openProjectPage") {
        ShellExecuteW(g_window, L"open", L"https://github.com/LCPD15/DXL", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (type == "openReFrameworkPage") {
        ShellExecuteW(g_window, L"open", L"https://github.com/praydog/REFramework-nightly/releases", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (type == "openReShadePage") {
        ShellExecuteW(g_window, L"open", L"https://www.reshade.me/#download", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (type == "getExtensions") {
        SendExtensionsToUi();
    } else if (type == "openSemanticFolder") {
        const auto folder = DXL::SemanticExtensionFolder(ExeDir());
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec) SendLog(L"无法创建语义扩展文件夹。");
        else ShellExecuteW(g_window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (type == "downloadSemantic") {
        const auto url = SemanticDownloadUrl();
        if (!url.empty()) ShellExecuteW(g_window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else SendLog(L"语义蒙版下载地址尚未设置：config\\extensions.json");
    } else if (type == "setAdminLaunch") {
		const bool enabled = ExtractNumberField(json, "on") == 1;
		const auto path = LauncherPreferencesPath();
		const std::filesystem::path temporary = path.wstring() + L".tmp";
		if (!WriteFileUtf8(temporary, DXL::LauncherPreferences::Serialize(enabled)) ||
			!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			SendLog(L"默认管理员启动选项保存失败，原设置保留。");
		} else {
			SendLog(enabled ? L"已保存：下次启动 DXL 时请求管理员权限。"
				: L"已保存：下次启动 DXL 时使用普通权限。");
		}
		SendLauncherPreferencesToUi();
	} else if (type == "applySettings") {
		const std::string payload = ExtractPayload(json);
        const bool written = !payload.empty() && WriteFileUtf8(SettingsPath(), payload);
        const double deletionRequest = ExtractNumberField(json, "deleteRequestId", 0);
        if (deletionRequest > 0) g_profileDeletionSave.Record(deletionRequest, written, payload);
		if (!written) {
			SendLog(L"设置保存失败");
			return;
		}
		// Core-origin parameter sync only persists; never reload its own echo.
		if (g_status.IsOpen() && ExtractNumberField(json, "noReload") != 1) {
			const bool ok = DXL::SendCommand(
				g_status.Pid(), DXL::Ipc::CommandId::ReloadSettings);
			SendLog(ok ? L"设置已应用" : L"设置已保存，但 core 没有响应");
		} else {
			SendLog(L"设置已保存");
		}
		SendStatusToUi();
	} else if (type == "applyProfile") {
		// UI 把选中的 profile 展平后连文件名一起发过来，宿主只管把字节写进去。
		// 这样 core 那边只需要读一个扁平文件，不用理解 profile 结构。
		const std::string file = ExtractStringField(json, "file");
		const std::string payload = ExtractPayload(json);
		if (!IsSafeProfileFileName(file)) {
			SendLog(L"配置文件名不合法，已拒绝写入");
			return;
		}
		const std::filesystem::path path =
			ProfilesDir() / Utf8ToWide(file);
		if (payload.empty() || !WriteFileUtf8(path, payload)) {
			SendLog(L"配置写入失败：" + path.wstring());
			return;
		}
		// **写的是哪份文件必须可见**：核心参数同步排查两轮查不出断点，
		// 就是因为这条路完全静默（UI 日志里一条都没有）。
		// 带上关键值，下次"改了没生效"一眼看出是写错了文件还是值本身旧。
		{
			wchar_t tone[32]{};
			_snwprintf_s(tone, _TRUNCATE, L"%.2f",
				ExtractNumberField(json, "nrLocalTone", -1.0));
			SendLog(L"配置已写入：" + path.filename().wstring() +
				L"（色调=" + tone + L"）");
		}
		// noReload=1：**这份文件的内容来自 core**（浮层改的参数随状态块报回来）。
		// 只落盘、不叫 core 重读 —— core 刚报的就是这个值；重读会在玩家继续拖的
		// 半路上把落盘那一下的旧值压回 core，把他正调的新值打回去（双向同步
		// 互相覆盖的根子，用户实测）。UI 本侧的修改不带这个标记，照旧回推。
		if (ExtractNumberField(json, "noReload") != 1) {
			DXL::DispatchProfileCommand(g_targets, Utf8ToWide(file), [](DWORD pid) {
				return DXL::SendCommand(pid, DXL::Ipc::CommandId::ReloadSettings);
			});
		}
	} else if (type == "setLang") {
		// UI 切语言时报上来。宿主自己发的提示（注入成功那几条）按它选文案，
		// 不然英文界面下这些提示还是中文，看起来就像"没提示"。
		g_uiLang.store(int(ExtractNumberField(json, "lang", 0)));
	} else if (type == "setMaster") {
		// 配置页里切总开关。写文件 + ReloadSettings 对它**不生效**：
		// core 只在第一次读设置时取 masterEnabled（之后以 Del / 命令为准），
		// 所以运行中只能走 SetEnabled 命令 —— 文件那份是给下一局启动用的。
		const uint32_t on = ExtractNumberField(json, "on", 0) != 0 ? 1u : 0u;
		const auto file = ExtractStringField(json, "file");
		if (!IsSafeProfileFileName(file)) return;
		const auto result = DXL::DispatchProfileCommand(g_targets, Utf8ToWide(file), [on](DWORD pid) {
			return DXL::SendCommand(pid, DXL::Ipc::CommandId::SetEnabled, on);
		});
		if (result.matched) {
			const bool ok = result.succeeded == result.matched;
			SendLog(ok ? (on ? L"总开关：ON（已推送给正在运行的游戏）"
					: L"总开关：OFF（已推送给正在运行的游戏）")
				: L"总开关：游戏没在跑，只存进配置（下一局启动生效）");
		} else {
			SendLog(on ? L"总开关：ON —— 已存进配置，游戏启动时自动照此开启"
				: L"总开关：OFF —— 已存进配置，游戏启动时自动照此保持关闭");
		}
	} else if (type == "uiError") {
		// 界面脚本的未捕获异常。**必须落到文件里** —— WebView2 没有可见控制台，
		// 不记的话"点了没反应"这类故障在外面完全查不到。
		const std::wstring text = Utf8ToWide(ExtractStringField(json, "text"));
		D5_LOG_ERROR(L"%s", text.c_str());
		SendLog(text);
	} else if (type == "openLogs") {
		// core 的日志是在游戏进程里写的，UI 自己看不到内容 —— 直接把文件夹开给用户。
		// 用 explorer 而不是 ShellExecute(dir)：后者在某些 shell 扩展下会挑错程序。
		const std::filesystem::path dir = DXL::Log::LogDir();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::wstring args = L"\"" + dir.wstring() + L"\"";
		ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
			nullptr, SW_SHOWNORMAL);
		SendLog(L"已打开日志文件夹：" + dir.wstring());
	} else if (type == "openNgxDir") {
		// 运行时 DLL（nvngx_*.dll）在 exe 旁的 ngx\ 目录里，开给用户看。
		// 用 explorer 而不是 ShellExecute(dir)：理由同 openLogs。
		const std::filesystem::path dir = ExeDir() / L"ngx";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::wstring args = L"\"" + dir.wstring() + L"\"";
		ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
			nullptr, SW_SHOWNORMAL);
		SendLog(L"已打开 DLL 文件夹：" + dir.wstring());
	} else if (type == "registerHotkeys") {
		// UI 每次改完快捷键都会重发一遍
		const auto injectKey = Utf8ToWide(ExtractStringField(json, "toggleInject"));
        const bool injectRegistered = RegisterOneHotkey(g_window, HOTKEY_TOGGLE_INJECT, injectKey, L"Inject/disconnect");
        PostToUi("{\"type\":\"hotkeysRegistered\",\"payload\":{\"toggleInject\":" + JsonQuoted(injectRegistered ? injectKey : L"") + "}}");
		// Core owns the effect and panel keys only while its game has focus.
		// Registering bare Del here would steal normal editing keys and double-toggle.
		UnregisterHotKey(g_window, HOTKEY_TOGGLE_ALL);
		// Debug 视图循环。这一个是**后来加回来的** —— 当初撤掉是因为功能不存在
		// （见下面那段注释），现在深度/矢量可视化真的做出来了，而它的全部用处就在
		// "边动边切"上：走界面要切窗口 + 点应用，画面早就不是那一刻了。
		RegisterOneHotkey(g_window, HOTKEY_TOGGLE_DEBUG_VIEW,
			Utf8ToWide(ExtractStringField(json, "toggleDebugView")),
			L"Debug 视图");
		// 性能窗口的全局快捷键仍然**不注册**：它需要在游戏画面上叠加绘制，没实现。
		// 为一个不存在的功能长期占住用户的 Alt+P（RegisterHotKey 是全系统独占的）
		// 没有道理。
		//
		// Debug 视图当初和它一起被撤掉，现在加回来了 —— 因为它真的实现了
		// （深度/矢量可视化直接写进画面，不需要 overlay）。
	} else if (type == "launch") {
		const std::wstring exePath = Utf8ToWide(DXL::LaunchArguments::ReadField(json, "exePath"));
		if (exePath.empty()) {
			SendLog(L"这个配置还没设置游戏 exe 路径，无法从工具启动。");
			return;
		}
		auto request = std::make_unique<LaunchRequest>();
		request->exePath = exePath;
		request->args = Utf8ToWide(DXL::LaunchArguments::ReadField(json, "args"));
		request->exeName =
			std::filesystem::path(exePath).filename().wstring();
		request->coreDll = CoreDllPath();
		request->lateInject = DXL::LaunchArguments::ReadField(json, "timing") == "late";
        request->notify = g_window;
        if (!g_injectionCoordination.BeginLaunch(request->exeName)) {
            SendLog(g_uiLang.load() == 2 ? L"This game is already starting. Please wait." : L"该游戏正在启动，请稍候。");
            return;
        }
		SendLog(request->lateInject
			? L"正在启动 " + request->exeName +
				L"，等它起窗口后再注入（可能无法捕获原生 DLSS）…"
			: L"正在启动 " + request->exeName + L"，进程一出现就注入…");
        HANDLE launchThread = CreateThread(nullptr, 0, LaunchWorker, request.get(), 0, nullptr);
        if (!launchThread) {
            g_injectionCoordination.EndLaunch(request->exeName);
            SendLog(L"创建启动线程失败");
        } else {
            request.release();
            CloseHandle(launchThread);
        }
	} else if (type == "pickExe") {
		// 8. 新建配置走**选文件**，不是选进程。选进程等于鼓励迟到注入，
		// 而迟到注入拿不到原生矢量 —— 那是这个工具画质的上限所在。
		const std::wstring picked = DXL::PickGameExe(g_window);
		if (!picked.empty()) {
			const std::filesystem::path path(picked);
			std::string reply = "{\"type\":\"exePicked\",\"payload\":{";
			reply += "\"exePath\":" + JsonQuoted(picked);
			reply += ",\"exe\":" + JsonQuoted(path.filename().wstring());
			// 默认名字用 exe 所在文件夹名，比 exe 名可读得多
			// （"OnimushaWotS_Demo" 比 "game.exe" 有信息量，而很多游戏的 exe
			// 就叫 game.exe / launcher.exe）。
			reply += ",\"suggestedName\":" +
				JsonQuoted(path.parent_path().filename().wstring());
			reply += "}}";
			PostToUi(reply);
		}
	} else if (type == "requestLibraryMetadata") {
        const auto list = DXL::LaunchArguments::ReadField(json, "items");
        if (list.size() > 1024 * 1024) return;
        auto request = std::make_unique<LibraryRequest>();
        for (size_t pos = 0; pos < list.size() && request->items.size() < 512;) {
            const auto end = list.find('*', pos);
            const auto entry = list.substr(pos, end == std::string::npos ? end : end - pos);
            pos = end == std::string::npos ? list.size() : end + 1;
            const auto divider = entry.find('|');
            if (divider == std::string::npos || divider == 0 || divider > 128) continue;
            const auto path = Utf8ToWide(entry.substr(divider + 1));
            if (std::filesystem::path(path).is_absolute())
                request->items.emplace_back(entry.substr(0, divider), path);
        }
        StartLibraryWorker(std::move(request));
	} else if (type == "pickGameCover") {
        const auto id = DXL::LaunchArguments::ReadField(json, "id");
        if (id.empty() || id.size() > 128) return;
        // Only the chooser runs on the UI thread; decode/copy stays in the worker.
        const auto picked = DXL::LibraryMetadata::PickCover(g_window);
        if (!picked.empty()) {
            auto request = std::make_unique<LibraryRequest>();
            request->pickedCover = picked;
            request->items.emplace_back(id, Utf8ToWide(DXL::LaunchArguments::ReadField(json, "exePath")));
            StartLibraryWorker(std::move(request));
        }
	} else if (type == "requestIcons") {
		// UI 传来一串 "id|exePath" （用 | 分隔），我们把图标抠成 PNG 放进
		// web/iconcache/，回一个 id -> 相对 url 的映射。
		// **走文件 + 虚拟主机，不走 base64 data URL** —— 图标是 256x256，
		// 十几个配置的 base64 会让状态消息膨胀到几百 KB，而那条通道每 500ms 走一次。
		const std::string list = ExtractStringField(json, "items");
		std::string icons = "{";
		std::vector<std::pair<std::string, std::wstring>> dxItems;
		bool first = true;
		size_t pos = 0;
		while (pos <= list.size()) {
			const size_t end = list.find('*', pos);
			const std::string entry =
				list.substr(pos, end == std::string::npos ? end : end - pos);
			pos = end == std::string::npos ? list.size() + 1 : end + 1;
			const size_t bar = entry.find('|');
			if (bar == std::string::npos) continue;
			const std::string id = entry.substr(0, bar);
			const std::wstring exe = Utf8ToWide(entry.substr(bar + 1));
			if (id.empty() || exe.empty()) continue;
			// id 直接进文件名，必须过滤 —— 它来自 UI，是不可信输入。
			if (!IsSafeProfileFileName(id + ".json")) continue;
			const std::filesystem::path png =
				ExeDir() / L"web" / L"iconcache" / (Utf8ToWide(id) + L".png");
			std::error_code ec;
			if (!std::filesystem::exists(png, ec)) {
				if (!DXL::ExtractExeIconPng(exe, png)) continue;
			}
			if (!first) icons += ",";
			first = false;
			icons += JsonQuoted(Utf8ToWide(id)) + ":" +
				JsonQuoted(Utf8ToWide("iconcache/" + id + ".png"));
			// DX 版本判定挪后台线程：全文件扫描要几百毫秒，
			// UI 线程上跑会卡住整个界面。见 DxScanWorker。
			dxItems.emplace_back(id, exe);
		}
		icons += "}";
		PostToUi("{\"type\":\"icons\",\"payload\":" + icons + "}");
		if (!dxItems.empty()) {
			auto* request = new DxScanRequest{ std::move(dxItems) };
			if (!CreateThread(nullptr, 0, DxScanWorker, request, 0, nullptr)) {
				delete request;
			}   // 线程负责释放
		}
	} else if (type == "listTargets") {
		SendTargetsToUi();
	} else if (type == "attach") {
		const DWORD pid = ExtractUint(json, "pid");
		// 记下名字用于显示；枚举一次比在 UI 侧回传更可靠
		std::wstring exePath;
		for (const DXL::TargetCandidate& candidate :
			DXL::EnumerateTargets()) {
			if (candidate.pid == pid) {
				g_attachedName = candidate.exeName;
				exePath = candidate.exePath;
				break;
			}
		}
		// 注入之前先警告，破坏是不可逆的（只能重启游戏）
		if (DXL::LooksLikeGameHasOwnDlss(exePath)) {
            SendLog(L"检测到游戏自带 DLSS / Streamline，已保留原生功能。"
                L"实际 NR 路径以游戏内面板为准；若持续停在 Present，请从工具重新启动游戏以便更早捕获 NGX。");
		}
		Attach(pid);
	} else if (type == "cleanupInvalidGames") {
        auto request = std::make_unique<CleanupRequest>();
        request->notify = g_window;
        const std::string list = ExtractStringField(json, "items");
        size_t pos = 0;
        while (pos <= list.size()) {
            const size_t end = list.find('*', pos);
            const std::string entry = list.substr(pos, end == std::string::npos ? end : end - pos);
            pos = end == std::string::npos ? list.size() + 1 : end + 1;
            const size_t bar = entry.find('|');
            if (bar == std::string::npos) continue;
            const std::string id = entry.substr(0, bar);
            const std::wstring path = Utf8ToWide(entry.substr(bar + 1));
            if (id.empty() || id == "default" || path.empty()) continue;
            // A running, registered game remains in the list even if its install is moving.
            bool running = false;
            for (const auto& target : g_targets)
                if (_wcsicmp(target.path.c_str(), path.c_str()) == 0) { running = true; break; }
            if (!running) request->entries.emplace_back(id, path);
        }
        HANDLE thread = CreateThread(nullptr, 0, CleanupWorker, request.get(), 0, nullptr);
        if (thread) { request.release(); CloseHandle(thread); }
        else {
            SendLog(L"无法启动失效游戏检查，请稍后重试。");
            PostToUi("{\"type\":\"invalidGames\",\"payload\":{\"missing\":[],\"failed\":true}}");
        }
	} else if (type == "scanGames") {
		// 7. 扫 Steam / Epic / GOG。放后台线程 —— 要遍历每个游戏目录挑主 exe，
		// 装了几十个游戏的机器上要几秒，卡在 UI 线程上界面会假死。
		SendLog(L"正在扫描 Steam / Epic / GOG 的已安装游戏…");
		CreateThread(nullptr, 0, ScanWorker, nullptr, 0, nullptr);
	} else if (type == "setWatch") {
		SetWatchEnabled(ExtractUint(json, "on") != 0);
	} else if (type == "watchList") {
		// UI 把要监控的 exe 名（小写，'*' 分隔）推过来。每次应用设置都会推一次，
		// 所以列表永远和界面一致 —— 不用在宿主里理解 profile 结构。
		//
		// **分隔符必须可打印。** 以前用 0x1E，而 JSON.stringify 会把控制字符转义成
		// ``，我们这个朴素取值器只取引号之间的原文 —— split 分不出任何东西，
		// 于是监控列表永远只有一个分不开的条目，**全局监控一直没匹配到任何游戏**，
		// 而且不报错，只是什么都不发生。
		const std::string list = ExtractStringField(json, "names");
		std::vector<std::wstring> names;
		size_t pos = 0;
		while (pos <= list.size()) {
			const size_t end = list.find('*', pos);
			const std::string entry =
				list.substr(pos, end == std::string::npos ? end : end - pos);
			pos = end == std::string::npos ? list.size() + 1 : end + 1;
			if (!entry.empty()) names.push_back(Utf8ToWide(entry));
		}
		// **报一下收到几个。** 这条链路以前因为分隔符被 JSON 转义而静默失效
		// （列表里永远只有一个分不开的条目，全局监控什么都匹配不到，也不报错）。
		// 一个数字就能让下次同类故障立刻显形。
		static size_t lastCount = SIZE_MAX;
		if (names.size() != lastCount) {
			lastCount = names.size();
			D5_LOG_INFO(L"全局监控：收到 %zu 个要盯的 exe 名", names.size());
		}
        std::vector<std::wstring> lateNames;
        const auto lateList = ExtractStringField(json, "lateNames");
        for (size_t start = 0; start < lateList.size();) {
            const size_t end = lateList.find('*', start);
            const auto name = Utf8ToWide(lateList.substr(start, end == std::string::npos ? end : end - start));
            if (std::find(names.begin(), names.end(), name) != names.end()) lateNames.push_back(name);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        std::lock_guard<std::mutex> guard(g_watchMutex);
        g_watchNames = std::move(names);
        g_watchLateNames = std::move(lateNames);
	} else if (type == "detach") {
		Detach();
	} else if (type == "switchTarget") {
		// 状态条的下拉框：切换查看另一个被注入的进程。
		// 多目标表在宿主这边，UI 只报 pid —— 它那边只存一串显示名。
		SwitchTarget(ExtractUint(json, "pid"));
	}
}

bool g_windowStateReady = false;
void SaveWindowState(HWND hwnd) noexcept {
    if (!g_windowStateReady) return;
    try {
        if (const auto state=DXL::WindowState::Capture(hwnd))
            DXL::WindowState::Save(ConfigDir()/L"window.json",*state);
    } catch (...) { /* Keep the last valid window placement on I/O failure. */ }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
    case WM_APP_UPDATE_DONE: {
        std::unique_ptr<std::string> response(reinterpret_cast<std::string*>(lParam));
        g_updateBusy=false;
        if (response) PostToUi(*response);
        return 0;
    }
    case WM_CLOSE:
        SaveWindowState(hwnd);
        DestroyWindow(hwnd);
        return 0;
    case WM_EXITSIZEMOVE:
        SaveWindowState(hwnd);
        return 0;
    case WM_ENDSESSION:
        if (wParam) SaveWindowState(hwnd);
        return 0;
	case WM_SIZE:
		if (g_controller) {
			RECT bounds{};
			GetClientRect(hwnd, &bounds);
			g_controller->put_Bounds(bounds);
		}
		return 0;
	case WM_GETMINMAXINFO: {
		auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
		MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))
            info->ptMinTrackSize = {std::min(860L,monitor.rcWork.right-monitor.rcWork.left),std::min(560L,monitor.rcWork.bottom-monitor.rcWork.top)};
        else info->ptMinTrackSize = {860,560};
		return 0;
	}
	case WM_HOTKEY:
		switch ((int)wParam) {
		case HOTKEY_TOGGLE_INJECT:
			// 核心交互：在游戏里按一下就注入/断开，不用切出来选进程
			if (g_status.IsOpen()) {
				if (g_launchedByTool) {
					// 见 g_launchedByTool 的说明：这一局断开就再也拿不回原生矢量了
					SendLog(L"这一局是从工具启动的，**不能取消注入** —— "
						L"断开之后再注入就是迟到注入，拿不到游戏的原生深度和矢量，"
						L"DLSSNR 只能吃零矢量，效果会静默变差。"
						L"要临时关掉效果请按已设置的效果快捷键，它不动 hook。");
				} else {
					Detach();
				}
			} else {
				AttachForegroundWindow();
			}
			break;
		case HOTKEY_TOGGLE_DEBUG_VIEW:
			if (g_status.IsOpen()) {
				// 0xFFFFFFFF = 让 core 循环到下一个（关 -> 深度 -> 矢量）。
				// 由 core 记当前值而不是 UI 记：core 那边才是权威，
				// 而且用户也可能在界面上直接改过。
				DXL::SendCommand(g_status.Pid(),
					DXL::Ipc::CommandId::ToggleDebugView, 0xFFFFFFFFu);
				SendLog(L"Debug 视图：切到下一个（关 → 深度 → 运动矢量）");
			}
			break;
		case HOTKEY_TOGGLE_ALL:
			if (g_status.IsOpen()) {
				// **从 core 读当前值再取反**，不要用宿主自己记的那份 ——
				// 两边不一致时第一下会反着来（详见 ReadMasterEnabled 的说明）。
				const bool next = !ReadMasterEnabled();
				DXL::SendCommand(g_status.Pid(),
					DXL::Ipc::CommandId::SetEnabled, next ? 1u : 0u);
				SendLog(next ? L"总开关：ON" : L"总开关：OFF");
				// 把这次选择存回配置：下一局按同样的状态启动。
				// 存盘交给界面那边做（它才知道当前是哪个配置、也才有写文件的那套路），
				// 这里只把结果告诉它。
				PostToUi(std::string("{\"type\":\"masterToggled\",\"payload\":{\"on\":")
					+ (next ? "true" : "false") + "}}");
			}
			break;
		}
		return 0;
	case WM_APP_WORKER_DONE: {
        std::unique_ptr<WorkerResult> result(reinterpret_cast<WorkerResult*>(lParam));
        if (!result) return 0;
		// 后台线程（从工具启动游戏）干完了，回到 UI 线程再和 WebView2 说话
		if (!result->log.empty()) SendLog(result->log);
		if (result->attachPid) {
			// 这条分支就是"从工具启动游戏"干完之后回到 UI 线程的地方。
			// 总开关不在这里动 —— core 已经按配置里的 masterEnabled 取好初值了。
			g_launchedByTool = true;
			g_attachedName = result->attachName;
			g_attachedPath = result->attachPath;
			for (int attempt = 0;
				attempt < 60 && !g_status.Open(result->attachPid); ++attempt) {
				Sleep(50);
			}
			RegisterTarget(result->attachPid);
			NotifyUiOfAttachedGame();
		}
        g_injectionCoordination.FinishLaunch(result->launchName, result->attachPid);
		SendStatusToUi();
		return 0;
    }
	case WM_APP_SCAN_DONE: {
		std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lParam));
		if (json) PostToUi(*json);
		return 0;
	}
	case WM_APP_CLEANUP_DONE:
	case WM_APP_LIBRARY_DONE:
	case WM_APP_DX_DONE: {
		// 后台 DX 版本扫描的补发（图标那条消息已经先回去了）。
		std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lParam));
		if (json) PostToUi(*json);
		return 0;
	}
    case WM_APP_PROFILE_DELETE_DONE: {
        if (g_profileDeletionThread) {
            WaitForSingleObject(g_profileDeletionThread, INFINITE);
            CloseHandle(g_profileDeletionThread);
            g_profileDeletionThread = nullptr;
        }
        std::unique_ptr<std::string> reply;
        { std::lock_guard lock(g_profileDeletionMutex); reply = std::move(g_profileDeletionReply); }
        if (reply) SendProfileDeletionReply(*reply);
        return 0;
    }
	case WM_APP_WATCH_HIT: {
        const DWORD pid = (DWORD)wParam;
        std::wstring name = std::filesystem::path(DXL::ProcessImagePath(pid)).filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
        bool late = false, monitored = false;
        {
            std::lock_guard guard(g_watchMutex);
            monitored = std::find(g_watchNames.begin(), g_watchNames.end(), name) != g_watchNames.end();
            late = std::find(g_watchLateNames.begin(), g_watchLateNames.end(), name) != g_watchLateNames.end();
        }
        if (!g_watchEnabled.load() || !monitored) { g_injectionCoordination.CancelWatch(pid); return 0; }
        if (!late && !g_injectionCoordination.ObserveLoader(pid, DXL::ModuleCount(pid), GetTickCount64())) {
            g_injectionCoordination.CancelWatch(pid); return 0;
        }
        if (!g_injectionCoordination.BeginWatch(pid, name, late,
            late && DXL::HasVisibleWindow(pid), GetTickCount64())) return 0;
        D5_LOG_INFO(L"Automatic injection: pid=%lu timing=%s", pid, late ? L"compatibility (window settled)" : L"early (loader quiet)");
		// **多目标：已经接着的不再挡住新的。** 以前这里 `if (g_status.IsOpen())
		// return 0;` —— 一次只支持一个目标，blender 这类常驻软件先被接上之后，
		// 后面启动的游戏全被这条静默丢掉（用户实测：开游戏根本看不出注入没注入）。
		// 现在每个命中的进程都注入、都进多目标表；界面上切换查看。
		SendLog(L"全局监控：看到列表里的游戏启动了（pid " +
			std::to_wstring(pid) + L"），正在注入…");
		Attach(pid, !late && !DXL::HasVisibleWindow(pid));
		// 让界面知道接上了谁 —— 配置列表的"正在运行"高亮和 Del 存盘都靠它
		NotifyUiOfAttachedGame();
		return 0;
	}
	case WM_TIMER:
		if (wParam == STATUS_TIMER_ID && g_webview) {
			// 多目标存活检查：表里每个目标都查一遍（游戏退出后共享内存还能
			// 映射但内容不再更新，只能用进程存活性判断）。退出的从表里删掉，
			// 顺带把它的 core 状态块关闭 —— 当前查看目标退出后就换到表里
			// 下一个还活着的。
			bool removed = false;
			for (size_t i = 0; i < g_targets.size();) {
				const DWORD pid = g_targets[i].pid;
				HANDLE process = OpenProcess(
					PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
				DWORD exitCode = STILL_ACTIVE;
				if (!process) {
					exitCode = 0;
				} else {
					GetExitCodeProcess(process, &exitCode);
					CloseHandle(process);
				}
				if (exitCode == STILL_ACTIVE) {
					++i;
					continue;
				}
				SendLog(L"目标进程已退出，断开连接：" +
					g_targets[i].name + L"（pid " + std::to_wstring(pid) + L"）");
				if (pid == g_currentPid) {
					g_status.Close();
					g_attachedName.clear();
				}
				g_targets.erase(g_targets.begin() + long(i));
				removed = true;
			}
			if (removed) {
				// 当前目标被删了：退到表里第一个；表空就回到未注入态
				if (CurrentTarget()) {
					SwitchTarget(g_currentPid);
				} else {
					g_currentPid = 0;
				}
			} else if (!CurrentTarget()) {
				EnsureCurrentTarget();   // 表里还有但没有当前目标（没发生过，兜底）
				if (CurrentTarget()) SwitchTarget(g_currentPid);
			}
			SendStatusToUi();
		}
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, STATUS_TIMER_ID);
        // Finish an explicitly requested driver write before process exit.
        if (g_profileDeletionThread) {
            WaitForSingleObject(g_profileDeletionThread, INFINITE);
            CloseHandle(g_profileDeletionThread);
            g_profileDeletionThread = nullptr;
        }
        { std::lock_guard lock(g_profileDeletionMutex); g_profileDeletionReply.reset(); }
		g_webview.Reset();
		g_controller.Reset();
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowFatal(const wchar_t* text) {
	MessageBoxW(g_window, text, WINDOW_TITLE, MB_ICONERROR | MB_OK);
}

HRESULT OnWebViewCreated(HRESULT result, ICoreWebView2Controller* controller) {
	if (FAILED(result) || !controller) {
		ShowFatal(g_uiLang.load() == 2 ? L"Could not create WebView2.\n\nPlease install WebView2 Runtime." : L"创建 WebView2 失败。\n\n请确认已安装 WebView2 Runtime。");
		PostQuitMessage(1);
		return result;
	}
	g_controller = controller;
	if (FAILED(g_controller->get_CoreWebView2(&g_webview))) {
		ShowFatal(g_uiLang.load() == 2 ? L"Could not get ICoreWebView2." : L"获取 ICoreWebView2 失败。");
		PostQuitMessage(1);
		return E_FAIL;
	}

	ComPtr<ICoreWebView2Settings> settings;
	if (SUCCEEDED(g_webview->get_Settings(&settings))) {
		settings->put_AreDefaultContextMenusEnabled(FALSE);
		settings->put_IsZoomControlEnabled(FALSE);
		settings->put_AreDevToolsEnabled(TRUE);   // 里程碑 0 保留，方便调 UI
		settings->put_IsStatusBarEnabled(FALSE);
	}

	// NVIDIA Sans：**不随工具分发**（NVIDIA 专有字体），到用户自己的
	// NVIDIA App 安装目录里取。没装就什么都不做，CSS 自动回退到 Segoe UI。
	BorrowNvidiaSans();

	// 把 web/ 映射成虚拟主机，避免 file:// 的各种限制
	ComPtr<ICoreWebView2_3> webview3;
	if (SUCCEEDED(g_webview->QueryInterface(IID_PPV_ARGS(&webview3)))) {
		const std::wstring webRoot = (ExeDir() / L"web").wstring();
		webview3->SetVirtualHostNameToFolderMapping(
			VIRTUAL_HOST, webRoot.c_str(),
			COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
	}

	g_webview->add_WebMessageReceived(
		Callback<ICoreWebView2WebMessageReceivedEventHandler>(
			[](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
				LPWSTR raw = nullptr;
				if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
					HandleUiMessage(WideToUtf8(raw));
					CoTaskMemFree(raw);
				}
				return S_OK;
			}).Get(), nullptr);

	RECT bounds{};
	GetClientRect(g_window, &bounds);
	g_controller->put_Bounds(bounds);

	const std::wstring url = std::wstring(L"https://") + VIRTUAL_HOST + L"/index.html?lang=" +
        (g_uiLang.load()==2 ? L"en" : L"zh") + L"&systemLang=" +
        (DXL::UiLanguage::System()==2 ? L"en" : L"zh");
	g_webview->Navigate(url.c_str());
	return S_OK;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR arguments, int) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
	g_uiLang.store(DXL::UiLanguage::Resolve(ExtractStringField(ReadFileUtf8(SettingsPath()), "lang")));
	if (StartElevatedIfPreferred(arguments ? arguments : L"")) {
		CoUninitialize();
		return 0;
	}

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = instance;
	// 应用图标（app.rc 里的 ICON 资源 id 1）：窗口左上角 + 任务栏 + Alt-Tab。
	// 大小两种：标题栏/任务栏会按 DPI 自己挑合适的。SMALL 取不到时系统会
	// 缩放 hIcon，所以只给 hIcon 也能跑，但给全更清楚。
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
	wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(RGB(0xEE, 0xF0, 0xF4));
	wc.lpszClassName = WINDOW_CLASS;
	RegisterClassExW(&wc);

	// 默认 1560x1050（原来 1040x700 的 1.5 倍）。**不是随便放大** ——
	// 参数页现在是两列卡片，1040 宽只能挤成一列，底部的「应用」和常驻耗时读数
	// 也要往下翻才看得到。宽度不够时那份界面是不能用的，不是不好看而已。
	// 屏幕放不下就退回屏幕的九成，别开出一个比桌面还大的窗口。
	int width = 1560;
	int height = 1050;
	const int screenW = GetSystemMetrics(SM_CXSCREEN);
	const int screenH = GetSystemMetrics(SM_CYSCREEN);
	if (screenW > 0 && width > screenW * 9 / 10) width = screenW * 9 / 10;
	if (screenH > 0 && height > screenH * 9 / 10) height = screenH * 9 / 10;
	g_window = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE,
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
		nullptr, nullptr, instance, nullptr);
	if (!g_window) return 1;
	if (!DXL::WindowState::Restore(g_window, ConfigDir()/L"window.json")) ShowWindow(g_window, SW_SHOW);
    g_windowStateReady = true;
	SetTimer(g_window, STATUS_TIMER_ID, STATUS_TIMER_INTERVAL_MS, nullptr);

	DXL::Log::Get().Open(L"ui");
	D5_LOG_INFO(L"UI 宿主启动，配置目录 %s", ConfigDir().c_str());


	const std::wstring userData = (ConfigDir() / L"WebView2").wstring();
	const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
		nullptr, userData.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT {
				if (FAILED(envResult) || !env) {
					ShowFatal(g_uiLang.load() == 2 ? L"Could not create the WebView2 environment.\n\nInstall WebView2 Runtime:\nhttps://developer.microsoft.com/microsoft-edge/webview2/" : L"创建 WebView2 环境失败。\n\n"
						L"请安装 WebView2 Runtime：\n"
						L"https://developer.microsoft.com/microsoft-edge/webview2/");
					PostQuitMessage(1);
					return envResult;
				}
				return env->CreateCoreWebView2Controller(g_window,
					Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						&OnWebViewCreated).Get());
			}).Get());
	if (FAILED(hr)) {
		ShowFatal(g_uiLang.load() == 2 ? L"CreateCoreWebView2EnvironmentWithOptions failed." : L"CreateCoreWebView2EnvironmentWithOptions 失败。");
		return 1;
	}

	MSG msg{};
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	SetWatchEnabled(false);
	CoUninitialize();
	return (int)msg.wParam;
}
