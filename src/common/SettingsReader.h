#pragma once

// 从 settings.json 里取值。
//
// 刻意不引入 JSON 库：core 会被注入到别人的进程里，依赖越少越安全，而我们的设置
// 结构是扁平的键值对（嵌套只有 hotkeys 一层，core 不关心）。所以按键名找 + 读到
// 分隔符为止就够了。UI 侧加字段不需要动这里，只有 core 真正要用的键才加读取。

#include <windows.h>
#include "DataPaths.h"
#include <shlobj.h>
#include <filesystem>
#include <cwctype>
#include <string>
#include <string_view>
#include <charconv>

namespace DXL {

class SettingsReader {
public:
	// All versions share the per-user DXL directory. Legacy files are imported once.
	static std::filesystem::path ConfigRoot(HMODULE selfModule) noexcept {
		return DataPaths::ConfigRoot(selfModule);
	}

	// 找本游戏该用的那份配置。
	//
	// UI 侧维护"每游戏配置文件"，但**不让 core 去理解嵌套的 profile 结构** ——
	// core 被注入到别人的进程里，越简单越安全。所以 UI 负责把选中的 profile
	// 展平成一份 `profiles\<游戏exe名>.json`，core 只按自己宿主的 exe 名去取。
	//
	// 查找顺序：
	//   profiles\<游戏exe名>.json   这个游戏的专属配置
	//   profiles\default.json       默认配置
	//   settings.json               老的单文件（保留兼容，测试脚本还在用）
	static std::filesystem::path FindPath(HMODULE selfModule) noexcept {
		std::error_code ec;
		const std::filesystem::path root = ConfigRoot(selfModule);
		if (root.empty()) return {};

		wchar_t hostPath[MAX_PATH]{};
		if (GetModuleFileNameW(nullptr, hostPath, MAX_PATH)) {
			std::wstring exeName =
				std::filesystem::path(hostPath).filename().wstring();
			// 统一小写，免得大小写差异导致找不到
			for (wchar_t& ch : exeName) ch = (wchar_t)towlower(ch);
			const std::filesystem::path perGame =
				root / L"profiles" / (exeName + L".json");
			if (std::filesystem::exists(perGame, ec)) return perGame;
		}

		const std::filesystem::path fallback = root / L"profiles" / L"default.json";
		if (std::filesystem::exists(fallback, ec)) return fallback;
		return root / L"settings.json";
	}

	// 诊断键的专用文件。**UI 从来不写、也从来不读它** —— 这就是它存在的全部理由。
	//
	// 为什么非要单独一个文件：`profiles\<exe>.json` 是 UI **生成**的，从工具启动游戏时
	// 会用 settings.json 里的模型把它整个重写一遍。手写进去的诊断键活不过一次启动 ——
	// 实测踩过：`diagNrDumpPixels` 和一个开关一起被覆盖，结果那一局什么都没测到，
	// 而画面"看起来正常了"差点被当成修好了。
	//
	// 查找顺序（都在 profiles\ 下）：
	//   <游戏exe名>.diag.json   这个游戏的诊断覆盖
	//   diag.json               所有游戏共用的诊断覆盖
	static std::filesystem::path FindDiagPath(HMODULE selfModule) noexcept {
		std::error_code ec;
		const std::filesystem::path root = ConfigRoot(selfModule);
		if (root.empty()) return {};

		wchar_t hostPath[MAX_PATH]{};
		if (GetModuleFileNameW(nullptr, hostPath, MAX_PATH)) {
			std::wstring exeName =
				std::filesystem::path(hostPath).filename().wstring();
			for (wchar_t& ch : exeName) ch = (wchar_t)towlower(ch);
			const std::filesystem::path perGame =
				root / L"profiles" / (exeName + L".diag.json");
			if (std::filesystem::exists(perGame, ec)) return perGame;
		}
		const std::filesystem::path shared = root / L"profiles" / L"diag.json";
		if (std::filesystem::exists(shared, ec)) return shared;
		return {};
	}

