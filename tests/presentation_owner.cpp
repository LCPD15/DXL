#include "../src/core/PresentationOwner.h"
#include <cstdio>
#include <cstdlib>
#pragma comment(lib,"user32.lib")
void Check(bool value,const char* message){if(!value){printf("FAIL %s\n",message);exit(1);}}
int main(){
    using namespace DXL;
    using Api=Ipc::GraphicsApi;
    HWND tiny=CreateWindowW(L"STATIC",L"DXL startup auxiliary",WS_POPUP,20,20,186,17,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(tiny!=nullptr,"startup auxiliary created");ShowWindow(tiny,SW_SHOWNOACTIVATE);
    RECT tinyClient{};
    Check(GetClientRect(tiny,&tinyClient) && tinyClient.right==186 && tinyClient.bottom==17,"startup auxiliary has exact 186x17 client");
    Check(!AcceptDxgiPresentation(Api::Unknown,tiny,186,17),"visible tiny startup surface cannot claim before main exists");
    Check(!AcceptDxgiPresentation(Api::Unknown,tiny,0,0),"automatic tiny dimensions cannot claim before main exists");
    Check(!AcceptDxgiPresentation(Api::Unknown,tiny,186,0),"partly automatic tiny dimensions rejected");
    Check(!AcceptDxgiPresentation(Api::Unknown,nullptr,0,0),"unknown zero dimensions rejected");
    Check(!AcceptDxgiPresentation(Api::Unknown,nullptr,64,360),"tiny width rejected without HWND");
    Check(!AcceptDxgiPresentation(Api::Unknown,nullptr,640,64),"tiny height rejected without HWND");
    HWND hidden=CreateWindowW(L"STATIC",L"DXL hidden fixture",WS_OVERLAPPEDWINDOW,20,20,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(hidden!=nullptr,"normal hidden window created");
    Check(AcceptDxgiPresentation(Api::Unknown,hidden,640,360),"normal standalone hidden fixture accepted");
    Check(!AcceptDxgiPresentation(Api::Unknown,hidden,8,8),"tiny hidden surface rejected even without primary");
    HWND main=CreateWindowW(L"STATIC",L"DXL primary ownership test",WS_OVERLAPPEDWINDOW,100,100,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(main!=nullptr,"primary window created");ShowWindow(main,SW_SHOWNOACTIVATE);
    Check(!AcceptDxgiPresentation(Api::Unknown,hidden,640,360),"hidden auxiliary rejected with visible primary");
    Check(!AcceptDxgiPresentation(Api::Unknown,tiny,186,17),"tiny auxiliary remains rejected with primary");
    Check(AcceptDxgiPresentation(Api::Unknown,main,640,360),"visible primary accepted");
    Check(!AcceptDxgiPresentation(Api::OpenGL,main,640,360),"selected OpenGL owns presentation");
    Check(!AcceptDxgiPresentation(Api::D3D9,main,640,360),"selected D3D9 owns presentation");
    Check(AcceptDxgiPresentation(Api::D3D12,main,640,360),"native DXGI remains accepted");
    Check(!AcceptDxgiPresentation(Api::D3D12,tiny,186,17),"tiny surface cannot displace selected D3D12 primary");
    HWND second=CreateWindowW(L"STATIC",L"DXL other visible window",WS_OVERLAPPEDWINDOW,100,100,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(second!=nullptr,"second visible window created");ShowWindow(second,SW_SHOWNOACTIVATE);
    Check(AcceptDxgiPresentation(Api::Unknown,main,0,0),"automatic DXGI dimensions use primary client size");
    DestroyWindow(second);
    DestroyWindow(main);
    Check(AcceptDxgiPresentation(Api::D3D12,hidden,640,360),"normal hidden fixture resumes after primary dies");
    Check(!AcceptDxgiPresentation(Api::D3D12,tiny,186,17),"tiny surface remains rejected after primary dies");
    DestroyWindow(hidden);
    DestroyWindow(tiny);puts("PASS presentation owner policy");return 0;
}
