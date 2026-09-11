#pragma once

// UI 宿主这一侧和"某个游戏进程"打交道所需要的全部东西：找目标、注入、读状态、
// 发命令。放在单独一个头里，好让 main.cpp 只管窗口和消息转发。
//
// 三条通道各司其职（和 IpcProtocol.h 里说的一致）：
//   注入    CreateRemoteThread(LoadLibraryW)  —— 一次性
//   状态    命名共享内存 + seqlock            —— UI 轮询
//   命令    命名管道                          —— 低频请求/应答

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <memory>
#include <string>
#include <fstream>
#include <vector>

#include "../common/IpcClient.h"
#include "../common/Log.h"
#include "LaunchArguments.h"
#include "InjectionDiagnostics.h"

#pragma comment(lib, "psapi.lib")

namespace DXL {

struct TargetCandidate {
	DWORD pid = 0;
	bool is64Bit = true;
	std::wstring exeName;
	std::wstring exePath;      // 完整路径，用来检查游戏目录里有没有自带 DLSS
	std::wstring windowTitle;
};

// 游戏自带 DLSS 的迹象。为什么必须检查：NGX 的初始化是**每进程每设备一次**的，
// 游戏已经有一个活着的 NGX 会话时我们再初始化一次会把它弄坏 —— 游戏自己的 DLSS
// 上采样从此失效，3D 场景渲进一个再也不会被解析出来的目标，而 UI 在那之后画所以
// 看起来完好。关掉我们的开关不会撤销这个破坏，只有重启游戏能恢复。
//
// 注意只能"提示"不能"拒绝"：游戏**装了** DLSS 不等于**开着** DLSS。玩家在游戏内
// 关掉 DLSS 之后，本工具是可以正常工作的（鬼武者 demo 实测如此）。Streamline 的
// interposer 无论开不开都会加载，所以按"模块是否加载"去判断会误报。
inline bool LooksLikeGameHasOwnDlss(const std::wstring& exePath) {
	if (exePath.empty()) return false;
	const std::filesystem::path directory =
		std::filesystem::path(exePath).parent_path();
	static const wchar_t* const MARKERS[]{
		L"nvngx_dlss.dll",      // 游戏自带的 DLSS SR snippet
		L"sl.interposer.dll",   // Streamline
		L"sl.dlss.dll",
	};
	std::error_code ec;
	for (const wchar_t* marker : MARKERS) {
		if (std::filesystem::exists(directory / marker, ec)) return true;
	}
	return false;
}

// 有可见顶层窗口的进程。判断标准和注入器的 --list 一致：一个进程只算一次，
// 取它第一个可见且有标题的窗口。
inline std::vector<TargetCandidate> EnumerateTargets() {
	struct Context {
		std::vector<TargetCandidate> found;
		DWORD selfPid = GetCurrentProcessId();
	} context;

	EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
		auto* ctx = reinterpret_cast<Context*>(param);
		if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (!pid || pid == ctx->selfPid) return TRUE;
		for (const TargetCandidate& existing : ctx->found) {
			if (existing.pid == pid) return TRUE;
		}

		wchar_t title[256]{};
		if (!GetWindowTextW(hwnd, title, 256) || !title[0]) return TRUE;

		HANDLE process = OpenProcess(
			PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) return TRUE;

		TargetCandidate candidate;
		candidate.pid = pid;
		candidate.windowTitle = title;
		wchar_t path[MAX_PATH]{};
		DWORD length = MAX_PATH;
		if (QueryFullProcessImageNameW(process, 0, path, &length)) {
			candidate.exeName = std::filesystem::path(path).filename().wstring();
			candidate.exePath = path;
		}
		BOOL isWow64 = FALSE;
		if (IsWow64Process(process, &isWow64)) candidate.is64Bit = !isWow64;
		CloseHandle(process);

		if (!candidate.exeName.empty()) ctx->found.push_back(std::move(candidate));
		return TRUE;
	}, reinterpret_cast<LPARAM>(&context));

	return context.found;
}

// 按 exe 名找进程。**不看窗口** —— 从工具启动游戏时要尽早注入，而进程存在远早于
// 它创建窗口和 swapchain，用窗口去等就太晚了。
//
// exclude 里的 pid 会被跳过：Steam 游戏会先起一个同名的壳进程再通过 Steam 拉起
// 真正的自己，所以调用方需要能"换下一个再试"。
inline DWORD FindProcessIdByName(
	const std::wstring& exeName, const std::vector<DWORD>& exclude = {}) {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;

	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	DWORD pid = 0;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szExeFile, exeName.c_str()) != 0) continue;
			bool skip = false;
			for (const DWORD excluded : exclude) {
				if (excluded == entry.th32ProcessID) {
					skip = true;
					break;
				}
			}
			if (!skip) {
				pid = entry.th32ProcessID;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return pid;
}

