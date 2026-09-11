// DXL 注入器。
//
//   DXL-inject.exe --pid 1234
//   DXL-inject.exe --exe Game.exe
//
// 标准的 LoadLibrary 远程线程注入：在目标进程里分配一段内存写入 DLL 路径，
// 然后 CreateRemoteThread 调用 LoadLibraryW。够用且不需要任何第三方库。
//
// 位数必须匹配：64 位注入器只能注入 64 位进程。NGX 本身也只有 x64，所以这个
// 限制无所谓 —— 32 位游戏将来要走独立的 helper 进程方案（和 DLSS5-Feeder 一样）。

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <filesystem>
#include <string>
#include <algorithm>
#include <vector>

#include "../common/IpcClient.h"
// 复用 UI 宿主那边的启动+注入逻辑：跳过启动壳这件事只能靠真实进程验证，
// 所以命令行必须能跑同一份代码。
#include "../ui/GameSession.h"
#include "../ui/StartupReport.h"

#pragma comment(lib, "psapi.lib")

namespace {

// 必须做这一步，否则本文件里所有带中文的 wprintf 都会**静默失效**：默认的 "C"
// locale 无法把 >0x7F 的宽字符转成多字节，wprintf 返回 -1 并给流置上错误位，
// 之后连纯 ASCII 的输出也一起没了。_O_U8TEXT 让 CRT 直接按 UTF-8 写，
// 重定向到文件和输出到控制台都正常。
void EnableUnicodeStdout() {
	_setmode(_fileno(stdout), _O_U8TEXT);
	_setmode(_fileno(stderr), _O_U8TEXT);
}

void PrintUsage() {
	wprintf(L"usage:\n"
		L"  DXL-inject.exe --pid <pid>          注入\n"
		L"  DXL-inject.exe --exe <name.exe>     按进程名注入\n"
		L"  DXL-inject.exe --list               列出候选进程\n"
		L"  DXL-inject.exe --wait <name.exe>    等进程出现并立刻注入\n"
		L"  DXL-inject.exe --launch <exe> [参数] [late]  启动并注入\n"
		L"      late = 等游戏起窗口后再注入（和游戏原生 DLSS 共存时必须这样）\n"
		L"  DXL-inject.exe --reload <pid>       让 core 重读 settings.json\n"
		L"  DXL-inject.exe --status <pid>       打印 core 的状态块\n"
		L"  DXL-inject.exe --deploy <exe> [名字] 部署代理壳（不注入，见 --deploy help）\n"
		L"      名字 = d3d12|d3d11|dxgi|xinput1_4 手动指定；省略 = 读导入表自动选\n"
		L"  DXL-inject.exe --undeploy <exe>     撤掉我们部署的壳\n");
}

// 状态块的可读输出。和 UI 看到的是同一份数据，方便对照排查。
void PrintStatus(DWORD pid) {
	DXL::StatusView view;
	if (!view.Open(pid)) {
		wprintf(L"打不开 pid %lu 的状态块 —— core 没注入，或进程已退出。\n", pid);
		return;
	}
	DXL::Ipc::Status status{};
	if (!view.Read(status)) {
		wprintf(L"状态块存在但读不到一份自洽的快照。\n");
		return;
	}
	auto stateName = [](uint32_t value) -> const wchar_t* {
		using DXL::Ipc::FeatureState;
		switch (static_cast<FeatureState>(value)) {
		case FeatureState::Disabled:    return L"已关闭";
		case FeatureState::Standby:     return L"待生效";
		case FeatureState::Active:      return L"生效中";
		case FeatureState::Failed:      return L"失败";
		case FeatureState::Unavailable: break;
		}
		return L"不可用";
	};
	wprintf(L"pid %lu  api=%u  hooked=%u\n", status.gamePid, status.api,
		status.hooked);
	wprintf(L"  DLSS SR   %s\n", stateName(status.srState));
	wprintf(L"  DLSS5/NR  %s\n", stateName(status.nrState));
	wprintf(L"  补帧      %s\n", stateName(status.fgState));
	wprintf(L"  分辨率    %ux%u -> %ux%u\n", status.renderWidth,
		status.renderHeight, status.outputWidth, status.outputHeight);
	wprintf(L"  present   %llu  帧时间 %.2f ms\n",
		(unsigned long long)status.presentCount, status.frameMs);
	wprintf(L"  evaluate  SR %llu (失败 %llu)   NR %llu (失败 %llu)\n",
		(unsigned long long)status.evaluateCount,
		(unsigned long long)status.evaluateFailures,
		(unsigned long long)status.nrEvaluateCount,
		(unsigned long long)status.nrEvaluateFailures);
	wprintf(L"  旁听      模块 %u  NGX解析 %u  抄到帧 %llu",
		status.eavesdropModules, status.eavesdropLookups,
		(unsigned long long)status.eavesdropFrames);
	if (status.eavesdropFrames) {
		wprintf(L"  子矩形 %ux%u  深度 %s  矢量 %s  jitter(%.4f, %.4f)",
			status.eavesdropRenderWidth, status.eavesdropRenderHeight,
			status.eavesdropHasDepth ? L"有" : L"无",
			status.eavesdropHasMotion ? L"有" : L"无",
			status.eavesdropJitterX, status.eavesdropJitterY);
	}
	wprintf(L"\n");
	// message 是 UTF-8，不能直接用 %hs —— 那会按当前 ANSI 代码页解释，
	// 中文会变成乱码
	wchar_t message[256]{};
	MultiByteToWideChar(CP_UTF8, 0, status.message, -1, message, 256);
	wprintf(L"  消息      %s\n", message);
}

std::filesystem::path ExeDir() {
	wchar_t buffer[MAX_PATH]{};
	GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	return std::filesystem::path(buffer).parent_path();
}

bool IsWow64Process64(HANDLE process, bool& is32Bit) {
	BOOL wow64 = FALSE;
	if (!IsWow64Process(process, &wow64)) return false;
	is32Bit = wow64 != FALSE;   // 在 64 位 Windows 上，WOW64 == 32 位进程
	return true;
}

DWORD FindProcessByName(const wchar_t* name) {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return 0;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	DWORD found = 0;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szExeFile, name) == 0) {
				found = entry.th32ProcessID;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return found;
}

