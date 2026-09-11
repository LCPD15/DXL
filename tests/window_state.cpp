#include "../src/ui/WindowState.h"
#include "../src/ui/UiLanguage.h"
#include <stdexcept>
#include <cstdio>
using namespace DXL::WindowState;
static void Check(bool v,const char* msg) { if (!v) throw std::runtime_error(msg); }
int main() try {
    const State expected{-1300,42,1100,760,true};
    const auto restored=Parse(Serialize(expected));
    Check(restored && restored->left==-1300 && restored->width==1100 && restored->height==760 && restored->maximized,"round trip");
    Check(!Parse("{}") && !Parse("{\"version\":2}") && !Parse("{\"version\":1junk}"),"invalid storage");
    auto invalid=expected;invalid.width=-1;Check(!Parse(Serialize(invalid)),"invalid dimensions");
    const auto fitted=Fit({30000,-30000,1600,1100,true},RECT{0,40,1280,1000});
    Check(fitted.left==0 && fitted.top==40 && fitted.width==1280 && fitted.height==960 && fitted.maximized,"monitor removal/clamp");
    const auto secondary=Fit(expected,RECT{-1920,0,0,1040});
    Check(secondary.left==-1300 && secondary.width==1100,"secondary monitor");
    const auto small=Fit(expected,RECT{0,0,640,480});
    Check(small.width==640 && small.height==480,"small display");
    const auto path=std::filesystem::temp_directory_path()/("dxl-window-test-"+std::to_string(GetCurrentProcessId())+".json");
    Check(Save(path,expected),"save");
    Check(Load(path)->maximized,"load");
    HWND wnd=CreateWindowExW(0,L"STATIC",L"DXL window fixture",WS_OVERLAPPEDWINDOW,100,100,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(wnd!=nullptr,"create hidden fixture");
    const auto captured=Capture(wnd);DestroyWindow(wnd);
    Check(captured && captured->width==1000 && captured->height==700 && !captured->maximized,"native normal placement");
    std::filesystem::remove(path);
    using namespace DXL::UiLanguage;
    Check(Resolve("auto",MAKELANGID(LANG_CHINESE,SUBLANG_CHINESE_TRADITIONAL))==0,"Chinese OS");
    Check(Resolve("",MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US))==2,"English OS");
    Check(Resolve("auto",MAKELANGID(LANG_GERMAN,SUBLANG_GERMAN))==2,"unsupported OS English fallback");
    Check(Resolve("en",MAKELANGID(LANG_CHINESE,SUBLANG_CHINESE_SIMPLIFIED))==2,"saved choice");
    puts("PASS window placement persistence, invalid settings, monitor bounds, native capture and OS language policy");
    return 0;
} catch (const std::exception& e) { fprintf(stderr,"FAIL %s\n",e.what());return 1; }