// 轻量版"这个 pid 有可见窗口吗" —— 晚注入模式要每 50ms 问一次，
// 不能用 EnumerateTargets（那个会对每个窗口 OpenProcess + 取路径）。
inline bool HasVisibleWindow(DWORD pid) {
	struct Context {
		DWORD pid;
		bool found = false;
	} context{ pid };

	EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
		auto* ctx = reinterpret_cast<Context*>(param);
		if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
		DWORD owner = 0;
		GetWindowThreadProcessId(hwnd, &owner);
		if (owner != ctx->pid) return TRUE;
		// 排除 splash / 隐形的工具窗口：真正的游戏主窗口总是有标题且够大
		wchar_t title[8]{};
		if (!GetWindowTextLengthW(hwnd) || !GetWindowTextW(hwnd, title, 8)) return TRUE;
		RECT rect{};
		if (!GetClientRect(hwnd, &rect)) return TRUE;
		if (rect.right - rect.left < 320 || rect.bottom - rect.top < 240) return TRUE;
		ctx->found = true;
		return FALSE;
	}, reinterpret_cast<LPARAM>(&context));

	return context.found;
}

inline bool IsProcessAlive(DWORD pid) {
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) return false;
	DWORD exitCode = STILL_ACTIVE;
	const bool ok = GetExitCodeProcess(process, &exitCode) != FALSE;
	CloseHandle(process);
	return ok && exitCode == STILL_ACTIVE;
}

// 拿某个进程的完整 exe 路径。窗口还没出现时 EnumerateTargets 找不到它，
// 但我们已经有 pid 了。
// core 已经在这个进程里了吗？判据和 UI 那边一致：状态共享内存能打开就说明在。
// 用它挡住"重复启动"—— 见 LaunchAndInject 开头那段。
inline bool IsCoreInjected(DWORD pid) {
	if (!pid) return false;
	wchar_t name[128]{};
	_snwprintf_s(name, _TRUNCATE, L"%s.%lu", Ipc::STATUS_MEMORY_BASE, pid);
	const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
	if (!mapping) return false;
	CloseHandle(mapping);
	return true;
}

inline std::wstring ProcessImagePath(DWORD pid) {
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) return {};
	wchar_t path[MAX_PATH]{};
	DWORD length = MAX_PATH;
	const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
	CloseHandle(process);
	return ok ? std::wstring(path) : std::wstring{};
}

/* ---------------- 注入时机：必须等目标的加载器安静下来 ---------------- */
//
// 为什么必须等：进程刚创建的那几百毫秒里，主线程正在 loader 里加载自己的依赖，
// 持有 loader lock。这时候 CreateRemoteThread 调 LoadLibraryW，而我们的 core 又
// 静态依赖 d3d11/d3d12/dxgi，两边在 loader 上撞车 —— **实测三次里有两次整个目标
// 进程直接挂死**：进程还活着，但一行输出都没有，连我们 DllMain 里的第一句日志都
// 没打出来（说明死在 DLL 加载本身，还没轮到我们的代码）。
//
// 判据刻意不用绝对时间，而是"模块表还在不在变"：连续一段时间模块数量没有变化，
// 就认为初始加载告一段落了。固定 sleep 多少都是猜，这个是直接看 loader 在不在干活。
//
// 注意这依然是**早注入** —— 游戏加载完自己的依赖离它创建 swapchain 还早得很
// （实测游戏从启动到建 swapchain 有好几秒到几十秒）。

// 目标进程当前加载了多少个模块。0 = 问不出来（进程刚起/已退出/权限不足）。
// 给"加载器安静了吗"的判断当采样点用。
inline DWORD ModuleCount(DWORD pid) {
	HANDLE process = OpenProcess(
		PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!process) return 0;
	HMODULE modules[1024];
	DWORD needed = 0;
	const bool ok =
		EnumProcessModules(process, modules, sizeof(modules), &needed) != FALSE;
	CloseHandle(process);
	return ok ? needed / DWORD(sizeof(HMODULE)) : 0;
}

// 阻塞版，只给命令行注入器用（那里是个一次性的前台流程，阻塞没关系）。
// UI 那条路走 LaunchAndInject 里的非阻塞版本 —— 它不能停下来等，否则就错过
// Steam 启动壳拉起来的真进程了。
inline bool WaitForLoaderQuiet(
	DWORD pid, int quietMs = 700, int timeoutMs = 20000) {
	HANDLE process = OpenProcess(
		PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!process) return false;

	DWORD lastCount = 0;
	int stableFor = 0;
	bool quiet = false;
	for (int waited = 0; waited < timeoutMs; waited += 100) {
		HMODULE modules[1024];
		DWORD needed = 0;
		if (!EnumProcessModules(process, modules, sizeof(modules), &needed)) {
			// 进程可能刚退出，或者还没到能枚举模块的阶段
			Sleep(100);
			continue;
		}
		const DWORD count = needed / sizeof(HMODULE);
		if (count == lastCount && count > 0) {
			stableFor += 100;
			if (stableFor >= quietMs) {
				quiet = true;
				break;
			}
		} else {
			lastCount = count;
			stableFor = 0;
		}
		Sleep(100);
	}
	CloseHandle(process);
	return quiet;
}

