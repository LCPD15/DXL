// 代理壳（#83 部署模式）：拷成游戏目录里的 <候选名>.dll（d3d12/d3d11/xinput1_4/dxgi）。
// 游戏加载器按「应用目录优先于 System32」的搜索顺序先找到我们这份 ——
// 与 ReShade 完全同一条加载机制（改名拷贝 + 运行时转发），没有任何注入 API。
//
// 三条底线：
//   1. **运行时转发，不用 .def forwarder**（写 =d3d12.D3D12CreateDevice 会让
//      加载器再按搜索顺序找 d3d12.dll —— 又找到我们这份 → 无限递归）。
//      真 DLL 用 System32 绝对路径 LoadLibrary，GetProcAddress 拿地址。
//   2. **转发跳板在 .asm（ml64），签名无关**；**序号 = 真 DLL 的序号**
//     （表由 gen_shell_asm.py 从 link /dump 生成）—— 游戏经 .lib 常按序号
//      导入（d3d12.lib 把 D3D12CreateDevice 映到 #101），按名的壳对不上序号，
//      加载器解析导入失败，进程无输出秒死（v1 冒烟抓的）。
//      未命名槽位（d3d12 #99、xinput 一大串）按序号 GetProcAddress。
//   3. **core 缺失 = 退化纯转发**。工具目录被挪走时游戏照跑，只是功能没了。
//
// core 定位（LoadCoreOnce，按序取第一个命中）：
//   a. 环境变量 D5Q_CORE_DLL（显式覆盖，调试用）
//   b. 壳同目录\DXL-core.dll（全本地部署形态）
//   c. 壳同目录\d5q-deploy.txt —— 部署器写的一行 core 绝对路径（默认形态：
//      core/nvinfer/models 全留工具 build 目录，core 不搬家，工具更新 core
//      所有部署游戏下次启动自动用新版；游戏目录只有壳 + txt 两个小文件）
//
// 早注入标记：拉起 core **之前**创建 Local\DXL.EarlyInject.<pid>
//（与 UI 那边 EarlyInjectMarker 同名，core 的 InitThread 用 OpenEventW 探它）
// → core 走「工厂钩子」路径，不建探测设备。壳的进场时机 = 游戏第一次调被代理
// 的 API —— 必然在 swapchain 创建之前，早注入语义正确；与 --wait 注入走的是
// core 里同一段代码。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "shell_exports.h"

// ---- per-shell 选择（SHELL_ID 由 build.cmd 传）：槽位表 + 真 DLL 名 ----
// 表结构见 shell_exports.h（生成）：{ordinal, name|nullptr}，序号升序，
// asm 跳板序 == C 数组序 == .def 导出序，三处由生成器从同一张表派生。
#if SHELL_ID == 1
#define D5Q_SHELL_ARRAY d5q::kShellExports1
#define D5Q_SHELL_NAME_W L"d3d12"
#elif SHELL_ID == 2
#define D5Q_SHELL_ARRAY d5q::kShellExports2
#define D5Q_SHELL_NAME_W L"d3d11"
#elif SHELL_ID == 3
#define D5Q_SHELL_ARRAY d5q::kShellExports3
#define D5Q_SHELL_NAME_W L"xinput1_4"
#elif SHELL_ID == 4
#define D5Q_SHELL_ARRAY d5q::kShellExports4
#define D5Q_SHELL_NAME_W L"dxgi"
#else
#error "SHELL_ID must be 1 (d3d12) / 2 (d3d11) / 3 (xinput1_4) / 4 (dxgi)"
#endif

namespace {
constexpr const d5q::ShellExport* kSlots = D5Q_SHELL_ARRAY;
constexpr size_t kSlotCount = sizeof(D5Q_SHELL_ARRAY) / sizeof(d5q::ShellExport);
}  // namespace

// 链接器提供的本模块基址（取壳自身路径用，不定义 DllMain —— CRT 默认的够）。
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

// 早注入事件保活到进程退出：core 在 InitThread（异步线程）里 OpenEventW，
// 这里创建完立刻 Close 的话存在竞态 —— 干脆不关，进程退出统一回收。
HANDLE g_earlyEvent = nullptr;
bool g_coreTried = false;

// 名字与 IpcProtocol.h 的 EARLY_INJECT_EVENT_BASE 一致；不引那个头
//（common/ 是注入器侧的，壳保持零依赖 —— 换部署形态不用带一串头文件）。
void CreateEarlyInjectEvent() {
	wchar_t name[128]{};
	_snwprintf_s(name, _TRUNCATE, L"Local\\DXL.EarlyInject.%lu",
		GetCurrentProcessId());
	g_earlyEvent = CreateEventW(nullptr, TRUE, FALSE, name);
}

