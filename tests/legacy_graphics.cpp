// Real GL/D3D9 capture, channel order, state restoration and lifetime fixture.
// Include the implementation to inspect the post-filter backbuffer before the
// native Present discards it. Public-hook dispatch is also exercised separately.
#include "../src/core/LegacyGraphics.cpp"
#include <cstdio>
#include <stdexcept>
#include <source_location>
#include <d3d9on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#pragma comment(lib,"d3dcompiler.lib")

using namespace DXL;
namespace {
bool effect = true;
unsigned processedFrames = 0;
unsigned inputChecks = 0;
unsigned beginFrames = 0;
bool inputOkay = true;
UINT expectedWidth = 320, expectedHeight = 192;
void Check(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
void HR(HRESULT hr, const std::source_location& loc = std::source_location::current()) {
    if (FAILED(hr)) { printf("HRESULT %08X at line %u\n", unsigned(hr), loc.line()); throw std::runtime_error("D3D operation failed"); }
}
bool Begin(Ipc::GraphicsApi, HWND) noexcept { ++beginFrames; return effect; }
bool Process(ID3D11Device* device, ID3D11Texture2D* image, Ipc::GraphicsApi, HWND) noexcept {
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{}; image->GetDesc(&desc);
    if (desc.Width != expectedWidth || desc.Height != expectedHeight) inputOkay = false;
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return false;
    context->CopyResource(staging.Get(), image);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) return false;
    auto* top = static_cast<unsigned char*>(map.pData) + (desc.Height / 4) * map.RowPitch + (desc.Width / 2) * 4;
    auto* bottom = static_cast<unsigned char*>(map.pData) + (desc.Height * 3 / 4) * map.RowPitch + (desc.Width / 2) * 4;
    // Input top red / bottom blue must survive GL inversion and D3D9 swizzle.
    inputOkay &= top[0] > 240 && top[1] < 10 && top[2] < 10 &&
        bottom[0] < 10 && bottom[1] < 10 && bottom[2] > 240;
    context->Unmap(staging.Get(),0); ++inputChecks;
    std::vector<unsigned> output(size_t(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x)
        output[size_t(y) * desc.Width + x] = y < desc.Height / 2 ? 0xff00ffffu : 0xffffff00u;
    context->UpdateSubresource(image,0,nullptr,output.data(),desc.Width*4,0);
    ++processedFrames; return true;
}
HWND Window(UINT w, UINT h) {
    RECT rect{0,0,LONG(w),LONG(h)}; AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0,L"STATIC",L"DXL legacy graphics fixture",WS_OVERLAPPEDWINDOW,
        60,60,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window != nullptr,"window creation");
    ShowWindow(window, SW_SHOWNOACTIVATE); return window;
}
void Pump() { MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }

void TestGL() {
    HWND window = Window(expectedWidth, expectedHeight);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd{}; pfd.nSize=sizeof(pfd); pfd.nVersion=1;
    pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
    pfd.iPixelType=PFD_TYPE_RGBA; pfd.cColorBits=32;
    Check(SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd),"pixel format");
    HGLRC gl=wglCreateContext(dc); Check(gl && wglMakeCurrent(dc,gl),"GL context");
    printf("OpenGL %s\n",glGetString(GL_VERSION));
    Check(InstallLegacyGraphicsHooks({Begin,Process}),"GL hooks");
    GL::Functions fn; Check(fn.Load(),"GL 3.2 functions");
    GLuint gameFbo{}, gameTexture{}, gamePbo{}, unpackPbo{};
    fn.GenFramebuffers(1,&gameFbo); glGenTextures(1,&gameTexture);
    glBindTexture(GL_TEXTURE_2D,gameTexture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    fn.BindFramebuffer(GL::ReadFbo,gameFbo);
    fn.FramebufferTexture2D(GL::ReadFbo,GL::Color0,GL_TEXTURE_2D,gameTexture,0);
    fn.GenBuffers(1,&gamePbo); fn.GenBuffers(1,&unpackPbo);
    fn.BindBuffer(GL::PackBuffer,gamePbo); fn.BufferData(GL::PackBuffer,4096,nullptr,GL::StreamRead);
    fn.BindBuffer(GL::UnpackBuffer,unpackPbo); fn.BufferData(GL::UnpackBuffer,4096,nullptr,GL::StreamRead);
    unsigned displayed = 0;
    for (unsigned frame=0;frame<55;++frame) {
        Pump();
        if(frame==28) {
            expectedWidth=384; expectedHeight=224; RECT size{0,0,LONG(expectedWidth),LONG(expectedHeight)};
            AdjustWindowRect(&size,WS_OVERLAPPEDWINDOW,FALSE);
            SetWindowPos(window,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        fn.BindFramebuffer(GL::DrawFbo,0); glDrawBuffer(GL_BACK);
        glDisable(GL::FramebufferSrgb); glDisable(GL_SCISSOR_TEST);
        glClearColor(0,0,1,1); glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST); glScissor(0,expectedHeight/2,expectedWidth,expectedHeight/2);
        glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        fn.BindFramebuffer(GL::ReadFbo,gameFbo); glReadBuffer(GL::Color0);
        fn.BindFramebuffer(GL::DrawFbo,gameFbo); glDrawBuffer(GL::Color0);
        fn.BindBuffer(GL::PackBuffer,gamePbo); fn.BindBuffer(GL::UnpackBuffer,unpackPbo);
        glBindTexture(GL_TEXTURE_2D,gameTexture);
        glPixelStorei(GL_PACK_ALIGNMENT,8); glPixelStorei(GL_PACK_ROW_LENGTH,77);
        glPixelStorei(GL_PACK_SKIP_ROWS,3); glPixelStorei(GL_PACK_SKIP_PIXELS,5);
        glPixelStorei(GL_UNPACK_ALIGNMENT,8); glPixelStorei(GL_UNPACK_ROW_LENGTH,79);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,7); glPixelStorei(GL_UNPACK_SKIP_PIXELS,11);
        glEnable(GL::FramebufferSrgb);
        Check(glGetError()==GL_NO_ERROR,"game GL setup");
        // Full public hook dispatch every fifth frame; other frames inspect
        // the exact image immediately before the driver's SwapBuffers call.
        if (frame%5==0) SwapBuffers(dc);
        else GL::Find(gl)->Present(dc,window);
        const GLenum error=glGetError();
        if(error)printf("GL error 0x%X at frame %u\n",error,frame);
        Check(error==GL_NO_ERROR,"GL bridge error");
        GLint value{};
        glGetIntegerv(GL::ReadFboBinding,&value); Check(value==GLint(gameFbo),"GL read FBO restored");
        glGetIntegerv(GL::DrawFboBinding,&value); Check(value==GLint(gameFbo),"GL draw FBO restored");
        glGetIntegerv(GL::PackBinding,&value); Check(value==GLint(gamePbo),"GL pack PBO restored");
        glGetIntegerv(GL::UnpackBinding,&value); Check(value==GLint(unpackPbo),"GL unpack PBO restored");
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&value); Check(value==GLint(gameTexture),"GL texture restored");
        glGetIntegerv(GL_PACK_ROW_LENGTH,&value); Check(value==77,"GL pack rows restored");
        glGetIntegerv(GL_PACK_SKIP_ROWS,&value); Check(value==3,"GL pack skip rows restored");
        glGetIntegerv(GL_PACK_SKIP_PIXELS,&value); Check(value==5,"GL pack skip pixels restored");
        glGetIntegerv(GL_UNPACK_ROW_LENGTH,&value); Check(value==79,"GL unpack rows restored");
        glGetIntegerv(GL_UNPACK_SKIP_ROWS,&value); Check(value==7,"GL unpack skip rows restored");
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&value); Check(value==11,"GL unpack skip pixels restored");
        Check(glIsEnabled(GL_SCISSOR_TEST)&&glIsEnabled(GL::FramebufferSrgb),"GL enables restored");
        if (frame%5 && GL::Find(gl)->textureValid) {
            fn.BindFramebuffer(GL::ReadFbo,0); glReadBuffer(GL_BACK); fn.BindBuffer(GL::PackBuffer,0);
            glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
            glPixelStorei(GL_PACK_SKIP_ROWS,0); glPixelStorei(GL_PACK_SKIP_PIXELS,0);
            unsigned char top[4]{},bottom[4]{};
            glReadPixels(expectedWidth/2,expectedHeight*3/4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,top);
            glReadPixels(expectedWidth/2,expectedHeight/4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,bottom);
            Check(top[0]>240&&top[1]>240&&top[2]<10,"GL output top orientation/channels");
            Check(bottom[0]<10&&bottom[1]>240&&bottom[2]>240,"GL output bottom orientation/channels");
            ++displayed;
        }
        originalSwap(dc); Sleep(5);
    }
    Check(displayed>25 && processedFrames>30 && inputOkay,"GL capture/process/output");
    const auto before=GetLegacyGraphicsStats(); effect=false;
    for(unsigned i=0;i<12;++i) SwapBuffers(dc);
    const auto after=GetLegacyGraphicsStats();
    Check(before.captures==after.captures&&before.processed==after.processed&&before.displayed==after.displayed,"disabled GL has no transfer work");
    effect=true;
    // A second context presenting the same window must be forwarded without
    // changing the selected context's ring or NR history.
    HGLRC auxiliary=wglCreateContext(dc); Check(auxiliary&&wglMakeCurrent(dc,auxiliary),"auxiliary GL context");
    const auto beforeAux=GetLegacyGraphicsStats();
    SwapBuffers(dc);
    const auto afterAux=GetLegacyGraphicsStats();
    Check(beforeAux.captures==afterAux.captures&&beforeAux.processed==afterAux.processed,"auxiliary GL context preserved");
    Check(wglDeleteContext(auxiliary)&&wglMakeCurrent(dc,gl),"auxiliary context deletion");
    // Context deletion must remove ownership and never issue GL calls on a
    // different context. Another context at the same window can be created.
    fn.BindFramebuffer(GL::ReadFbo,0); fn.BindFramebuffer(GL::DrawFbo,0);
    fn.BindBuffer(GL::PackBuffer,0); fn.BindBuffer(GL::UnpackBuffer,0);
    fn.DeleteFramebuffers(1,&gameFbo); glDeleteTextures(1,&gameTexture);
    fn.DeleteBuffers(1,&gamePbo); fn.DeleteBuffers(1,&unpackPbo);
    Check(wglDeleteContext(gl),"delete tracked GL context");
    Check(GL::states.empty(),"GL context resources retired");
    HGLRC second=wglCreateContext(dc); Check(second&&wglMakeCurrent(dc,second),"second GL context");
    effect=false; SwapBuffers(dc); Check(GL::states.size()==1,"new context tracked independently");
    Check(wglDeleteContext(second),"second context deletion");
    ReleaseDC(window,dc); DestroyWindow(window);
    printf("PASS OpenGL: %u processed, %u output checks, exact state restore, resize, disabled gate, auxiliary context, context recreation; capture %.3f ms\n",processedFrames,displayed,before.lastCaptureMs);
}

