// 已安装游戏的自动识别：Steam / Epic / GOG。
//
// 做法参考 RHI（github.com/RankFTW/RHI）的 GameDetectionService，只是换成 C++：
//   Steam — HKLM\SOFTWARE\Valve\Steam 的 SteamPath -> steamapps\libraryfolders.vdf
//           里的每个 "path" -> 各库的 appmanifest_*.acf 里的 name + installdir
//   Epic  — %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item 里的
//           DisplayName + InstallLocation
//   GOG   — HKLM\SOFTWARE\GOG.com\Games\* 下的 GAMENAME + PATH
//
// **只做"找到安装目录"这一步是不够的**：本工具要的是能启动的 exe。
// 一个游戏目录里往往有七八个 exe（启动器、崩溃上报、各种 redist、反作弊），
// 挑错了会启动一个立刻退出的壳子，然后注入到一个已经没了的进程上。
// 挑法见 PickMainExe。

#pragma once

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace DXL {

struct DetectedGame {
	std::wstring name;      // 平台报的显示名
	std::wstring exePath;   // 我们挑出来的主 exe
	std::wstring source;    // Steam / Epic / GOG
	std::wstring steamAppId; // digits only; available for Steam library artwork
};

namespace scan_detail {

inline std::wstring ReadRegString(
	HKEY root, const wchar_t* subKey, const wchar_t* value) {
	HKEY key = nullptr;
	// KEY_WOW64_32KEY 那一份单独查（见调用处），这里只按传进来的视图读
	if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
	wchar_t buffer[MAX_PATH * 2]{};
	DWORD size = sizeof(buffer);
	DWORD type = 0;
	const LSTATUS status =
		RegQueryValueExW(key, value, nullptr, &type,
			reinterpret_cast<LPBYTE>(buffer), &size);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
		return {};
	}
	return buffer;
}

// 从 VDF / ACF 里取 "key"  "value" 形式的值。
// **不是通用 VDF 解析器**：这两种文件的结构固定，只要按引号对取就够了。
inline std::wstring ExtractVdfValue(
	const std::string& text, const std::string& key) {
	const std::string needle = "\"" + key + "\"";
	size_t pos = 0;
	while ((pos = text.find(needle, pos)) != std::string::npos) {
		size_t cursor = pos + needle.size();
		// 跳过空白，然后必须紧跟一个引号 —— 否则是 "installdir_backup" 之类的近似键
		while (cursor < text.size() &&
			(text[cursor] == ' ' || text[cursor] == '\t')) {
			++cursor;
		}
		if (cursor < text.size() && text[cursor] == '"') {
			const size_t end = text.find('"', cursor + 1);
			if (end != std::string::npos) {
				const std::string raw = text.substr(cursor + 1, end - cursor - 1);
				// VDF 里的路径是 C 风格转义的（D:\\Games）
				std::string unescaped;
				for (size_t i = 0; i < raw.size(); ++i) {
					if (raw[i] == '\\' && i + 1 < raw.size()) {
						++i;
						unescaped.push_back(raw[i]);
					} else {
						unescaped.push_back(raw[i]);
					}
				}
				const int needed = MultiByteToWideChar(
					CP_UTF8, 0, unescaped.data(), (int)unescaped.size(), nullptr, 0);
				std::wstring wide((size_t)needed, L'\0');
				MultiByteToWideChar(CP_UTF8, 0, unescaped.data(),
					(int)unescaped.size(), wide.data(), needed);
				return wide;
			}
		}
		pos = cursor;
	}
	return {};
}

inline std::string ReadAllText(const std::filesystem::path& path) {
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return {};
	std::string result;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
		size.QuadPart < (8 << 20)) {
		result.resize((size_t)size.QuadPart);
		DWORD read = 0;
		if (!ReadFile(file, result.data(), (DWORD)result.size(), &read, nullptr)) {
			result.clear();
		} else {
			result.resize(read);
		}
	}
	CloseHandle(file);
	return result;
}

inline std::wstring Lowered(std::wstring text) {
	std::transform(text.begin(), text.end(), text.begin(),
		[](wchar_t ch) { return (wchar_t)towlower(ch); });
	return text;
}