// 在注入**之前**告诉 core"这次是早注入"。
//
// 为什么要由注入方来说：core 静态链接了 d3d11/d3d12/dxgi，它一被加载这三个模块就
// 都在进程里了，所以 core 自己没法用"图形 DLL 加载了吗"来分辨早晚。而注入方一清二楚
// —— 是它启动了游戏，还是它挂到一个已经在跑的游戏上。
//
// core 拿这一位决定要不要自己造探测设备读 DXGI vtable：早注入时**不造**，改成等游戏
// 自己创建 swapchain。见 core.cpp 顶部的说明。
//
// 返回的句柄必须由调用方持有到注入完成之后（core 在 InitThread 里读它），
// 所以用 unique_ptr 风格的持有者，析构时自动关。
class EarlyInjectMarker {
public:
	explicit EarlyInjectMarker(DWORD pid) noexcept {
		wchar_t name[128]{};
		_snwprintf_s(name, _TRUNCATE, L"%s.%lu",
			Ipc::EARLY_INJECT_EVENT_BASE, pid);
		_event = CreateEventW(nullptr, TRUE, FALSE, name);
	}
	~EarlyInjectMarker() {
		if (_event) CloseHandle(_event);
	}
	EarlyInjectMarker(const EarlyInjectMarker&) = delete;
	EarlyInjectMarker& operator=(const EarlyInjectMarker&) = delete;
	bool IsValid() const noexcept { return _event != nullptr; }

private:
	HANDLE _event = nullptr;
};

// 标准的 LoadLibrary 远程线程注入。失败原因写进 error，直接给用户看。
inline bool InjectCore(
	DWORD pid, const std::filesystem::path& dll, std::wstring& error) {
	error.clear();
	const ULONGLONG startedAt = GetTickCount64();
	D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=begin dll=%s", pid, dll.c_str());
	std::error_code ec;
	const bool exists = std::filesystem::is_regular_file(dll, ec);
	D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=check-dll ok=%u error=0x%08X", pid, unsigned(exists), unsigned(ec.value()));
	if (!exists) {
		error = L"找不到 core DLL：" + dll.wstring();
		return false;
	}

	HANDLE process = OpenProcess(
		PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
		PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
	const DWORD openError = process ? ERROR_SUCCESS : GetLastError();
	D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=OpenProcess ok=%u error=0x%08X", pid, unsigned(process != nullptr), openError);
	if (!process) {
		error = openError == ERROR_ACCESS_DENIED
			? L"打开目标进程被拒绝。游戏以管理员身份运行时，本程序也需要管理员权限。"
			: L"OpenProcess 失败，错误码 " + std::to_wstring(openError);
		return false;
	}
	struct ProcessGuard {
		HANDLE handle; DWORD pid;
		~ProcessGuard() {
			const BOOL closed = CloseHandle(handle);
			const DWORD code = closed ? ERROR_SUCCESS : GetLastError();
			D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=CloseProcess ok=%u error=0x%08X", pid, unsigned(closed != FALSE), code);
		}
	} processGuard{process, pid};

	BOOL isWow64 = FALSE;
	const BOOL archRead = IsWow64Process(process, &isWow64);
	const DWORD archError = archRead ? ERROR_SUCCESS : GetLastError();
	D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=IsWow64Process ok=%u wow64=%u error=0x%08X", pid,
		unsigned(archRead != FALSE), unsigned(isWow64 != FALSE), archError);
	if (archRead && isWow64) {
		error = L"目标是 32 位进程。NGX 只有 x64 版本，32 位游戏需要另外的方案。";
		return false;
	}

	const std::wstring path = dll.wstring();
	const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
	void* remote = VirtualAllocEx(
		process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	const DWORD allocError = remote ? ERROR_SUCCESS : GetLastError();
	D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=VirtualAllocEx ok=%u bytes=%llu error=0x%08X", pid,
		unsigned(remote != nullptr), static_cast<unsigned long long>(bytes), allocError);
	if (!remote) {
		error = L"VirtualAllocEx 失败，错误码 " + std::to_wstring(allocError);
		return false;
	}
	InjectionDiagnostics::RemoteArgument argument(process, remote);

	bool ok = false;
	// One existing remote LoadLibrary thread, no alternate loader or retry.
	const auto runLoad = [&]() -> bool {
		SIZE_T written = 0;
		const BOOL wrote = WriteProcessMemory(process, remote, path.c_str(), bytes, &written);
		const DWORD writeError = wrote ? ERROR_SUCCESS : GetLastError();
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=WriteProcessMemory ok=%u bytes=%llu/%llu error=0x%08X", pid,
			unsigned(wrote != FALSE), static_cast<unsigned long long>(written), static_cast<unsigned long long>(bytes), writeError);
		if (!wrote || written != bytes) {
			error = L"WriteProcessMemory 失败或写入不完整，错误码 " + std::to_wstring(writeError);
			return false;
		}
		const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		const DWORD kernelError = kernel ? ERROR_SUCCESS : GetLastError();
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=GetModuleHandle(kernel32) ok=%u error=0x%08X", pid,
			unsigned(kernel != nullptr), kernelError);
		if (!kernel) { error = L"找不到本机 kernel32，错误码 " + std::to_wstring(kernelError); return false; }
		auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
		const DWORD exportError = loader ? ERROR_SUCCESS : GetLastError();
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=GetProcAddress(LoadLibraryW) ok=%u error=0x%08X", pid,
			unsigned(loader != nullptr), exportError);
		if (!loader) { error = L"找不到 LoadLibraryW，错误码 " + std::to_wstring(exportError); return false; }
		DWORD threadId = 0;
		HANDLE thread = CreateRemoteThread(process, nullptr, 0, loader, remote, 0, &threadId);
		const DWORD threadError = thread ? ERROR_SUCCESS : GetLastError();
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=CreateRemoteThread ok=%u tid=%lu error=0x%08X", pid,
			unsigned(thread != nullptr), threadId, threadError);
		if (!thread) { error = L"CreateRemoteThread 失败，错误码 " + std::to_wstring(threadError); return false; }
		argument.ThreadStarted();
		const DWORD wait = WaitForSingleObject(thread, 10000);
		const DWORD waitError = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
		// Only a signaled thread handle proves that nobody can still read the
		// parameter. A wait error leaves completion unknown, like a timeout.
		if (wait == WAIT_OBJECT_0) argument.ThreadStopped();
		DWORD exitCode = 0;
		const BOOL exitRead = GetExitCodeThread(thread, &exitCode);
		const DWORD exitError = exitRead ? ERROR_SUCCESS : GetLastError();
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=RemoteThread wait=0x%08X waitError=0x%08X exitRead=%u exit=0x%08X exitError=0x%08X elapsedMs=%llu", pid,
			wait, waitError, unsigned(exitRead != FALSE), exitCode, exitError,
			static_cast<unsigned long long>(GetTickCount64() - startedAt));
		const BOOL threadClosed = CloseHandle(thread);
		const DWORD closeError = threadClosed ? ERROR_SUCCESS : GetLastError();
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=CloseThread ok=%u error=0x%08X", pid,
			unsigned(threadClosed != FALSE), closeError);
		if (wait == WAIT_TIMEOUT) {
			error = L"远程加载线程 10 秒内未结束，原因尚未确认；参数内存已保留，避免线程继续读取时失效。";
			return false;
		}
		if (wait != WAIT_OBJECT_0) {
			error = L"无法确认远程加载线程是否结束，错误码 " + std::to_wstring(waitError) + L"；参数内存已保留。";
			return false;
		}
		const auto module = InjectionDiagnostics::InspectModule(process, path);
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=VerifyModule state=%s modules=%lu unreadablePaths=%lu error=0x%08X base=0x%llX path=%s", pid,
			InjectionDiagnostics::StateName(module.state), module.count, module.unreadablePaths, module.error,
			static_cast<unsigned long long>(module.base), module.path.empty() ? L"(unconfirmed)" : module.path.c_str());
		if (!exitRead) {
			error = L"远程线程已结束，但读取退出码失败，错误码 " + std::to_wstring(exitError) + L"；加载结果尚未确认。";
			return false;
		}
		if (InjectionDiagnostics::LoadConfirmed(wait, true, module)) {
			if (!exitCode) D5_LOG_WARN(L"[InjectDiag] pid=%lu zero thread exit DWORD, but full-path module is confirmed; x64 handle cannot be inferred from DWORD", pid);
			return true;
		}
		wchar_t rawCode[16]{}; _snwprintf_s(rawCode, _TRUNCATE, L"0x%08X", exitCode);
		if (module.state == InjectionDiagnostics::ModuleState::Unknown) {
			error = L"远程线程已结束（退出码 " + std::wstring(rawCode) + L"），但目标模块枚举不完整/不可访问，无法确认 DLL 是否加载（错误码 " +
				std::to_wstring(module.error) + L"）。";
		} else {
			error = L"远程线程已结束（退出码 " + std::wstring(rawCode) + L"），但模块列表未发现指定完整路径的 core DLL；非零退出码不代表加载成功。";
		}
		return false;
	};
	ok = runLoad();
	if (argument.Retained()) {
		D5_LOG_WARN(L"[InjectDiag] pid=%lu stage=RemoteArgument retained=true bytes=%llu reason=thread-completion-unconfirmed", pid,
			static_cast<unsigned long long>(bytes));
	} else {
		DWORD freeError = 0;
		const BOOL freed = argument.Release(freeError);
		D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=VirtualFreeEx ok=%u error=0x%08X", pid, unsigned(freed != FALSE), freeError);
	}
	D5_LOG_INFO(L"[InjectDiag] pid=%lu stage=result loadConfirmed=%u elapsedMs=%llu", pid, unsigned(ok),
		static_cast<unsigned long long>(GetTickCount64() - startedAt));
	return ok;
}

