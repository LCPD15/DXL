#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#pragma comment(lib,"shell32.lib")

int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    int argc=0;
    auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv || argc!=2) { if (argv) LocalFree(argv); return 2; }
    const std::wstring request=argv[1]; LocalFree(argv);
    if (request.find(L'"')!=std::wstring::npos) return 2;
    wchar_t exe[32768]{},system[32768]{};
    if (!GetModuleFileNameW(nullptr,exe,32768) || !GetSystemDirectoryW(system,32768)) return 3;
    const auto script=std::filesystem::path(exe).parent_path()/L"updater"/L"Update.ps1";
    const auto powershell=std::filesystem::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    std::wstring cmd=L"\""+powershell.wstring()+L"\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script.wstring()+L"\" -Request \""+request+L"\"";
    STARTUPINFOW si{sizeof(si)}; si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(powershell.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) return 4;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD code=1; GetExitCodeProcess(pi.hProcess,&code); CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