// 明显不是游戏本体的 exe。命中就跳过。
//
// 这张表是"挑错 exe"的唯一防线，而挑错的后果很具体：启动一个立刻退出的壳，
// 我们注入到一个马上就没了的进程上，然后 UI 一直显示"等待 present"。
inline bool LooksLikeHelperExe(const std::wstring& fileName) {
	static const wchar_t* const BAD[] = {
		L"unins", L"crashreport", L"crashpad", L"vcredist", L"dxsetup",
		L"directx", L"dotnet", L"setup", L"install", L"redist",
		L"launcher", L"webhelper", L"easyanticheat", L"eac", L"battleye",
		L"benchmark", L"config", L"settings", L"editor", L"server",
		L"dxwebsetup", L"oalinst", L"activation", L"touchup", L"prereq",
		L"cefprocess", L"subprocess", L"helper", L"updater", L"patcher",
		// 实测在这台机器上真的被挑中过的两个，都是"最大的 exe"这条判据的受害者：
		//   Risk of Rain 2 -> UnityCrashHandler64.exe
		//   Oxygen Not Included -> Restarter.exe
		// 用 crashhandler 而不是光 crash —— "Crash Bandicoot" 是真游戏名。
		L"crashhandler", L"restarter", L"bootstrappackagedgame",
	};
	const std::wstring lower = Lowered(fileName);
	for (const wchar_t* bad : BAD) {
		if (lower.find(bad) != std::wstring::npos) return true;
	}
	return false;
}

// 在安装目录里挑主 exe。
//
// 优先级：
//   1. *-Shipping.exe（UE 的标准命名，几乎一定是本体）
//   2. 安装目录同名的 .NET apphost（runtimeconfig/deps/managed DLL 三项旁证）
//   3. 排除 helper 之后**最大**的那个 exe
// 大小当判据是有依据的：游戏本体带着一堆链进去的引擎代码，通常比任何工具 exe 大
// 一个数量级。深度限制 3 层 —— UE 的本体在 Game\Binaries\Win64\ 下，正好 3 层；
// 再深就开始扫到引擎自带的第三方工具了。
inline std::wstring PickMainExe(const std::filesystem::path& root) {
	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec)) return {};

	// **Unity 游戏必须特判。** Unity 的本体 exe 只有几 MB（内容全在 <名字>_Data\ 里），
	// 于是"最大的 exe"会挑到 UnityCrashHandler64.exe —— 这台机器上实测就这么错过。
	// 判据很硬：同级目录里存在 <名字>_Data 文件夹，那 <名字>.exe 就是本体。
	{
		std::error_code dirEc;
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(root, dirEc)) {
			if (!entry.is_directory(dirEc)) continue;
			const std::wstring leaf = entry.path().filename().wstring();
			if (leaf.size() <= 5) continue;
			if (leaf.compare(leaf.size() - 5, 5, L"_Data") != 0) continue;
			const std::filesystem::path exe =
				root / (leaf.substr(0, leaf.size() - 5) + L".exe");
			if (std::filesystem::exists(exe, dirEc)) return exe.wstring();
		}
	}

	std::wstring shipping;
	std::wstring namedManagedHost;
	std::wstring biggest;
	uintmax_t biggestSize = 0;

	for (std::filesystem::recursive_directory_iterator
			it(root, std::filesystem::directory_options::skip_permission_denied, ec),
			end;
		it != end; it.increment(ec)) {
		if (ec) { ec.clear(); continue; }
		if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
		if (!it->is_regular_file(ec)) continue;
		const std::filesystem::path& path = it->path();
		if (Lowered(path.extension().wstring()) != L".exe") continue;
		const std::wstring leaf = path.filename().wstring();
		if (LooksLikeHelperExe(leaf)) continue;

		// MonoGame/.NET executables can be tiny launch hosts; a bundled audio
		// codec test program may be larger. Prefer an install-name host only
		// when all three same-name runtime companions confirm its identity.
		if (it.depth() == 0 && Lowered(path.stem().wstring()) == Lowered(root.filename().wstring())) {
			const auto stem = path.stem().wstring();
			std::error_code companionError;
			if (std::filesystem::is_regular_file(root / (stem + L".runtimeconfig.json"), companionError) &&
				std::filesystem::is_regular_file(root / (stem + L".deps.json"), companionError) &&
				std::filesystem::is_regular_file(root / (stem + L".dll"), companionError)) {
				namedManagedHost = path.wstring();
			}
		}

		const std::wstring lower = Lowered(leaf);
		if (lower.size() > 13 &&
			lower.compare(lower.size() - 13, 13, L"-shipping.exe") == 0) {
			// 多个 shipping 就取第一个（多平台构建才会出现，通常等价）
			if (shipping.empty()) shipping = path.wstring();
		}
		const uintmax_t size = it->file_size(ec);
		if (!ec && size > biggestSize) {
			biggestSize = size;
			biggest = path.wstring();
		}
		ec.clear();
	}
	return !shipping.empty() ? shipping : !namedManagedHost.empty() ? namedManagedHost : biggest;
}

}  // namespace scan_detail

