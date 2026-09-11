// GameScan.h 的独立验证程序。**不是单元测试** —— 它的价值在于"在这台真机上
// 到底扫出了什么"，那是任何 mock 都替代不了的信息。
//
// 编译：scripts\test-game-scan.cmd
//
// 要看的三件事：
//   1. Steam/Epic/GOG 各扫到几个（0 个说明那条路的注册表/清单位置猜错了）
//   2. 挑出来的 exe 对不对（PickMainExe 是整个功能里最容易错的一步）
//   3. 耗时（这东西要在后台线程跑，得知道它有多慢）

#include <cstdio>
#include <chrono>
#include "../src/ui/GameScan.h"

int wmain() {
	SetConsoleOutputCP(CP_UTF8);
	const auto start = std::chrono::steady_clock::now();

	struct Group {
		const char* label;
		std::vector<DXL::DetectedGame> games;
	};
	Group groups[] = {
		{ "Steam", DXL::FindSteamGames() },
		{ "Epic", DXL::FindEpicGames() },
		{ "GOG", DXL::FindGogGames() },
	};

	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();

	size_t total = 0;
	for (const Group& group : groups) {
		printf("=== %s: %zu 个 ===\n", group.label, group.games.size());
		for (const DXL::DetectedGame& game : group.games) {
			printf("  %ls\n      -> %ls\n",
				game.name.c_str(), game.exePath.c_str());
		}
		total += group.games.size();
	}
	printf("\n合计 %zu 个，耗时 %lld ms\n", total, (long long)elapsed);
	if (!total) {
		printf("一个都没扫到 —— 要么这台机器上三个平台都没装，"
			"要么注册表/清单路径猜错了。\n");
	}
	// Steam 库路径单独打一下：库找不全是"游戏少了一半"的最常见原因
	printf("\nSteam 库：\n");
	for (const std::wstring& library : DXL::SteamLibraries()) {
		printf("  %ls\n", library.c_str());
	}
	return 0;
}