// 列出有可见窗口的进程，方便挑目标
void ListCandidates() {
	struct Ctx {
		int printed = 0;
	} ctx;
	auto callback = [](HWND hwnd, LPARAM param) -> BOOL {
		auto* ctx = reinterpret_cast<Ctx*>(param);
		if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
		wchar_t title[256]{};
		if (!GetWindowTextW(hwnd, title, 256) || !title[0]) return TRUE;
		RECT rect{};
		GetClientRect(hwnd, &rect);
		if (rect.right - rect.left < 320 || rect.bottom - rect.top < 240) return TRUE;

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		HANDLE process = OpenProcess(
			PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		wchar_t exe[MAX_PATH] = L"?";
		bool is32 = false;
		if (process) {
			DWORD size = MAX_PATH;
			QueryFullProcessImageNameW(process, 0, exe, &size);
			IsWow64Process64(process, is32);
			CloseHandle(process);
		}
		wprintf(L"  pid=%-6lu %-5s %-38.38s  %s\n", pid, is32 ? L"x86" : L"x64",
			std::filesystem::path(exe).filename().c_str(), title);
		++ctx->printed;
		return TRUE;
	};
	wprintf(L"有可见窗口的进程：\n");
	EnumWindows(callback, reinterpret_cast<LPARAM>(&ctx));
	if (!ctx.printed) wprintf(L"  （没找到）\n");
}

bool Inject(DWORD pid, const std::filesystem::path& dllPath) {
    DXL::Log::Get().Open(L"inject");
    std::wstring error;
    const bool loaded = DXL::InjectCore(pid, dllPath, error);
    DXL::StatusView status;
    if (loaded) {
        for (int attempt = 0; attempt < 40 && !status.Open(pid); ++attempt) Sleep(50);
    }
    const std::wstring report = DXL::StartupReport(pid,
        status.IsOpen() ? L"cli-connected" : L"cli-unconfirmed");
    D5_LOG_INFO(L"%s", report.c_str());
    wprintf(L"%s\n", report.c_str());
    if (!loaded) {
        wprintf(L"Load not confirmed: %s\n", error.c_str());
        return false;
    }
    wprintf(status.IsOpen() ? L"Core status channel connected: pid=%lu\n"
        : L"Core DLL found, but initialization is not confirmed: pid=%lu\n", pid);
    return status.IsOpen();
}

// ===================== 部署模式（#83） =====================
// 代理壳拷进游戏目录 + d5q-deploy.txt 指回工具目录的 core。游戏自己加载壳
//（应用目录优先于 System32），壳在首次转发调用时拉起 core —— 全程没有
// 任何注入 API，拦注入的游戏拦不到。壳的机制见 src\shell\proxy_shell.cpp 头注释。

// 游戏 EXE 的静态导入表（小写 dll 名）。部署器只支持 x64（PE32+，32 位游戏
// 本来就拒绝）。只看静态导入：UE/Unity 的 d3d12/d3d11/dxgi 基本都静态导入；
// 纯动态加载（DelayLoad/LoadLibrary）的引擎查不到 —— 那时 --deploy <exe> <名>。
// **纯文件解析，不 LoadLibrary**：DONT_RESOLVE_DLL_REFERENCES / AS_IMAGE_RESOURCE
// 在新加载器上都不稳（EXE 直接 126；AS_IMAGE_RESOURCE 句柄低位是标志位，
// 当指针解引用就 AV）。EXE 又不是 API-set 桩——导入表必然在节里，
// 节表换算 RVA→文件偏移完全可靠。头+节表几 KB 一次读完，导入名逐个 fseek。
std::vector<std::wstring> ExeImports(const wchar_t* exePath) {
	std::vector<std::wstring> names;
	FILE* f = _wfsopen(exePath, L"rb", _SH_DENYNO);
	if (!f) return names;
	auto readAt = [&](long offset, void* buffer, size_t bytes) -> bool {
		return fseek(f, offset, SEEK_SET) == 0 &&
			fread(buffer, 1, bytes, f) == bytes;
	};
	IMAGE_DOS_HEADER dos{};
	if (!readAt(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
		fclose(f);
		return names;
	}
	IMAGE_NT_HEADERS64 nt{};
	if (!readAt(long(dos.e_lfanew), &nt, sizeof(nt)) ||
			nt.Signature != IMAGE_NT_SIGNATURE ||
			nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		fclose(f);   // 32 位 / 损坏：拒绝（NGX 只有 x64，和注入同一道门）
		return names;
	}
	std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
	if (!sections.empty() &&
			!readAt(long(dos.e_lfanew) + long(sizeof(IMAGE_NT_HEADERS64)),
				sections.data(),
				sizeof(IMAGE_SECTION_HEADER) * sections.size())) {
		fclose(f);
		return names;
	}
	// RVA -> 文件偏移。导入表在节里（EXE 不是 API-set 桩），宽松取
	// max(Misc.VirtualSize, SizeOfRawData) 当节界足够。
	//（VirtualSize 在 Misc 联合体里 —— 直接 .VirtualSize 编译不过）
	auto rvaToOff = [&](uint32_t rva) -> long {
		for (const auto& s : sections) {
			const uint32_t size = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
			if (rva >= s.VirtualAddress && rva < s.VirtualAddress + size) {
				return long(s.PointerToRawData + (rva - s.VirtualAddress));
			}
		}
		return -1;
	};
	const uint32_t dirRva =
		nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	const long dirOff = dirRva ? rvaToOff(dirRva) : -1;
	if (dirOff >= 0) {
		for (uint32_t i = 0; ; ++i) {
			IMAGE_IMPORT_DESCRIPTOR desc{};
			if (!readAt(dirOff + long(sizeof(desc) * i), &desc, sizeof(desc)) ||
					!desc.Name) {
				break;
			}
			const long nameOff = rvaToOff(desc.Name);
			if (nameOff < 0) continue;
			char raw[MAX_PATH]{};
			fseek(f, nameOff, SEEK_SET);
			size_t n = fread(raw, 1, sizeof(raw) - 1, f);
			raw[n] = '\0';
			wchar_t wide[MAX_PATH]{};
			MultiByteToWideChar(CP_ACP, 0, raw, -1, wide, MAX_PATH);
			_wcslwr_s(wide);
			if (wide[0]) names.push_back(wide);
		}
	}
	fclose(f);
	return names;
}

// 逐字节比对（壳 ~139KB，一次读进内存比完）。升级场景已不用它认主
//（D5QShellMarker 认，任意版本都是我们的）；留着只给"完全同版"做快速路径。
bool FilesIdentical(const std::wstring& a, const std::wstring& b) {
	FILE* fa = _wfsopen(a.c_str(), L"rb", _SH_DENYNO);
	FILE* fb = _wfsopen(b.c_str(), L"rb", _SH_DENYNO);
	if (!fa || !fb) {
		if (fa) fclose(fa);
		if (fb) fclose(fb);
		return false;
	}
	bool same = true;
	char ba[65536], bb[65536];
	size_t ra = 0, rb = 0;
	do {
		ra = fread(ba, 1, sizeof(ba), fa);
		rb = fread(bb, 1, sizeof(bb), fb);
		if (ra != rb || memcmp(ba, bb, ra) != 0) same = false;
	} while (same && ra == sizeof(ba));
	fclose(fa);
	fclose(fb);
	return same;
}

// dll 的导出表里有没有 D5QShellMarker（壳 cpp 导出的识别符号）。
// 纯文件解析（ExeImports 同款节表换算）—— 不 LoadLibrary 游戏目录里的
// 陌生 dll：那是别人可能放的东西，映射它就是执行面。
bool HasShellMarker(const wchar_t* dllPath) {
	FILE* f = _wfsopen(dllPath, L"rb", _SH_DENYNO);
	if (!f) return false;
	auto readAt = [&](long offset, void* buffer, size_t bytes) -> bool {
		return fseek(f, offset, SEEK_SET) == 0 &&
			fread(buffer, 1, bytes, f) == bytes;
	};
	IMAGE_DOS_HEADER dos{};
	if (!readAt(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
		fclose(f);
		return false;
	}
	IMAGE_NT_HEADERS64 nt{};
	if (!readAt(long(dos.e_lfanew), &nt, sizeof(nt)) ||
			nt.Signature != IMAGE_NT_SIGNATURE ||
			nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		fclose(f);
		return false;
	}
	std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
	if (!sections.empty() &&
			!readAt(long(dos.e_lfanew) + long(sizeof(IMAGE_NT_HEADERS64)),
				sections.data(),
				sizeof(IMAGE_SECTION_HEADER) * sections.size())) {
		fclose(f);
		return false;
	}
	auto rvaToOff = [&](uint32_t rva) -> long {
		for (const auto& s : sections) {
			const uint32_t size = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
			if (rva >= s.VirtualAddress && rva < s.VirtualAddress + size) {
				return long(s.PointerToRawData + (rva - s.VirtualAddress));
			}
		}
		return -1;
	};
	bool found = false;
	const uint32_t dirRva =
		nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	const long dirOff = dirRva ? rvaToOff(dirRva) : -1;
	if (dirOff >= 0) {
		IMAGE_EXPORT_DIRECTORY exp{};
		if (readAt(dirOff, &exp, sizeof(exp)) && exp.AddressOfNames) {
			const long namesOff = rvaToOff(exp.AddressOfNames);
			if (namesOff >= 0) {
				for (uint32_t i = 0; i < exp.NumberOfNames && !found; ++i) {
					uint32_t nameRva = 0;
					if (!readAt(namesOff + long(4 * i), &nameRva, 4)) break;
					const long nameOff = rvaToOff(nameRva);
					if (nameOff < 0) continue;
					char raw[32]{};
					fseek(f, nameOff, SEEK_SET);
					const size_t n = fread(raw, 1, sizeof(raw) - 1, f);
					raw[n] = '\0';
					found = strcmp(raw, "D5QShellMarker") == 0;
				}
			}
		}
	}
	fclose(f);
	return found;
}

// 0 = 空位或我们的壳（**任意版本**，可覆盖可卸）；1 = 别人的（跳过/拒覆盖）；
// 2 = 我们没这份壳。认主看 D5QShellMarker（导出符号）—— 逐字节比对只能认
// 当前版本，升级场景（旧壳留游戏目录、build 已是新版）会把我们自己的旧壳
// 当别人，拒删拒覆盖 → 新壳永远部署不进去（v0 那局就是这么死的）。
// 完全同版走 FilesIdentical 快速路径免解析。
int SlotState(const std::filesystem::path& gameDir,
		const std::filesystem::path& shellDir, const wchar_t* name) {
	const std::wstring file = std::wstring(name) + L".dll";
	const std::filesystem::path src = shellDir / file;
	if (!std::filesystem::exists(src)) return 2;
	const std::filesystem::path dst = gameDir / file;
	if (!std::filesystem::exists(dst)) return 0;
	if (FilesIdentical(dst.wstring(), src.wstring())) return 0;
	return HasShellMarker(dst.c_str()) ? 0 : 1;
}

int Deploy(const wchar_t* exeArg, const wchar_t* forceName) {
	const std::filesystem::path exe(exeArg);
	if (!std::filesystem::exists(exe)) {
		wprintf(L"找不到 %s\n", exeArg);
		return 1;
	}
	const std::filesystem::path gameDir = exe.parent_path();
	const std::filesystem::path shellDir = ExeDir() / L"shell";
	const std::filesystem::path corePath = ExeDir() / L"DXL-core.dll";
	if (!std::filesystem::exists(corePath)) {
		wprintf(L"找不到 %s（部署模式的 core 仍留在工具目录）\n", corePath.c_str());
		return 1;
	}

	const std::vector<std::wstring> imports = ExeImports(exe.c_str());
	// 优先序 = 加载时机序：d3d12/d3d11（设备创建，必然在 swapchain 之前）→
	// dxgi（进场更早，但 ReShade 默认占它——被占就自动跳过）→ xinput1_4（垫底，
	// 纯动态加载 XInput 的引擎可能根本不加载它）。导入表是权威依据，
	// 不赌"这游戏大概会加载 xxx"。
	const wchar_t* kCandidates[] = { L"d3d12", L"d3d11", L"dxgi", L"xinput1_4" };

	const wchar_t* chosen = nullptr;
	const wchar_t* why = nullptr;
	if (forceName) {
		chosen = forceName;
		why = L"--name 指定";
	} else {
		// 导入表里存的是带扩展名的 "d3d12.dll" —— 候选名是裸名，比较补 .dll。
		const auto isImported = [&](const wchar_t* name) {
			return std::find(imports.begin(), imports.end(),
				std::wstring(name) + L".dll") != imports.end();
		};
		for (const wchar_t* name : kCandidates) {
			if (SlotState(gameDir, shellDir, name) == 2) continue;
			if (!isImported(name)) continue;
			if (SlotState(gameDir, shellDir, name) == 1) {
				wprintf(L"  跳过 %s.dll：导入表命中但位置被别人占着（ReShade？）\n", name);
				continue;
			}
			chosen = name;
			why = L"导入表命中";
			break;
		}
		if (!chosen) {
			wprintf(L"没有自动选出代理名。游戏导入表：\n");
			for (const auto& dll : imports) wprintf(L"  %s\n", dll.c_str());
			wprintf(L"各候选状态：\n");
			const auto isImported2 = [&](const wchar_t* name) {
				return std::find(imports.begin(), imports.end(),
					std::wstring(name) + L".dll") != imports.end();
			};
			for (const wchar_t* name : kCandidates) {
				const int slot = SlotState(gameDir, shellDir, name);
				wprintf(L"  %-10s 导入:%ls  位置:%ls\n", name,
					isImported2(name) ? L"是" : L"否",
					slot == 0 ? L"空/我们的" : slot == 1 ? L"别人占" : L"没这份壳");
			}
			wprintf(L"用 --deploy <exe> <d3d12|d3d11|dxgi|xinput1_4> 手动指定，"
				L"或这游戏回退注入模式。\n");
			return 1;
		}
	}
	// 强制指定时：位置被别人占 = 拒绝（绝不动别人的文件）
	const int slot = SlotState(gameDir, shellDir, chosen);
	if (slot == 2) {
		wprintf(L"没有 %s 这份壳（build\\shell\\%s.dll）\n", chosen, chosen);
		return 1;
	}
	if (slot == 1) {
		wprintf(L"%s.dll 已被别人占用 —— 不覆盖。换一个名字（dxgi 常被 ReShade 占）。\n",
			chosen);
		return 1;
	}

	// 拷壳 + 写 marker（marker 是我们的文件，随便覆盖）
	const std::wstring dllName = std::wstring(chosen) + L".dll";
	std::error_code ec;
	std::filesystem::copy_file(shellDir / dllName, gameDir / dllName,
		std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		wprintf(L"拷壳失败: %hs（游戏在跑？关了再部署）\n", ec.message().c_str());
		return 1;
	}
	char coreUtf8[MAX_PATH * 3]{};
	WideCharToMultiByte(CP_UTF8, 0, corePath.wstring().c_str(), -1,
		coreUtf8, sizeof(coreUtf8), nullptr, nullptr);
	FILE* marker = _wfsopen((gameDir / L"d5q-deploy.txt").c_str(), L"wb", _SH_DENYNO);
	if (!marker) {
		wprintf(L"写 d5q-deploy.txt 失败\n");
		return 1;
	}
	fwrite(coreUtf8, 1, strlen(coreUtf8), marker);
	fclose(marker);

	wprintf(L"部署完成：%s\n", gameDir.c_str());
	wprintf(L"  代理名   %s（%ls）\n", dllName.c_str(), why);
	wprintf(L"  core     %s（不搬家，工具更新自动跟上）\n", corePath.c_str());
	wprintf(L"  游戏目录只多了 %s + d5q-deploy.txt 两个小文件\n", dllName.c_str());
	wprintf(L"注意：这游戏别再注入（--launch/--wait 会双载 core）。\n");
	return 0;
}

int Undeploy(const wchar_t* exeArg) {
	const std::filesystem::path exe(exeArg);
	if (!std::filesystem::exists(exe)) {
		wprintf(L"找不到 %s\n", exeArg);
		return 1;
	}
	const std::filesystem::path gameDir = exe.parent_path();
	const std::filesystem::path shellDir = ExeDir() / L"shell";
	const wchar_t* kCandidates[] = { L"d3d12", L"d3d11", L"dxgi", L"xinput1_4" };
	int removed = 0;
	for (const wchar_t* name : kCandidates) {
		if (SlotState(gameDir, shellDir, name) != 0) continue;
		// 只删逐字节等于我们壳的文件 —— 别人的（ReShade 等）绝不碰
		std::error_code ec;
		std::filesystem::remove(gameDir / (std::wstring(name) + L".dll"), ec);
		if (!ec) {
			wprintf(L"已删 %s.dll\n", name);
			++removed;
		}
	}
	const std::filesystem::path marker = gameDir / L"d5q-deploy.txt";
	if (std::filesystem::exists(marker)) {
		std::error_code ec;
		std::filesystem::remove(marker, ec);
		if (!ec) {
			wprintf(L"已删 d5q-deploy.txt\n");
			++removed;
		}
	}
	if (!removed) wprintf(L"没找到我们的部署件（或游戏在跑删不掉）\n");
	return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
	EnableUnicodeStdout();

	if (argc < 2) {
		PrintUsage();
		return 1;
	}
	if (_wcsicmp(argv[1], L"--list") == 0) {
		ListCandidates();
		return 0;
	}
	if (argc < 3) {
		PrintUsage();
		return 1;
	}

	// 这两个不注入，只是通过 IPC 和已经注入的 core 说话
	if (_wcsicmp(argv[1], L"--status") == 0) {
		PrintStatus((DWORD)_wtoi(argv[2]));
		return 0;
	}
	if (_wcsicmp(argv[1], L"--deploy") == 0) {
		if (argc < 3) {
			PrintUsage();
			return 1;
		}
		return Deploy(argv[2], argc > 3 ? argv[3] : nullptr);
	}
	if (_wcsicmp(argv[1], L"--undeploy") == 0) {
		if (argc < 3) {
			PrintUsage();
			return 1;
		}
		return Undeploy(argv[2]);
	}
	if (_wcsicmp(argv[1], L"--launch") == 0) {
		// 第 4 个参数写 late 就走"等窗口出现再注入"（和游戏原生 DLSS 共存）
		const bool lateInject = argc > 4 && _wcsicmp(argv[4], L"late") == 0;
		DXL::Log::Get().Open(L"inject");
		const DXL::LaunchOutcome outcome = DXL::LaunchAndInject(
			argv[2], argc > 3 ? argv[3] : L"",
			ExeDir() / L"DXL-core.dll", lateInject);
		wprintf(L"%s\n", outcome.message.c_str());
		return outcome.pid ? 0 : 1;
	}
	if (_wcsicmp(argv[1], L"--reload") == 0) {
		const DWORD target = (DWORD)_wtoi(argv[2]);
		const bool ok = DXL::SendCommand(
			target, DXL::Ipc::CommandId::ReloadSettings);
		wprintf(ok ? L"pid %lu 已重读设置\n"
			: L"pid %lu 没有响应 —— core 没注入，或管道不通\n", target);
		return ok ? 0 : 1;
	}

	// 真超分必须在游戏创建 swapchain 之前注入，手动掐时间几乎不可能，所以提供
	// 一个"守着等它出现"的模式：进程一出现就注入，那时它还远没到建 swapchain。
	if (_wcsicmp(argv[1], L"--wait") == 0) {
		const std::filesystem::path dll = ExeDir() / L"DXL-core.dll";
		std::error_code ec;
		if (!std::filesystem::exists(dll, ec)) {
			wprintf(L"找不到 core DLL: %s\n", dll.c_str());
			return 1;
		}
		wprintf(L"等待 %s 启动…（Ctrl+C 取消）\n", argv[2]);
		for (;;) {
			const DWORD found = FindProcessByName(argv[2]);
			if (found) {
				// 必须等它自己的依赖加载完再注入，否则会和 loader lock 撞车把目标
				// 挂死 —— 实测固定 Sleep(200) 三次里挂两次。见 WaitForLoaderQuiet。
				wprintf(L"发现 pid %lu，等它的模块加载稳定…\n", found);
				if (!DXL::WaitForLoaderQuiet(found)) {
					wprintf(L"模块加载一直没停下来（或进程已退出），放弃注入。\n");
					return 1;
				}
				wprintf(L"注入…\n");
				// --wait 就是为早注入准备的，所以要告诉 core 别自己造探测设备。
				// 标记的生存期必须覆盖 core 的 InitThread，所以放在返回之前才析构。
				const DXL::EarlyInjectMarker marker(found);
				const bool ok = Inject(found, dll);
				// core 在 InitThread 里读标记，那个线程可能比 LoadLibrary 返回稍晚
				Sleep(1500);
				return ok ? 0 : 1;
			}
			Sleep(50);
		}
	}

	DWORD pid = 0;
	if (_wcsicmp(argv[1], L"--pid") == 0) {
		pid = (DWORD)_wtoi(argv[2]);
	} else if (_wcsicmp(argv[1], L"--exe") == 0) {
		pid = FindProcessByName(argv[2]);
		if (!pid) {
			wprintf(L"没找到进程: %s\n", argv[2]);
			return 1;
		}
	} else {
		PrintUsage();
		return 1;
	}

	// D5_INJECT_DLL 环境变量可覆盖注入目标（默认 core）——
	// 探针类测试（detachprobe）用它复用同一条注入路径，不加新参数。
	const std::filesystem::path dll = [] {
		wchar_t overrideDll[MAX_PATH]{};
		if (GetEnvironmentVariableW(L"D5_INJECT_DLL", overrideDll, MAX_PATH) &&
			overrideDll[0]) {
			return std::filesystem::path(overrideDll);
		}
		return ExeDir() / L"DXL-core.dll";
	}();
	std::error_code ec;
	if (!std::filesystem::exists(dll, ec)) {
		wprintf(L"找不到 core DLL: %s\n", dll.c_str());
		return 1;
	}
	wprintf(L"注入 %s -> pid %lu\n", dll.filename().c_str(), pid);
	return Inject(pid, dll) ? 0 : 1;
}