inline std::vector<std::wstring> SteamLibraries() {
	std::vector<std::wstring> libraries;
	std::wstring steamPath =
		scan_detail::ReadRegString(HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\Valve\\Steam", L"SteamPath");
	if (steamPath.empty()) {
		steamPath = scan_detail::ReadRegString(HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"SteamPath");
	}
	if (steamPath.empty()) {
		// 有些机器上只有 HKCU 那一份（Steam 装在用户目录）
		steamPath = scan_detail::ReadRegString(HKEY_CURRENT_USER,
			L"SOFTWARE\\Valve\\Steam", L"SteamPath");
	}
	if (steamPath.empty()) return libraries;
	std::replace(steamPath.begin(), steamPath.end(), L'/', L'\\');
	libraries.push_back(steamPath);

	const std::filesystem::path vdf =
		std::filesystem::path(steamPath) / L"steamapps" / L"libraryfolders.vdf";
	const std::string text = scan_detail::ReadAllText(vdf);
	// libraryfolders.vdf 里每个库一段，段里有 "path"。逐段取。
	size_t pos = 0;
	while ((pos = text.find("\"path\"", pos)) != std::string::npos) {
		const std::wstring library =
			scan_detail::ExtractVdfValue(text.substr(pos, 512), "path");
		pos += 6;
		if (library.empty()) continue;
		std::error_code ec;
		if (!std::filesystem::is_directory(library, ec)) continue;
		// **大小写要忽略。** libraryfolders.vdf 里写的是 "f:\steam"，
		// 注册表里是 "F:\Steam" —— 按原样比会当成两个库，整个库扫两遍。
		const std::wstring key = scan_detail::Lowered(library);
		const bool known = std::any_of(libraries.begin(), libraries.end(),
			[&key](const std::wstring& existing) {
				return scan_detail::Lowered(existing) == key;
			});
		if (!known) libraries.push_back(library);
	}
	return libraries;
}

inline std::vector<DetectedGame> FindSteamGames() {
	std::vector<DetectedGame> games;
	for (const std::wstring& library : SteamLibraries()) {
		const std::filesystem::path steamapps =
			std::filesystem::path(library) / L"steamapps";
		std::error_code ec;
		if (!std::filesystem::is_directory(steamapps, ec)) continue;
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(steamapps, ec)) {
			const std::wstring leaf = entry.path().filename().wstring();
			if (leaf.rfind(L"appmanifest_", 0) != 0) continue;
			if (scan_detail::Lowered(entry.path().extension().wstring()) != L".acf") {
				continue;
			}
			const std::string text = scan_detail::ReadAllText(entry.path());
			const std::wstring name = scan_detail::ExtractVdfValue(text, "name");
			const std::wstring installDir =
				scan_detail::ExtractVdfValue(text, "installdir");
			if (name.empty() || installDir.empty()) continue;
			const std::filesystem::path root = steamapps / L"common" / installDir;
			if (!std::filesystem::is_directory(root, ec)) continue;
			const std::wstring exe = scan_detail::PickMainExe(root);
			if (exe.empty()) continue;
			std::wstring appId = scan_detail::ExtractVdfValue(text, "appid");
			if (appId.empty() || !std::all_of(appId.begin(), appId.end(),
				[](wchar_t c) { return c >= L'0' && c <= L'9'; })) appId.clear();
			games.push_back({ name, exe, L"Steam", appId });
		}
	}
	return games;
}

