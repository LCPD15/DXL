// Operates on the dedicated fixture PID passed by test_fg_e2e.ps1.
#include "../src/common/IpcClient.h"
#include "../src/common/NrParameterEdit.h"
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>

struct ScreenProbe {
    DWORD pid;
    HWND hwnd{};
    HDC dc{};
    HBITMAP bitmap{}, oldBitmap{};
    unsigned* pixels{};
    int width=0, aligned=0, intermediate=0, invisible=0, last=-1, transitions=0;
    int croppedLeaks=0;
    bool monitorPixels;
    ScreenProbe(DWORD target, bool monitor):pid(target),monitorPixels(monitor) {
        if (!monitorPixels) return;
        EnumWindows([](HWND window, LPARAM param)->BOOL {
            auto self=reinterpret_cast<ScreenProbe*>(param);
            DWORD owner=0; GetWindowThreadProcessId(window,&owner);
            if(owner!=self->pid || !IsWindowVisible(window)) return TRUE;
            self->hwnd=window; return FALSE;
        },reinterpret_cast<LPARAM>(this));
        RECT r{};
        if(!hwnd||!GetClientRect(hwnd,&r)||r.right<=0)return;
        width=r.right;
        HDC screen=GetDC(nullptr);
        dc=CreateCompatibleDC(screen);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-1;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        if(dc&&bitmap)oldBitmap=(HBITMAP)SelectObject(dc,bitmap);
        ReleaseDC(nullptr,screen);
    }
    ~ScreenProbe(){if(dc&&oldBitmap)SelectObject(dc,oldBitmap);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
    void sample() {
        if(!monitorPixels||!pixels||!dc)return;
        RECT r{};GetClientRect(hwnd,&r);
        POINT p{0, r.bottom * 11 / 16}; ClientToScreen(hwnd,&p);
        HDC screen=GetDC(nullptr);
        const bool copied=BitBlt(dc,0,0,width,1,screen,p.x,p.y,SRCCOPY);
        ReleaseDC(nullptr,screen);GdiFlush();
        int xFound=-1, run=0;
        // The crop fixture paints the excluded area solid red.
        if (copied && width > 16) {
            const auto rgb = pixels[width * 3 / 4] & 0xffffff;
            if (((rgb >> 16) & 255) > 200 && ((rgb >> 8) & 255) < 36 && (rgb & 255) < 36)
                ++croppedLeaks;
        }
        if(copied)for(int x=0;x<width;x++){
            const auto rgb=pixels[x];
            if(((rgb>>16)&255)>194 && ((rgb>>8)&255)>145 && (rgb&255)<110){
                if(++run==32){xFound=x-31;break;}
            }else run=0;
        }
        if(xFound<0){invisible++;return;}
        if(xFound==last)return;
        last=xFound;transitions++;
        const int mod=xFound%16;
        if(mod<=2||mod>=14)aligned++;else intermediate++;
    }
};

int wmain(int argc,wchar_t** argv) try {
    if(argc!=3)return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    setvbuf(stdout,nullptr,_IONBF,0);
    const DWORD pid=wcstoul(argv[1],nullptr,10);
    const bool probe=wcscmp(argv[2],L"12")==0;
    DXL::StatusView view;
    const auto expiry=GetTickCount64()+15000;
    while(!view.Open(pid)) {if(GetTickCount64()>=expiry)throw std::runtime_error("missing core mapping");Sleep(50);}
    const auto read=[&] {DXL::Ipc::Status s{};if(!view.Read(s)||s.version<16)throw std::runtime_error("incompatible or missing status");return s;};
    const auto edit=[&](bool enabled){
        uint32_t packed{};
        if(!DXL::EncodeNrEdit("fgEnabled",enabled,packed) || !DXL::SendCommand(pid,DXL::Ipc::CommandId::EditNrParameter,packed))
            throw std::runtime_error("FG edit rejected");
    };
    const auto master=[&](bool on){if(!DXL::SendCommand(pid,DXL::Ipc::CommandId::SetEnabled,on))throw std::runtime_error("master edit rejected");};
    const auto setRate=[&](unsigned rate) {
        uint32_t packed{};
        if (!DXL::EncodeNrEdit("fgMultiplier",rate,packed) || !DXL::SendCommand(pid,DXL::Ipc::CommandId::EditNrParameter,packed))
            throw std::runtime_error("FG multiplier edit rejected");
    };
    const auto readyDeadline=GetTickCount64()+15000;
    while (!read().presentCount) {
        if(GetTickCount64()>readyDeadline)throw std::runtime_error("present hook did not start");
        Sleep(50);
    }
    const auto check=[&](const char* label,bool fg,bool all,bool running, unsigned rate=2){
        Sleep(600);
        if((read().masterEnabled!=0)!=all)throw std::runtime_error("unexpected master setting");
        master(all); // publish a fresh baseline after draining any in-flight presentation
        if (running) {
            const auto until=GetTickCount64()+12000;
            do {
                master(all);
                const auto s=read();
                if (s.fgStatus==2 && s.fgMultiplier==rate) break;
                Sleep(200);
            } while(GetTickCount64()<until);
        }
        const auto start=read();
        const auto sampleStart=GetTickCount64();
        const auto stop=GetTickCount64()+(running?10000:2200);
        DXL::Ipc::Status end{};
        ScreenProbe screen(pid,probe);
        unsigned attempts=0;
        do {
            screen.sample();Sleep(4);
            if(++attempts%70==0) {
                master(all);
                end=read();
                if(running&&end.presentCount>=start.presentCount+48)break;
            }
        }while(GetTickCount64()<stop);
        // The core publishes every 120 presents or on command. Refresh the
        // snapshot without changing the active master setting.
        master(all);
        end=read();
        const auto host=end.presentCount-start.presentCount;
        const auto generated=end.fgPresented-start.fgPresented;
        printf("%s opted=%u master=%u state=%u source_delta=%llu generated_delta=%llu generated_total=%llu visible_anchored=%d intermediate_edges=%d invisible_samples=%d\n",
                label,end.fgEnabled,end.masterEnabled,end.fgStatus,host,generated,end.fgPresented,screen.aligned,screen.intermediate,screen.invisible);
        printf("multiplier=%u max=%u generated_per_source=%.3f cropped_red_samples=%d\n",
            end.fgMultiplier,end.fgMaxMultiplier,double(generated)/std::max<double>(host,1),screen.croppedLeaks);
        const double elapsed=double(GetTickCount64()-sampleStart)*0.001;
        const double observedIn=double(host)/elapsed;
        const double observedOut=rate>1 ? generated*(double(rate)/(rate-1))/elapsed : 0;
        printf("telemetry input_fps=%.2f observed=%.2f output_fps=%.2f observed=%.2f output_latency_ms=%.3f\n",
            end.fgInputFps, observedIn, end.fgOutputFps, observedOut, end.fgOutputLatencyMs);
        const auto comparable=[](double reported,double measured) {
            return measured>0 && std::isfinite(reported) && std::fabs(reported-measured)<std::max(2.0,measured*0.27);
        };
        if(!comparable(end.fgInputFps,observedIn) ||
           (running ? !comparable(end.fgOutputFps,observedOut) || !std::isfinite(end.fgOutputLatencyMs) || end.fgOutputLatencyMs<0
                    : end.fgOutputFps>0 || end.fgOutputLatencyMs>=0))
            throw std::runtime_error("FG timing does not match the observed fixture cadence");
        if(end.fgMultiplier!=rate || (running && (end.fgMaxMultiplier<rate ||
             generated < host * (rate-1) * 0.82 || generated > host * (rate-1) + 5)) || (probe && screen.croppedLeaks>5))
            throw std::runtime_error("multiplier or source geometry incorrect");
        if((end.fgEnabled!=0)!=fg||(end.masterEnabled!=0)!=all||host<4||
            (running?(end.fgStatus!=2||generated<8):generated!=0))throw std::runtime_error(label);
    };
    check("default",false,true,false);
    master(false);edit(true);check("armed",true,false,false);
    master(true);check("active",true,true,true);
    setRate(3);check("active_3x",true,true,true,3);
    setRate(4);check("active_4x",true,true,true,4);
    master(false);check("master_off",true,false,false,4);
    master(true);check("resumed",true,true,true,4);
    edit(false);check("FG_off",false,true,false,4);
    printf("PASS FG default, opt-in while master off, running, paused, resumed, FG off\n");
    return 0;
} catch (const std::exception& ex) {printf("FAIL %s\n",ex.what());return 1;}

