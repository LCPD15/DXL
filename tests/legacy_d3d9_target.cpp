// Bounded native D3D9Ex target for complete-core NR injection validation.
#include <windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#pragma comment(lib,"d3d9.lib")
#pragma comment(lib,"user32.lib")
using Microsoft::WRL::ComPtr;
LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM p,LPARAM l) {
    if(m==WM_DESTROY) { PostQuitMessage(0);return 0; }
    return DefWindowProcW(w,m,p,l);
}
bool LoadCore(const wchar_t* dll,bool handshake) {
    HANDLE ready=nullptr;
    if(handshake) {
        wchar_t readyName[128]{};swprintf_s(readyName,L"Local\\DXL.Ready.%lu",GetCurrentProcessId());
        ready=CreateEventW(nullptr,TRUE,FALSE,readyName);
        if(!ready)return false;
    }
    const HMODULE core=LoadLibraryW(dll);
    if(!core) { printf("LoadLibrary failed=%lu\n",GetLastError());if(ready)CloseHandle(ready);return false; }
    if(ready) {
        const DWORD wait=WaitForSingleObject(ready,10000);CloseHandle(ready);
        if(wait!=WAIT_OBJECT_0) { printf("DXL ready handshake timed out=%lu\n",wait);return false; }
    }
    return true;
}
int wmain(int argc,wchar_t** argv) {
    unsigned frames=900,loadAt=0;bool resize=false,msaa=false;const wchar_t* dll=nullptr;
    for(int i=1;i<argc;++i) {
        if(wcsncmp(argv[i],L"--frames=",9)==0)frames=wcstoul(argv[i]+9,nullptr,10);
        else if(wcsncmp(argv[i],L"--load-at=",10)==0)loadAt=wcstoul(argv[i]+10,nullptr,10);
        else if(wcsncmp(argv[i],L"--load=",7)==0)dll=argv[i]+7;
        else if(wcscmp(argv[i],L"--resize")==0)resize=true;
        else if(wcscmp(argv[i],L"--msaa=4")==0)msaa=true;
        else { fwprintf(stderr,L"Unknown option: %ls\n",argv[i]);return 2; }
    }
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);setvbuf(stdout,nullptr,_IONBF,0);
    WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"DXLLegacyD3D9Target";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&wc);
    RECT rect{0,0,640,360};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"DXL D3D9Ex NR fixture",WS_OVERLAPPEDWINDOW,
        110,110,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)return 3;
    if(dll&&!loadAt&&!LoadCore(dll,true))return 4;
    ComPtr<IDirect3D9Ex> factory;HRESULT hr=Direct3DCreate9Ex(D3D_SDK_VERSION,&factory);
    if(FAILED(hr)) { printf("Create9Ex failed=0x%X\n",unsigned(hr));return 5; }
    D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.hDeviceWindow=window;p.SwapEffect=D3DSWAPEFFECT_DISCARD;
    p.BackBufferWidth=640;p.BackBufferHeight=360;p.BackBufferFormat=D3DFMT_X8R8G8B8;
    p.MultiSampleType=msaa?D3DMULTISAMPLE_4_SAMPLES:D3DMULTISAMPLE_NONE;
    p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    ComPtr<IDirect3DDevice9Ex> device;
    hr=factory->CreateDeviceEx(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,nullptr,&device);
    if(FAILED(hr)) { printf("CreateDeviceEx failed=0x%X\n",unsigned(hr));return 6; }
    printf("PID=%lu D3D9Ex native HAL MSAA=%u\n",GetCurrentProcessId(),msaa?4:1);ShowWindow(window,SW_SHOWNOACTIVATE);
    unsigned errors=0,presents=0;
    for(unsigned frame=0;frame<frames;++frame) {
        MSG msg{};bool quit=false;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT){quit=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(quit)break;
        if(dll&&loadAt&&frame==loadAt) {
            if(!LoadCore(dll,false))return 4;
            printf("Late core load at existing-device frame=%u\n",frame);
        }
        if(resize&&(frame==frames/3||frame==frames*2/3)) {
            const bool large=frame==frames/3;p.BackBufferWidth=large?800:640;p.BackBufferHeight=large?450:360;
            RECT next{0,0,LONG(p.BackBufferWidth),LONG(p.BackBufferHeight)};AdjustWindowRect(&next,WS_OVERLAPPEDWINDOW,FALSE);
            SetWindowPos(window,nullptr,0,0,next.right-next.left,next.bottom-next.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            hr=device->ResetEx(&p,nullptr);
            if(FAILED(hr)){++errors;printf("ResetEx failed=0x%X\n",unsigned(hr));break;}
        }
        device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(8,12,24),1,0);
        for(int y=0;y<18;++y)for(int x=0;x<32;++x) {
            const float wave=.5f+.5f*std::sin(float(x+y)*.32f+float(frame)*.025f);
            D3DRECT r{LONG(x*p.BackBufferWidth/32),LONG(y*p.BackBufferHeight/18),LONG((x+1)*p.BackBufferWidth/32-2),LONG((y+1)*p.BackBufferHeight/18-2)};
            device->Clear(1,&r,D3DCLEAR_TARGET,D3DCOLOR_XRGB(int(40+115*wave),44+y*5,30+x*3),1,0);
        }
        const LONG x=LONG((250+100*std::sin(float(frame)*.018f))*p.BackBufferWidth/640);
        D3DRECT r{x,LONG(p.BackBufferHeight/3),x+LONG(p.BackBufferWidth/9),LONG(p.BackBufferHeight*2/3)};
        device->Clear(1,&r,D3DCLEAR_TARGET,D3DCOLOR_XRGB(244,64,20),1,0);
        hr=device->PresentEx(nullptr,nullptr,nullptr,nullptr,0);++presents;
        if(FAILED(hr)){++errors;printf("PresentEx failed=0x%X frame=%u\n",unsigned(hr),frame);}
        if(frame%120==0)printf("frame=%u size=%ux%u errors=%u\n",frame,p.BackBufferWidth,p.BackBufferHeight,errors);
        Sleep(16);
    }
    DestroyWindow(window);device.Reset();factory.Reset();
    printf("DONE presents=%u D3D9_errors=%u\n",presents,errors);return errors?1:0;
}
