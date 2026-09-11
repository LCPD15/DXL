// Synthetic loader chain only. Never loads game or NVIDIA code.
#include <windows.h>
#if defined(FIXTURE_UNITY)
extern "C" __declspec(dllexport) FARPROC UnityLookup(
    const wchar_t* pluginPath, const wchar_t* modelPath, const char* symbol) {
    const HMODULE plugin = LoadLibraryW(pluginPath);
    using Lookup = FARPROC (*)(const wchar_t*, const char*);
    const auto lookup = plugin ? reinterpret_cast<Lookup>(GetProcAddress(plugin, "StreamlineLookup")) : nullptr;
    return lookup ? lookup(modelPath, symbol) : nullptr;
}
#else
extern "C" __declspec(dllexport) FARPROC StreamlineLookup(const wchar_t* path, const char* symbol) {
    const HMODULE model = LoadLibraryExW(path, nullptr, 0);
    return model ? GetProcAddress(model, symbol) : nullptr;
}
#endif
