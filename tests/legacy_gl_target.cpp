// Bounded native OpenGL target for testing the complete injected DXL core.
// No hooks or mock model code is linked into this executable.
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#pragma comment(lib,"opengl32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")
LRESULT CALLBACK WindowProc(HWND w,UINT m,WPARAM p,LPARAM l) {
    if(m==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w,m,p,l);
}
int wmain(int argc,wchar_t** argv) {
    unsigned frames=900; bool resize=false; const wchar_t* dll=nullptr;
    for(int i=1;i<argc;++i) {
        if(wcsncmp(argv[i],L"--frames=",9)==0) frames=wcstoul(argv[i]+9,nullptr,10);
        else if(wcsncmp(argv[i],L"--load=",7)==0) dll=argv[i]+7;
        else if(wcscmp(argv[i],L"--resize")==0) resize=true;
        else { fwprintf(stderr,L"Unknown option: %ls\n",argv[i]); return 2; }
    }
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX); setvbuf(stdout,nullptr,_IONBF,0);
    WNDCLASSW wc{}; wc.style=CS_OWNDC; wc.lpfnWndProc=WindowProc;
    wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"DXLLegacyGLTarget";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassW(&wc);
    RECT rect{0,0,640,360}; AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"DXL OpenGL NR fixture",WS_OVERLAPPEDWINDOW,
        100,100,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window) return 3;
    HDC dc=GetDC(window); PIXELFORMATDESCRIPTOR pf{}; pf.nSize=sizeof(pf);pf.nVersion=1;
    pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
    pf.iPixelType=PFD_TYPE_RGBA;pf.cColorBits=32;
    const int format=ChoosePixelFormat(dc,&pf);
    if(!format||!SetPixelFormat(dc,format,&pf)) return 4;
    HGLRC gl=wglCreateContext(dc);
    if(!gl||!wglMakeCurrent(dc,gl)) return 5;
    printf("PID=%lu OpenGL=%s\n",GetCurrentProcessId(),glGetString(GL_VERSION));
    if(dll) { HMODULE mod=LoadLibraryW(dll); if(!mod) { printf("LoadLibrary failed=%lu\n",GetLastError());return 6; } }
    ShowWindow(window,SW_SHOWNOACTIVATE);
    unsigned glErrors=0, presents=0;
    for(unsigned frame=0;frame<frames;++frame) {
        MSG msg{}; bool quit=false;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT) { quit=true;break; }
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(quit) break;
        if(resize&&(frame==frames/3||frame==frames*2/3)) {
            const bool large=frame==frames/3; RECT next{0,0,large?800:640,large?450:360};
            AdjustWindowRect(&next,WS_OVERLAPPEDWINDOW,FALSE);
            SetWindowPos(window,nullptr,0,0,next.right-next.left,next.bottom-next.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        RECT client{}; GetClientRect(window,&client);
        glViewport(0,0,client.right,client.bottom);
        glDisable(GL_SCISSOR_TEST); glClearColor(.04f,.055f,.09f,1);glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,640,0,360,-1,1);
        glMatrixMode(GL_MODELVIEW);glLoadIdentity();
        glBegin(GL_QUADS);
        for(int y=0;y<18;++y)for(int x=0;x<32;++x) {
            const float wave=.5f+.5f*std::sin(float(x+y)*.32f+float(frame)*.025f);
            glColor3f(.15f+.45f*wave,.17f+.4f*float(y)/18,.12f+.32f*float(x)/32);
            const float left=float(x*20),bottom=float(y*20);
            glVertex2f(left,bottom);glVertex2f(left+18,bottom);glVertex2f(left+18,bottom+18);glVertex2f(left,bottom+18);
        }
        const float x=250+100*std::sin(float(frame)*.018f);
        glColor3f(.95f,.24f,.08f);glVertex2f(x,120);glVertex2f(x+70,120);glVertex2f(x+70,210);glVertex2f(x,210);
        glEnd();
        if(!SwapBuffers(dc)) printf("SwapBuffers failed=%lu\n",GetLastError());
        ++presents;
        for(GLenum error=glGetError();error!=GL_NO_ERROR;error=glGetError()) { ++glErrors;printf("GL error=0x%X at frame=%u\n",error,frame); }
        if(frame%120==0)printf("frame=%u size=%ldx%ld errors=%u\n",frame,client.right,client.bottom,glErrors);
        Sleep(16);
    }
    DestroyWindow(window); // exercises DXL window teardown before context destruction
    wglMakeCurrent(nullptr,nullptr);wglDeleteContext(gl);ReleaseDC(window,dc);
    printf("DONE presents=%u GL_errors=%u\n",presents,glErrors);return glErrors?1:0;
}
