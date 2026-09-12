#include <windows.h>
#include <filesystem>
#include <fstream>
#ifndef DXL_TEST_UPDATE_CLIENT
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    const auto root=std::filesystem::path(path).parent_path();
    {std::ofstream file(root/L"starts.txt",std::ios::app);file << GetCurrentProcessId() << '\n';}
    while (!std::filesystem::exists(root/L"exit.flag")) Sleep(50);
    return 0;
}
#else
#include <shellapi.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <stdexcept>
#include <cstdio>

static std::filesystem::path testRoot;
static HWND g_window=nullptr;
static std::atomic<int> g_uiLang{2};
static unsigned closeRequests=0,doneMessages=0;
static std::string lastResponse;
static std::filesystem::path ConfigDir() { return testRoot/L"config"; }
static std::filesystem::path ExeDir() { return testRoot/L"install"; }
static std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
    std::wstring out(count,L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),out.data(),count);
    return out;
}
static std::string JsonQuoted(const std::wstring& text) {
    const int count=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string utf8(count,'\0'),out="\"";
    WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),utf8.data(),count,nullptr,nullptr);
    for (char c:utf8) { if(c=='\\' || c=='"') out+='\\'; out+=c; }
    return out+'"';
}
static bool WriteFileUtf8(const std::filesystem::path& path,const std::string& text) {
    std::ofstream file(path,std::ios::binary); file<<text; return bool(file);
}
static std::string ReadFileUtf8(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
static void PostToUi(const std::string& text) {lastResponse=text;++doneMessages;}
#include "../src/ui/UpdateClient.h"

static LRESULT CALLBACK TestWindowProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_CLOSE) {++closeRequests;return 0;} // Deliberately keep the parent alive.
    if (message==WM_APP_UPDATE_DONE) {
        std::unique_ptr<std::string> response(reinterpret_cast<std::string*>(lparam));
        g_updateBusy=false;
        if(response) PostToUi(*response);
        return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
int wmain(int argc,wchar_t** argv) try {
    wchar_t image[32768]{}; GetModuleFileNameW(nullptr,image,32768);
    const std::filesystem::path self=image;
    if(self.filename()==L"DXL-update.exe") {
        if(argc!=2) return 2;
        const auto job=std::filesystem::path(argv[1]).parent_path();
        WriteFileUtf8(job/L"ready.json","{\"state\":\"ready\"}");
        // Model the worker's parent-exit timeout without slowing this client test.
        Sleep(1200);
        WriteFileUtf8(job/L"result.json","{\"state\":\"error\",\"detail\":\"DXL did not exit\"}");
        return 0;
    }
    if(argc!=2) throw std::runtime_error("Expected an isolated fixture directory");
    testRoot=std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(ExeDir()/L"updater");
    std::filesystem::copy_file(self,ExeDir()/L"DXL-update.exe");
    WriteFileUtf8(ExeDir()/L"updater"/L"Update.ps1","fixture");
    WriteFileUtf8(ExeDir()/L"updater"/L"UpdateEngine.psm1","fixture");
    WNDCLASSW wc{};wc.lpfnWndProc=TestWindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DXL.UpdateClient.Fixture";
    if(!RegisterClassW(&wc)) throw std::runtime_error("RegisterClass failed");
    g_window=CreateWindowW(wc.lpszClassName,L"Update client fixture",0,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
    if(!g_window) throw std::runtime_error("CreateWindow failed");
    StartUpdateAction("install","0.5");
    const auto deadline=GetTickCount64()+10000;
    while(GetTickCount64()<deadline && !doneMessages) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&message);
        Sleep(10);
    }
    DestroyWindow(g_window);g_window=nullptr;
    if(closeRequests!=1) throw std::runtime_error("Ready must request close exactly once");
    if(doneMessages!=1 || g_updateBusy || lastResponse.find("DXL did not exit")==std::string::npos)
        throw std::runtime_error("Worker timeout was lost or update busy state was not cleared");
    std::puts("PASS UpdateClient: ready requests close once; a parent refusing to exit still receives the worker error and clears busy");
    return 0;
} catch(const std::exception& error) {
    if(g_window) DestroyWindow(g_window);
    std::printf("FAIL UpdateClient: %s\n",error.what());return 1;
}
#endif
