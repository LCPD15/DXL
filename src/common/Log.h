#pragma once

// 极简日志。core DLL 在游戏进程里跑，没有控制台，所以必须落文件。
// 每个进程一个文件：%LOCALAPPDATA%\DXL\diagnostics\logs\DXL-<name>-<pid>.log
//
// 刻意不引入任何第三方库：core 会被注入到别人的进程里，依赖越少越安全。

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <string>

namespace DXL {

class Log {
public:
	static Log& Get() noexcept {
		static Log instance;
		return instance;
	}

	void Open(const wchar_t* name) noexcept {
		std::lock_guard guard(_mutex);
		if (_file) return;
		std::error_code ec;
		const std::filesystem::path dir = LogDir();
		std::filesystem::create_directories(dir, ec);
		wchar_t leaf[128]{};
		_snwprintf_s(leaf, _TRUNCATE, L"DXL-%s-%lu.log", name, GetCurrentProcessId());
		// 必须用 _wfsopen + _SH_DENYWR：_wfopen_s 是独占打开的，会导致日志在进程
		// 运行期间完全读不了 —— 而注入侧的日志恰恰要能边跑边看。
		_file = _wfsopen((dir / leaf).c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
		// 退出兜底直写用的路径在这就存好（CRT 活着的时候）——
		// WriteExit 里不能再碰 std::filesystem（那时 CRT 可能已经跑完析构）
		wcsncpy_s(ExitPath(), MAX_PATH + 16, (dir / leaf).c_str(), _TRUNCATE);
		if (_file) {
			Write(L"info", L"log opened");
		}
		// 机制自检：这条出现了说明 WriteExitMarker 这条路是通的；
		// 之后 DETACH 标记不出现才能定论"DETACH 没执行"（而不是机制坏了）
		WriteExitMarker(L"OPEN-marker: exit-marker path works");
	}

	void Info(const wchar_t* format, ...) noexcept {
		va_list args; va_start(args, format);
		WriteV(L"info", format, args);
		va_end(args);
	}

	void Warn(const wchar_t* format, ...) noexcept {
		va_list args; va_start(args, format);
		WriteV(L"warn", format, args);
		va_end(args);
	}

	void Error(const wchar_t* format, ...) noexcept {
		va_list args; va_start(args, format);
		WriteV(L"error", format, args);
		va_end(args);
	}

	static std::filesystem::path LogDir() noexcept {
		PWSTR local = nullptr;
		std::filesystem::path result;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
			result = std::filesystem::path(local) / L"DXL" / L"diagnostics" / L"logs";
			CoTaskMemFree(local);
		}
		return result;
	}

	// 退出兜底的判别直写：**只走 Win32，不碰 CRT。**
	// DLL_PROCESS_DETACH 时 CRT 可能已经跑完析构（FILE* 已 fclose，
	// fputws 静默丢行）——"退出清理"日志条目整段消失就是这个嫌疑。
	// 用 CreateFileW + WriteFile 追加一行 UTF-8 标记，谁活着谁写得进。
	// **必须写另一个文件**（<log>.exit）：日志本体被 _wfsopen 用 _SH_DENYWR
	// 打开——允许别人读、拒绝别人**写**，我们的追加句柄会被自己拒绝
	// （第一版就死在这：标记没写进去≠DETACH 没跑，判别实验白做）。
	static void WriteExitMarker(const wchar_t* text) noexcept {
		// 路径在 Open() 时存好（CRT 活着）；这里只做 raw 拼接，不再碰 std::filesystem
		const wchar_t* stored = ExitPath();
		if (!stored || !stored[0]) return;
		wchar_t path[MAX_PATH + 32]{};
		if (_snwprintf_s(path, _TRUNCATE, L"%s.exit", stored) < 0) return;
		// UTF-8 编码 text（ASCII/中文都覆盖：中文占 3 字节，缓冲给足）
		char utf8[512]{};
		int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8),
			nullptr, nullptr);
		if (n <= 0) return;
		HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return;
		DWORD written = 0;
		WriteFile(h, utf8, DWORD(n - 1), &written, nullptr);
		// 行尾
		WriteFile(h, "\r\n", 2, &written, nullptr);
		CloseHandle(h);
	}

private:
	Log() = default;

	~Log() {
		if (_file) fclose(_file);
	}

	// Open() 存 / WriteExitMarker 读 —— 见那两处的说明。
	// **必须是 POD（裸数组），不能是 std::wstring**：CRT 静态析构可能先于
	// 用户 DllMain(DETACH) 跑，wstring 的缓冲已释放，c_str() 读的是悬垂
	// 指针——判别实验钉死过（同进程注入的裸探针 DETACH 正常落盘、
	// core 的标记缺失，两者唯一差别就是这条路径上的 wstring）。
	// POD 没有析构，数组内容原样留在内存里，谁读都安全。
	static wchar_t* ExitPath() noexcept {
		static wchar_t path[MAX_PATH + 16]{};
		return path;
	}

	void WriteV(const wchar_t* level, const wchar_t* format, va_list args) noexcept {
		wchar_t buffer[1024]{};
		_vsnwprintf_s(buffer, _TRUNCATE, format, args);
		std::lock_guard guard(_mutex);
		Write(level, buffer);
	}

	// 调用方须持有 _mutex
	void Write(const wchar_t* level, const wchar_t* text) noexcept {
		SYSTEMTIME now{};
		GetLocalTime(&now);
		// 无论有没有文件都发到调试器，方便用 DebugView 看注入侧
		wchar_t line[1200]{};
		_snwprintf_s(line, _TRUNCATE, L"[%02u:%02u:%02u.%03u][%s] %s\n",
			now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, level, text);
		OutputDebugStringW(line);
		if (_file) {
			fputws(line, _file);
			fflush(_file);
		}
	}

	std::mutex _mutex;
	FILE* _file = nullptr;
};

}  // namespace DXL

#define D5_LOG_INFO(...)  ::DXL::Log::Get().Info(__VA_ARGS__)
#define D5_LOG_WARN(...)  ::DXL::Log::Get().Warn(__VA_ARGS__)
#define D5_LOG_ERROR(...) ::DXL::Log::Get().Error(__VA_ARGS__)
