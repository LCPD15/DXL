#pragma once

// D3D12 接口的 vtable 索引，以及**装 hook 之前的自检**。
//
// 为什么要自检：深度探测需要 hook 到 ID3D12Device 的第 21 项、
// ID3D12GraphicsCommandList 的第 47 项这种位置。索引数错一位，就会在游戏进程里
// 用错误的签名调用错误的函数 —— 立刻崩，而且崩在游戏里，很难查。
//
// 自检的办法：挑几个**无副作用、返回值可预测**的方法，分别通过 vtable 索引和
// C++ 接口各调一次，结果一致才说明整张表的布局和我们的假设吻合。这几个方法和
// 我们真正要 hook 的方法在同一张 vtable 里，所以锚定它们就锚定了整张表。
//
// 任何一项不过就拒绝安装深度相关的 hook —— 宁可少一个功能，不要在别人的游戏里崩。

#include <windows.h>
#include <d3d12.h>

#include "../common/Log.h"

namespace DXL {

/* ---------------- ID3D12Device ---------------- */
// IUnknown(0-2) ID3D12Object(3-6: GetPrivateData / SetPrivateData /
// SetPrivateDataInterface / SetName) 之后才是 ID3D12Device 自己的方法
constexpr size_t VT_DEVICE_GET_NODE_COUNT = 7;
constexpr size_t VT_DEVICE_GET_DESCRIPTOR_HANDLE_INCREMENT_SIZE = 15;
constexpr size_t VT_DEVICE_CREATE_DEPTH_STENCIL_VIEW = 21;
constexpr size_t VT_DEVICE_CREATE_COMMITTED_RESOURCE = 27;

/* ---------------- ID3D12GraphicsCommandList ---------------- */
// IUnknown(0-2) ID3D12Object(3-6) ID3D12DeviceChild(7: GetDevice)
// ID3D12CommandList(8: GetType) 之后是 ID3D12GraphicsCommandList
constexpr size_t VT_LIST_GET_TYPE = 8;
constexpr size_t VT_LIST_DRAW_INSTANCED = 12;
constexpr size_t VT_LIST_DRAW_INDEXED_INSTANCED = 13;
// 记录游戏绑过什么用的四项（见 CommandListTracker.h）。
// 顺序是 ID3D12GraphicsCommandList 的标准 vtable 布局，26/46/47 已经实测对得上，
// 所以夹在它们中间的这几项也一定对。
constexpr size_t VT_LIST_SET_PIPELINE_STATE = 25;
constexpr size_t VT_LIST_RESOURCE_BARRIER = 26;
constexpr size_t VT_LIST_SET_DESCRIPTOR_HEAPS = 28;
constexpr size_t VT_LIST_SET_COMPUTE_ROOT_SIGNATURE = 29;
constexpr size_t VT_LIST_SET_GRAPHICS_ROOT_SIGNATURE = 30;
constexpr size_t VT_LIST_OM_SET_RENDER_TARGETS = 46;
constexpr size_t VT_LIST_CLEAR_DEPTH_STENCIL_VIEW = 47;

namespace VTableCheck {

template <typename Fn>
Fn At(void* instance, size_t index) noexcept {
	return reinterpret_cast<Fn>((*reinterpret_cast<void***>(instance))[index]);
}

// GetNodeCount 和 GetDescriptorHandleIncrementSize 都是纯查询，调多少次都无副作用
inline bool VerifyDevice(ID3D12Device* device) noexcept {
	if (!device) return false;

	using GetNodeCountFn = UINT(STDMETHODCALLTYPE*)(ID3D12Device*);
	const UINT viaVTable =
		At<GetNodeCountFn>(device, VT_DEVICE_GET_NODE_COUNT)(device);
	const UINT viaInterface = device->GetNodeCount();
	if (viaVTable != viaInterface) {
		D5_LOG_ERROR(L"ID3D12Device vtable 自检失败：第 %zu 项不是 GetNodeCount"
			L"（vtable=%u 接口=%u）", VT_DEVICE_GET_NODE_COUNT,
			viaVTable, viaInterface);
		return false;
	}

	using IncrementSizeFn =
		UINT(STDMETHODCALLTYPE*)(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE);
	const UINT sizeViaVTable =
		At<IncrementSizeFn>(device, VT_DEVICE_GET_DESCRIPTOR_HANDLE_INCREMENT_SIZE)(
			device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const UINT sizeViaInterface = device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	// 顺带排掉"两边都返回 0"这种碰巧相等的情况
	if (sizeViaVTable != sizeViaInterface || sizeViaInterface == 0) {
		D5_LOG_ERROR(L"ID3D12Device vtable 自检失败：第 %zu 项不是 "
			L"GetDescriptorHandleIncrementSize（vtable=%u 接口=%u）",
			VT_DEVICE_GET_DESCRIPTOR_HANDLE_INCREMENT_SIZE,
			sizeViaVTable, sizeViaInterface);
		return false;
	}

	D5_LOG_INFO(L"ID3D12Device vtable 自检通过（节点数=%u 描述符步长=%u）",
		viaInterface, sizeViaInterface);
	return true;
}

// GetType 是纯查询
inline bool VerifyCommandList(ID3D12GraphicsCommandList* commandList) noexcept {
	if (!commandList) return false;

	using GetTypeFn =
		D3D12_COMMAND_LIST_TYPE(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
	const D3D12_COMMAND_LIST_TYPE viaVTable =
		At<GetTypeFn>(commandList, VT_LIST_GET_TYPE)(commandList);
	const D3D12_COMMAND_LIST_TYPE viaInterface = commandList->GetType();
	if (viaVTable != viaInterface) {
		D5_LOG_ERROR(L"ID3D12GraphicsCommandList vtable 自检失败："
			L"第 %zu 项不是 GetType（vtable=%d 接口=%d）",
			VT_LIST_GET_TYPE, (int)viaVTable, (int)viaInterface);
		return false;
	}

	D5_LOG_INFO(L"ID3D12GraphicsCommandList vtable 自检通过（类型=%d）",
		(int)viaInterface);
	return true;
}

}  // namespace VTableCheck

}  // namespace DXL
