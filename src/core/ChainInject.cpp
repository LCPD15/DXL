#include "ChainInject.h"
#include "ChildProcessPolicy.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../common/IpcProtocol.h"
#include "../common/Log.h"

namespace DXL {

namespace {

using CreateProcessWFn = BOOL(WINAPI*)(
	LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
	LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
using CreateProcessAFn = BOOL(WINAPI*)(
	LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
	LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

CreateProcessWFn g_originalW = nullptr;
CreateProcessAFn g_originalA = nullptr;
wchar_t g_selfDllPath[MAX_PATH]{};
std::atomic<bool> g_active{ false };
std::atomic<unsigned int> g_hooks{ 0 };
std::atomic<unsigned int> g_injected{ 0 };

// 链式注入的次数上限。正常情况下 1~2 层就到真身了；给到 4 是留余量，
// 同时挡住病态情况（比如某个进程反复自我重启）无限展开。
constexpr unsigned int MAX_CHAIN = 4;

// 在模块的导入表里找某个 API 的 IAT 槽位。
//
// 改 IAT 只影响**这一个模块**的调用，比 inline hook 安全得多：进程里别人调
// CreateProcessW 不受影响，也不需要反汇编函数序言。
// CreateProcess 可能从 kernel32 导入，也可能走 api-set 转发，所以库名要多认几个。
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
		if (_strnicmp(library, "KERNEL32", 8) != 0 &&
			_strnicmp(library, "api-ms-win-core-processthreads", 30) != 0) {
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
			if (nameRva >= imageSize) break;
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

bool WriteSlot(void** slot, void* value, void** previous) noexcept {
	DWORD oldProtect = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
		return false;
	}
	if (previous) *previous = *slot;
	*slot = value;
	VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
	return true;
}

// 位数必须一致。往 32 位进程里塞 64 位 DLL 只会让它起不来 —— 那比少一个功能糟糕。
bool SameBitness(HANDLE process) noexcept {
	BOOL childWow = FALSE;
	BOOL selfWow = FALSE;
	if (!IsWow64Process(process, &childWow)) return false;
	if (!IsWow64Process(GetCurrentProcess(), &selfWow)) return false;
	return childWow == selfWow;
}

// 往子进程里注入同一个 core DLL。用最朴素的
// VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryW)。
//
// 子进程此刻是**挂起**的：主线程一条指令都没跑，所以我们一定赢在游戏加载
// Streamline / NGX 之前 —— 这正是整件事的目的。
// 挂起进程里 CreateRemoteThread 是可行的：新线程启动时会自己跑 LdrInitializeThunk
// 把加载器初始化好，之后才执行起始例程，所以 LoadLibraryW 的地址那时是有效的。
bool InjectInto(HANDLE process, DWORD pid) noexcept {
	if (!g_selfDllPath[0]) return false;
	const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	const auto loadLibrary = kernel32
		? reinterpret_cast<LPTHREAD_START_ROUTINE>(
			GetProcAddress(kernel32, "LoadLibraryW"))
		: nullptr;
	if (!loadLibrary) return false;

	const SIZE_T bytes = (wcslen(g_selfDllPath) + 1) * sizeof(wchar_t);
	void* remote = VirtualAllocEx(process, nullptr, bytes,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remote) return false;

	bool ok = false;
	if (WriteProcessMemory(process, remote, g_selfDllPath, bytes, nullptr)) {
		const HANDLE thread = CreateRemoteThread(process, nullptr, 0,
			loadLibrary, remote, 0, nullptr);
		if (thread) {
			// 等 LoadLibrary 返回。它只是把 DLL 映射进去并跑 DllMain（DllMain 里
			// 我们只起一个线程就返回），所以这一步很快。
			ok = WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0;
			CloseHandle(thread);
		}
	}
	// **不释放 remote**：LoadLibraryW 已经读完了，但释放要再等一次同步，
	// 而这几十字节的代价可以忽略。挂起注入的路径上，少一次跨进程操作就少一个出错面。
	(void)pid;
	return ok;
}

// CreateProcess 的公共处理。addedSuspend = 我们自己加的 CREATE_SUSPENDED。
void HandleChild(
	const PROCESS_INFORMATION& info,
	bool addedSuspend,
	const wchar_t* what) noexcept {
	// 无论下面成不成，最后都要保证子进程能跑起来
	bool resumed = false;
	const unsigned int already = g_injected.load(std::memory_order_relaxed);
	do {
		// Query the actual created image rather than guessing from an optional
		// application name or quoted command line. A Vulkan game may never pass
		// the D3D graphics handoff gate, but its crash reporter still must run
		// normally without inheriting our injected graphics core.
		wchar_t childPath[32768]{};
		DWORD childPathSize = DWORD(_countof(childPath));
		if (QueryFullProcessImageNameW(info.hProcess, 0, childPath, &childPathSize) &&
			IsDedicatedCrashReporter(std::wstring_view(childPath, childPathSize))) {
			D5_LOG_INFO(L"Chain injection skipped crash reporter: pid=%lu path=%s",
				info.dwProcessId, childPath);
			break;
		}
		if (already >= MAX_CHAIN) {
			D5_LOG_WARN(L"链式注入：已经注过 %u 次，不再往下（防止病态展开）", already);
			break;
		}
		if (!SameBitness(info.hProcess)) {
			D5_LOG_INFO(L"链式注入：子进程 pid=%lu 位数和我们不同，放过",
				info.dwProcessId);
			break;
		}
		// 让子进程的 core 知道"这是早注入"，行为要和 UI 的挂起注入一致。
		wchar_t markerName[128]{};
		_snwprintf_s(markerName, _TRUNCATE, L"%s.%lu",
			Ipc::EARLY_INJECT_EVENT_BASE, info.dwProcessId);
		const HANDLE marker = CreateEventW(nullptr, TRUE, TRUE, markerName);

		wchar_t readyName[128]{};
		_snwprintf_s(readyName, _TRUNCATE, L"%s.%lu",
			Ipc::READY_EVENT_BASE, info.dwProcessId);
		const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName);

		const bool injected = InjectInto(info.hProcess, info.dwProcessId);
		if (injected && ready) {
			// 等它报告"钩子装好了"再放行。**这一步才是整件事的关键** ——
			// LoadLibrary 返回不等于 hook 装好，中间那段时间游戏可能已经
			// 把 Streamline 加载完了，那就又变成了原来那个竞态。
			if (WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0) {
				D5_LOG_WARN(L"链式注入：pid=%lu 的 core 没在 10 秒内报告 hook 装好，"
					L"照样放它跑", info.dwProcessId);
			}
		}
		if (injected) {
			g_injected.fetch_add(1, std::memory_order_relaxed);
			D5_LOG_INFO(L"**链式注入成功**：%s 拉起的 pid=%lu 已经在它跑第一条指令"
				L"之前注入好了（这就是旁听接不上那个竞态的根治）",
				what, info.dwProcessId);
		} else {
			D5_LOG_WARN(L"链式注入：往 pid=%lu 注入失败，它会照常运行（少一个功能）",
				info.dwProcessId);
		}
		if (ready) CloseHandle(ready);
		// marker 故意**不关**：子进程的 core 起来之后要能查到它。
		// 进程退出时系统自然回收。
		(void)marker;
	} while (false);

	if (addedSuspend) {
		ResumeThread(info.hThread);
		resumed = true;
	}
	(void)resumed;
}

BOOL WINAPI CreateProcessWHook(
	LPCWSTR applicationName, LPWSTR commandLine,
	LPSECURITY_ATTRIBUTES processAttributes,
	LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles,
	DWORD creationFlags, LPVOID environment, LPCWSTR currentDirectory,
	LPSTARTUPINFOW startupInfo, LPPROCESS_INFORMATION processInformation) {
	if (!g_originalW) return FALSE;
	const bool takeOver = g_active.load(std::memory_order_relaxed) &&
		processInformation != nullptr;
	const bool addSuspend = takeOver && !(creationFlags & CREATE_SUSPENDED);
	const BOOL result = g_originalW(applicationName, commandLine,
		processAttributes, threadAttributes, inheritHandles,
		addSuspend ? (creationFlags | CREATE_SUSPENDED) : creationFlags,
		environment, currentDirectory, startupInfo, processInformation);
	if (!result || !takeOver) return result;
	D5_LOG_INFO(L"链式注入：本进程创建了子进程 pid=%lu（%s）",
		processInformation->dwProcessId,
		commandLine ? commandLine : (applicationName ? applicationName : L"?"));
	HandleChild(*processInformation, addSuspend, L"CreateProcessW");
	return result;
}

BOOL WINAPI CreateProcessAHook(
	LPCSTR applicationName, LPSTR commandLine,
	LPSECURITY_ATTRIBUTES processAttributes,
	LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles,
	DWORD creationFlags, LPVOID environment, LPCSTR currentDirectory,
	LPSTARTUPINFOA startupInfo, LPPROCESS_INFORMATION processInformation) {
	if (!g_originalA) return FALSE;
	const bool takeOver = g_active.load(std::memory_order_relaxed) &&
		processInformation != nullptr;
	const bool addSuspend = takeOver && !(creationFlags & CREATE_SUSPENDED);
	const BOOL result = g_originalA(applicationName, commandLine,
		processAttributes, threadAttributes, inheritHandles,
		addSuspend ? (creationFlags | CREATE_SUSPENDED) : creationFlags,
		environment, currentDirectory, startupInfo, processInformation);
	if (!result || !takeOver) return result;
	D5_LOG_INFO(L"链式注入：本进程创建了子进程 pid=%lu（%hs）",
		processInformation->dwProcessId,
		commandLine ? commandLine : (applicationName ? applicationName : "?"));
	HandleChild(*processInformation, addSuspend, L"CreateProcessA");
	return result;
}

}  // namespace

void InstallChainInject(HMODULE selfModule) noexcept {
	static bool installed = false;
	if (installed) return;
	installed = true;

	if (!GetModuleFileNameW(selfModule, g_selfDllPath, MAX_PATH)) {
		D5_LOG_WARN(L"链式注入：拿不到自己的 DLL 路径，装不了");
		return;
	}

	// 只补**主可执行文件**的导入表：启动壳自己调 CreateProcess 拉真身，
	// 而我们不想去动进程里其他模块（那些调用和游戏本体无关）。
	const HMODULE exe = GetModuleHandleW(nullptr);
	unsigned int hooks = 0;
	if (void** slot = FindImportSlot(exe, "CreateProcessW")) {
		void* previous = nullptr;
		if (WriteSlot(slot, reinterpret_cast<void*>(&CreateProcessWHook),
			&previous)) {
			g_originalW = reinterpret_cast<CreateProcessWFn>(previous);
			++hooks;
		}
	}
	if (void** slot = FindImportSlot(exe, "CreateProcessA")) {
		void* previous = nullptr;
		if (WriteSlot(slot, reinterpret_cast<void*>(&CreateProcessAHook),
			&previous)) {
			g_originalA = reinterpret_cast<CreateProcessAFn>(previous);
			++hooks;
		}
	}
	g_hooks.store(hooks, std::memory_order_relaxed);
	g_active.store(hooks > 0, std::memory_order_relaxed);

	if (hooks) {
		D5_LOG_INFO(L"链式注入已装（%u 个钩子）—— 本进程拉起的子进程会在它跑第一条"
			L"指令之前被注入。启动壳转手那个竞态到此为止。", hooks);
	} else {
		// **这一条必须打出来。** 主 exe 没有静态导入 CreateProcess，说明真身不是
		// 它用 CreateProcess 拉起来的（可能走 Steam、ShellExecute 或别的路径）——
		// 那这条修法在这个游戏上就不适用，得换别的办法，而不是以为已经修好了。
		D5_LOG_WARN(L"链式注入：主 exe 的导入表里没有 CreateProcessW/A —— "
			L"它拉起真身不走这条路（可能经由 Steam 或 ShellExecute）。"
			L"这一局仍然靠「发现进程再注入」，旁听可能接不上。");
	}
}

void ChainInjectStopAfterGraphics() noexcept {
	if (g_active.exchange(false, std::memory_order_relaxed)) {
		D5_LOG_INFO(L"链式注入：本进程已经接管图形，之后不再注入它拉起的子进程");
	}
}

unsigned int ChainInjectHooks() noexcept {
	return g_hooks.load(std::memory_order_relaxed);
}

unsigned int ChainInjectCount() noexcept {
	return g_injected.load(std::memory_order_relaxed);
}

}  // namespace DXL