inline std::vector<DetectedGame> FindEpicGames() {
	std::vector<DetectedGame> games;
	PWSTR programData = nullptr;
	if (FAILED(SHGetKnownFolderPath(
			FOLDERID_ProgramData, 0, nullptr, &programData))) {
		return games;
	}
	const std::filesystem::path manifests =
		std::filesystem::path(programData) / L"Epic" / L"EpicGamesLauncher" /
		L"Data" / L"Manifests";
	CoTaskMemFree(programData);

	std::error_code ec;
	if (!std::filesystem::is_directory(manifests, ec)) return games;
	for (const std::filesystem::directory_entry& entry :
		std::filesystem::directory_iterator(manifests, ec)) {
		if (scan_detail::Lowered(entry.path().extension().wstring()) != L".item") {
			continue;
		}
		const std::string text = scan_detail::ReadAllText(entry.path());
		// .item 是 JSON，但键值形式和 VDF 一样是 "key": "value"，
		// 同一个取值器够用（冒号会被"跳空白后必须是引号"这一条挡掉，
		// 所以这里单独处理一下冒号）。
		auto jsonString = [&text](const char* key) -> std::wstring {
			const std::string needle = std::string("\"") + key + "\"";
			size_t pos = text.find(needle);
			if (pos == std::string::npos) return {};
			pos = text.find(':', pos + needle.size());
			if (pos == std::string::npos) return {};
			pos = text.find('"', pos);
			if (pos == std::string::npos) return {};
			const size_t end = text.find('"', pos + 1);
			if (end == std::string::npos) return {};
			std::string raw = text.substr(pos + 1, end - pos - 1);
			std::string unescaped;
			for (size_t i = 0; i < raw.size(); ++i) {
				if (raw[i] == '\\' && i + 1 < raw.size()) {
					++i;
					unescaped.push_back(raw[i]);
				} else {
					unescaped.push_back(raw[i]);
				}
			}
			const int needed = MultiByteToWideChar(CP_UTF8, 0, unescaped.data(),
				(int)unescaped.size(), nullptr, 0);
			std::wstring wide((size_t)needed, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, unescaped.data(),
				(int)unescaped.size(), wide.data(), needed);
			return wide;
		};
		const std::wstring name = jsonString("DisplayName");
		const std::wstring root = jsonString("InstallLocation");
		if (name.empty() || root.empty()) continue;
		if (!std::filesystem::is_directory(root, ec)) continue;

		// **必须按 AppCategories 过滤，否则扫出来的全是引擎和插件。**
		// 这台机器上实测：Unreal Engine 的清单是 ["engines/ue5","engines"]，
		// Quixel Bridge / Fab UE Plugin 是空数组，真游戏才有 "games"。
		// 不过滤的话给用户加出一堆 UnrealEditor.exe 的"游戏"配置。
		{
			const size_t at = text.find("\"AppCategories\"");
			if (at == std::string::npos) continue;
			const size_t close = text.find(']', at);
			if (close == std::string::npos) continue;
			if (text.find("\"games\"", at) == std::string::npos ||
				text.find("\"games\"", at) > close) {
				continue;
			}
		}
		// Epic 的清单里有 LaunchExecutable，比我们自己猜准 —— 优先用它
		std::wstring exe;
		const std::wstring launch = jsonString("LaunchExecutable");
		if (!launch.empty()) {
			const std::filesystem::path candidate =
				std::filesystem::path(root) / launch;
			if (std::filesystem::exists(candidate, ec)) {
				exe = candidate.wstring();
			}
		}
		if (exe.empty()) exe = scan_detail::PickMainExe(root);
		if (exe.empty()) continue;
		// Epic 的清单里正斜杠反斜杠混着写，统一成反斜杠 ——
		// 后面要拿这个路径和进程表里的 exe 名比，也要写进配置文件给人看。
		std::replace(exe.begin(), exe.end(), L'/', L'\\');
		games.push_back({ name, exe, L"Epic" });
	}
	return games;
}

inline std::vector<DetectedGame> FindGogGames() {
	std::vector<DetectedGame> games;
	for (const wchar_t* subKey : {
			L"SOFTWARE\\GOG.com\\Games", L"SOFTWARE\\WOW6432Node\\GOG.com\\Games" }) {
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &key) !=
			ERROR_SUCCESS) {
			continue;
		}
		for (DWORD index = 0;; ++index) {
			wchar_t name[256]{};
			DWORD nameSize = 256;
			if (RegEnumKeyExW(key, index, name, &nameSize, nullptr, nullptr,
					nullptr, nullptr) != ERROR_SUCCESS) {
				break;
			}
			const std::wstring child = std::wstring(subKey) + L"\\" + name;
			std::wstring title = scan_detail::ReadRegString(
				HKEY_LOCAL_MACHINE, child.c_str(), L"GAMENAME");
			const std::wstring root = scan_detail::ReadRegString(
				HKEY_LOCAL_MACHINE, child.c_str(), L"PATH");
			if (root.empty()) continue;
			std::error_code ec;
			if (!std::filesystem::is_directory(root, ec)) continue;
			// GOG 会记 EXE 值，优先用
			std::wstring exe = scan_detail::ReadRegString(
				HKEY_LOCAL_MACHINE, child.c_str(), L"EXE");
			if (!exe.empty() && !std::filesystem::exists(exe, ec)) {
				const std::filesystem::path candidate =
					std::filesystem::path(root) / exe;
				exe = std::filesystem::exists(candidate, ec)
					? candidate.wstring() : std::wstring();
			}
			if (exe.empty()) exe = scan_detail::PickMainExe(root);
			if (exe.empty()) continue;
			if (title.empty()) {
				title = std::filesystem::path(root).filename().wstring();
			}
			games.push_back({ title, exe, L"GOG" });
		}
		RegCloseKey(key);
	}
	return games;
}

// 三个平台一起扫，按 exe 路径去重（一个游戏可能同时被 Steam 和 GOG 记到）。
inline std::vector<DetectedGame> ScanInstalledGames() {
	std::vector<DetectedGame> all;
	for (auto&& list : { FindSteamGames(), FindEpicGames(), FindGogGames() }) {
		for (const DetectedGame& game : list) {
			const std::wstring key = scan_detail::Lowered(game.exePath);
			const bool duplicate = std::any_of(all.begin(), all.end(),
				[&key](const DetectedGame& existing) {
					return scan_detail::Lowered(existing.exePath) == key;
				});
			if (!duplicate) all.push_back(game);
		}
	}
	return all;
}

}  // namespace DXL
