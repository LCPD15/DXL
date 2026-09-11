// Complete-core UI-only regression. Toggle the real panel through production
// IPC; no keyboard/mouse input is synthesized. Read only this target's pixels.
#include "../src/common/IpcClient.h"
#include <GL/gl.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <cstdlib>
#pragma comment(lib,"opengl32.lib")
#pragma comment(lib,"d3d9.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")
using Microsoft::WRL::ComPtr;
static constexpr unsigned W=800,H=600;
static LONG CALLBACK DiagnoseException(EXCEPTION_POINTERS* info) {
    if(info->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
    printf("AV code=0x%lX address=%p access=%llu target=%p\n",info->ExceptionRecord->ExceptionCode,
        info->ExceptionRecord->ExceptionAddress,info->ExceptionRecord->ExceptionInformation[0],
        reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    void* frames[24]{};const auto count=CaptureStackBackTrace(0,24,frames,nullptr);
    for(USHORT i=0;i<count;++i){
        HMODULE module=nullptr;wchar_t path[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[i]),&module);
        if(module)GetModuleFileNameW(module,path,MAX_PATH);
        printf("  stack[%u]=%p module=%ls+0x%llX\n",i,frames[i],path,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])-reinterpret_cast<uintptr_t>(module)));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM p,LPARAM l) {
    if(m==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(w,m,p,l);
}
static unsigned Changed(const unsigned char* bytes,unsigned pitch,bool bgra) {
    unsigned count=0;
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
        const auto* p=bytes+y*pitch+x*4;
        if(abs(int(p[bgra?2:0])-12)>2||abs(int(p[1])-24)>2||abs(int(p[bgra?0:2])-40)>2)++count;
    }
    return count;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    const bool baseline=wcscmp(argv[1],L"--baseline")==0;
    const bool d3d9=wcscmp(argv[2],L"D3D9")==0;
    setvbuf(stdout,nullptr,_IONBF,0);
    AddVectoredExceptionHandler(0,DiagnoseException);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    WNDCLASSW wc{};wc.style=CS_OWNDC;wc.lpfnWndProc=WindowProc;
    wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DXLLegacyUiOnlyFixture";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&wc);
    RECT wr{0,0,W,H};AdjustWindowRect(&wr,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"DXL UI-only regression (no NR)",WS_OVERLAPPEDWINDOW,
        120,120,wr.right-wr.left,wr.bottom-wr.top,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)return 3;
    wchar_t eventName[128]{};swprintf_s(eventName,L"Local\\DXL.Ready.%lu",GetCurrentProcessId());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,eventName);
    if(!ready||(!baseline&&(!LoadLibraryW(argv[1])||WaitForSingleObject(ready,15000)!=WAIT_OBJECT_0)))return 4;
    CloseHandle(ready);
    HDC dc=nullptr;HGLRC gl=nullptr;
    ComPtr<IDirect3D9Ex> factory;ComPtr<IDirect3DDevice9Ex> device;
    ComPtr<IDirect3DSurface9> backbuffer,readback;
    std::vector<unsigned char> pixels(W*H*4);
    if(d3d9){
        if(FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION,&factory)))return 5;
        D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.hDeviceWindow=window;p.SwapEffect=D3DSWAPEFFECT_COPY;
        p.BackBufferWidth=W;p.BackBufferHeight=H;p.BackBufferFormat=D3DFMT_X8R8G8B8;
        p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
        if(FAILED(factory->CreateDeviceEx(0,D3DDEVTYPE_HAL,window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,nullptr,&device)))return 6;
        if(FAILED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer))||
           FAILED(device->CreateOffscreenPlainSurface(W,H,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr)))return 7;
    }else{
        dc=GetDC(window);PIXELFORMATDESCRIPTOR p{};p.nSize=sizeof(p);p.nVersion=1;
        p.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;p.iPixelType=PFD_TYPE_RGBA;p.cColorBits=32;
        const int format=ChoosePixelFormat(dc,&p);
        if(!format||!SetPixelFormat(dc,format,&p))return 8;
        gl=wglCreateContext(dc);if(!gl||!wglMakeCurrent(dc,gl))return 9;
    }
    ShowWindow(window,SW_SHOWNORMAL);
    printf("PID=%lu API=%s visible=%u IPC panel toggles; no synthetic input\n",GetCurrentProcessId(),d3d9?"D3D9Ex":"OpenGL",IsWindowVisible(window));
    DXL::StatusView status;DXL::Ipc::Status last{};
    unsigned errors=0,baseChanged=~0u,openChanged=0,closedChanged=~0u,frames=0;
    bool opened=false,closed=false,clipObserved=false,clipRestored=false;
    RECT initialClip{};const bool canReadClip=GetClipCursor(&initialClip)!=FALSE;
    const auto start=GetTickCount64();
    while(GetTickCount64()-start<14500){
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT)return 10;TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        const auto ms=GetTickCount64()-start;
        if(ms>=8500&&!opened){opened=baseline||DXL::SendCommand(GetCurrentProcessId(),DXL::Ipc::CommandId::TogglePerfWindow);if(!opened)++errors;}
        if(ms>=11500&&opened&&!closed){closed=baseline||DXL::SendCommand(GetCurrentProcessId(),DXL::Ipc::CommandId::TogglePerfWindow);if(!closed)++errors;}
        if(d3d9){
            if(FAILED(device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(12,24,40),1,0))||
                FAILED(device->PresentEx(nullptr,nullptr,nullptr,nullptr,0)))++errors;
        }else{
            glViewport(0,0,W,H);glDisable(GL_SCISSOR_TEST);
            glClearColor(12.f/255,24.f/255,40.f/255,1);glClear(GL_COLOR_BUFFER_BIT);
            if(!SwapBuffers(dc))++errors;
        }
        ++frames;
        const bool sample=(ms>=8000&&ms<8250&&baseChanged==~0u)||
            (ms>=10500&&ms<11000&&openChanged==0)||(ms>=13500&&closedChanged==~0u);
        if(sample){
            unsigned changed=0;
            if(d3d9){
                D3DLOCKED_RECT map{};
                if(FAILED(device->GetRenderTargetData(backbuffer.Get(),readback.Get()))||
                    FAILED(readback->LockRect(&map,nullptr,D3DLOCK_READONLY))){++errors;break;}
                changed=Changed(static_cast<const unsigned char*>(map.pBits),map.Pitch,true);readback->UnlockRect();
            }else{
                glReadBuffer(GL_FRONT);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                changed=Changed(pixels.data(),W*4,false);glReadBuffer(GL_BACK);
            }
            if(ms<8500)baseChanged=changed;else if(ms<11500)openChanged=changed;else closedChanged=changed;
            RECT clip{};const bool gotClip=GetClipCursor(&clip)!=FALSE;
            if(ms>8500&&ms<11500&&canReadClip&&gotClip&&!EqualRect(&clip,&initialClip)&&GetForegroundWindow()==window)clipObserved=true;
            if(ms>11500&&canReadClip&&gotClip&&EqualRect(&clip,&initialClip))clipRestored=true;
            printf("sample ms=%llu changedPixels=%u foreground=%u clipReadable=%u clip=%ld,%ld,%ld,%ld\n",
                ms,changed,GetForegroundWindow()==window,gotClip,clip.left,clip.top,clip.right,clip.bottom);
        }
        if(!d3d9)for(GLenum error=glGetError();error!=GL_NO_ERROR;error=glGetError()){++errors;printf("GL error=0x%X\n",error);}
        if(!status.IsOpen())status.Open(GetCurrentProcessId());status.Read(last);
        Sleep(16);
    }
    const bool passed=opened&&closed&&baseChanged==0&&(baseline?openChanged==0:openChanged>10000)&&closedChanged==0&&
        errors==0&&last.nrEvaluateCount==0&&last.nrEvaluateFailures==0&&(!clipObserved||clipRestored);
    printf("LEGACY_UI_RESULT passed=%u frames=%u baseline=%u open=%u closed=%u nr=%llu errors=%u cursorConstrained=%u cursorRestored=%u\n",
        passed,frames,baseChanged,openChanged,closedChanged,last.nrEvaluateCount,errors,clipObserved,clipObserved&&clipRestored);
    puts("teardown DestroyWindow");DestroyWindow(window);
    puts("teardown backbuffer/readback");backbuffer.Reset();readback.Reset();
    puts("teardown D3D9 device/factory");device.Reset();factory.Reset();
    if(gl){wglMakeCurrent(nullptr,nullptr);wglDeleteContext(gl);ReleaseDC(window,dc);}
    puts("teardown complete");return passed?0:1;
}
