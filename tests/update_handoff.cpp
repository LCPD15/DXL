#include <windows.h>
#include <filesystem>
#include <fstream>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    const auto root=std::filesystem::path(path).parent_path();
    {std::ofstream file(root/L"starts.txt",std::ios::app);file << GetCurrentProcessId() << '\n';}
    while (!std::filesystem::exists(root/L"exit.flag")) Sleep(50);
    return 0;
}