// 启动游戏并注入。
//
// 放在这里而不是留在 UI 宿主里，是为了让命令行注入器能跑**同一份**逻辑 ——
// 这段的关键在于"跳过启动壳"，而那件事只能靠真实进程验证，不能靠点按钮。
//
// 为什么不能注入了第一个同名进程就收工：Steam 游戏直接跑 exe 时，它会通过 Steam
// 重新拉起自己，第一个进程只是个活几百毫秒的壳。实测踩过 —— 注入成功 0.25 秒后
// 进程就退出了。所以要循环：挑一个还没试过的 pid，注入后确认它活着且真的发布了
// 状态块，否则继续等下一个。
struct LaunchOutcome {
	DWORD pid = 0;
	std::wstring exePath;
	std::wstring message;
	int shimsSkipped = 0;
	// 启动这一步的补充说明（比如"已设 SteamAppId，游戏不会经 Steam 重启"）。
	// **必须暴露给调用方打进日志** —— 它决定了这一局旁听能不能接上，而失败时的
	// 症状是"工具装上了、没有任何报错、画面毫无变化"，没这行只能靠猜。
	std::wstring note;
};

// waitForWindow = true 时**故意晚注入**：等游戏出现可见窗口、再多等两秒才挂上。
//
// 为什么需要这个选项：早注入的全部价值是真超分和原生深度。如果配置里 DLSS SR 是
// 关的（也就是"用游戏原生 DLSS，我们只做 DLSS5"这个组合），早注入没有任何收益，
// 却要在游戏初始化图形栈的同时去建我们的 D3D11 探测设备、和 Streamline 的
// interposer 抢同一批 DXGI 入口。
//
// 鬼武者实测：早注入时在游戏里切换 DLSS 会卡住画面；晚注入则 DLSS5 和游戏原生
// DLSS 都正常，游戏内改 DLSS 分辨率也正常。
// **让 Steam 游戏别再经 Steam 重启自己。**
//
// 这是"旁听接不上"那个竞态的根治。实测链条是这样的：
//   1. 游戏 exe 一起来就调 SteamAPI_RestartAppIfNecessary()
//      （鬼武者的 exe 里能直接搜到这个符号，而它**没有** CreateProcessW/A 的导入 ——
//       所以真身不是它自己拉起来的，是 Steam 拉的）
//   2. 该函数发现"我不是 Steam 启动的"，就请 Steam 重新拉一份，然后当前进程退出
//   3. Steam 是**另一个进程**，真身不是我们创建的 —— 挂起注入用不上，
//      只能退回"发现进程再注入"，那就是竞态。实测赢一次输一次：
//      注入时 98 个模块 -> 旁听接上 5 个钩子；125 个模块 -> 一个都没接上，
//      表现为"工具装上了但完全没效果"，而日志里几乎看不出区别。
//
// 而这个函数在环境变量里有 SteamAppId 时**直接返回 false**（不重启）——
// 那是 Steam 官方给开发者的口子。于是只要我们在子进程的环境里设上它，
// 就没有壳、没有转手，我们挂起创建的那个进程就是真身，注入 100% 赢。
//
// AppID 从 Steam 自己的清单里查，不用问用户：
// <库>\steamappsappmanifest_<id>.acf 里的 installdir 等于游戏目录名的那一个。
// 查不到就什么都不做（退回原来的行为），绝不猜一个 ID 塞进去。
inline std::wstring FindSteamAppId(const std::wstring& exePath) {
	std::error_code ec;
	const std::filesystem::path exe(exePath);
	// 期望形状：...\steamapps\common\<installdir>\...\game.exe
	std::filesystem::path dir = exe.parent_path();
	std::filesystem::path steamapps;
	std::wstring installDir;
	for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
		const std::filesystem::path parent = dir.parent_path();
		if (parent.filename() == L"common" &&
			parent.parent_path().filename() == L"steamapps") {
			installDir = dir.filename().wstring();
			steamapps = parent.parent_path();
			break;
		}
		if (parent == dir) break;
		dir = parent;
	}
	if (installDir.empty() || steamapps.empty()) return {};

	for (const auto& entry :
		std::filesystem::directory_iterator(steamapps, ec)) {
		if (ec) break;
		if (!entry.is_regular_file(ec)) continue;
		const std::wstring name = entry.path().filename().wstring();
		if (name.rfind(L"appmanifest_", 0) != 0) continue;
		if (entry.path().extension() != L".acf") continue;
		std::ifstream file(entry.path());
		if (!file) continue;
		std::string line;
		bool matches = false;
		while (std::getline(file, line)) {
			if (line.find("\"installdir\"") == std::string::npos) continue;
			// 形如: 	"installdir"		"OnimushaWotS_Demo"
			const size_t last = line.rfind('"');
			if (last == std::string::npos || last == 0) break;
			const size_t first = line.rfind('"', last - 1);
			if (first == std::string::npos) break;
			const std::string value = line.substr(first + 1, last - first - 1);
			std::wstring wide(value.begin(), value.end());
			matches = _wcsicmp(wide.c_str(), installDir.c_str()) == 0;
			break;
		}
		if (!matches) continue;
		// appmanifest_<id>.acf
		const std::wstring stem = entry.path().stem().wstring();
		const size_t underscore = stem.find(L'_');
		if (underscore == std::wstring::npos) continue;
		return stem.substr(underscore + 1);
	}
	return {};
}