	// 浮层参数的持久层：profiles\<游戏exe名>.params.json。
	//
	// **只有 core 读写这一个文件。** 它存在的理由和 .diag.json 同一条：`<exe>.json`
	// 是 UI 生成的，UI 的 persistAll 会拿它自己那份（过期的）模型整批重写 —— 实测
	// 把玩家在浮层里调好的 2.00 弹回 1.10（core-44052：滤镜 92 条日志证明收到了
	// 2.00，UI 从没写盘过 2.00，persistAll 却连写五次旧值 + ReloadSettings）。
	// 参数收归 core 自己持久化之后，UI 写什么都盖不到这一层。
	//
	// 不检查存在性：不存在 = 还没在浮层里改过参数，core 第一次保存时会建出来。
	static std::filesystem::path FindParamsPath(HMODULE selfModule) noexcept {
		const std::filesystem::path root = ConfigRoot(selfModule);
		if (root.empty()) return {};
		wchar_t hostPath[MAX_PATH]{};
		if (!GetModuleFileNameW(nullptr, hostPath, MAX_PATH)) return {};
		std::wstring exeName =
			std::filesystem::path(hostPath).filename().wstring();
		for (wchar_t& ch : exeName) ch = (wchar_t)towlower(ch);
		return root / L"profiles" / (exeName + L".params.json");
	}

	// 装一份覆盖层。里面出现的键**盖掉**主配置里的同名键。
	// **不 clear、往后面叠**：这个函数被连着调两次（diag.json -> params.json，
	// 见 ReloadSettings），clear 再装会把先装的那层整个抹掉 —— 实测踩过（#67）：
	// diag.json 钉的 srEnable=false 在 params.json 存在时静默失效，日志还打着
	// "诊断覆盖已生效"，那一局测试全废（SR 代理半路激活把分辨率改了）。
	// 叠着装之后 FindKey 找到的是先装（diag）那处 —— 手写诊断键优先，
	// 正是 FindDiagPath 注释承诺的行为。两层都没有的键才回主配置。
	bool LoadDiagOverlay(const std::filesystem::path& path) noexcept {
		if (path.empty()) return false;
		SettingsReader temp;
		if (!temp.Load(path)) return false;
		_diag.append(temp._text);
		return true;
	}

	bool HasDiagOverlay() const noexcept { return !_diag.empty(); }

	// Per-game UI values may override tunables, never launcher routing/master
	// policy. Older .params files can contain those keys; ignore them without
	// rewriting a player's file or overriding an intentional .diag setting.
	bool LoadParamsOverlay(const std::filesystem::path& path) noexcept {
		SettingsReader temp;
		if (path.empty() || !temp.Load(path)) return false;
		const bool newerHotkeys = temp.GetUInt64("hotkeyRevision", 0) >= GetUInt64("hotkeyRevision", 0);
		const auto append = [&](std::string_view key) {
			if (!newerHotkeys && (key == "hkEnable" || key == "hkEnableMods" || key == "hkUi" ||
				key == "hkUiMods" || key == "hotkeyRevision")) return;
			const auto value = temp.RawValue(key);
			if (!value.empty()) { _diag += "\""; _diag += key; _diag += "\":"; _diag += value; _diag += ",\n"; }
		};
		for (const auto key : { "nrUseRealDepth", "nrUseRealMotion", "nrOpticalFlow", "nrOpticalFlowQuality",
			"nrRenderScale", "nrPreset", "nrStyle", "nrIntensity", "nrColourStrength", "nrSelfLayers", "nrTrueLayers",
			"nrLocalTone", "nrLocalStructure", "nrSkinStructure", "nrAutoMask", "nrUiCorrection", "nrToneScale",
			"nrDebugView", "mvScale", "depthInverted", "hkEnable", "hkEnableMods", "hkUi", "hkUiMods", "hotkeyRevision", "nrControlMask",
			"nrControlMaskR", "nrControlMaskG", "nrControlMaskB", "nrControlMaskA", "nrSemanticMask", "nrSemOn",
			"nrSemBgInt", "nrSemanticDebugView", "nrSemanticFlipY", "nrSemanticFeather" }) append(key);
		for (unsigned g = 0; g < 18; ++g) append("nrSemInt" + std::to_string(g));
		return true;
	}

