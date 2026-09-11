#pragma once

// IAT（导入地址表）补丁。
//
// 为什么本项目靠这个而不是 inline hook：core 被注入到别人的进程里，依赖越少越安全，
// 所以刻意不引 MinHook/Detours。vtable 打补丁能覆盖 COM 接口，但**普通导出函数**
// （LoadLibraryW / GetProcAddress / NGX 的入口）不在任何 vtable 里 —— 那些只能靠
// 改调用方的导入表。
//
// 改 IAT 的好处是作用域精确：只影响**这一个模块**的调用，进程里其他人调同一个函数
// 完全不受影响。代价是只对**静态导入**有效；调用方自己用 GetProcAddress 拿地址就
// 抓不到 —— 所以要抓的恰恰是 GetProcAddress 本身。
//
// 实测（鬼武者，直接解析磁盘上的 PE）：游戏 exe 和 sl.interposer / sl.common /
// sl.dlss 全都**没有**静态导入 nvngx 或 sl.*（那些都是 LoadLibrary + GetProcAddress
// 动态解析的），但**每一个都静态导入了 KERNEL32 的 LoadLibrary* 和 GetProcAddress**。
// 所以链式补丁加载器函数是唯一可行、也确实可行的路。

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace DXL::Iat {

// 一个模块的导入表里，某个函数的 IAT 槽位地址。找不到返回 nullptr。
//
// fromLibrary 为 nullptr 时匹配所有导入库；给了名字就只在那个库的导入里找
// （不区分大小写）。api-set 转发很常见，所以调用方通常传 nullptr 更省事。
inline void** FindSlot(
	HMODULE module, const char* functionName,
	const char* fromLibrary = nullptr) noexcept {
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
		if (fromLibrary) {
			const char* library =
				reinterpret_cast<const char*>(base + descriptor->Name);
			if (_stricmp(library, fromLibrary) != 0) continue;
		}
		// OriginalFirstThunk 是名字表，FirstThunk 是地址表（要改的就是它）。
		// 有些模块 OriginalFirstThunk 为 0，那就只能用 FirstThunk 当名字表 ——
		// 那种情况下它在绑定前后含义不同，不去碰更安全。
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

// 往 IAT 槽位写新值。previous 拿回原值（也就是"原函数"）。
inline bool WriteSlot(void** slot, void* value, void** previous) noexcept {
	if (!slot) return false;
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

// 找到并替换。返回是否成功；原函数写进 previous。
//
// **幂等性由调用方负责**：重复给同一个模块打补丁会把我们自己的 hook 当成"原函数"
// 存下来，直接无限递归。这个坑在 swapchain 的 vtable 上已经踩过一次了。
inline bool Patch(HMODULE module, const char* functionName, void* replacement,
	void** previous, const char* fromLibrary = nullptr) noexcept {
	void** slot = FindSlot(module, functionName, fromLibrary);
	if (!slot) return false;
	// 已经指向我们了就别再打 —— 否则 previous 会变成我们自己
	if (*slot == replacement) return false;
	return WriteSlot(slot, replacement, previous);
}

}  // namespace DXL::Iat
