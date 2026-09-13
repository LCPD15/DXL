/* DXL private host entry point for ReShade 6.8.0. No graphics/input hooks.
 * ReShade source: Copyright (C) 2014 Patrick Mours, BSD-3-Clause.
 * DXL hosting changes: distributed under the project's AGPL-3.0 license.
 */
#include "version.h"
#include "dll_log.hpp"
#include "ini_file.hpp"
#include "runtime.hpp"
#include <Windows.h>
#include <delayimp.h>

extern "C" __declspec(dllexport) const char *DxlReShadeVersion = VERSION_STRING_PRODUCT;
extern "C" __declspec(dllexport) const unsigned int DxlReShadeApiVersion = 20;
HANDLE g_exit_event = nullptr;
HMODULE g_module_handle = nullptr;
std::filesystem::path g_reshade_dll_path;
std::filesystem::path g_reshade_base_path;
std::filesystem::path g_target_executable_path;

bool is_uwp_app() { return false; }
bool is_windows7() { return false; }
std::filesystem::path get_module_path(HMODULE module)
{
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    return length && length < ARRAYSIZE(path) ? std::filesystem::path(path, path + length) : std::filesystem::path();
}
std::filesystem::path get_system_path()
{
    wchar_t path[32768];
    const UINT length = GetSystemDirectoryW(path, ARRAYSIZE(path));
    return length && length < ARRAYSIZE(path) ? std::filesystem::path(path, path + length) : std::filesystem::path();
}
std::filesystem::path get_base_path(bool = false)
{
    wchar_t path[32768];
    DWORD length = GetEnvironmentVariableW(L"DXL_RESHADE_BASE_PATH", path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path))
        return std::filesystem::path(path, path + length);
    length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAYSIZE(path));
    if (length && length < ARRAYSIZE(path))
        return std::filesystem::path(path, path + length) / L"DXL" / L"ReShade";
    length = GetTempPathW(ARRAYSIZE(path), path);
    return std::filesystem::path(path, path + length) / L"DXL" / L"ReShade";
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Static CRT/TLS needs thread notifications. No LoadLibrary, hook
        // registration, module scanning, config lookup or disk writes here.
        g_module_handle = module;
        g_reshade_dll_path = get_module_path(module);
        g_target_executable_path = get_module_path(nullptr);
        g_reshade_base_path = get_base_path();
    }
    return TRUE;
}
extern "C" __declspec(dllexport) bool DxlReShadeOpenLog(const wchar_t *path)
{
    if (!path || !*path) return false;
    std::error_code ec;
    return reshade::log::open_log_file(std::filesystem::path(path), ec);
}
extern "C" __declspec(dllexport) void DxlReShadeFlushIni()
{
    reshade::ini_file::flush_cache(true);
}
extern "C" __declspec(dllexport) bool DxlReShadeSavePreset(reshade::api::effect_runtime *runtime)
{
    if (!runtime || !static_cast<reshade::runtime *>(runtime)->dxl_can_save_preset())
        return false;
    runtime->save_current_preset();
    return reshade::ini_file::flush_cache(true);
}
static FARPROC WINAPI DxlDelayLoad(unsigned reason, PDelayLoadInfo info)
{
    if (reason != dliNotePreLoadLibrary || _stricmp(info->szDll, "D3DCompiler_47.dll") != 0)
        return nullptr;
    return reinterpret_cast<FARPROC>(LoadLibraryExW(L"D3DCompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = DxlDelayLoad;