	bool Load(const std::filesystem::path& path) noexcept {
		_text.clear();
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER size{};
		if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
			size.QuadPart < (4 << 20)) {
			_text.resize((size_t)size.QuadPart);
			DWORD read = 0;
			if (ReadFile(file, _text.data(), (DWORD)_text.size(), &read, nullptr)) {
				_text.resize(read);
			} else {
				_text.clear();
			}
		}
		CloseHandle(file);
		return !_text.empty();
	}

	bool GetBool(std::string_view key, bool fallback) const noexcept {
		const std::string_view value = RawValue(key);
		if (value.empty()) return fallback;
		if (value.starts_with("true")) return true;
		if (value.starts_with("false")) return false;
		return fallback;
	}

	float GetFloat(std::string_view key, float fallback) const noexcept {
		const std::string_view value = RawValue(key);
		if (value.empty()) return fallback;
		try {
			return std::stof(std::string(value));
		} catch (...) {
			return fallback;
		}
	}

	int GetInt(std::string_view key, int fallback) const noexcept {
		return (int)std::lround(GetFloat(key, (float)fallback));
	}
	uint64_t GetUInt64(std::string_view key, uint64_t fallback) const noexcept {
		const auto value = RawValue(key);
		if (value.empty()) return fallback;
		uint64_t result = 0;
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
		return parsed.ec == std::errc{} ? result : fallback;
	}

	// 字符串值（已去引号）。相等比较用 IsString 更省事。
	std::string GetString(std::string_view key, std::string_view fallback) const {
		const std::string& text = TextFor(key);
		const size_t pos = ValuePos(key);
		if (pos == std::string::npos || text[pos] != '"') {
			return std::string(fallback);
		}
		const size_t end = text.find('"', pos + 1);
		if (end == std::string::npos) return std::string(fallback);
		return text.substr(pos + 1, end - pos - 1);
	}

	bool IsString(std::string_view key, std::string_view expected) const {
		return GetString(key, "") == expected;
	}

	bool Empty() const noexcept { return _text.empty(); }

private:
	static size_t FindKey(const std::string& text, std::string_view key) noexcept {
		if (text.empty()) return std::string::npos;
		return text.find("\"" + std::string(key) + "\"");
	}

	// 这个 key 该从哪份文本里取：**诊断覆盖优先**。
	// 覆盖层里没有这个键时才回到主配置 —— 所以覆盖文件只写要改的那几项就行。
	const std::string& TextFor(std::string_view key) const noexcept {
		if (FindKey(_diag, key) != std::string::npos) return _diag;
		return _text;
	}

	// 返回 key 对应的值在文本里的起始下标（跳过空白）
	size_t ValuePos(std::string_view key) const noexcept {
		const std::string& text = TextFor(key);
		const size_t start = FindKey(text, key);
		if (start == std::string::npos) return std::string::npos;
		const std::string needle = "\"" + std::string(key) + "\"";
		size_t pos = text.find(':', start + needle.size());
		if (pos == std::string::npos) return std::string::npos;
		++pos;
		while (pos < text.size() &&
			(text[pos] == ' ' || text[pos] == '\t' ||
			 text[pos] == '\r' || text[pos] == '\n')) {
			++pos;
		}
		return pos < text.size() ? pos : std::string::npos;
	}

	std::string_view RawValue(std::string_view key) const noexcept {
		const std::string& text = TextFor(key);
		const size_t pos = ValuePos(key);
		if (pos == std::string::npos) return {};
		const size_t end = text.find_first_of(",}\r\n", pos);
		return std::string_view(text).substr(pos,
			(end == std::string::npos ? text.size() : end) - pos);
	}

	std::string _text;
	// 诊断覆盖。空 = 没有。
	std::string _diag;
};

}  // namespace DXL