void Test9(bool ex, bool on12 = false, bool late = false) {
    HWND window=Window(expectedWidth,expectedHeight);
    D3DPRESENT_PARAMETERS present{}; present.Windowed=TRUE; present.hDeviceWindow=window;
    present.SwapEffect=D3DSWAPEFFECT_DISCARD; present.BackBufferWidth=expectedWidth;
    present.BackBufferHeight=expectedHeight; present.BackBufferFormat=D3DFMT_X8R8G8B8;
    ComPtr<IDirect3D9> factory; ComPtr<IDirect3D9Ex> factoryEx;
    ComPtr<IDirect3DDevice9> device; ComPtr<IDirect3DDevice9Ex> deviceEx;
    ComPtr<ID3D12Device> on12Device;
    if(on12) {
        auto create=reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(LoadLibraryW(L"d3d12.dll"),"D3D12CreateDevice"));
        Check(create!=nullptr,"D3D12 entry point"); HR(create(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&on12Device)));
    }
    PollLegacyGraphicsHooks();
    if(ex) {
        if (on12) { D3D9ON12_ARGS args{}; args.Enable9On12=TRUE; args.pD3D12Device=on12Device.Get();
            auto create=reinterpret_cast<PFN_Direct3DCreate9On12Ex>(GetProcAddress(GetModuleHandleW(L"d3d9.dll"),"Direct3DCreate9On12Ex"));
            Check(create!=nullptr,"D3D9On12 entry point"); HR(create(D3D_SDK_VERSION,&args,1,&factoryEx)); TrackFactory(factoryEx.Get()); }
        else HR(Direct3DCreate9Ex(D3D_SDK_VERSION,&factoryEx));
        HR(factoryEx->CreateDeviceEx(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&present,nullptr,&deviceEx)); device=deviceEx; }
    else {
        if (on12) { D3D9ON12_ARGS args{}; args.Enable9On12=TRUE; args.pD3D12Device=on12Device.Get();
            auto create=reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(GetModuleHandleW(L"d3d9.dll"),"Direct3DCreate9On12"));
            Check(create!=nullptr,"D3D9On12 entry point"); factory.Attach(create(D3D_SDK_VERSION,&args,1)); TrackFactory(factory.Get()); }
        else factory.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        Check(bool(factory),"D3D9 factory");
        for (UINT a=0; a<factory->GetAdapterCount(); ++a) { D3DADAPTER_IDENTIFIER9 id{};
            factory->GetAdapterIdentifier(a,0,&id); printf("D3D9 adapter %u: %s\n",a,id.Description); }
        HR(factory->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&present,&device)); }
    if(late) {
        Check(InstallLegacyGraphicsHooks({Begin,Process}),"late hook installation");
        PollLegacyGraphicsHooks();
        const auto probe=GetLegacyGraphicsStats();printf("Late probe classic=0x%X Ex=0x%X\n",unsigned(probe.d3d9Probe),unsigned(probe.d3d9ExProbe));
    }
    effect=true; const unsigned initial=processedFrames;
    unsigned displayed=0;
    for(unsigned frame=0;frame<40;++frame) {
        Pump(); HR(device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,255),1,0));
        D3DRECT top{0,0,LONG(expectedWidth),LONG(expectedHeight/2)};
        HR(device->Clear(1,&top,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,0,0),1,0));
        HR(device->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE));
        ComPtr<IDirect3DSurface9> bb; HR(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb));
        Frame9(device.Get(),nullptr,window);
        DWORD value{}; HR(device->GetRenderState(D3DRS_ZENABLE,&value)); Check(value==D3DZB_FALSE,"D3D9 render state restored");
        if(state9.transfer.valid) {
            ComPtr<IDirect3DSurface9> read;
            HR(device->CreateOffscreenPlainSurface(expectedWidth,expectedHeight,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr));
            HR(device->GetRenderTargetData(bb.Get(),read.Get())); D3DLOCKED_RECT map{}; HR(read->LockRect(&map,nullptr,D3DLOCK_READONLY));
            const unsigned t=*reinterpret_cast<const unsigned*>(static_cast<const char*>(map.pBits)+expectedHeight/4*map.Pitch+expectedWidth*2);
            const unsigned b=*reinterpret_cast<const unsigned*>(static_cast<const char*>(map.pBits)+expectedHeight*3/4*map.Pitch+expectedWidth*2);
            read->UnlockRect(); Check((t&0xffffff)==0xffff00&&(b&0xffffff)==0x00ffff,"D3D9 output orientation/channels"); ++displayed;
        }
        // Avoid capturing the post-filter frame twice; public hook runs with
        // processing disabled, then a separate public-hook-only section follows.
        { Reentry bypass; HR(device->Present(nullptr,nullptr,nullptr,nullptr)); }
        Sleep(5);
    }
    Check(processedFrames-initial>30&&displayed>25&&inputOkay,"D3D9 capture/process/output");
    const auto before=GetLegacyGraphicsStats(); effect=false;
    for(unsigned i=0;i<8;++i) HR(device->Present(nullptr,nullptr,nullptr,nullptr));
    const auto after=GetLegacyGraphicsStats();
    Check(before.captures==after.captures&&before.processed==after.processed&&before.displayed==after.displayed,"disabled D3D9 has no transfer work");
    effect=true;
    const auto hookBegins=beginFrames;
    for(unsigned frame=0;frame<10;++frame) {
        HR(device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,255),1,0));
        D3DRECT top{0,0,LONG(expectedWidth),LONG(expectedHeight/2)};
        HR(device->Clear(1,&top,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,0,0),1,0));
        if(ex) HR(deviceEx->PresentEx(nullptr,nullptr,nullptr,nullptr,0));
        else HR(device->Present(nullptr,nullptr,nullptr,nullptr));
        Sleep(5);
    }
    if(beginFrames-hookBegins!=10)printf("Public begin delta=%u\n",beginFrames-hookBegins);
    Check(beginFrames-hookBegins==10,"native Present hook executes exactly once per frame");
    {
        ComPtr<IDirect3DSwapChain9> extra;D3DPRESENT_PARAMETERS extraParams=present;
        HR(device->CreateAdditionalSwapChain(&extraParams,&extra));
        ComPtr<IDirect3DSurface9> buffer;HR(extra->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,&buffer));
        const auto extraBegins=beginFrames;
        for(unsigned frame=0;frame<8;++frame) {
            HR(device->ColorFill(buffer.Get(),nullptr,D3DCOLOR_XRGB(0,0,255)));
            RECT top{0,0,LONG(expectedWidth),LONG(expectedHeight/2)};
            HR(device->ColorFill(buffer.Get(),&top,D3DCOLOR_XRGB(255,0,0)));
            HR(extra->Present(nullptr,nullptr,nullptr,nullptr,0));Sleep(5);
        }
        Check(beginFrames-extraBegins==8,"additional swapchain hooks execute exactly once");
    }
    Check(inputOkay,"public presentation hooks preserve source frame input");
    // Reset with buffered NR output must succeed without retained backbuffers.
    if(ex) HR(deviceEx->ResetEx(&present,nullptr)); else HR(device->Reset(&present));
    Check(!state9.readback&&!state9.upload,"reset retired native surfaces");
    DestroyWindow(window);
    printf("PASS D3D9%s%s: %u processed, %u output checks, disabled gate, public Present, extra swapchain, Reset; capture %.3f ms\n",ex?"Ex":"",on12?"On12":"",processedFrames-initial,displayed,before.lastCaptureMs);
}