// 拷一份当前环境，塞进 SteamAppId / SteamGameId。返回的块可以直接交给
// CreateProcessW（配 CREATE_UNICODE_ENVIRONMENT）。空 vector = 别改环境。
inline std::vector<wchar_t> EnvironmentWithSteamAppId(const std::wstring& appId) {
	std::vector<wchar_t> block;
	if (appId.empty()) return block;
	wchar_t* existing = GetEnvironmentStringsW();
	if (!existing) return block;
	for (const wchar_t* cursor = existing; *cursor;) {
		const size_t length = wcslen(cursor);
		// 已有的同名变量丢掉，用我们的值（避免出现两份）
		const bool drop = _wcsnicmp(cursor, L"SteamAppId=", 11) == 0 ||
			_wcsnicmp(cursor, L"SteamGameId=", 12) == 0;
		if (!drop) block.insert(block.end(), cursor, cursor + length + 1);
		cursor += length + 1;
	}
	FreeEnvironmentStringsW(existing);
	const std::wstring appVar = L"SteamAppId=" + appId;
	const std::wstring gameVar = L"SteamGameId=" + appId;
	block.insert(block.end(), appVar.c_str(), appVar.c_str() + appVar.size() + 1);
	block.insert(block.end(), gameVar.c_str(), gameVar.c_str() + gameVar.size() + 1);
	block.push_back(L'\0');   // 块结尾的第二个 NUL
	return block;
}

