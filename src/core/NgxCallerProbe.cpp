#include "NgxCallerProbe.h"

#include <psapi.h>
#include <cstdint>
#include <cwctype>
#include <cstring>

#include "../common/Log.h"

#pragma comment(lib, "psapi.lib")

namespace DXL {

namespace {

// 大小写无关的子串查找。刻意不引 shlwapi（core 被注入到别人进程里，依赖越少越好）。
const wchar_t* FindSubstringNoCase(const wchar_t* haystack, const wchar_t* needle) {
	if (!haystack || !needle || !*needle) return nullptr;
	for (const wchar_t* start = haystack; *start; ++start) {
		const wchar_t* a = start;
		const wchar_t* b = needle;
		while (*a && *b && towlower(*a) == towlower(*b)) {
			++a;
			++b;
		}
		if (!*b) return start;
	}
	return nullptr;
}

// 扫一个模块的导入表，报告它有没有静态导入 nvngx 的东西。
//
// 返回导入的 NGX 函数个数；names 里塞前几个函数名，够看出它用的是哪一套 API。
uint32_t ScanNgxImports(HMODULE module, char* names, size_t namesCapacity) noexcept {
	if (names && namesCapacity) names[0] = '\0';
	if (!module) return 0;

	auto* base = reinterpret_cast<uint8_t*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
	const auto* nt =
		reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return 0;
	}

	const IMAGE_DATA_DIRECTORY& directory =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
	if (!directory.VirtualAddress || !directory.Size ||
		directory.VirtualAddress >= imageSize) {
		return 0;
	}

	uint32_t found = 0;
	size_t written = 0;
	auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
		base + directory.VirtualAddress);
	for (; descriptor->Name && descriptor->Name < imageSize; ++descriptor) {
		const char* library = reinterpret_cast<const char*>(base + descriptor->Name);
		// 只关心从 nvngx 家族导入的
		bool isNgx = false;
		for (const char* p = library; *p; ++p) {
			if ((p[0] == 'n' || p[0] == 'N') && _strnicmp(p, "nvngx", 5) == 0) {
				isNgx = true;
				break;
			}
		}
		if (!isNgx || !descriptor->OriginalFirstThunk) continue;

		auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
			base + descriptor->OriginalFirstThunk);
		for (; thunk->u1.AddressOfData; ++thunk) {
			if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal)) {
				++found;
				continue;
			}
			const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
				base + thunk->u1.AddressOfData);
			++found;
			// 只记前几个，够判断它用的是哪套 API 就行
			if (names && written + 2 < namesCapacity && found <= 6) {
				const char* fn = byName->Name;
				if (written) names[written++] = ' ';
				while (*fn && written + 1 < namesCapacity) names[written++] = *fn++;
				names[written] = '\0';
			}
		}
	}
	return found;
}

}  // namespace

void LogNgxCallers(const wchar_t* whenLabel) noexcept {
	HMODULE modules[1024];
	DWORD needed = 0;
	if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
		D5_LOG_WARN(L"NGX 调用方探针：EnumProcessModules 失败 %lu", GetLastError());
		return;
	}
	const DWORD count = (needed / sizeof(HMODULE) < _countof(modules))
		? DWORD(needed / sizeof(HMODULE)) : DWORD(_countof(modules));

	D5_LOG_INFO(L"===== 谁在调 NGX（%s，共 %lu 个模块）=====", whenLabel, count);

	uint32_t interesting = 0;
	uint32_t staticImporters = 0;
	for (DWORD i = 0; i < count; ++i) {
		wchar_t path[MAX_PATH]{};
		if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
		const wchar_t* leaf = wcsrchr(path, L'\\');
		leaf = leaf ? leaf + 1 : path;

		char ngxNames[256]{};
		const uint32_t ngxImports =
			ScanNgxImports(modules[i], ngxNames, sizeof(ngxNames));

		const bool interestingName =
			_wcsicmp(leaf, L"UnityPlayer.dll") == 0 ||
			FindSubstringNoCase(leaf, L"nvngx") ||
			FindSubstringNoCase(leaf, L"sl.") ||
			FindSubstringNoCase(leaf, L"nvapi") ||
			FindSubstringNoCase(leaf, L"dlss");
		if (!interestingName && !ngxImports) continue;

		++interesting;
		// **打完整路径**，不能只打文件名：实测鬼武者进程里同时有**两个**
		// nvngx_dlssnr.dll（一个是我们的，一个是别的 mod 的），只看文件名根本
		// 分不出谁是谁 —— 而这恰恰是最需要分清的事。
		if (ngxImports) {
			++staticImporters;
			D5_LOG_INFO(L"  base=%p  **静态导入 %u 个 NGX 函数**: %hs  <- %s",
				modules[i], ngxImports, ngxNames, path);
		} else {
			D5_LOG_INFO(L"  base=%p  （没有静态导入 NGX）           <- %s",
				modules[i], path);
		}
	}

	if (!interesting) {
		D5_LOG_INFO(L"  （进程里没有任何 NVIDIA/NGX/Streamline 模块 —— "
			L"游戏的 DLSS 大概没开）");
	}
	// 这一行就是路线判据
	if (staticImporters) {
		D5_LOG_INFO(L"结论：有 %u 个模块**静态导入**了 NGX —— 改它们的 IAT 就能旁听"
			L"（路线 B，便宜）。", staticImporters);
	} else if (interesting) {
		D5_LOG_INFO(L"未发现 NGX 静态导入；模块存在不代表已旁听到 Evaluate。"
			L"请核对 exe / UnityPlayer / Streamline 的加载器补丁和 NGX Evaluate attached 日志。"
			L"动态解析可通过加载器链旁听，当前信息不能判定必须替换 NGX。");
	}
	D5_LOG_INFO(L"===== 探针结束 =====");
}

}  // namespace DXL