std::vector<unsigned char> Snapshot9(IDirect3DDevice9* device) {
    std::vector<unsigned char> result;
    auto append=[&](const auto& value) { const auto* p=reinterpret_cast<const unsigned char*>(&value);result.insert(result.end(),p,p+sizeof(value)); };
    for(unsigned state=0;state<210;++state) { DWORD v=0;
        if(SUCCEEDED(device->GetRenderState(D3DRENDERSTATETYPE(state),&v))) { append(state);append(v); } }
    for(unsigned stage=0;stage<8;++stage)for(unsigned state=0;state<33;++state) { DWORD v=0;
        if(SUCCEEDED(device->GetTextureStageState(stage,D3DTEXTURESTAGESTATETYPE(state),&v))) { append(stage);append(state);append(v); } }
    for(unsigned sampler=0;sampler<16;++sampler)for(unsigned state=1;state<14;++state) { DWORD v=0;
        if(SUCCEEDED(device->GetSamplerState(sampler,D3DSAMPLERSTATETYPE(state),&v))) { append(sampler);append(state);append(v); } }
    for(unsigned stage=0;stage<16;++stage) { ComPtr<IDirect3DBaseTexture9> texture;device->GetTexture(stage,&texture);append(texture.Get()); }
    for(unsigned stream=0;stream<16;++stream) { ComPtr<IDirect3DVertexBuffer9> buffer;UINT offset=0,stride=0,frequency=0;
        device->GetStreamSource(stream,&buffer,&offset,&stride);device->GetStreamSourceFreq(stream,&frequency);
        append(buffer.Get());append(offset);append(stride);append(frequency); }
    ComPtr<IDirect3DIndexBuffer9> index;device->GetIndices(&index);append(index.Get());
    ComPtr<IDirect3DVertexShader9> vs;device->GetVertexShader(&vs);append(vs.Get());
    ComPtr<IDirect3DPixelShader9> ps;device->GetPixelShader(&ps);append(ps.Get());
    ComPtr<IDirect3DVertexDeclaration9> decl;device->GetVertexDeclaration(&decl);append(decl.Get());
    DWORD fvf=0;device->GetFVF(&fvf);append(fvf);
    for(unsigned index=0;index<4;++index) { ComPtr<IDirect3DSurface9> rt;device->GetRenderTarget(index,&rt);append(rt.Get()); }
    ComPtr<IDirect3DSurface9> depth;device->GetDepthStencilSurface(&depth);append(depth.Get());
    D3DVIEWPORT9 viewport{};device->GetViewport(&viewport);append(viewport);
    RECT scissor{};device->GetScissorRect(&scissor);append(scissor);
    D3DMATERIAL9 material{};device->GetMaterial(&material);append(material);
    for(unsigned index=0;index<6;++index) { float plane[4]{};device->GetClipPlane(index,plane);append(plane); }
    for(unsigned state=0;state<512;++state) { D3DMATRIX matrix{};
        if(SUCCEEDED(device->GetTransform(D3DTRANSFORMSTATETYPE(state),&matrix))) { append(state);append(matrix); } }
    float vertexConstants[256*4]{},pixelConstants[224*4]{};int vertexInt[16*4]{},pixelInt[16*4]{};
    BOOL vertexBool[16]{},pixelBool[16]{};
    HR(device->GetVertexShaderConstantF(0,vertexConstants,256));HR(device->GetPixelShaderConstantF(0,pixelConstants,224));
    HR(device->GetVertexShaderConstantI(0,vertexInt,16));HR(device->GetPixelShaderConstantI(0,pixelInt,16));
    HR(device->GetVertexShaderConstantB(0,vertexBool,16));HR(device->GetPixelShaderConstantB(0,pixelBool,16));
    append(vertexConstants);append(pixelConstants);append(vertexInt);append(pixelInt);append(vertexBool);append(pixelBool);
    return result;
}
void Test9Msaa() {
    expectedWidth=320;expectedHeight=192;
    HWND window=Window(expectedWidth,expectedHeight);
    ComPtr<IDirect3D9Ex> factory;HR(Direct3DCreate9Ex(D3D_SDK_VERSION,&factory));
    DWORD quality=0;HR(factory->CheckDeviceMultiSampleType(0,D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,TRUE,D3DMULTISAMPLE_4_SAMPLES,&quality));
    Check(quality>0,"native 4x MSAA support");
    D3DPRESENT_PARAMETERS p{};p.Windowed=TRUE;p.hDeviceWindow=window;p.BackBufferWidth=expectedWidth;p.BackBufferHeight=expectedHeight;
    p.BackBufferFormat=D3DFMT_X8R8G8B8;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.MultiSampleType=D3DMULTISAMPLE_4_SAMPLES;
    p.EnableAutoDepthStencil=TRUE;p.AutoDepthStencilFormat=D3DFMT_D24S8;
    ComPtr<IDirect3DDevice9Ex> device;
    HR(factory->CreateDeviceEx(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,nullptr,&device));
    Check(InstallLegacyGraphicsHooks({Begin,Process}),"MSAA late hook installation");
    PollLegacyGraphicsHooks();
    unsigned pixelChecks=0,stateChecks=0;
    {
        ComPtr<IDirect3DSurface9> bb,originalDepth,gameRt,gameMrt,gameDepth,resolved,readback;
        HR(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb));HR(device->GetDepthStencilSurface(&originalDepth));
        HR(device->CreateRenderTarget(64,64,D3DFMT_X8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&gameRt,nullptr));
        HR(device->CreateRenderTarget(64,64,D3DFMT_X8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&gameMrt,nullptr));
        HR(device->CreateDepthStencilSurface(64,64,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,TRUE,&gameDepth,nullptr));
        HR(device->CreateRenderTarget(expectedWidth,expectedHeight,D3DFMT_X8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr));
        HR(device->CreateOffscreenPlainSurface(expectedWidth,expectedHeight,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr));
        ComPtr<IDirect3DTexture9> texture;HR(device->CreateTexture(16,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture,nullptr));
        ComPtr<IDirect3DVertexBuffer9> vertices;ComPtr<IDirect3DIndexBuffer9> indices;
        HR(device->CreateVertexBuffer(256,0,0,D3DPOOL_DEFAULT,&vertices,nullptr));
        HR(device->CreateIndexBuffer(64,0,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&indices,nullptr));
        ComPtr<ID3DBlob> vsCode,psCode,errors;
        const char vsSource[]="float4 main(float4 p:POSITION):POSITION {return p;}";
        const char psSource[]="float4 main():COLOR {return float4(.3,.4,.5,.6);}";
        HR(D3DCompile(vsSource,sizeof(vsSource),nullptr,nullptr,nullptr,"main","vs_3_0",0,0,&vsCode,&errors));
        HR(D3DCompile(psSource,sizeof(psSource),nullptr,nullptr,nullptr,"main","ps_3_0",0,0,&psCode,&errors));
        ComPtr<IDirect3DVertexShader9> vs;ComPtr<IDirect3DPixelShader9> ps;
        HR(device->CreateVertexShader(static_cast<DWORD*>(vsCode->GetBufferPointer()),&vs));
        HR(device->CreatePixelShader(static_cast<DWORD*>(psCode->GetBufferPointer()),&ps));
        const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
        ComPtr<IDirect3DVertexDeclaration9> declaration;HR(device->CreateVertexDeclaration(elements,&declaration));
        for(unsigned frame=0;frame<30;++frame) {
            HR(device->SetDepthStencilSurface(nullptr));HR(device->SetRenderTarget(1,nullptr));HR(device->SetRenderTarget(0,bb.Get()));
            HR(device->SetDepthStencilSurface(originalDepth.Get()));
            HR(device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,255),1,0));
            D3DRECT top{0,0,LONG(expectedWidth),LONG(expectedHeight/2)};
            HR(device->Clear(1,&top,D3DCLEAR_TARGET,D3DCOLOR_XRGB(255,0,0),1,0));
            HR(device->SetDepthStencilSurface(nullptr));HR(device->SetRenderTarget(0,gameRt.Get()));
            HR(device->SetRenderTarget(1,gameMrt.Get()));HR(device->SetDepthStencilSurface(gameDepth.Get()));
            const D3DVIEWPORT9 view{2,3,48,40,.25f,.75f};HR(device->SetViewport(&view));
            const RECT scissor{4,5,16,17};HR(device->SetScissorRect(&scissor));
            HR(device->SetTexture(0,texture.Get()));HR(device->SetTexture(1,texture.Get()));
            HR(device->SetStreamSource(0,vertices.Get(),16,12));HR(device->SetIndices(indices.Get()));
            HR(device->SetVertexShader(vs.Get()));HR(device->SetPixelShader(ps.Get()));HR(device->SetVertexDeclaration(declaration.Get()));
            const std::pair<D3DRENDERSTATETYPE,DWORD> states[]={{D3DRS_ZENABLE,TRUE},{D3DRS_ZWRITEENABLE,TRUE},{D3DRS_STENCILENABLE,TRUE},
                {D3DRS_ALPHABLENDENABLE,TRUE},{D3DRS_SEPARATEALPHABLENDENABLE,TRUE},{D3DRS_ALPHATESTENABLE,TRUE},{D3DRS_SCISSORTESTENABLE,TRUE},
                {D3DRS_FOGENABLE,TRUE},{D3DRS_LIGHTING,TRUE},{D3DRS_SPECULARENABLE,TRUE},{D3DRS_SRGBWRITEENABLE,TRUE},{D3DRS_DITHERENABLE,TRUE},
                {D3DRS_CLIPPLANEENABLE,1},{D3DRS_CULLMODE,D3DCULL_CCW},{D3DRS_FILLMODE,D3DFILL_WIREFRAME},{D3DRS_COLORWRITEENABLE,8},
                {D3DRS_MULTISAMPLEANTIALIAS,FALSE},{D3DRS_MULTISAMPLEMASK,5}};
            for(const auto& state:states)HR(device->SetRenderState(state.first,state.second));
            HR(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_ADD));HR(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_ADD));
            HR(device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_MODULATE));HR(device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,1));
            HR(device->SetTextureStageState(0,D3DTSS_RESULTARG,D3DTA_TEMP));HR(device->SetRenderState(D3DRS_WRAP0,D3DWRAP_U|D3DWRAP_V));
            HR(device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_COUNT2));
            HR(device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));HR(device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR));
            HR(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR));HR(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_MIRROR));
            HR(device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP));HR(device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,TRUE));
            const float constants[4]={.7f,.2f,.8f,.6f};HR(device->SetVertexShaderConstantF(9,constants,1));HR(device->SetPixelShaderConstantF(11,constants,1));
            const auto before=Snapshot9(device.Get());
            Frame9(device.Get(),nullptr,window);
            Check(before==Snapshot9(device.Get()),"MSAA output restores complete queried D3D9 device state");++stateChecks;
            if(state9.transfer.valid) {
                HR(device->StretchRect(bb.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE));
                HR(device->GetRenderTargetData(resolved.Get(),readback.Get()));D3DLOCKED_RECT map{};HR(readback->LockRect(&map,nullptr,D3DLOCK_READONLY));
                for(UINT y:{0u,expectedHeight/4,expectedHeight*3/4,expectedHeight-1})for(UINT x:{0u,expectedWidth/2,expectedWidth-1}) {
                    const auto value=*reinterpret_cast<const unsigned*>(static_cast<const char*>(map.pBits)+size_t(y)*map.Pitch+x*4);
                    Check((value&0xffffff)==(y<expectedHeight/2?0xffff00u:0x00ffffu),"MSAA output exact pixels including image edges");++pixelChecks;
                }
                readback->UnlockRect();
            }
            { Reentry bypass; HR(device->PresentEx(nullptr,nullptr,nullptr,nullptr,0)); }Sleep(5);
        }
        const auto offBefore=GetLegacyGraphicsStats();const auto stateBefore=Snapshot9(device.Get());effect=false;
        for(unsigned i=0;i<8;++i)Frame9(device.Get(),nullptr,window);
        const auto offAfter=GetLegacyGraphicsStats();effect=true;
        Check(offBefore.captures==offAfter.captures&&offBefore.processed==offAfter.processed&&offBefore.displayed==offAfter.displayed,
            "MSAA disabled gate has zero resolve/upload/draw work");
        Check(stateBefore==Snapshot9(device.Get()),"MSAA disabled state unchanged");
        HR(device->SetDepthStencilSurface(nullptr));HR(device->SetRenderTarget(1,nullptr));HR(device->SetRenderTarget(0,bb.Get()));
        HR(device->SetTexture(0,nullptr));HR(device->SetTexture(1,nullptr));HR(device->SetStreamSource(0,nullptr,0,0));
        HR(device->SetIndices(nullptr));HR(device->SetVertexShader(nullptr));HR(device->SetPixelShader(nullptr));HR(device->SetVertexDeclaration(nullptr));
    }
    Check(inputOkay&&pixelChecks>300,"MSAA capture/process/output sequence");
    HR(device->ResetEx(&p,nullptr));Check(!state9.resolve&&!state9.outputTexture,"MSAA reset releases owned default-pool resources");
    DestroyWindow(window);
    printf("PASS native D3D9Ex 4x MSAA: %u input frames, %u edge/interior pixel checks, %u complete state snapshots, disabled gate, ResetEx\n",inputChecks,pixelChecks,stateChecks);
}

