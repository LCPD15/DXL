// exe 图标提取 + 文件选择框。
//
// 为什么单开一个头文件：这两件事都要拖进 WIC / commdlg 的一堆头和库，
// 而 main.cpp 已经够长了。两者都只被 HandleUiMessage 用到。

#pragma once

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace DXL {

// 让用户挑一个 exe。返回空串 = 取消。
//
// **必须用 OFN_NOCHANGEDIR。** 不加的话对话框会把整个进程的当前目录改到用户
// 浏览到的最后一个文件夹，之后所有相对路径（core DLL、web/、profiles/）
// 全都指到别处去 —— 这种 bug 只在"用过一次文件选择框之后"才出现，很难联想。
inline std::wstring PickGameExe(HWND owner) {
	wchar_t buffer[MAX_PATH * 4]{};
	OPENFILENAMEW dialog{};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = owner;
	dialog.lpstrFilter = L"游戏可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
	dialog.lpstrFile = buffer;
	dialog.nMaxFile = MAX_PATH * 4;
	dialog.lpstrTitle = L"选择游戏的 exe（工具会用它启动游戏）";
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
		OFN_EXPLORER;
	if (!GetOpenFileNameW(&dialog)) return {};
	return buffer;
}

namespace detail {

// HICON -> PNG 文件。走 WIC，因为它是系统自带、且是唯一能正确处理图标 alpha 的
// 现成路径（GDI 那条要自己拆 AND/XOR 掩码，32 位图标还得手搓 premultiply）。
inline bool SaveIconAsPng(HICON icon, const std::filesystem::path& destination) {
	using Microsoft::WRL::ComPtr;
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
		return false;
	}
	ComPtr<IWICBitmap> bitmap;
	if (FAILED(factory->CreateBitmapFromHICON(icon, &bitmap))) return false;

	ComPtr<IWICStream> stream;
	if (FAILED(factory->CreateStream(&stream))) return false;
	if (FAILED(stream->InitializeFromFilename(
			destination.c_str(), GENERIC_WRITE))) {
		return false;
	}
	ComPtr<IWICBitmapEncoder> encoder;
	if (FAILED(factory->CreateEncoder(
			GUID_ContainerFormatPng, nullptr, &encoder))) {
		return false;
	}
	if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
		return false;
	}
	ComPtr<IWICBitmapFrameEncode> frame;
	ComPtr<IPropertyBag2> options;
	if (FAILED(encoder->CreateNewFrame(&frame, &options))) return false;
	if (FAILED(frame->Initialize(options.Get()))) return false;
	if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return false;
	if (FAILED(frame->Commit())) return false;
	return SUCCEEDED(encoder->Commit());
}

}  // namespace detail

// 把 exe 的图标存成 PNG。成功返回 true。
//
// 取图标用 SHDefExtractIconW 并**显式要 64px**，不用 SHGFI_LARGEICON ——
// 后者只给 32x32，在 150% 缩放的屏上侧栏图标就已经是放大的糊块了。
// 拿不到 64 再退回系统大图标。
inline bool ExtractExeIconPng(
	const std::wstring& exePath, const std::filesystem::path& destination) {
	if (exePath.empty()) return false;
	std::error_code ec;
	if (!std::filesystem::exists(exePath, ec)) return false;

	HICON icon = nullptr;
	// LOWORD = 大图标尺寸，HIWORD = 小图标尺寸。只要大的。
	if (SHDefExtractIconW(exePath.c_str(), 0, 0, &icon, nullptr,
			MAKELONG(64, 32)) != S_OK) {
		icon = nullptr;
	}
	if (!icon) {
		SHFILEINFOW large{};
		if (SHGetFileInfoW(exePath.c_str(), 0, &large, sizeof(large),
				SHGFI_ICON | SHGFI_LARGEICON)) {
			icon = large.hIcon;
		}
	}
	if (!icon) return false;
	const bool ownsIcon = true;

	std::filesystem::create_directories(destination.parent_path(), ec);
	const bool ok = detail::SaveIconAsPng(icon, destination);
	if (ownsIcon) DestroyIcon(icon);
	return ok;
}

}  // namespace DXL
