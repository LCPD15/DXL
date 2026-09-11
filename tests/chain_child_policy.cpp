#include "../src/core/ChainInject.cpp"
#include <filesystem>
#include <stdexcept>
#include <cstdio>
using namespace DXL;
static void Check(bool yes,const char* message) { if (!yes) throw std::runtime_error(message); }
int wmain(int argc,wchar_t** argv) try {
    if (argc>1 && wcscmp(argv[1],L"--child")==0) return 41;
    for (const auto* path : {L"crashpad_handler.exe",L"C:\\Game\\CRASHPAD_HANDLER.EXE",
        L"C:/Game/CrashReportClient.exe",L"CrashReportClientEditor.exe",L"UnityCrashHandler32.exe",
        L"UnityCrashHandler64.exe",L"WerFault.exe",L"WerFaultSecure.exe"})
        Check(IsDedicatedCrashReporter(path),"dedicated crash reporter missed");
    for (const auto* path : {L"",L"RDR2.exe",L"yysls.exe",L"renderer.exe",L"game_helper.exe",
        L"UnityPlayer.exe",L"game-Win64-Shipping.exe",L"C:/crashpad_handler.exe/RDR2.exe",
        L"my_crashpad_handler.exe",L"crashpad_handler.exe.game.exe",L"CrashReportClientGame.exe",
        L"C:/Game/RDR2.exe --crashpad_handler.exe"})
        Check(!IsDedicatedCrashReporter(path),"legitimate renderer/launcher was excluded");
    wchar_t ownPath[32768]{}; GetModuleFileNameW(nullptr,ownPath,_countof(ownPath));
    const auto helper=std::filesystem::path(ownPath).parent_path()/L"crashpad_handler.exe";
    Check(CopyFileW(ownPath,helper.c_str(),FALSE)!=0,"create synthetic helper");
    // No DLL path is configured, so even a broken policy cannot inject code.
    Check(!g_selfDllPath[0],"test unexpectedly configured injection");
    for (const bool addedSuspend : {true,false}) {
        std::wstring command=L"\""+helper.wstring()+L"\" --child";
        STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
        Check(CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,
            nullptr,nullptr,&si,&pi)!=0,"create suspended helper");
        HandleChild(pi,addedSuspend,L"synthetic regression");
        wchar_t markerName[128]{};
        _snwprintf_s(markerName,_TRUNCATE,L"%s.%lu",Ipc::EARLY_INJECT_EVENT_BASE,pi.dwProcessId);
        const HANDLE marker=OpenEventW(SYNCHRONIZE,FALSE,markerName);
        Check(marker==nullptr,"crash helper entered injection setup");
        if (!addedSuspend) {
            Check(WaitForSingleObject(pi.hProcess,100)==WAIT_TIMEOUT,"caller-requested suspension was removed");
            Check(ResumeThread(pi.hThread)!=DWORD(-1),"resume caller-owned test suspension");
        }
        Check(WaitForSingleObject(pi.hProcess,3000)==WAIT_OBJECT_0,"skipped helper was left suspended");
        DWORD code=0; GetExitCodeProcess(pi.hProcess,&code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        Check(code==41 && ChainInjectCount()==0,"helper behavior or injection count changed");
    }
    puts("PASS exact crash reporter identities, legitimate renderer children, real child image lookup, no injection setup, normal/caller-suspended child behavior");
    return 0;
} catch(const std::exception& e) { printf("FAIL %s\n",e.what()); return 1; }