// Controlled COM surfaces isolate D3D9 channel/pitch/reset behavior when this
// host's native D3D9 HAL is unavailable. The accelerator and GPU transfers are
// still real D3D11. This is deliberately separate from --d3d9's native test.
struct SurfaceFixture {
    void** vtable;
    D3DSURFACE_DESC desc{};
    std::vector<unsigned char> data;
    UINT pitch = 0;
    static ULONG STDMETHODCALLTYPE Release(SurfaceFixture* s) { delete s; return 0; }
    static HRESULT STDMETHODCALLTYPE Desc(SurfaceFixture* s,D3DSURFACE_DESC* out) { *out=s->desc;return S_OK; }
    static HRESULT STDMETHODCALLTYPE Lock(SurfaceFixture* s,D3DLOCKED_RECT* out,const RECT*,DWORD) { out->Pitch=s->pitch;out->pBits=s->data.data();return S_OK; }
    static HRESULT STDMETHODCALLTYPE Unlock(SurfaceFixture*) { return S_OK; }
    static void** Table() {
        static void* table[17]{}; table[2]=reinterpret_cast<void*>(Release);table[12]=reinterpret_cast<void*>(Desc);
        table[13]=reinterpret_cast<void*>(Lock);table[14]=reinterpret_cast<void*>(Unlock);return table;
    }
    SurfaceFixture(UINT w,UINT h,D3DFORMAT format) : vtable(Table()) {
        desc.Width=w;desc.Height=h;desc.Format=format;desc.Type=D3DRTYPE_SURFACE;
        desc.MultiSampleType=D3DMULTISAMPLE_NONE;
        pitch=(w*4+63)&~63u;data.resize(size_t(pitch)*h);
    }
    IDirect3DSurface9* Get() { return reinterpret_cast<IDirect3DSurface9*>(this); }
};
struct DeviceFixture {
    void** vtable;
    unsigned reads = 0, writes = 0;
    static HRESULT STDMETHODCALLTYPE Create(DeviceFixture*,UINT w,UINT h,D3DFORMAT f,D3DPOOL,IDirect3DSurface9** out,HANDLE*) {
        *out=(new SurfaceFixture(w,h,f))->Get();return S_OK;
    }
    static HRESULT Copy(SurfaceFixture* src,SurfaceFixture* dest) {
        if(src->desc.Width!=dest->desc.Width||src->desc.Height!=dest->desc.Height||src->desc.Format!=dest->desc.Format)return E_INVALIDARG;
        for(UINT y=0;y<src->desc.Height;++y) memcpy(dest->data.data()+size_t(y)*dest->pitch,src->data.data()+size_t(y)*src->pitch,src->desc.Width*4);
        return S_OK;
    }
    static HRESULT STDMETHODCALLTYPE Read(DeviceFixture* d,IDirect3DSurface9* src,IDirect3DSurface9* dest) {
        ++d->reads;return Copy(reinterpret_cast<SurfaceFixture*>(src),reinterpret_cast<SurfaceFixture*>(dest));
    }
    static HRESULT STDMETHODCALLTYPE Write(DeviceFixture* d,IDirect3DSurface9* src,const RECT*,IDirect3DSurface9* dest,const POINT*) {
        ++d->writes;return Copy(reinterpret_cast<SurfaceFixture*>(src),reinterpret_cast<SurfaceFixture*>(dest));
    }
    static void** Table() {
        static void* table[119]{};table[30]=reinterpret_cast<void*>(Write);table[32]=reinterpret_cast<void*>(Read);
        table[36]=reinterpret_cast<void*>(Create);return table;
    }
    DeviceFixture() : vtable(Table()) {}
    IDirect3DDevice9* Get() { return reinterpret_cast<IDirect3DDevice9*>(this); }
};
void Test9Transfer() {
    callbacks={Begin,Process};stopped=false;
    HWND window=Window(320,192);DeviceFixture device;
    unsigned displayed=0;
    for(unsigned segment=0;segment<3;++segment) {
        expectedWidth=321+16*segment;expectedHeight=192+16*segment;
        SurfaceFixture back(expectedWidth,expectedHeight,segment%2?D3DFMT_A8R8G8B8:D3DFMT_X8R8G8B8);
        for(unsigned frame=0;frame<20;++frame) {
            for(UINT y=0;y<expectedHeight;++y)for(UINT x=0;x<expectedWidth;++x) {
                auto* p=back.data.data()+size_t(y)*back.pitch+x*4;
                p[0]=y<expectedHeight/2?0:255;p[1]=0;p[2]=y<expectedHeight/2?255:0;p[3]=255;
            }
            state9.Present(device.Get(),back.Get(),window);
            if(state9.transfer.valid) {
                const auto* t=back.data.data()+size_t(expectedHeight/4)*back.pitch;
                const auto* b=back.data.data()+size_t(expectedHeight*3/4)*back.pitch;
                Check(t[0]==0&&t[1]==255&&t[2]==255&&b[0]==255&&b[1]==255&&b[2]==0,"controlled D3D9 output RGB/orientation/padded pitch");
                ++displayed;
            }
            Sleep(5);
        }
        const auto before=GetLegacyGraphicsStats();const auto reads=device.reads,writes=device.writes;
        effect=false;
        for(unsigned i=0;i<5;++i)state9.Present(device.Get(),back.Get(),window);
        Check(reads==device.reads&&writes==device.writes&&before.processed==GetLegacyGraphicsStats().processed,"controlled D3D9 disabled cost");
        effect=true;state9.Release();Check(!state9.readback&&!state9.upload,"controlled D3D9 reset release");
    }
    Check(inputOkay&&displayed>50,"controlled D3D9 transfer sequence");
    SurfaceFixture unsupported(320,192,D3DFMT_R5G6B5);
    const auto reads=device.reads;state9.Present(device.Get(),unsupported.Get(),window);
    Check(state9.failed&&device.reads==reads,"unsupported D3D9 format bypassed");
    state9.Release();
    DestroyWindow(window);
    printf("PASS controlled D3D9 surfaces + real D3D11: %u input, %u output, padded pitches, A8/X8, resize/reset, disabled gate, unsupported bypass\n",inputChecks,displayed);
}
}
int main(int argc,char** argv) try {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    setvbuf(stdout,nullptr,_IONBF,0);
    if (argc>1&&strcmp(argv[1],"--probe9")==0) {
        HWND w=Window(320,192); ComPtr<IDirect3D9> d; d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        Check(bool(d),"native probe factory");
        wchar_t module[MAX_PATH]{};GetModuleFileNameW(GetModuleHandleW(L"d3d9.dll"),module,MAX_PATH);
        DWORD session{};ProcessIdToSessionId(GetCurrentProcessId(),&session);
        printf("D3D_SDK_VERSION=%u session=%lu remote=%d\n",D3D_SDK_VERSION,session,GetSystemMetrics(SM_REMOTESESSION));
        wprintf(L"D3D9 module=%ls\n",module);
        for (UINT a=0;a<d->GetAdapterCount();++a) {
            D3DCAPS9 caps{}; D3DDISPLAYMODE mode{}; d->GetAdapterDisplayMode(a,&mode);
            D3DADAPTER_IDENTIFIER9 id{};d->GetAdapterIdentifier(a,0,&id);
            const auto hr=d->GetDeviceCaps(a,D3DDEVTYPE_HAL,&caps);
            printf("Adapter %u: %s vendor=0x%X device=0x%X caps hr=0x%X caps=0x%X display=%ux%u format=%u CheckDeviceType=0x%X\n",
                a,id.Description,id.VendorId,id.DeviceId,unsigned(hr),caps.DevCaps,mode.Width,mode.Height,unsigned(mode.Format),
                unsigned(d->CheckDeviceType(a,D3DDEVTYPE_HAL,mode.Format,D3DFMT_X8R8G8B8,TRUE)));
        }
        ComPtr<IDirect3D9Ex> ex;const auto exHr=Direct3DCreate9Ex(D3D_SDK_VERSION,&ex);
        printf("Direct3DCreate9Ex hr=0x%X\n",unsigned(exHr));
        if(ex)for(UINT a=0;a<ex->GetAdapterCount();++a) { D3DCAPS9 caps{};
            printf("Ex adapter %u caps hr=0x%X\n",a,unsigned(ex->GetDeviceCaps(a,D3DDEVTYPE_HAL,&caps))); }
        for (DWORD flags : {DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING),DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING),DWORD(D3DCREATE_MIXED_VERTEXPROCESSING)}) {
            D3DPRESENT_PARAMETERS p{}; p.Windowed=TRUE;p.hDeviceWindow=w;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.BackBufferCount=1;
            p.BackBufferWidth=320;p.BackBufferHeight=192;p.BackBufferFormat=D3DFMT_UNKNOWN;
            ComPtr<IDirect3DDevice9> device; printf("Native CreateDevice flags=0x%X => 0x%X\n",flags,unsigned(d->CreateDevice(0,D3DDEVTYPE_HAL,w,flags,&p,&device)));
        }
        DestroyWindow(w); return 0;
    }
    if(argc>1&&strcmp(argv[1],"--d3d9")==0) {
        Check(InstallLegacyGraphicsHooks({Begin,Process}),"hook installation"); Test9(false); Test9(true);
    } else if(argc>1&&strcmp(argv[1],"--d3d9ex")==0) {
        Check(InstallLegacyGraphicsHooks({Begin,Process}),"hook installation"); Test9(true);
    } else if(argc>1&&strcmp(argv[1],"--d3d9ex-late")==0) {
        Test9(true,false,true);
    } else if(argc>1&&strcmp(argv[1],"--d3d9ex-msaa")==0) {
        Test9Msaa();
    } else if(argc>1&&strcmp(argv[1],"--d3d9on12")==0) {
        Check(InstallLegacyGraphicsHooks({Begin,Process}),"hook installation"); Test9(false,true); Test9(true,true);
    } else if(argc>1&&strcmp(argv[1],"--d3d9-transfer")==0) {
        Test9Transfer();
    } else TestGL();
    StopLegacyGraphicsHooks(); RestoreAllVtablePatches();
    printf("PASS legacy bridge: %u verified input frames\n",inputChecks); return 0;
} catch(const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what()); StopLegacyGraphicsHooks(); RestoreAllVtablePatches(); return 1; }
