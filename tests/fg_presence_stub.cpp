// Test-only module-presence fixture. Never a runtime dependency.
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
