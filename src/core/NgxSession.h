#pragma once

// NGX 的进程级会话。整个进程只初始化一次，SR / NR / FG 三个 feature 共用。
//
// 为什么要单独一个类：NVSDK_NGX_D3D12_Init 是**每进程每设备**一次的，重复调用
// 会失败或行为未定义。而我们有三个 feature 都要用它，所以必须集中管理生命周期。

#include <windows.h>
#include <d3d12.h>
#include <string>

namespace DXL {

// 游戏是不是已经有一个活着的 NGX 会话（判据是 DLSS 的 snippet 在不在）。
//
// 这个必须能在 **NgxSession 之外**问到：真超分的代理 backbuffer 是在
// CreateSwapChainForHwnd 里就建的，比我们 init NGX 早好几秒。建代理之前不问一句，
// 就会出现"代理已经交给游戏、但 SR 因为守卫拒绝而永远跑不起来"——而 SR 是唯一把代理
// 放大回真 backbuffer 的环节，于是**真 backbuffer 从来没人写，画面全黑**。
// 鬼武者实测：1595 帧"代理生效但 SR 没运行"，玩家看到的就是黑屏。
bool GameAlreadyOwnsNgx(std::string* detail = nullptr) noexcept;

class NgxSession {
public:
	static NgxSession& Get() noexcept;

	NgxSession(const NgxSession&) = delete;
	NgxSession& operator=(const NgxSession&) = delete;

	// 幂等：已经初始化过就直接返回 true。
	// selfModule 是 core DLL 自己的模块句柄，用来定位旁边的 ngx\ 运行时目录。
	//
	// allowCoexist = true 时**明知会弄坏游戏画面也照样 init**，只给诊断用
	// （settings.json 里的 diagAllowNgxCoexist）。默认 false：游戏已经在跑 DLSS 就
	// 直接拒绝，因为那个损坏是不可逆的。
	bool Initialize(ID3D12Device* device, HMODULE selfModule,
		bool allowCoexist = false) noexcept;
	void Shutdown() noexcept;

	bool IsInitialized() const noexcept { return _initialized; }
	bool IsSuperSamplingAvailable() const noexcept { return _srAvailable; }
	const char* UnavailableReason() const noexcept { return _reason; }
	// 因为游戏自己在跑 DLSS 而被我们主动拦下了。UI 用它显示一句能操作的提示。
	bool IsBlockedByGameNgx() const noexcept { return _blockedByGameNgx; }

private:
	NgxSession() = default;

	void QueryCapabilities() noexcept;

	ID3D12Device* _device = nullptr;
	HMODULE _selfModule = nullptr;
	bool _initialized = false;
	bool _srAvailable = false;
	bool _blockedByGameNgx = false;
	const char* _reason = "not initialised";
};

}  // namespace DXL