inline LaunchOutcome LaunchAndInject(
	const std::wstring& exePath,
	const std::wstring& args,
	const std::filesystem::path& coreDll,
	bool waitForWindow = false,
	int timeoutMs = 90000) {
	LaunchOutcome outcome;
	const std::wstring exeName =
		std::filesystem::path(exePath).filename().wstring();

	// **已经有一个注入好的实例在跑就别再开一个。**
	//
	// 这是实测踩出来的：提示语让人以为启动失败，于是再点一次 —— 第一个（挂起注入、
	// 最理想）的实例还活着，第二个只能靠"发现进程再注入"，旁听接不上。
	// 结果是**第二次点击把第一次的成功抹掉了**，而两个实例的日志各写一份，
	// 排查的人（我）还以为是同一次运行。
	{
		const DWORD existing = FindProcessIdByName(exeName);
		if (existing && IsCoreInjected(existing)) {
			outcome.pid = existing;
			outcome.exePath = ProcessImagePath(existing);
			outcome.message = L"这个游戏已经在跑，而且已经注入好了（pid " +
				std::to_wstring(existing) +
				L"）——**没有再开一个实例**。要重新来一次请先关掉游戏。";
			return outcome;
		}
	}

	const std::wstring commandLine = LaunchArguments::CommandLine(exePath, args);
	std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
	mutableCommandLine.push_back(L'\0');

	const std::wstring workingDir =
		std::filesystem::path(exePath).parent_path().wstring();
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	// 早注入时**挂起创建**：这样我们能在游戏一行代码都还没跑之前就注入。
	//
	// 为什么必须这样：实测鬼武者在进程起来的头几秒内就把 Streamline + NGX 全加载完了
	// （日志里"注入时进程里已有 _nvngx.dll / sl.interposer.dll / sl.dlss.dll"）。
	// 而普通注入要等它的加载器安静下来才敢动手（不然撞 loader lock，三次挂两次），
	// 那时已经晚了 —— 要旁听游戏的 DLSS 调用就再也插不进去。
	// 挂起创建把这个竞争彻底消掉：主线程还没执行过一条指令。
	//
	// 注意 CreateRemoteThread + LoadLibraryW 在挂起进程里是可行的：新线程启动时会自己
	// 跑 LdrInitializeThunk，把进程的加载器和 kernel32 初始化好，之后才执行我们的
	// 起始例程。所以 LoadLibraryW 的地址那时是有效的。
	const bool suspendedInject = !waitForWindow;

	// **把 SteamAppId 塞进子进程的环境，游戏就不会经 Steam 重启自己。**
	// 见 FindSteamAppId 上面那段 —— 这是"旁听接不上"那个竞态的根治：
	// 没有壳、没有转手，我们挂起创建的这个进程就是真身。
	const std::wstring steamAppId =
		suspendedInject ? FindSteamAppId(exePath) : std::wstring();
	std::vector<wchar_t> environment = EnvironmentWithSteamAppId(steamAppId);
	DWORD creationFlags = suspendedInject ? CREATE_SUSPENDED : 0;
	if (!environment.empty()) creationFlags |= CREATE_UNICODE_ENVIRONMENT;

	if (!CreateProcessW(exePath.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
		creationFlags,
		environment.empty() ? nullptr : environment.data(),
		workingDir.empty() ? nullptr : workingDir.c_str(),
		&startup, &process)) {
		outcome.message = L"启动失败，错误码 " +
			std::to_wstring(GetLastError()) + L"：" + exePath;
		return outcome;
	}

	// 我们亲手创建的那个进程。挂起注入只能对它做 —— Steam 那种"壳再拉起真身"的情况
	// 下真身不是我们创建的，只能退回下面的发现循环。
	const DWORD createdPid = process.dwProcessId;
	if (!steamAppId.empty()) {
		outcome.note = L"Steam 游戏 appid=" + steamAppId +
			L"（从 appmanifest 查到）：已在子进程环境里设 SteamAppId，"
			L"游戏不会再经 Steam 重启自己 —— 挂起注入这次一定生效";
	} else if (suspendedInject) {
		outcome.note = L"没查到 Steam appid（不在 steamapps\\common 下，"
			L"或清单里没有匹配项）：挂起注入照做，但游戏若自己重启，"
			L"真身仍然只能靠发现循环补注入";
	}
	bool suspendedInjected = false;
	std::wstring suspendedError;
	if (suspendedInject) {
		const EarlyInjectMarker marker(createdPid);

		// core 装完 hook 会 set 这个事件。必须等它再 resume：注入方在 LoadLibrary
		// 返回后就能往下走，而那时 core 的 InitThread 还在装 hook —— 游戏一旦先跑
		// 起来就可能抢先加载 Streamline，旁听它 DLSS 调用的链条就插不进去了。
		wchar_t readyName[128]{};
		_snwprintf_s(readyName, _TRUNCATE, L"%s.%lu",
			Ipc::READY_EVENT_BASE, createdPid);
		HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName);

		suspendedInjected = InjectCore(createdPid, coreDll, suspendedError);
		if (suspendedInjected && ready) {
			// 10 秒够了：core 装 hook 是毫秒级的，等这么久只是为了万一磁盘很慢。
			// 超时也照样 resume —— 挂着游戏比少一个功能糟糕得多。
			if (WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0) {
				suspendedError = L"core 没有在 10 秒内报告 hook 装好，"
					L"仍然放游戏继续跑（旁听功能可能没接上）";
			}
		}
		if (ready) CloseHandle(ready);

		// **无论注入成功还是失败都必须 resume**，否则游戏会永远挂在那里 ——
		// 我们宁可少一个功能，也不能把玩家的游戏卡死在启动瞬间。
		ResumeThread(process.hThread);
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);

	// 不预先判断"哪个才是游戏本体"，而是**给每个同名进程都注入**，然后看谁在渲染。
	//
	// 为什么不能用"发布了状态块"当判据：core 的状态块是在 InitThread 里发布的，
	// 和有没有渲染毫无关系 —— 启动壳只要加载了我们的 DLL 就会立刻发布。实测踩过：
	// 注入壳进程 0.2 秒后它就退出了，而我们已经认定成功了。
	// 唯一可靠的信号是 presentCount > 0。
	// 每个发现的同名进程都有自己的状态，整个循环**不阻塞** —— 所以在等 A 起窗口的
	// 同时，A 拉起来的 B 照样能被发现。上一版把等窗口写成嵌套的阻塞循环，结果启动壳
	// 只要不退出，就能把发现流程堵死 60 秒。
	struct Watch {
		DWORD pid = 0;
		int discoveredMs = 0;
		int windowSinceMs = -1;   // 第一次看到主窗口的时刻，-1 = 还没看到
		DWORD moduleCount = 0;    // 上次采样到的模块数
		int moduleStableMs = 0;   // 模块数连续多久没变
		bool injected = false;
		bool skipped = false;     // 已经死了，或注入失败 —— 不用再管
	};
	// 进程刚出生时 loader 还没初始化完，往里 CreateRemoteThread 调 LoadLibraryW
	// 可能死在 loader lock 上。
	constexpr int SPAWN_GRACE_MS = 300;
	// 窗口出现后再让它跑一会儿，确保 swapchain 和渲染资源都建完了。
	constexpr int WINDOW_SETTLE_MS = 2000;
	// 模块数连续这么久没变，才认为它的初始依赖加载完了，可以安全注入。
	// 见 WaitForLoaderQuiet 上面那段说明 —— 注入太早会把游戏挂死。
	constexpr int LOADER_QUIET_MS = 700;

	std::vector<Watch> watched;
	std::vector<DWORD> seen;   // FindProcessIdByName 的排除表
	// 早注入标记，一直持有到整个循环结束
	std::vector<std::unique_ptr<EarlyInjectMarker>> markers;
	std::wstring lastError;
	int injectedCount = 0;
	int aliveSince = -1;       // 唯一存活候选的观察时长，用于给渲染很慢的游戏兜底

	// 挂起阶段已经注入过的那个进程直接登记成"已注入"，别再走一遍发现+注入。
	// 它仍然要参加下面的 present 检查 —— 万一它是启动壳，我们还得换真身。
	if (suspendedInject) {
		seen.push_back(createdPid);
		Watch watch;
		watch.pid = createdPid;
		watch.injected = suspendedInjected;
		watch.skipped = !suspendedInjected;
		watched.push_back(watch);
		if (suspendedInjected) {
			++injectedCount;
			markers.push_back(std::make_unique<EarlyInjectMarker>(createdPid));
		} else {
			lastError = L"挂起注入失败：" + suspendedError;
		}
	}

	for (int elapsed = 0; elapsed < timeoutMs && !outcome.pid; elapsed += 50) {
		// (a) 发现新的同名进程
		const DWORD candidate = FindProcessIdByName(exeName, seen);
		if (candidate) {
			seen.push_back(candidate);
			Watch watch;
			watch.pid = candidate;
			watch.discoveredMs = elapsed;
			watched.push_back(watch);
		}

		// (b) 到点的就注入
		for (Watch& watch : watched) {
			if (watch.injected || watch.skipped) continue;
			if (!IsProcessAlive(watch.pid)) {
				watch.skipped = true;   // 短命的启动壳
				continue;
			}
			if (waitForWindow) {
				// 故意等它起了窗口再注入 —— 窗口出现意味着图形栈已经搭好，
				// 我们就不会和它的初始化撞车。启动壳没有窗口，自然被跳过。
				if (watch.windowSinceMs < 0) {
					if (!HasVisibleWindow(watch.pid)) continue;
					watch.windowSinceMs = elapsed;
				}
				if (elapsed - watch.windowSinceMs < WINDOW_SETTLE_MS) continue;
			} else if (elapsed - watch.discoveredMs < SPAWN_GRACE_MS) {
				continue;
			} else {
				// 等这个进程的加载器安静下来。**非阻塞**：每一跳采一次模块数，
				// 稳定够久了才注入。写成阻塞的 WaitForLoaderQuiet 会把发现流程
				// 一起停住 —— 而 Steam 启动壳拉起来的真进程正是要在这期间被发现的。
				const DWORD count = ModuleCount(watch.pid);
				if (!count || count != watch.moduleCount) {
					watch.moduleCount = count;
					watch.moduleStableMs = 0;
					continue;
				}
				watch.moduleStableMs += 50;
				if (watch.moduleStableMs < LOADER_QUIET_MS) continue;
			}

			// 只有"尽早"模式才声明早注入。晚注入模式本来就是等游戏跑起来才挂上，
			// 那时它的 swapchain 早建好了，core 必须走探测设备那条路。
			//
			// 标记要在注入前建、注入后再放：core 是在 InitThread 里读它的，
			// 那个线程可能比 InjectCore 返回还晚一点。整个循环结束前都留着，
			// 时间上绝对够。
			if (!waitForWindow) markers.push_back(
				std::make_unique<EarlyInjectMarker>(watch.pid));

			std::wstring error;
			if (InjectCore(watch.pid, coreDll, error)) {
				watch.injected = true;
				++injectedCount;
			} else {
				watch.skipped = true;
				lastError = error;
			}
		}

		// (c) 检查已注入的里面有谁真的在 present
		int aliveCount = 0;
		DWORD lastAlive = 0;
		for (const Watch& watch : watched) {
			if (!watch.injected || !IsProcessAlive(watch.pid)) continue;
			++aliveCount;
			lastAlive = watch.pid;

			StatusView probe;
			Ipc::Status status{};
			if (probe.Open(watch.pid) && probe.Read(status) &&
				status.presentCount > 0) {
				outcome.pid = watch.pid;
				break;
			}
		}

		if (outcome.pid) break;

		// 兜底：只有一个存活候选、而且已经观察了 20 秒还没 present（着色器编译、
		// 过场动画都可能这么久），那它基本就是游戏本体，先认下来。
		if (aliveCount == 1) {
			if (aliveSince < 0) aliveSince = elapsed;
			if (elapsed - aliveSince > 20000) {
				outcome.pid = lastAlive;
				// **这句话的措辞是有代价的。** 老版本写成"但它 20 秒内还没开始渲染"，
				// 读起来像失败，于是用户又点了一次启动 —— 开出第二个实例，
				// 而那个实例是"发现进程再注入"的，旁听接不上，功能就真的没了。
				// 实测就这么浪费掉一次完整的验证：第一个进程其实是最理想的早注入
				// （注入时 64 个模块、零个 NVIDIA 模块、钩子全接上）。
				// **提示语必须先说结论（成功），再说下一步（等），并且明确劝阻重试。**
				outcome.message = L"注入成功（pid " + std::to_wstring(lastAlive) +
					L"）。这个游戏加载慢，20 秒了还没出画面，属于正常 —— "
					L"**请耐心等它出图，不要重复点启动**：再点会开出第二个实例，"
					L"那个实例注入得晚，旁听接不上，功能反而用不了。";
				break;
			}
		} else {
			aliveSince = -1;
		}
		Sleep(50);
	}

	if (outcome.pid) {
		outcome.exePath = ProcessImagePath(outcome.pid);
		// 见过但没被选中的同名进程都是壳
		outcome.shimsSkipped = (int)seen.size() - 1;
		if (outcome.message.empty()) {
			// 说清楚是不是走到了最早的那条路 —— 只有挂起注入才能赶在游戏加载
			// Streamline/NGX 之前，而这决定了能不能旁听它的 DLSS 调用。
			const bool viaSuspended =
				suspendedInjected && outcome.pid == createdPid;
			outcome.message = L"已从工具启动并注入 pid " +
				std::to_wstring(outcome.pid) +
				(viaSuspended ? L"（挂起启动注入，早于游戏任何代码）"
					: L"（已确认在渲染）");
			if (outcome.shimsSkipped > 0) {
				outcome.message += L"，跳过了 " +
					std::to_wstring(outcome.shimsSkipped) + L" 个启动壳进程";
			}
			if (suspendedInject && !viaSuspended) {
				outcome.message += L"。注意：真身不是我们创建的那个进程"
					L"（启动壳转手），所以没能用上挂起注入。";
			}
		}
	} else if (seen.empty()) {
		outcome.message = L"游戏已启动，但超时内没找到进程 " + exeName +
			L"。如果是 Steam 游戏，确认配的是真正的游戏 exe 名。";
	} else if (injectedCount == 0) {
		// 晚注入模式最可能走到这里：进程找到了，但一直没出现主窗口
		outcome.message = L"找到了 " + std::to_wstring(seen.size()) +
			L" 个同名进程，但没有一个满足注入条件" +
			(waitForWindow ? L"（晚注入要等游戏出现主窗口，超时内没等到）。"
				L"可以改用「尽早」注入，或等游戏进去了用快捷键手动注入。"
				: L"。") +
			(lastError.empty() ? L"" : L"最后一次注入的错误：" + lastError);
	} else {
		outcome.message = L"给 " + std::to_wstring(injectedCount) +
			L" 个同名进程都注入过，但没有一个开始渲染。" +
			(lastError.empty() ? L"" : L"最后一次注入的错误：" + lastError);
	}
	return outcome;
}

}  // namespace DXL
