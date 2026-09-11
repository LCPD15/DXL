#include <windows.h>

// Benign local test DLL only: no game hooks, initialization worker or IPC.
extern "C" __declspec(dllexport) int InjectionDiagnosticFixture() { return 42; }
BOOL WINAPI DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
