#pragma once

// 一张纹理 + 它在滤镜边界上的资源状态。
//
// 为什么需要它：滤镜要能串起来（NR 的输出接 SR 的输入），而每个滤镜自己录命令
// 列表，所以必须明确约定"我拿到它时它是什么状态、我还回去时也是什么状态"。
// 光传指针会让每个滤镜都得猜前一个留下的状态，那正是最容易写错、又只有 D3D12
// 调试层才看得见的地方。
//
// 约定：**进入和离开时状态相同**。滤镜内部想怎么迁移都行，出去之前必须还原。

#include <d3d12.h>

namespace DXL {

struct GpuImage {
	ID3D12Resource* resource = nullptr;
	// backbuffer 用 PRESENT（== COMMON），我们自己的中间纹理一般用
	// NON_PIXEL_SHADER_RESOURCE 或 UNORDERED_ACCESS
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

	bool IsValid() const noexcept { return resource != nullptr; }
};

}  // namespace DXL
