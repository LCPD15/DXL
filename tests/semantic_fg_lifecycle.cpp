// Reproduce enabling semantics after the FG Present guard suppresses RunFilters.
// The fake FG module only triggers detection; it performs no frame generation.
#define DXL_TEST_FRAME_COUNT 6
#define wmain OriginalFixtureMain
#include "../src/testapp/testapp.cpp"
#undef wmain
#include "../src/common/IpcClient.h"
#include <atomic>
#include <thread>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    std::atomic<bool> finished{false},passed{false};
    const DWORD tid=GetCurrentThreadId();
    auto ownWindow=[tid] {
        HWND hwnd=nullptr;
        EnumThreadWindows(tid,[](HWND h,LPARAM p)->BOOL { *reinterpret_cast<HWND*>(p)=h; return FALSE; },reinterpret_cast<LPARAM>(&hwnd));
        return hwnd;
    };
    std::thread monitor([&] {
        DXL::StatusView view;
        DXL::Ipc::Status state{};
        const auto start=GetTickCount64();
        bool activated=false;
        ULONGLONG activatedAt=0;
        while(!finished && GetTickCount64()-start<25000) {
            if(auto hwnd=ownWindow()) ShowWindow(hwnd,SW_HIDE);
            if(!view.IsOpen()) view.Open(GetCurrentProcessId());
            if(view.Read(state) && !activated && state.nrEvaluateCount>=10) {
                if(GetModuleHandleW(L"nvinfer_lean_11.dll")) break;
                if(!LoadLibraryW(L"sl.dlss_g.dll")) break;
                // Let the next Present observe the live six-buffer FG chain.
                Sleep(250);
                wchar_t corePath[32768]{};
                if(!GetModuleFileNameW(GetModuleHandleW(L"DXL-core.dll"),corePath,32768)) break;
                const auto profile=std::filesystem::path(corePath).parent_path()/L"profiles"/L"semantic-fg-target.exe.json";
                FILE* file=nullptr;
                if(_wfopen_s(&file,profile.c_str(),L"wb") || !file) break;
                const char settings[]=R"({"srEnable":false,"dlss5Enable":true,"masterEnabled":true,"diagEavesdrop":true,"nrAutoRoute":true,"nrOpticalFlow":false,"nrSemanticMask":true,"nrSemOn":1})";
                fwrite(settings,1,sizeof(settings)-1,file); fclose(file);
                if(!DXL::SendCommand(GetCurrentProcessId(),DXL::Ipc::CommandId::ReloadSettings)) break;
                activated=true; activatedAt=GetTickCount64();
                puts("FG_SEMANTIC activated after six-buffer FG detection; no native Evaluate fixture");
            }
            if(activated && GetTickCount64()-activatedAt>2500) {
                const bool loaded=GetModuleHandleW(L"nvinfer_lean_11.dll")!=nullptr;
#ifdef DXL_WITHOUT_SEMANTIC
                passed=!loaded && view.Read(state) && state.nrParamSemanticMask==0;
#else
                passed=loaded;
#endif
                printf("FG_SEMANTIC runtimeLoaded=%u effectiveMask=%u\n",loaded?1u:0u,state.nrParamSemanticMask);
                break;
            }
            Sleep(40);
        }
        printf("FG_SEMANTIC passed=%u pid=%lu\n",passed.load()?1u:0u,GetCurrentProcessId());
        if(auto hwnd=ownWindow()) PostMessageW(hwnd,WM_CLOSE,0,0);
    });
    const int code=OriginalFixtureMain(argc,argv);
    finished=true; monitor.join();
    return code?code:passed?0:2;
}