void LoadCoreFromMarker(const wchar_t* selfDir) {
	// 读 d5q-deploy.txt：一行 core 的绝对路径（UTF-8，部署器写的）。
	wchar_t marker[MAX_PATH]{};
	if (_snwprintf_s(marker, _TRUNCATE, L"%s\\d5q-deploy.txt", selfDir) <= 0) {
		return;
	}
	HANDLE file = CreateFileW(marker, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	char text[600]{};
	DWORD read = 0;
	const BOOL ok = ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
	CloseHandle(file);
	if (!ok || !read) return;
	text[read] = '\0';
	// 掐掉行尾：部署器写的是一行，但手编辑过的文件可能带 \r\n / 空格
	for (char* p = text; *p; ++p) {
		if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
	}
	if (!text[0]) return;
	wchar_t core[MAX_PATH]{};
	if (MultiByteToWideChar(CP_UTF8, 0, text, -1, core, MAX_PATH) && core[0]) {
		LoadLibraryW(core);
	}
}

// 只试一次：失败静默退化（纯转发），别每次转发都重试 LoadLibrary。
void LoadCoreOnce() {
	if (g_coreTried) return;
	g_coreTried = true;

	// a. 环境变量覆盖（调试 / 非常规部署）
	wchar_t core[MAX_PATH]{};
	if (GetEnvironmentVariableW(L"D5Q_CORE_DLL", core, MAX_PATH) && core[0]) {
		CreateEarlyInjectEvent();
		LoadLibraryW(core);
		return;
	}
	// 壳自身路径
	wchar_t self[MAX_PATH]{};
	if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase),
			self, MAX_PATH)) {
		return;
	}
	wchar_t* slash = wcsrchr(self, L'\\');
	if (!slash) return;
	*slash = L'\0';
	// b. 壳同目录（全本地部署）
	wchar_t local[MAX_PATH]{};
	if (_snwprintf_s(local, _TRUNCATE, L"%s\\DXL-core.dll", self) > 0 &&
			GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES) {
		CreateEarlyInjectEvent();
		LoadLibraryW(local);
		return;
	}
	// c. 部署器 marker（默认形态：core 在工具 build 目录，不搬家）
	CreateEarlyInjectEvent();
	LoadCoreFromMarker(self);
}

}  // namespace

// ---- asm 跳板引用的状态（shell_<name>.asm 里 extern 这些符号）----
extern "C" {

// g_procs[i] = 真 DLL 第 i 个槽位的地址（i = 序号升序的槽位号，非 ordinal）；
// Bootstrap 填，跳板只读。xinput 有 108 槽 —— 128 上限留余量。
void* g_procs[128] = {};
// 非零 = g_procs 已填好。跳板每次进入先 cmp 它 —— 初始化后每次转发的
// 稳态开销只有 cmp + jne + jmp 三条指令。
unsigned char g_inited = 0;

// 真 DLL 加载失败时的兜底：跳板 jmp 到它，返回 E_FAIL。
// 设备创建干净地失败，好过硬崩（游戏有自己的降级路径）。
long FailStub() { return 0x80004005L; }

// 部署器识别标记（非 API，纯符号）：部署器解析游戏目录里同名 dll 的导出表，
// 有它 = 我们的壳（**任意版本**，可覆盖可卸）；没有 = 别人的（ReShade 等，拒动）。
// 逐字节比对只能认"当前版本"—— 升级场景（旧壳留在游戏目录、build 里已是新版）
// 会把我们自己的旧壳当别人，拒删拒覆盖，新壳永远部署不进去。
__declspec(dllexport) const char* D5QShellMarker() { return "d5q-proxy-shell-1"; }

// 跳板调它（g_inited 守卫，每个导出只在首次进入时调）。幂等：并发首调时
// 两个线程都进来也没关系 —— LoadLibraryW 加引用计数、GetProcAddress
// 无副作用，重复执行结果相同。
void ProxyBootstrap() {
	if (g_inited) return;

	// 真 DLL：System32 绝对路径（不写死 C:\Windows —— 系统盘符可变；
	// 也是 ReShade 的做法）。
	wchar_t realPath[MAX_PATH]{};
	const int sysLen = GetSystemDirectoryW(realPath, MAX_PATH);
	HMODULE real = nullptr;
	if (sysLen > 0 && sysLen + 16 < MAX_PATH) {
		lstrcatW(realPath, L"\\" D5Q_SHELL_NAME_W L".dll");
		real = LoadLibraryW(realPath);
	}

	// 先拉 core 再填 g_procs：core 的钩子要挂在游戏拿到的东西上，
	// 越早越好；g_procs 填完跳板就开转发了。
	LoadCoreOnce();

	for (size_t i = 0; i < kSlotCount; ++i) {
		FARPROC proc = nullptr;
		if (real) {
			// 具名槽位按名；未命名槽位（d3d12 #99、xinput 一大串）按序号 ——
			// 序号就是真 DLL 的绝对 ordinal，MAKEINTRESOURCE 即取。
			proc = kSlots[i].name
				? GetProcAddress(real, kSlots[i].name)
				: GetProcAddress(real, MAKEINTRESOURCEA(kSlots[i].ordinal));
		}
		// 解析失败（系统版本差异）或真 DLL 没加载：兜 E_FAIL ——
		// 跳板 jmp null 就是硬崩。
		g_procs[i] = proc ? reinterpret_cast<void*>(proc)
			: reinterpret_cast<void*>(&FailStub);
	}
	g_inited = 1;
}

}  // extern "C"
